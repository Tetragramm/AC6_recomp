/**
 * ReXGlue native filesystem layer
 * Part of the AC6 Recompilation project
 */

#include "host_path_entry.h"
#include "host_path_file.h"

#include <native/filesystem.h>
#include <native/filesystem/device.h>
#include <native/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/mapped_memory.h>
#include <rex/string.h>

namespace rex::filesystem {

HostPathEntry::HostPathEntry(Device* device, Entry* parent, const std::string_view path,
                             const std::filesystem::path& host_path)
    : Entry(device, parent, path), host_path_(host_path) {}

HostPathEntry::~HostPathEntry() = default;

HostPathEntry* HostPathEntry::Create(Device* device, Entry* parent,
                                     const std::filesystem::path& full_path,
                                     rex::filesystem::FileInfo file_info) {
  auto path = rex::string::utf8_join_guest_paths(parent->path(), rex::path_to_utf8(file_info.name));
  auto entry = new HostPathEntry(device, parent, path, full_path);

  entry->create_timestamp_ = file_info.create_timestamp;
  entry->access_timestamp_ = file_info.access_timestamp;
  entry->write_timestamp_ = file_info.write_timestamp;
  if (file_info.type == rex::filesystem::FileInfo::Type::kDirectory) {
    entry->attributes_ = kFileAttributeDirectory;
  } else {
    entry->attributes_ = kFileAttributeNormal;
    if (device->is_read_only()) {
      entry->attributes_ |= kFileAttributeReadOnly;
    }
    entry->size_ = file_info.total_size;
    entry->allocation_size_ = rex::round_up(file_info.total_size, device->bytes_per_sector());
  }
  return entry;
}

X_STATUS HostPathEntry::Open(uint32_t desired_access, File** out_file) {
  if (is_read_only() &&
      (desired_access & (FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    REXFS_ERROR("Attempting to open file for write access on read-only device");
    return X_STATUS_ACCESS_DENIED;
  }

  const bool wants_write =
      (desired_access & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                         FileAccess::kFileAppendData)) != 0;
  const bool truncate_pending = pending_truncate_;
  pending_truncate_ = false;

  // Atomic write session: guest writes land in "<name>.rex-tmp" in
  // the same directory (same volume, so the commit rename is atomic) and are
  // committed over the real file when the handle closes, keeping one
  // ".rex-bak" generation. A crash, kill or failed write mid-save leaves the
  // OLD file intact instead of a torn one - the mechanism that left saves
  // permanently stuck behind the game's "Game Data is corrupted / please
  // delete it" dialog.
  if (wants_write && !is_read_only()) {
    if (!atomic_write_active_) {
      std::filesystem::path temp_path = host_path_;
      temp_path += kAtomicWriteTempSuffix;

      std::error_code ec;
      bool temp_ready = false;
      if (!truncate_pending && std::filesystem::is_regular_file(host_path_, ec)) {
        // Preserve read-modify-write semantics: the handle must see the
        // current contents until the guest overwrites them.
        std::filesystem::copy_file(host_path_, temp_path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        temp_ready = !ec;
      } else {
        // Fresh create, or a deferred truncation: start from empty.
        auto temp_file = rex::filesystem::OpenFile(temp_path, "wb");
        if (temp_file) {
          fclose(temp_file);
          temp_ready = true;
        }
      }

      if (temp_ready) {
        auto temp_handle = rex::filesystem::FileHandle::OpenExisting(temp_path, desired_access);
        if (temp_handle) {
          atomic_write_active_ = true;
          static_cast<HostPathDevice*>(device_)->OnAtomicWriteBegin();
          *out_file = new HostPathFile(desired_access, this, std::move(temp_handle),
                                       std::move(temp_path), truncate_pending);
          return X_STATUS_SUCCESS;
        }
      }
      std::error_code cleanup_ec;
      std::filesystem::remove(temp_path, cleanup_ec);
      REXFS_WARN("Atomic write unavailable for '{}' - writing in place",
                 rex::path_to_utf8(host_path_));
    } else {
      REXFS_WARN("Second concurrent write handle for '{}' - writing in place (not atomic)",
                 rex::path_to_utf8(host_path_));
    }
  }

  // Fallback / read-only path: direct handle on the real file. A truncation
  // that could not ride an atomic session must land on disk here after all.
  if (truncate_pending) {
    auto file = rex::filesystem::OpenFile(host_path_, "wb");
    if (!file) {
      return X_STATUS_ACCESS_DENIED;
    }
    fclose(file);
  }

  auto file_handle = rex::filesystem::FileHandle::OpenExisting(host_path_, desired_access);
  if (!file_handle) {
    return X_STATUS_NO_SUCH_FILE;
  }
  *out_file = new HostPathFile(desired_access, this, std::move(file_handle));
  return X_STATUS_SUCCESS;
}

void HostPathEntry::CommitAtomicWrite(const std::filesystem::path& temp_path, bool commit,
                                      bool dirty) {
  atomic_write_active_ = false;
  auto* host_device = static_cast<HostPathDevice*>(device_);

  std::error_code ec;
  if (!commit) {
    // A write failed during the session: abandon the temp, the previous file
    // stays exactly as it was. The guest already saw the write error.
    std::filesystem::remove(temp_path, ec);
    REXFS_ERROR("Write to '{}' failed - previous contents kept intact",
                rex::path_to_utf8(host_path_));
  } else if (!dirty) {
    // Write handle closed without writing anything: nothing to commit.
    std::filesystem::remove(temp_path, ec);
  } else {
    // Keep exactly one backup generation, then swap the finished temp in.
    std::filesystem::path bak_path = host_path_;
    bak_path += kAtomicWriteBackupSuffix;
    bool have_bak = false;
    if (std::filesystem::exists(host_path_, ec) && !ec) {
      std::error_code bak_ec;
      std::filesystem::remove(bak_path, bak_ec);
      bak_ec.clear();
      std::filesystem::rename(host_path_, bak_path, bak_ec);
      have_bak = !bak_ec;
    }
    ec.clear();
    std::filesystem::rename(temp_path, host_path_, ec);
    if (ec) {
      REXFS_ERROR("Failed to commit write to '{}': {} - restoring previous contents",
                  rex::path_to_utf8(host_path_), ec.message());
      if (have_bak) {
        std::error_code restore_ec;
        std::filesystem::rename(bak_path, host_path_, restore_ec);
      }
      std::error_code rm_ec;
      std::filesystem::remove(temp_path, rm_ec);
    }
    update();
  }

  host_device->OnAtomicWriteEnd();
}

std::unique_ptr<memory::MappedMemory> HostPathEntry::OpenMapped(memory::MappedMemory::Mode mode,
                                                                size_t offset, size_t length) {
  return memory::MappedMemory::Open(host_path_, mode, offset, length);
}

bool HostPathEntry::Truncate() {
  if (is_read_only() || (attributes_ & kFileAttributeDirectory)) {
    return false;
  }
  // Probe writability without destroying anything: the old behaviour opened
  // "wb" (truncating the real file on the spot), so an interrupted overwrite
  // had already lost the previous contents before the first new byte landed.
  // A locked file (AV/cloud sync) must still fail LOUDLY here - the guest
  // gets a save error and retries - rather than silently later.
  auto file = rex::filesystem::OpenFile(host_path_, "r+b");
  if (!file) {
    return false;
  }
  fclose(file);
  // Defer the on-disk truncation into the atomic write session the VFS opens
  // right after: the session starts from an empty temp and the
  // real file is only replaced at commit.
  pending_truncate_ = true;
  size_ = 0;
  allocation_size_ = 0;
  return true;
}

std::unique_ptr<Entry> HostPathEntry::CreateEntryInternal(const std::string_view name,
                                                          uint32_t attributes) {
  auto full_path = host_path_ / rex::to_path(name);
  if (attributes & kFileAttributeDirectory) {
    if (!std::filesystem::create_directories(full_path)) {
      return nullptr;
    }
  } else {
    auto file = rex::filesystem::OpenFile(full_path, "wb");
    if (!file) {
      return nullptr;
    }
    fclose(file);
  }
  rex::filesystem::FileInfo file_info;
  if (!rex::filesystem::GetInfo(full_path, &file_info)) {
    return nullptr;
  }
  return std::unique_ptr<Entry>(HostPathEntry::Create(device_, this, full_path, file_info));
}

bool HostPathEntry::DeleteEntryInternal(Entry* entry) {
  auto full_path = host_path_ / rex::to_path(entry->name());
  std::error_code ec;  // avoid exception on remove/remove_all failure
  if (entry->attributes() & kFileAttributeDirectory) {
    // Delete entire directory and contents.
    auto removed = std::filesystem::remove_all(full_path, ec);
    return removed >= 1 && removed != static_cast<std::uintmax_t>(-1);
  } else {
    // Delete file.
    return !std::filesystem::is_directory(full_path) && std::filesystem::remove(full_path, ec);
  }
}

void HostPathEntry::RenameEntryInternal(const std::vector<std::string_view>& path_parts) {
  auto new_host_path = static_cast<HostPathDevice*>(device_)->host_path();
  for (const auto& path_part : path_parts) {
    new_host_path /= rex::to_path(path_part);
  }

  std::error_code ec;
  std::filesystem::rename(host_path_, new_host_path, ec);
  if (ec) {
    REXFS_ERROR("RenameEntryInternal: failed to rename '{}' to '{}': {}",
                rex::path_to_utf8(host_path_), rex::path_to_utf8(new_host_path), ec.message());
    return;
  }

  host_path_ = new_host_path;
}

void HostPathEntry::update() {
  // During an atomic write session the in-flight contents live in the temp
  // file; size queries must reflect what the guest just wrote, not the
  // yet-to-be-replaced previous file.
  std::filesystem::path query_path = host_path_;
  if (atomic_write_active_) {
    query_path += kAtomicWriteTempSuffix;
  }
  rex::filesystem::FileInfo file_info;
  if (!rex::filesystem::GetInfo(query_path, &file_info)) {
    return;
  }
  if (file_info.type == rex::filesystem::FileInfo::Type::kFile) {
    size_ = file_info.total_size;
    allocation_size_ = rex::round_up(file_info.total_size, device()->bytes_per_sector());
  }
}

bool HostPathEntry::SetAttributes(uint64_t attributes) {
  if (device_->is_read_only()) {
    return false;
  }
  attributes_ = static_cast<uint32_t>(attributes);
  return true;
}

bool HostPathEntry::SetCreateTimestamp(uint64_t timestamp) {
  if (device_->is_read_only()) {
    return false;
  }
  create_timestamp_ = timestamp;
  return true;
}

bool HostPathEntry::SetAccessTimestamp(uint64_t timestamp) {
  if (device_->is_read_only()) {
    return false;
  }
  access_timestamp_ = timestamp;
  return true;
}

bool HostPathEntry::SetWriteTimestamp(uint64_t timestamp) {
  if (device_->is_read_only()) {
    return false;
  }
  write_timestamp_ = timestamp;
  return true;
}

}  // namespace rex::filesystem
