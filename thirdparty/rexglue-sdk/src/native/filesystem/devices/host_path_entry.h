/**
 * ReXGlue native filesystem layer
 * Part of the AC6 Recompilation project
 */

#pragma once

#include <string>

#include <native/filesystem.h>
#include <native/filesystem/entry.h>

namespace rex::filesystem {

class HostPathDevice;

// Host-side artifacts of the atomic write path. Never guest-visible:
// the device skips them when populating the entry tree.
inline constexpr char kAtomicWriteTempSuffix[] = ".rex-tmp";
inline constexpr char kAtomicWriteBackupSuffix[] = ".rex-bak";

class HostPathEntry : public Entry {
 public:
  HostPathEntry(Device* device, Entry* parent, const std::string_view path,
                const std::filesystem::path& host_path);
  ~HostPathEntry() override;

  static HostPathEntry* Create(Device* device, Entry* parent,
                               const std::filesystem::path& full_path,
                               rex::filesystem::FileInfo file_info);

  const std::filesystem::path& host_path() const { return host_path_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;
  bool Truncate() override;

  // Atomic write session: a write handle opened by Open() writes
  // into "<name>.rex-tmp" in the same directory; HostPathFile calls this on
  // close. commit=false abandons the temp (failed write - old data stays);
  // dirty=false means nothing was written (temp discarded, file untouched).
  // A successful commit keeps ONE previous generation at "<name>.rex-bak"
  // and renames the temp over the real file, so an interrupted or failed
  // save can never leave a torn file in place.
  void CommitAtomicWrite(const std::filesystem::path& temp_path, bool commit, bool dirty);

  bool can_map() const override { return true; }
  std::unique_ptr<memory::MappedMemory> OpenMapped(memory::MappedMemory::Mode mode, size_t offset,
                                                   size_t length) override;
  void update() override;
  bool SetAttributes(uint64_t attributes) override;
  bool SetCreateTimestamp(uint64_t timestamp) override;
  bool SetAccessTimestamp(uint64_t timestamp) override;
  bool SetWriteTimestamp(uint64_t timestamp) override;

 private:
  friend class HostPathDevice;

  std::unique_ptr<Entry> CreateEntryInternal(const std::string_view name,
                                             uint32_t attributes) override;
  bool DeleteEntryInternal(Entry* entry) override;
  void RenameEntryInternal(const std::vector<std::string_view>& path_parts) override;

  std::filesystem::path host_path_;
  // Truncate() defers the on-disk truncation into the atomic write session
  // the VFS opens immediately afterwards, so an interrupted overwrite leaves
  // the previous file intact. Consumed (and cleared) by the next Open().
  bool pending_truncate_ = false;
  // One atomic session per entry at a time; a second concurrent write handle
  // falls back to in-place access (logged).
  bool atomic_write_active_ = false;
};

}  // namespace rex::filesystem
