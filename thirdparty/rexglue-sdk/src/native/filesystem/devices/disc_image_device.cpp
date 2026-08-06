/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime; parser hardened
 *              against rebuilt/truncated images (0xFFFF node terminators,
 *              per-entry bounds checks, directory-cycle detection).
 */

#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>

#include <cstring>
#include <vector>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::filesystem {

// XDVDFS sector size. Independent of the reported bytes_per_sector (0x200),
// which mirrors what the console reports for the mounted volume.
const size_t kXESectorSize = 2048;

// Directory-entry ordinals are uint16 indexes of 4-byte words into the
// directory table. 0 and 0xFFFF both terminate a branch in the wild: 0 in
// original pressings, 0xFFFF in images rebuilt by common extraction tools.
const uint16_t kOrdinalTerminator = 0xFFFF;

// Fixed part of a directory entry before the name bytes.
const size_t kEntryHeaderSize = 14;

// Real discs nest a handful of levels; anything deeper is a damaged image.
const uint32_t kMaxDirectoryDepth = 64;

DiscImageDevice::DiscImageDevice(const std::string_view mount_path,
                                 const std::filesystem::path& host_path)
    : Device(mount_path), name_("GDFX"), host_path_(host_path) {}

DiscImageDevice::~DiscImageDevice() = default;

bool DiscImageDevice::Initialize() {
  mmap_ = memory::MappedMemory::Open(host_path_, memory::MappedMemory::Mode::kRead);
  if (!mmap_) {
    REXFS_ERROR("DiscImageDevice: could not map disc image: {}", rex::path_to_utf8(host_path_));
    return false;
  }

  ParseState state;
  state.ptr = mmap_->data();
  state.size = mmap_->size();
  auto result = Verify(&state);
  if (result != Error::kSuccess) {
    REXFS_ERROR("DiscImageDevice: failed to verify disc image header ({}): {}", int(result),
                rex::path_to_utf8(host_path_));
    return false;
  }

  result = ReadAllEntries(&state);
  if (result != Error::kSuccess) {
    REXFS_ERROR("DiscImageDevice: failed to read GDFX directory tree ({}): {}", int(result),
                rex::path_to_utf8(host_path_));
    return false;
  }

  return true;
}

void DiscImageDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* DiscImageDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  REXFS_TRACE("DiscImageDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

DiscImageDevice::Error DiscImageDevice::Verify(ParseState* state) {
  // Find sector 32 of the game partition - try at a few known base offsets:
  // 0 (rebuilt game-partition-only image), the XSF variants, XGD1/XGD2/XGD3
  // full-dump bases.
  static const size_t likely_offsets[] = {
      0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000, 0x18300000,
  };
  bool magic_found = false;
  for (size_t offset : likely_offsets) {
    state->game_offset = offset;
    if (VerifyMagic(state, state->game_offset + (32 * kXESectorSize))) {
      magic_found = true;
      break;
    }
  }
  if (!magic_found) {
    // File doesn't have the magic values - likely not a real GDFX source.
    return Error::kErrorFileMismatch;
  }

  // Read sector 32 to get FS state.
  size_t vd_offset = state->game_offset + (32 * kXESectorSize);
  if (state->size < vd_offset + kXESectorSize) {
    return Error::kErrorReadError;
  }
  const uint8_t* fs_ptr = state->ptr + vd_offset;
  // The volume descriptor carries the magic at both ends of the sector; a
  // missing tail magic means a truncated or corrupt descriptor.
  if (std::memcmp(fs_ptr + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20) != 0) {
    return Error::kErrorDamagedFile;
  }
  state->root_sector = memory::load<uint32_t>(fs_ptr + 20);
  state->root_size = memory::load<uint32_t>(fs_ptr + 24);
  state->root_offset = state->game_offset + (state->root_sector * kXESectorSize);
  if (state->root_size < kEntryHeaderSize - 1 || state->root_size > 32 * 1024 * 1024) {
    return Error::kErrorDamagedFile;
  }
  if (state->root_offset >= state->size || state->root_size > state->size - state->root_offset) {
    return Error::kErrorDamagedFile;
  }

  return Error::kSuccess;
}

bool DiscImageDevice::VerifyMagic(ParseState* state, size_t offset) {
  if (offset + 20 > state->size) {
    return false;
  }

  // Simple check to see if the given offset contains the magic value.
  return std::memcmp(state->ptr + offset, "MICROSOFT*XBOX*MEDIA", 20) == 0;
}

DiscImageDevice::Error DiscImageDevice::ReadAllEntries(ParseState* state) {
  auto root_entry = new DiscImageEntry(this, nullptr, "", mmap_.get());
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  if (!ReadDirectory(state, state->root_offset, state->root_size, root_entry, 0)) {
    return Error::kErrorDamagedFile;
  }

  return Error::kSuccess;
}

bool DiscImageDevice::ReadDirectory(ParseState* state, size_t table_offset, size_t table_size,
                                    DiscImageEntry* parent, uint32_t depth) {
  if (depth > kMaxDirectoryDepth) {
    return false;
  }
  if (table_offset >= state->size || table_size > state->size - table_offset) {
    return false;
  }
  if (!state->visited_tables.insert(table_offset).second) {
    // A directory table referenced twice = a cycle in a damaged image.
    return false;
  }

  const uint8_t* table = state->ptr + table_offset;

  // Iterative walk over the entry AVL tree. Ordinals 0 and 0xFFFF are branch
  // terminators; a seen-set guards against self-referencing nodes.
  std::vector<uint16_t> pending;
  std::set<uint16_t> seen;
  pending.push_back(0);
  while (!pending.empty()) {
    uint16_t ordinal = pending.back();
    pending.pop_back();
    if (!seen.insert(ordinal).second) {
      continue;
    }

    size_t offset = size_t(ordinal) * 4;
    if (offset + kEntryHeaderSize > table_size) {
      // Padding at the tail of a directory sector; nothing to read here.
      continue;
    }
    const uint8_t* p = table + offset;

    uint16_t node_l = memory::load<uint16_t>(p + 0);
    uint16_t node_r = memory::load<uint16_t>(p + 2);
    size_t sector = memory::load<uint32_t>(p + 4);
    size_t length = memory::load<uint32_t>(p + 8);
    uint8_t attributes = memory::load<uint8_t>(p + 12);
    uint8_t name_length = memory::load<uint8_t>(p + 13);

    if (node_l && node_l != kOrdinalTerminator) {
      pending.push_back(node_l);
    }
    if (node_r && node_r != kOrdinalTerminator) {
      pending.push_back(node_r);
    }

    if (!name_length || offset + kEntryHeaderSize + name_length > table_size) {
      continue;
    }
    auto name = std::string(reinterpret_cast<const char*>(p + kEntryHeaderSize), name_length);

    auto entry = DiscImageEntry::Create(this, parent, name, mmap_.get());
    entry->attributes_ = attributes | kFileAttributeReadOnly;
    entry->size_ = length;
    entry->allocation_size_ = rex::round_up(length, bytes_per_sector());

    // Set to January 1, 1970 (UTC) in 100-nanosecond intervals
    entry->create_timestamp_ = 10000 * 11644473600000LL;
    entry->access_timestamp_ = 10000 * 11644473600000LL;
    entry->write_timestamp_ = 10000 * 11644473600000LL;

    if (attributes & kFileAttributeDirectory) {
      // Folder.
      entry->data_offset_ = 0;
      entry->data_size_ = 0;
      auto* dir = entry.get();
      parent->children_.emplace_back(std::move(entry));
      if (length) {
        // Not a leaf - read in children.
        size_t child_offset = state->game_offset + (sector * kXESectorSize);
        if (!ReadDirectory(state, child_offset, length, dir, depth + 1)) {
          return false;
        }
      }
    } else {
      // File.
      entry->data_offset_ = state->game_offset + (sector * kXESectorSize);
      entry->data_size_ = length;
      if (entry->data_offset_ > state->size || length > state->size - entry->data_offset_) {
        // File data extends past the end of the image: truncated download or
        // bad dump. Fail the mount so the caller reports it clearly.
        return false;
      }
      parent->children_.emplace_back(std::move(entry));
    }
  }

  return true;
}

}  // namespace rex::filesystem
