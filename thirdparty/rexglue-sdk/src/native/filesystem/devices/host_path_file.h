/**
 * ReXGlue native filesystem layer
 * Part of the AC6 Recompilation project
 */

#pragma once

#include <filesystem>
#include <span>
#include <string>

#include <native/filesystem.h>
#include <native/filesystem/file.h>

namespace rex::filesystem {

class HostPathEntry;

class HostPathFile : public File {
 public:
  // Direct handle on the real file (read-only opens, and the fallback when an
  // atomic session cannot be established).
  HostPathFile(uint32_t file_access, HostPathEntry* entry,
               std::unique_ptr<rex::filesystem::FileHandle> file_handle);
  // Atomic write session: the handle points at "<name>.rex-tmp";
  // Destroy() flushes, closes and asks the entry to commit it over the real
  // file. started_dirty marks a deferred truncation as a modification even if
  // the guest then writes nothing.
  HostPathFile(uint32_t file_access, HostPathEntry* entry,
               std::unique_ptr<rex::filesystem::FileHandle> file_handle,
               std::filesystem::path temp_path, bool started_dirty);
  ~HostPathFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override;
  X_STATUS SetLength(size_t length) override;

 private:
  std::unique_ptr<rex::filesystem::FileHandle> file_handle_;
  // Atomic write session state.
  bool atomic_ = false;
  std::filesystem::path temp_path_;
  bool dirty_ = false;
  bool write_failed_ = false;
};

}  // namespace rex::filesystem
