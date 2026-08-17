/**
 * ReXGlue native filesystem layer
 * Part of the AC6 Recompilation project
 */

#pragma once

#include <filesystem>
#include <mutex>
#include <string>

#include <native/filesystem/device.h>

namespace rex::filesystem {

class HostPathEntry;

// Phase words carried on the first line of the write-in-progress marker
// file. The phase tells the NEXT mount what a leftover marker proves about
// the container next to it (see set_write_marker_path below).
inline constexpr char kWriteMarkerPhaseWriting[] = "writing";
inline constexpr char kWriteMarkerPhaseCommitting[] = "committing";
inline constexpr char kWriteMarkerPhaseComplete[] = "complete";

class HostPathDevice : public Device {
 public:
  HostPathDevice(const std::string_view mount_path, const std::filesystem::path& host_path,
                 bool read_only);
  ~HostPathDevice() override;

  bool Initialize() override;
  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return read_only_; }
  const std::filesystem::path& host_path() const { return host_path_; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 128 * 1024; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

  // Write-in-progress marker. When a marker path is set (content
  // packages do this), the device keeps a marker file on disk while any
  // atomic write session is open and removes it when the last one commits.
  // The marker's first line is a phase word telling the NEXT mount what a
  // leftover marker proves about the container:
  //   "writing"    - sessions were open but no commit rename had started;
  //                  every file is still its previous version: consistent.
  //   "committing" - at least one commit rename ran while the marker was
  //                  live; a death after that can tear the container
  //                  ACROSS files (file A committed, file B not).
  //   "complete"   - every commit finished but the marker itself could not
  //                  be deleted (typically an antivirus or indexer holding
  //                  the file): consistent.
  // The phase is monotonic within one marker lifetime: it starts at
  // "writing" when the first session of a burst opens and never returns
  // there until the marker is deleted (or recreated by a later burst).
  // The content manager reads the phase at mount time and only quarantines
  // a container whose marker does not prove consistency. Empty path (the
  // default) disables the marker.
  void set_write_marker_path(const std::filesystem::path& marker_path) {
    write_marker_path_ = marker_path;
  }
  void OnAtomicWriteBegin();
  // Called immediately before the first commit rename of a session; flips
  // the marker phase to "committing" once per marker lifetime.
  void OnAtomicWriteCommit();
  void OnAtomicWriteEnd();

  // Reads the phase word (first line, trimmed) from a marker file. Returns
  // an empty string when the file cannot be read - callers must treat that
  // as "proves nothing".
  static std::string ReadWriteMarkerPhase(const std::filesystem::path& marker_path);

 private:
  void PopulateEntry(HostPathEntry* parent_entry);
  void SweepStaleAtomicArtifacts();
  // Truncate-writes the marker file with the given phase word on its first
  // line. Caller holds write_marker_mutex_. Returns false when the file
  // could not be opened for writing.
  bool WriteMarkerPhaseLocked(const char* phase);

  std::string name_;
  std::filesystem::path host_path_;
  std::unique_ptr<Entry> root_entry_;
  bool read_only_;

  std::filesystem::path write_marker_path_;
  std::mutex write_marker_mutex_;
  int active_atomic_writes_ = 0;
  // Whether the current marker has already been flipped to "committing".
  // Reset when a new marker lifetime begins.
  bool marker_committing_ = false;
};

}  // namespace rex::filesystem
