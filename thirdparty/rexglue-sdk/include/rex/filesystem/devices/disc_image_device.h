/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>
#include <set>
#include <string>

#include <rex/filesystem/device.h>
#include <rex/memory/mapped_memory.h>

namespace rex::filesystem {

class DiscImageEntry;

// Read-only XDVDFS (GDFX) disc image device. Mounts a user-supplied .iso
// directly so no extraction step is required. Accepts both full dumps (game
// partition at a known base offset) and rebuilt game-partition-only images.
class DiscImageDevice : public Device {
 public:
  DiscImageDevice(const std::string_view mount_path, const std::filesystem::path& host_path);
  ~DiscImageDevice() override;

  bool Initialize() override;
  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  const std::filesystem::path& host_path() const { return host_path_; }

  uint32_t total_allocation_units() const override {
    return uint32_t(mmap_->size() / sectors_per_allocation_unit() / bytes_per_sector());
  }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  enum class Error {
    kSuccess = 0,
    kErrorOutOfMemory = -1,
    kErrorReadError = -10,
    kErrorFileMismatch = -30,
    kErrorDamagedFile = -31,
  };

  struct ParseState {
    uint8_t* ptr = nullptr;
    size_t size = 0;         // Size (bytes) of total image.
    size_t game_offset = 0;  // Offset (bytes) of game partition.
    size_t root_sector = 0;  // Offset (sector) of root.
    size_t root_offset = 0;  // Offset (bytes) of root.
    size_t root_size = 0;    // Size (bytes) of root.
    // Directory table offsets already walked; a repeat means a cycle in a
    // damaged/malicious image and aborts the parse instead of recursing
    // forever.
    std::set<size_t> visited_tables;
  };

  Error Verify(ParseState* state);
  bool VerifyMagic(ParseState* state, size_t offset);
  Error ReadAllEntries(ParseState* state);
  bool ReadDirectory(ParseState* state, size_t table_offset, size_t table_size,
                     DiscImageEntry* parent, uint32_t depth);

  std::string name_;
  std::filesystem::path host_path_;
  std::unique_ptr<Entry> root_entry_;
  std::unique_ptr<memory::MappedMemory> mmap_;
};

}  // namespace rex::filesystem
