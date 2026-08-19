#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgiformat.h>
#else
// This file uses the D3D12_RESOURCE_DIMENSION and DXGI_FORMAT enums for
// texture metadata only - it never calls into D3D12. Portable shims with the
// canonical Microsoft values keep DDS dumps and texture-swap packs
// wire-compatible with ones produced on Windows.
typedef enum D3D12_RESOURCE_DIMENSION {
  D3D12_RESOURCE_DIMENSION_UNKNOWN = 0,
  D3D12_RESOURCE_DIMENSION_BUFFER = 1,
  D3D12_RESOURCE_DIMENSION_TEXTURE1D = 2,
  D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3,
  D3D12_RESOURCE_DIMENSION_TEXTURE3D = 4,
} D3D12_RESOURCE_DIMENSION;

typedef enum DXGI_FORMAT {
  DXGI_FORMAT_UNKNOWN = 0,
  DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
  DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
  DXGI_FORMAT_R32G32B32A32_UINT = 3,
  DXGI_FORMAT_R32G32B32A32_SINT = 4,
  DXGI_FORMAT_R32G32B32_TYPELESS = 5,
  DXGI_FORMAT_R32G32B32_FLOAT = 6,
  DXGI_FORMAT_R32G32B32_UINT = 7,
  DXGI_FORMAT_R32G32B32_SINT = 8,
  DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
  DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
  DXGI_FORMAT_R16G16B16A16_UNORM = 11,
  DXGI_FORMAT_R16G16B16A16_UINT = 12,
  DXGI_FORMAT_R16G16B16A16_SNORM = 13,
  DXGI_FORMAT_R16G16B16A16_SINT = 14,
  DXGI_FORMAT_R32G32_TYPELESS = 15,
  DXGI_FORMAT_R32G32_FLOAT = 16,
  DXGI_FORMAT_R32G32_UINT = 17,
  DXGI_FORMAT_R32G32_SINT = 18,
  DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
  DXGI_FORMAT_R10G10B10A2_UNORM = 24,
  DXGI_FORMAT_R10G10B10A2_UINT = 25,
  DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
  DXGI_FORMAT_R8G8B8A8_UNORM = 28,
  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
  DXGI_FORMAT_R8G8B8A8_UINT = 30,
  DXGI_FORMAT_R8G8B8A8_SNORM = 31,
  DXGI_FORMAT_R8G8B8A8_SINT = 32,
  DXGI_FORMAT_R16G16_TYPELESS = 33,
  DXGI_FORMAT_R16G16_FLOAT = 34,
  DXGI_FORMAT_R16G16_UNORM = 35,
  DXGI_FORMAT_R16G16_UINT = 36,
  DXGI_FORMAT_R16G16_SNORM = 37,
  DXGI_FORMAT_R16G16_SINT = 38,
  DXGI_FORMAT_R32_TYPELESS = 39,
  DXGI_FORMAT_R32_FLOAT = 41,
  DXGI_FORMAT_R32_UINT = 42,
  DXGI_FORMAT_R32_SINT = 43,
  DXGI_FORMAT_R8G8_TYPELESS = 48,
  DXGI_FORMAT_R8G8_UNORM = 49,
  DXGI_FORMAT_R8G8_UINT = 50,
  DXGI_FORMAT_R8G8_SNORM = 51,
  DXGI_FORMAT_R8G8_SINT = 52,
  DXGI_FORMAT_R16_TYPELESS = 53,
  DXGI_FORMAT_R16_FLOAT = 54,
  DXGI_FORMAT_R16_UNORM = 56,
  DXGI_FORMAT_R16_UINT = 57,
  DXGI_FORMAT_R16_SNORM = 58,
  DXGI_FORMAT_R16_SINT = 59,
  DXGI_FORMAT_R8_TYPELESS = 60,
  DXGI_FORMAT_R8_UNORM = 61,
  DXGI_FORMAT_R8_UINT = 62,
  DXGI_FORMAT_R8_SNORM = 63,
  DXGI_FORMAT_R8_SINT = 64,
  DXGI_FORMAT_BC1_TYPELESS = 70,
  DXGI_FORMAT_BC1_UNORM = 71,
  DXGI_FORMAT_BC1_UNORM_SRGB = 72,
  DXGI_FORMAT_BC2_TYPELESS = 73,
  DXGI_FORMAT_BC2_UNORM = 74,
  DXGI_FORMAT_BC2_UNORM_SRGB = 75,
  DXGI_FORMAT_BC3_TYPELESS = 76,
  DXGI_FORMAT_BC3_UNORM = 77,
  DXGI_FORMAT_BC3_UNORM_SRGB = 78,
  DXGI_FORMAT_BC4_TYPELESS = 79,
  DXGI_FORMAT_BC4_UNORM = 80,
  DXGI_FORMAT_BC4_SNORM = 81,
  DXGI_FORMAT_BC5_TYPELESS = 82,
  DXGI_FORMAT_BC5_UNORM = 83,
  DXGI_FORMAT_BC5_SNORM = 84,
  DXGI_FORMAT_B5G6R5_UNORM = 85,
  DXGI_FORMAT_B5G5R5A1_UNORM = 86,
  DXGI_FORMAT_B8G8R8A8_UNORM = 87,
  DXGI_FORMAT_B8G8R8X8_UNORM = 88,
  DXGI_FORMAT_B4G4R4A4_UNORM = 115,
} DXGI_FORMAT;
#endif

namespace ac6::textures {

struct TextureSubresourceLayout {
  uint32_t row_pitch = 0;
  uint32_t slice_pitch = 0;
  uint32_t row_count = 0;
};

struct DdsSubresource {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;
  uint32_t row_pitch = 0;
  uint32_t slice_pitch = 0;
  std::vector<uint8_t> data;
};

struct DdsImageData {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth_or_array_size = 1;
  uint32_t mip_count = 1;
  bool is_cube = false;
  std::vector<DdsSubresource> subresources;
};

struct TextureDumpMetadata {
  std::string stable_key;
  uint64_t texture_key_hash = 0;
  uint32_t base_page = 0;
  uint32_t mip_page = 0;
  uint32_t dimension = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth_or_array_size = 1;
  uint32_t mip_count = 1;
  uint32_t guest_format = 0;
  uint32_t endianness = 0;
  uint32_t dxgi_format = 0;
  bool tiled = false;
  bool packed_mips = false;
  bool signed_separate = false;
  bool scaled_resolve = false;
  uint64_t frame_index = 0;
  uint64_t signature_stable_id = 0;
  uint64_t active_vertex_shader_hash = 0;
  uint64_t active_pixel_shader_hash = 0;
  std::string signature_tags;
};

bool TextureSwapsEnabled();
bool TextureDumpEnabled();
bool TextureReplacementEnabled();

bool IsSupportedTextureSwapFormat(DXGI_FORMAT format);
bool GetTightTextureSubresourceLayout(DXGI_FORMAT format, uint32_t width, uint32_t height,
                                      TextureSubresourceLayout& out);
std::string DescribeDxgiFormat(DXGI_FORMAT format);

std::string BuildTextureStableKey(uint64_t texture_key_hash, uint32_t base_page, uint32_t mip_page,
                                  uint32_t dimension, uint32_t width, uint32_t height,
                                  uint32_t depth_or_array_size, uint32_t mip_count,
                                  uint32_t guest_format, uint32_t endianness, bool tiled,
                                  bool packed_mips, bool signed_separate, bool scaled_resolve);

std::filesystem::path GetTextureDumpDdsPath(std::string_view stable_key);
std::filesystem::path GetTextureDumpMetadataPath(std::string_view stable_key);
std::filesystem::path GetTextureDumpCurrentSessionRoot();
std::filesystem::path GetTextureDumpCurrentSessionDdsPath(std::string_view stable_key);
std::filesystem::path GetTextureDumpCurrentSessionMetadataPath(std::string_view stable_key);
std::filesystem::path GetTextureDumpCurrentSessionInfoPath();
bool DumpExists(std::string_view stable_key);
bool MirrorDumpToCurrentSession(std::string_view stable_key, std::string* error_out = nullptr);

std::optional<std::filesystem::path> ResolveReplacementDdsPath(std::string_view stable_key);

bool LoadDdsFromFile(const std::filesystem::path& path, DdsImageData& out,
                     std::string* error_out = nullptr);
bool WriteDdsToFile(const std::filesystem::path& path, const DdsImageData& data,
                    std::string* error_out = nullptr);
bool WriteDumpMetadata(const std::filesystem::path& path, const TextureDumpMetadata& metadata,
                       std::string* error_out = nullptr);

}  // namespace ac6::textures
