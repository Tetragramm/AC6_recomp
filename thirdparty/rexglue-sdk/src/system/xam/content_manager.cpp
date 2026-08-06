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

#include <algorithm>
#include <array>
#include <cstring>
#include <queue>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xfile.h>
#include <rex/system/xobject.h>

// Defined here (system layer) so both the global XamContentGetLicenseMask
// (kernel layer, which links system PUBLIC) and the per-package license
// override in OpenContent share this one knob. Default 0xFFFFFFFF = all bits,
// the same as Xenia's license_mask = -1: a recomp has no online entitlement
// check, so present content is owned. This also un-hides DLC colours/skins that
// the STFS header only partially licensed (e.g. AC6's IDOLM@STER skins, whose
// packs ship a partial per-colour bitmask). Set to 0 for strict header checks.
REXCVAR_DEFINE_UINT32(license_mask, 0xFFFFFFFFu, "Kernel",
                      "Content license mask returned by XamContentGetLicenseMask and used to "
                      "override the license of installed DLC/marketplace content so every "
                      "colour/skin is owned. Default 0xFFFFFFFF (all bits, = Xenia -1); set 0 "
                      "for strict, header-accurate checks.");

namespace rex {
namespace system {
namespace xam {

static const char* kThumbnailFileName = "__thumbnail.png";

static const char* kGameUserContentDirName = "profile";

static const char* kGameContentHeaderDirName = "Headers";

static int content_device_id_ = 0;

ContentPackage::ContentPackage(KernelState* kernel_state, const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path)
    : kernel_state_(kernel_state), root_name_(root_name), package_path_(package_path), license_(0) {
  device_path_ = fmt::format("\\Device\\Content\\{0}\\", ++content_device_id_);
  content_data_ = data;

  auto fs = kernel_state_->file_system();
  std::error_code ec;
  if (std::filesystem::is_regular_file(package_path, ec)) {
    // A raw STFS/LIVE container: mount it directly, no extraction step.
    is_container_ = true;
    auto device =
        std::make_unique<rex::filesystem::StfsContainerDevice>(device_path_, package_path);
    if (device->Initialize()) {
      for (size_t i = 0; i < 0x10; i++) {
        if (device->header().header.licenses[i].license_flags) {
          container_license_ |= device->header().header.licenses[i].license_bits;
        }
      }
      fs->RegisterDevice(std::move(device));
      device_mounted_ = true;
    } else {
      REXSYS_ERROR("ContentPackage: failed to mount container {}", rex::path_to_utf8(package_path));
    }
  } else {
    // An extracted folder: the historical path, unchanged.
    auto device =
        std::make_unique<rex::filesystem::HostPathDevice>(device_path_, package_path, false);
    device->Initialize();
    fs->RegisterDevice(std::move(device));
    device_mounted_ = true;
  }
  fs->RegisterSymbolicLink(root_name_ + ":", device_path_);
}

ContentPackage::~ContentPackage() {
  auto fs = kernel_state_->file_system();
  fs->UnregisterSymbolicLink(root_name_ + ":");
  fs->UnregisterDevice(device_path_);
}

void ContentPackage::LoadPackageLicenseMask(const std::filesystem::path header_path) {
  if (!std::filesystem::exists(header_path)) {
    return;
  }

  auto file = rex::filesystem::OpenFile(header_path, "rb");
  if (!file) {
    return;
  }

  auto file_size = std::filesystem::file_size(header_path);
  if (file_size < sizeof(XCONTENT_AGGREGATE_DATA) + sizeof(license_)) {
    fclose(file);
    return;
  }

  fseek(file, sizeof(XCONTENT_AGGREGATE_DATA), SEEK_SET);
  fread(&license_, 1, sizeof(license_), file);
  fclose(file);
}

ContentManager::ContentManager(KernelState* kernel_state, const std::filesystem::path& root_path)
    : kernel_state_(kernel_state), root_path_(root_path) {}

ContentManager::~ContentManager() = default;

std::filesystem::path ContentManager::ResolvePackageRoot(uint64_t xuid, XContentType content_type,
                                                         uint32_t title_id) {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }
  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str = fmt::format("{:08X}", uint32_t(content_type));

  // Package root path:
  // content_root/xuid/title_id/content_type/
  return root_path_ / xuid_str / title_id_str / content_type_str;
}

std::filesystem::path ContentManager::ResolvePackagePath(uint64_t xuid,
                                                         const XCONTENT_AGGREGATE_DATA& data) {
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;

  // DLCs are stored in common directory
  if (data.content_type == XContentType::kMarketplaceContent) {
    used_xuid = 0;
  }

  // Content path:
  // content_root/xuid/title_id/content_type/data_file_name/
  auto package_root = ResolvePackageRoot(used_xuid, data.content_type, data.title_id);
  return package_root / rex::to_path(data.file_name());
}

std::filesystem::path ContentManager::ResolvePackageHeaderPath(const std::string_view file_name,
                                                               uint64_t xuid, uint32_t title_id,
                                                               XContentType content_type) const {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  if (content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str = fmt::format("{:08X}", uint32_t(content_type));
  std::string final_name = std::string(file_name) + ".header";

  // Header root path:
  // content_root/xuid/title_id/Headers/content_type/filename.header
  return root_path_ / xuid_str / title_id_str / kGameContentHeaderDirName / content_type_str /
         final_name;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(uint32_t device_id, uint64_t xuid,
                                                                 XContentType content_type,
                                                                 uint32_t title_id) {
  std::vector<XCONTENT_AGGREGATE_DATA> result;

  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  // Search path:
  // content_root/xuid/title_id/type_name/*
  auto package_root = ResolvePackageRoot(xuid, content_type, title_id);
  auto file_infos = rex::filesystem::ListFiles(package_root);
  for (const auto& file_info : file_infos) {
    if (file_info.type != rex::filesystem::FileInfo::Type::kDirectory) {
      // Directories only; raw container files are handled below.
      continue;
    }

    XCONTENT_AGGREGATE_DATA content_data;
    if (XSUCCEEDED(ReadContentHeaderFile(rex::path_to_utf8(file_info.name), xuid, title_id,
                                         content_type, content_data))) {
      result.emplace_back(std::move(content_data));
    } else {
      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.set_display_name(rex::path_to_utf16(file_info.name));
      content_data.set_file_name(rex::path_to_utf8(file_info.name));
      content_data.title_id = title_id;
      content_data.xuid = xuid;
      result.emplace_back(std::move(content_data));
    }
  }

  // Discovered raw containers take priority over an ambient extracted install
  // for the same package name - the user placed them next to the exe, so they
  // win, same rule as the assets folder (and Windows' local-DLL search order).
  // A container replaces its extracted twin in the listing; unique names
  // simply append.
  if (!containers_.empty() && title_id == kernel_state_->title_id()) {
    for (const auto& [key, container] : containers_) {
      if (container.content_type != content_type) {
        continue;
      }
      XCONTENT_AGGREGATE_DATA content_data;
      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.set_display_name(container.display_name.empty()
                                        ? rex::path_to_utf16(container.path.filename())
                                        : container.display_name);
      content_data.set_file_name(key.view());
      content_data.title_id = title_id;
      content_data.xuid = xuid;
      auto existing = std::find_if(result.begin(), result.end(), [&](const auto& data) {
        return rex::string::utf8_equal_case(data.file_name(), key.view());
      });
      if (existing != result.end()) {
        *existing = content_data;
      } else {
        result.emplace_back(std::move(content_data));
      }
    }
  }

  return result;
}

const DiscoveredContainer* ContentManager::FindContainer(const std::string_view file_name,
                                                         XContentType content_type) const {
  auto it = containers_.find(string::string_key_case(file_name));
  if (it == containers_.end() || it->second.content_type != content_type) {
    return nullptr;
  }
  return &it->second;
}

std::filesystem::path ContentManager::ResolvePackageDataPath(uint64_t xuid,
                                                             const XCONTENT_AGGREGATE_DATA& data) {
  // A container the user placed (the dlc folder, or dropped into the content
  // root) takes priority over an ambient extracted install - same rule as the
  // assets folder and Windows' local-DLL search order.
  if (const auto* container = FindContainer(data.file_name(), data.content_type)) {
    return container->path;
  }
  // Else the canonical package path - an extracted folder, or a container
  // file sitting at the package's canonical place in the content root.
  auto package_path = ResolvePackagePath(xuid, data);
  std::error_code ec;
  if (std::filesystem::exists(package_path, ec)) {
    return package_path;
  }
  return {};
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    const std::string_view root_name, uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto data_path = ResolvePackageDataPath(xuid, data);
  if (data_path.empty()) {
    return nullptr;
  }
  auto package = std::make_unique<ContentPackage>(kernel_state_, root_name, data, data_path);
  return package;
}

bool ContentManager::ContentExists(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  return !ResolvePackageDataPath(xuid, data).empty();
}

X_RESULT ContentManager::WriteContentHeaderFile(uint64_t xuid, XCONTENT_AGGREGATE_DATA data,
                                                uint32_t license_mask) {
  if (data.title_id == uint32_t(-1)) {
    data.title_id = kernel_state_->title_id();
  }
  if (data.xuid == uint64_t(-1)) {
    data.xuid = xuid;
  }
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;

  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  auto parent_path = header_path.parent_path();

  if (!std::filesystem::exists(parent_path)) {
    if (!std::filesystem::create_directories(parent_path)) {
      return X_ERROR_ACCESS_DENIED;
    }
  }

  rex::filesystem::CreateEmptyFile(header_path);

  auto file = rex::filesystem::OpenFile(header_path, "wb");
  if (!file) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  fwrite(&data, 1, sizeof(XCONTENT_AGGREGATE_DATA), file);
  if (license_mask != 0) {
    fwrite(&license_mask, 1, sizeof(license_mask), file);
  }
  fclose(file);
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::ReadContentHeaderFile(const std::string_view file_name, uint64_t xuid,
                                               uint32_t title_id, XContentType content_type,
                                               XCONTENT_AGGREGATE_DATA& data) const {
  auto header_file_path = ResolvePackageHeaderPath(file_name, xuid, title_id, content_type);
  constexpr uint32_t header_size = sizeof(XCONTENT_AGGREGATE_DATA);

  if (!std::filesystem::exists(header_file_path)) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto file = rex::filesystem::OpenFile(header_file_path, "rb");
  if (!file) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto file_size = std::filesystem::file_size(header_file_path);
  if (file_size < header_size) {
    fclose(file);
    return X_ERROR_FILE_NOT_FOUND;
  }

  std::array<uint8_t, header_size> buffer;
  size_t result = fread(buffer.data(), 1, header_size, file);
  fclose(file);

  if (result != header_size) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  std::memcpy(&data, buffer.data(), buffer.size());
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CreateContent(const std::string_view root_name, uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data) {
  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (std::filesystem::exists(package_path)) {
    return X_ERROR_ALREADY_EXISTS;
  }
  if (!std::filesystem::create_directories(package_path)) {
    return X_ERROR_ACCESS_DENIED;
  }
  auto package = ResolvePackage(root_name, xuid, data);
  assert_not_null(package);

  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
    open_packages_.insert({string::string_key_case::create(root_name), package.release()});
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContent(const std::string_view root_name, uint64_t xuid,
                                     const XCONTENT_AGGREGATE_DATA& data,
                                     uint32_t& content_license) {
  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
  }

  auto package = ResolvePackage(root_name, xuid, data);
  if (!package) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  if (!package->device_mounted()) {
    // A corrupt/truncated container must read as "content missing", not as an
    // empty package that succeeds and then mysteriously has no files.
    return X_ERROR_FILE_NOT_FOUND;
  }
  package->LoadPackageLicenseMask(ResolvePackageHeaderPath(
      data.file_name(), xuid, kernel_state_->title_id(), data.content_type));
  content_license = package->GetPackageLicense();
  // A present package means the player owns it (a recomp has no online
  // entitlement check). DLC (marketplace) packages frequently carry only a
  // PARTIAL per-colour license bitmask in their STFS header - the game then
  // hides every colour/skin whose bit is clear (this is what hid the missing
  // IDOLM@STER skins: e.g. PACK09/AZUSA shipped 0x5f and shows, PACK02/MIKI
  // shipped 0x0f and is hidden). Override marketplace content with the full
  // configured mask so every colour in the pack is owned; other content types
  // keep the zero-fallback. Set license_mask=0 in the toml for strict checks.
  if (data.content_type == XContentType::kMarketplaceContent || content_license == 0) {
    content_license = REXCVAR_GET(license_mask);
  }

  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
    open_packages_.insert({string::string_key_case::create(root_name), package.release()});
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CloseContent(const std::string_view root_name) {
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    // Some games use different casing between Create and Close (e.g. "save" vs "SAVE")
    auto it = open_packages_.find(string::string_key_case(root_name));
    if (it == open_packages_.end()) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    package = DetachPackage(it);
  }
  delete package;
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::GetContentThumbnail(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                                             std::vector<uint8_t>* buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  auto thumb_path = package_path / kThumbnailFileName;
  if (std::filesystem::exists(thumb_path)) {
    auto file = rex::filesystem::OpenFile(thumb_path, "rb");
    fseek(file, 0, SEEK_END);
    size_t file_len = ftell(file);
    fseek(file, 0, SEEK_SET);
    buffer->resize(file_len);
    fread(const_cast<uint8_t*>(buffer->data()), 1, buffer->size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::SetContentThumbnail(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                                             std::vector<uint8_t> buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path)) {
    auto thumb_path = package_path / kThumbnailFileName;
    auto file = rex::filesystem::OpenFile(thumb_path, "wb");
    fwrite(buffer.data(), 1, buffer.size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::DeleteContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  if (IsContentOpen(data)) {
    // TODO(Gliniak): Get real error code for this case.
    return X_ERROR_ACCESS_DENIED;
  }

  // Never delete a directly-mounted container: it is the user's only copy of
  // that package, not something this port installed.
  {
    std::error_code ec;
    auto data_path = ResolvePackageDataPath(xuid, data);
    if (!data_path.empty() && std::filesystem::is_regular_file(data_path, ec)) {
      REXSYS_WARN("ContentManager: refusing to delete container-backed content {}",
                  rex::path_to_utf8(data_path));
      return X_ERROR_ACCESS_DENIED;
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  std::error_code ec;
  auto dir_removed = std::filesystem::remove_all(package_path, ec);
  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  std::error_code ec2;
  bool header_removed = std::filesystem::remove(header_path, ec2);

  if (dir_removed > 0 || header_removed) {
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_FILE_NOT_FOUND;
}

X_RESULT ContentManager::UnmountContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = FindOpenPackageByData(data);
    if (it == open_packages_.end()) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    package = DetachPackage(it);
  }
  delete package;
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::UnmountAndDeleteContent(uint64_t xuid,
                                                 const XCONTENT_AGGREGATE_DATA& data) {
  // Unmount phase: tolerant of not-mounted state
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = FindOpenPackageByData(data);
    if (it != open_packages_.end()) {
      package = DetachPackage(it);
    }
  }
  delete package;

  // Never delete a directly-mounted container (the user's only copy).
  {
    std::error_code ec;
    auto data_path = ResolvePackageDataPath(xuid, data);
    if (!data_path.empty() && std::filesystem::is_regular_file(data_path, ec)) {
      REXSYS_WARN("ContentManager: refusing to delete container-backed content {}",
                  rex::path_to_utf8(data_path));
      return X_ERROR_ACCESS_DENIED;
    }
  }

  // Delete phase: remove package directory and .header file
  auto package_path = ResolvePackagePath(xuid, data);

  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);

  std::error_code ec;
  auto dir_removed = std::filesystem::remove_all(package_path, ec);
  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  std::error_code ec2;
  bool header_removed = std::filesystem::remove(header_path, ec2);

  if (dir_removed > 0 || header_removed) {
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_FILE_NOT_FOUND;
}

std::filesystem::path ContentManager::ResolveGameUserContentPath() {
  auto title_id = fmt::format("{:08X}", kernel_state_->title_id());
  auto user_name = rex::to_path(kernel_state_->user_profile()->name());

  // Per-game per-profile data location:
  // content_root/title_id/profile/user_name
  return root_path_ / title_id / kGameUserContentDirName / user_name;
}

std::unordered_map<string::string_key_case, ContentPackage*,
                   string::string_key_case::Hash>::iterator
ContentManager::FindOpenPackageByData(const XCONTENT_AGGREGATE_DATA& data) {
  // Resolve kCurrentlyRunningTitleId so both sides compare actual title IDs.
  uint32_t query_title = data.title_id;
  if (query_title == kCurrentlyRunningTitleId) {
    query_title = kernel_state_->title_id();
  }

  for (auto it = open_packages_.begin(); it != open_packages_.end(); ++it) {
    const auto& pkg = it->second->GetPackageContentData();

    uint32_t pkg_title = pkg.title_id;
    if (pkg_title == kCurrentlyRunningTitleId) {
      pkg_title = kernel_state_->title_id();
    }

    // Match on content_type + file_name + resolved title_id.
    // device_id is a virtual storage selector, not a content identifier.
    if (data.content_type == pkg.content_type && data.file_name() == pkg.file_name() &&
        query_title == pkg_title) {
      return it;
    }
  }
  return open_packages_.end();
}

ContentPackage* ContentManager::DetachPackage(
    std::unordered_map<string::string_key_case, ContentPackage*,
                       string::string_key_case::Hash>::iterator it) {
  CloseOpenedFilesFromContent(it->first.view());
  ContentPackage* package = it->second;
  open_packages_.erase(it);
  return package;
}

bool ContentManager::IsContentOpen(const XCONTENT_AGGREGATE_DATA& data) const {
  return std::any_of(open_packages_.cbegin(), open_packages_.cend(), [&data](const auto& content) {
    return data == content.second->GetPackageContentData();
  });
}

std::filesystem::path ContentManager::GetOpenPackagePath(const std::string_view root_name) const {
  auto it = open_packages_.find(string::string_key_case(root_name));
  if (it == open_packages_.end()) {
    return {};
  }
  return it->second->package_path();
}

void ContentManager::CloseOpenedFilesFromContent(const std::string_view root_name) {
  // TODO(Gliniak): Cleanup this code to care only about handles
  // related to provided content
  const std::vector<object_ref<XFile>> all_files_handles =
      kernel_state_->object_table()->GetObjectsByType<XFile>(XObject::Type::File);

  std::string resolved_path = "";
  kernel_state_->file_system()->FindSymbolicLink(std::string(root_name) + ':', resolved_path);

  for (const object_ref<XFile>& file : all_files_handles) {
    std::string file_path = file->entry()->absolute_path();
    bool is_file_inside_content = rex::string::utf8_starts_with(file_path, resolved_path);

    if (is_file_inside_content) {
      file->ReleaseHandle();
    }
  }
}

static X_RESULT ExtractEntry(rex::filesystem::Entry* entry,
                             const std::filesystem::path& base_path) {
  auto dest_path = base_path / rex::to_path(rex::string::utf8_fix_path_separators(entry->path()));

  if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
    std::error_code ec;
    std::filesystem::create_directories(dest_path, ec);
    if (ec) {
      return X_ERROR_ACCESS_DENIED;
    }
    return X_ERROR_SUCCESS;
  }

  // Ensure parent directory exists
  std::error_code ec;
  std::filesystem::create_directories(dest_path.parent_path(), ec);

  rex::filesystem::File* in_file = nullptr;
  X_STATUS status = entry->Open(rex::filesystem::FileAccess::kFileReadData, &in_file);
  if (status != X_STATUS_SUCCESS) {
    return X_ERROR_ACCESS_DENIED;
  }

  auto out_file = rex::filesystem::OpenFile(dest_path, "wb");
  if (!out_file) {
    in_file->Destroy();
    return X_ERROR_ACCESS_DENIED;
  }

  constexpr size_t kBufferSize = 4 * 1024 * 1024;  // 4 MiB
  auto buffer = std::make_unique<uint8_t[]>(kBufferSize);
  size_t remaining = entry->size();
  size_t offset = 0;

  while (remaining > 0) {
    size_t bytes_read = 0;
    size_t to_read = std::min(remaining, kBufferSize);
    in_file->ReadSync(std::span<uint8_t>(buffer.get(), to_read), offset, &bytes_read);
    if (bytes_read == 0) {
      break;
    }
    fwrite(buffer.get(), 1, bytes_read, out_file);
    offset += bytes_read;
    remaining -= bytes_read;
  }

  fclose(out_file);
  in_file->Destroy();
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::InstallContent(const std::filesystem::path& package_path) {
  if (!std::filesystem::exists(package_path)) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  // Mount the STFS package as a virtual filesystem device
  auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", package_path);
  if (!device->Initialize()) {
    return X_ERROR_ACCESS_DENIED;
  }

  // Derive install destination:
  // root_path_/0000000000000000/{title_id}/00000002/{filename}/
  auto file_name = rex::path_to_utf8(package_path.filename());

  XCONTENT_AGGREGATE_DATA content_data;
  content_data.device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
  content_data.content_type = XContentType::kMarketplaceContent;
  content_data.title_id = kernel_state_->title_id();
  content_data.xuid = 0;
  content_data.set_file_name(file_name);

  // Read display name from STFS metadata
  auto display_name = device->header().metadata.display_name(rex::system::XLanguage::kEnglish);
  if (!display_name.empty()) {
    content_data.set_display_name(display_name);
  } else {
    content_data.set_display_name(rex::path_to_utf16(package_path.filename()));
  }

  auto install_path = ResolvePackagePath(0, content_data);

  // Create destination directory
  std::error_code ec;
  std::filesystem::create_directories(install_path, ec);
  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  // Extract all files breadth-first
  auto* root = device->ResolvePath("");
  if (!root) {
    return X_ERROR_ACCESS_DENIED;
  }

  std::queue<rex::filesystem::Entry*> queue;
  queue.push(root);

  while (!queue.empty()) {
    auto* entry = queue.front();
    queue.pop();

    for (auto& child : entry->children()) {
      queue.push(child.get());
    }

    auto result = ExtractEntry(entry, install_path);
    if (result != X_ERROR_SUCCESS) {
      return result;
    }
  }

  // Compute license mask from STFS header licenses
  uint32_t license_mask = 0;
  for (size_t i = 0; i < 0x10; i++) {
    if (device->header().header.licenses[i].license_flags) {
      license_mask |= device->header().header.licenses[i].license_bits;
    }
  }

  // Write .header file
  return WriteContentHeaderFile(0, content_data, license_mask);
}

// Xbox 360 title update content type. Deliberately not mounted: this port
// statically recompiles the base executable at build time, so a TU's
// executable delta patch cannot apply and is not needed. Content sets in the
// wild usually ship one; that is normal and harmless.
static constexpr uint32_t kTitleUpdateContentType = 0x000B0000;

void ContentManager::DiscoverContainersInDir(const std::filesystem::path& dir, const char* source,
                                             bool recursive, uint32_t title_id) {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec) || ec) {
    return;
  }

  auto handle_file = [&](const std::filesystem::path& path) {
    auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(path);
    if (!header) {
      // Not an STFS/LIVE/PIRS container (thumbnails, .header sidecars, ...).
      REXSYS_DEBUG("ContentManager: ignoring non-container file {}", rex::path_to_utf8(path));
      return;
    }
    const uint32_t container_title = header->metadata.execution_info.title_id;
    if (container_title != title_id) {
      REXSYS_DEBUG("ContentManager: ignoring container for another title ({:08X}): {}",
                   container_title, rex::path_to_utf8(path));
      return;
    }
    const XContentType content_type = header->metadata.content_type;
    if (uint32_t(content_type) == kTitleUpdateContentType) {
      REXSYS_DEBUG("ContentManager: title update package present (not used by this port): {}",
                   rex::path_to_utf8(path.filename()));
      return;
    }
    if (content_type != XContentType::kMarketplaceContent) {
      REXSYS_DEBUG("ContentManager: ignoring container of content type {:08X}: {}",
                   uint32_t(content_type), rex::path_to_utf8(path));
      return;
    }

    // Key = the file name enumeration hands to the game (42-char field).
    auto key_name = rex::path_to_utf8(path.filename());
    if (key_name.size() > 42) {
      key_name.resize(42);
    }
    if (containers_.count(string::string_key_case(key_name))) {
      REXSYS_DEBUG("ContentManager: duplicate container name {}, keeping the first found",
                   key_name);
      return;
    }

    DiscoveredContainer container;
    container.path = path;
    container.content_type = content_type;
    container.title_id = container_title;
    container.display_name = header->metadata.display_name(XLanguage::kEnglish);
    container.source = source;
    containers_.insert({string::string_key_case::create(key_name), std::move(container)});
  };

  if (recursive) {
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec)) {
        handle_file(it->path());
      }
    }
  } else {
    for (auto it = std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec)) {
        handle_file(it->path());
      }
    }
  }
}

void ContentManager::DiscoverContainers(const std::filesystem::path& dlc_dir) {
  const uint32_t title_id = kernel_state_->title_id();
  containers_.clear();

  // The dlc folder next to the exe: recursive, so a flat dump, an unzipped
  // content set, and Xenia's content-directory layout all work unchanged.
  DiscoverContainersInDir(dlc_dir, "dlc", true, title_id);
  // Containers dropped straight into the content root's marketplace folder.
  DiscoverContainersInDir(ResolvePackageRoot(0, XContentType::kMarketplaceContent, title_id),
                          "content root", false, title_id);

  // Startup report, one line per package: "did my DLC install work?" becomes
  // a log grep instead of a guess. Containers take priority, so a folder with
  // a same-named container is the overridden one.
  size_t folder_count = 0;
  auto package_root = ResolvePackageRoot(0, XContentType::kMarketplaceContent, title_id);
  auto file_infos = rex::filesystem::ListFiles(package_root);
  for (const auto& file_info : file_infos) {
    if (file_info.type != rex::filesystem::FileInfo::Type::kDirectory) {
      continue;
    }
    auto name = rex::path_to_utf8(file_info.name);
    std::string display;
    XCONTENT_AGGREGATE_DATA content_data;
    if (XSUCCEEDED(ReadContentHeaderFile(name, 0, title_id, XContentType::kMarketplaceContent,
                                         content_data))) {
      display = rex::string::to_utf8(content_data.display_name());
    }
    const bool overridden =
        FindContainer(name, XContentType::kMarketplaceContent) != nullptr;
    REXSYS_INFO("DLC: \"{}\" [{}] - extracted folder{}", display.empty() ? name : display, name,
                overridden ? " (overridden by a container)" : "");
    folder_count++;
  }

  std::vector<const decltype(containers_)::value_type*> sorted_containers;
  sorted_containers.reserve(containers_.size());
  for (const auto& entry : containers_) {
    sorted_containers.push_back(&entry);
  }
  std::sort(sorted_containers.begin(), sorted_containers.end(),
            [](const auto* a, const auto* b) { return a->first.view() < b->first.view(); });
  for (const auto* entry : sorted_containers) {
    const auto& container = entry->second;
    REXSYS_INFO("DLC: \"{}\" [{}] - container ({}): {}",
                rex::string::to_utf8(container.display_name), entry->first.view(),
                container.source, rex::path_to_utf8(container.path));
  }
  REXSYS_INFO("DLC: {} extracted folder(s), {} container(s) mounted with priority", folder_count,
              containers_.size());
}

}  // namespace xam
}  // namespace system
}  // namespace rex
