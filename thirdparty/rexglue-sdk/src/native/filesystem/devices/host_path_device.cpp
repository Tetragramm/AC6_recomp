/**
 * ReXGlue native filesystem layer
 * Part of the AC6 Recompilation project
 */

#include "host_path_entry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <native/filesystem.h>
#include <native/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string/utf8.h>

namespace rex::filesystem {

namespace {

bool HasAsciiSuffix(const std::string& text, const char* suffix) {
  const size_t suffix_len = std::strlen(suffix);
  return text.size() >= suffix_len &&
         text.compare(text.size() - suffix_len, suffix_len, suffix) == 0;
}

// Host-side artifact of the atomic write path? (never guest-visible)
bool IsAtomicWriteArtifactName(const std::string& utf8_name) {
  return HasAsciiSuffix(utf8_name, kAtomicWriteTempSuffix) ||
         HasAsciiSuffix(utf8_name, kAtomicWriteBackupSuffix);
}

}  // namespace

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path, bool read_only)
    : Device(mount_path), name_("STFS"), host_path_(host_path), read_only_(read_only) {}

HostPathDevice::~HostPathDevice() = default;

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      REXFS_ERROR("Host path does not exist");
      return false;
    }
  }

  if (!read_only_) {
    SweepStaleAtomicArtifacts();
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);
  PopulateEntry(root_entry);

  return true;
}

void HostPathDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  auto* resolved = root_entry_->ResolvePath(path);
  if (resolved) {
    return resolved;
  }

  // Fallback to a lazy case-insensitive host lookup when an entry is missing
  // from the in-memory tree (for example because casing differs on Linux).
  auto* current_entry = static_cast<HostPathEntry*>(root_entry_.get());
  for (const auto& part : rex::string::utf8_split_path(path)) {
    if (part.empty()) {
      continue;
    }

    auto* child = current_entry->GetChild(part);
    if (!child) {
      auto child_infos = rex::filesystem::ListFiles(current_entry->host_path());
      auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {
        return rex::string::utf8_equal_case(rex::path_to_utf8(info.name), part);
      });
      if (match == child_infos.end()) {
        return nullptr;
      }

      auto new_child = HostPathEntry::Create(this, current_entry,
                                             current_entry->host_path() / match->name, *match);
      if (!new_child) {
        return nullptr;
      }
      child = new_child;
      current_entry->children_.push_back(std::unique_ptr<Entry>(new_child));
    }

    current_entry = static_cast<HostPathEntry*>(child);
  }

  return current_entry;
}

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  auto child_infos = rex::filesystem::ListFiles(parent_entry->host_path());
  for (auto& child_info : child_infos) {
    if (child_info.type == rex::filesystem::FileInfo::Type::kFile &&
        IsAtomicWriteArtifactName(rex::path_to_utf8(child_info.name))) {
      // .rex-tmp / .rex-bak are host-side artifacts of the atomic write
      // path; the guest must never see them.
      continue;
    }
    auto child = HostPathEntry::Create(this, parent_entry,
                                       parent_entry->host_path() / child_info.name, child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (child_info.type == rex::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child);
    }
  }
}

void HostPathDevice::SweepStaleAtomicArtifacts() {
  // A previous session that died mid-write can leave "<name>.rex-tmp" behind
  // (and, in the narrow window between the two commit renames, the real file
  // moved aside to "<name>.rex-bak" with the temp never renamed in). Recover
  // the backup when the real file is missing, then drop stale temps. .rex-bak
  // files themselves are kept: they are the one previous generation the
  // atomic write path maintains.
  std::vector<std::filesystem::path> stale_temps;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           host_path_, std::filesystem::directory_options::skip_permission_denied, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code file_ec;
    if (!it->is_regular_file(file_ec)) {
      continue;
    }
    if (HasAsciiSuffix(rex::path_to_utf8(it->path().filename()), kAtomicWriteTempSuffix)) {
      stale_temps.push_back(it->path());
    }
  }

  for (const auto& temp_path : stale_temps) {
    const std::string temp_utf8 = rex::path_to_utf8(temp_path);
    const auto real_path =
        rex::to_path(temp_utf8.substr(0, temp_utf8.size() - std::strlen(kAtomicWriteTempSuffix)));
    std::filesystem::path bak_path = real_path;
    bak_path += kAtomicWriteBackupSuffix;

    std::error_code sweep_ec;
    if (!std::filesystem::exists(real_path, sweep_ec) &&
        std::filesystem::exists(bak_path, sweep_ec)) {
      std::error_code restore_ec;
      std::filesystem::rename(bak_path, real_path, restore_ec);
      if (!restore_ec) {
        REXFS_WARN("Recovered '{}' from its backup after an interrupted write",
                   rex::path_to_utf8(real_path));
      }
    }
    std::error_code rm_ec;
    if (std::filesystem::remove(temp_path, rm_ec) && !rm_ec) {
      REXFS_INFO("Removed stale write temp '{}' (interrupted write; original kept)", temp_utf8);
    }
  }
}

bool HostPathDevice::WriteMarkerPhaseLocked(const char* phase) {
  auto marker = rex::filesystem::OpenFile(write_marker_path_, "wb");
  if (!marker) {
    return false;
  }
  // First line = the phase word (what the mount-time check parses); the rest
  // is for a human who finds the file.
  fprintf(marker,
          "%s\n\n"
          "Write-in-progress marker for the content folder next to this file.\n"
          "If it is still present at the next launch, the phase word on the first\n"
          "line decides what happens: 'committing' moves the folder aside into\n"
          "quarantine (kept, never deleted); 'writing' or 'complete' proves the\n"
          "folder consistent and only this marker is removed.\n",
          phase);
  fclose(marker);
  return true;
}

void HostPathDevice::OnAtomicWriteBegin() {
  if (write_marker_path_.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(write_marker_mutex_);
  if (active_atomic_writes_++ == 0) {
    // A new marker lifetime: phase starts at "writing" - no commit rename
    // has run, so a death anywhere before OnAtomicWriteCommit() leaves the
    // container provably consistent.
    marker_committing_ = false;
    if (!WriteMarkerPhaseLocked(kWriteMarkerPhaseWriting)) {
      REXFS_WARN("Could not create write-in-progress marker '{}'",
                 rex::path_to_utf8(write_marker_path_));
    }
  }
}

void HostPathDevice::OnAtomicWriteCommit() {
  if (write_marker_path_.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(write_marker_mutex_);
  if (active_atomic_writes_ > 0 && !marker_committing_) {
    // The first commit rename of this marker's lifetime is about to run:
    // from here until the marker is deleted, a death can tear the container
    // across files, so a leftover marker must quarantine. Monotonic - the
    // phase never returns to "writing" while this marker lives.
    if (WriteMarkerPhaseLocked(kWriteMarkerPhaseCommitting)) {
      marker_committing_ = true;
    } else {
      // Leave marker_committing_ unset so the next commit retries; a marker
      // stuck on "writing" while commits run would defeat torn-container
      // detection.
      REXFS_ERROR("Could not flip write-in-progress marker '{}' to phase '{}'",
                  rex::path_to_utf8(write_marker_path_), kWriteMarkerPhaseCommitting);
    }
  }
}

void HostPathDevice::OnAtomicWriteEnd() {
  if (write_marker_path_.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(write_marker_mutex_);
  if (active_atomic_writes_ > 0 && --active_atomic_writes_ == 0) {
    std::error_code ec;
    std::filesystem::remove(write_marker_path_, ec);
    if (ec) {
      // One retry: transient sharing violations (an antivirus scanning the
      // marker it just saw us write) often clear immediately.
      ec.clear();
      std::filesystem::remove(write_marker_path_, ec);
    }
    if (ec) {
      // Deletion is blocked, but a scanner that holds the file usually still
      // permits rewriting an existing one: record phase "complete" so the
      // next mount does not quarantine a container whose writes all
      // finished. This failure used to be swallowed silently.
      if (WriteMarkerPhaseLocked(kWriteMarkerPhaseComplete)) {
        REXFS_ERROR(
            "Could not delete write-in-progress marker '{}' ({}); rewrote it as phase '{}' so "
            "the container is not quarantined at the next launch",
            rex::path_to_utf8(write_marker_path_), ec.message(), kWriteMarkerPhaseComplete);
      } else {
        REXFS_ERROR(
            "Could not delete write-in-progress marker '{}' ({}) nor rewrite its phase - the "
            "container may be needlessly quarantined (kept, never deleted) at the next launch",
            rex::path_to_utf8(write_marker_path_), ec.message());
      }
    }
  }
}

std::string HostPathDevice::ReadWriteMarkerPhase(const std::filesystem::path& marker_path) {
  auto file = rex::filesystem::OpenFile(marker_path, "rb");
  if (!file) {
    return {};
  }
  char buffer[64] = {};
  const size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
  fclose(file);
  std::string phase(buffer, read);
  const size_t eol = phase.find_first_of("\r\n");
  if (eol != std::string::npos) {
    phase.resize(eol);
  }
  while (!phase.empty() && (phase.back() == ' ' || phase.back() == '\t')) {
    phase.pop_back();
  }
  return phase;
}

}  // namespace rex::filesystem
