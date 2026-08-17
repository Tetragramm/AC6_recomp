/**
 * @file        tests/unit/system/host_path_atomic_test.cpp
 * @brief       Unit tests for the atomic write path of the host-path device
 *              (torn saves / .rex-tmp / .rex-bak / write marker)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <native/filesystem.h>
#include <native/filesystem/devices/host_path_device.h>
#include <native/filesystem/entry.h>
#include <native/filesystem/file.h>

namespace fs = std::filesystem;
using rex::filesystem::FileAccess;
using rex::filesystem::HostPathDevice;

namespace {

struct TempDir {
  fs::path path;
  explicit TempDir(const char* name) {
    path = fs::temp_directory_path() / "rex_host_path_atomic_tests" / name;
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void WriteHostFile(const fs::path& p, const std::string& content) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << content;
}

std::string ReadHostFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("atomic write: in-flight data lives in the temp; commit swaps it in with one .bak",
          "[filesystem][atomic_write]") {
  TempDir dir("commit");
  WriteHostFile(dir.path / "save.dat", "OLD");

  HostPathDevice device("\\Device\\AtomicTest1\\", dir.path, false);
  REQUIRE(device.Initialize());
  auto* entry = device.ResolvePath("save.dat");
  REQUIRE(entry != nullptr);

  rex::filesystem::File* file = nullptr;
  REQUIRE(entry->Open(FileAccess::kGenericRead | FileAccess::kGenericWrite, &file) == 0);
  REQUIRE(file != nullptr);

  const std::string new_content = "NEW!";
  size_t written = 0;
  REQUIRE(file->WriteSync({reinterpret_cast<const uint8_t*>(new_content.data()),
                           new_content.size()},
                          0, &written) == 0);
  CHECK(written == new_content.size());

  // The write went to the temp, NOT the real file: an interruption here
  // leaves the old data untouched.
  CHECK(ReadHostFile(dir.path / "save.dat") == "OLD");
  CHECK(fs::exists(dir.path / "save.dat.rex-tmp"));

  // Close = commit: temp renamed over the real file, previous generation
  // kept as .rex-bak.
  file->Destroy();
  CHECK(ReadHostFile(dir.path / "save.dat") == "NEW!");
  CHECK(ReadHostFile(dir.path / "save.dat.rex-bak") == "OLD");
  CHECK(!fs::exists(dir.path / "save.dat.rex-tmp"));
}

TEST_CASE("atomic write: a stale temp from a dead session is swept at mount, old data intact",
          "[filesystem][atomic_write]") {
  TempDir dir("sweep");
  WriteHostFile(dir.path / "save.dat", "OLD");
  // A session died mid-write and left its temp behind.
  WriteHostFile(dir.path / "save.dat.rex-tmp", "TORN-PARTIAL-WRITE");

  HostPathDevice device("\\Device\\AtomicTest2\\", dir.path, false);
  REQUIRE(device.Initialize());

  CHECK(ReadHostFile(dir.path / "save.dat") == "OLD");
  CHECK(!fs::exists(dir.path / "save.dat.rex-tmp"));
}

TEST_CASE("atomic write: death between the two commit renames is recovered from the backup",
          "[filesystem][atomic_write]") {
  TempDir dir("recover");
  // The crash window: real file already moved to .rex-bak, temp never
  // renamed in.
  WriteHostFile(dir.path / "save.dat.rex-bak", "OLD");
  WriteHostFile(dir.path / "save.dat.rex-tmp", "NEW-NEVER-LANDED");

  HostPathDevice device("\\Device\\AtomicTest3\\", dir.path, false);
  REQUIRE(device.Initialize());

  CHECK(ReadHostFile(dir.path / "save.dat") == "OLD");
  CHECK(!fs::exists(dir.path / "save.dat.rex-tmp"));
}

TEST_CASE("atomic write: .rex artifacts are never guest-visible", "[filesystem][atomic_write]") {
  TempDir dir("hidden");
  WriteHostFile(dir.path / "save.dat", "OLD");
  WriteHostFile(dir.path / "save.dat.rex-bak", "PREVIOUS");

  HostPathDevice device("\\Device\\AtomicTest4\\", dir.path, false);
  REQUIRE(device.Initialize());

  auto* root = device.ResolvePath("");
  REQUIRE(root != nullptr);
  bool saw_artifact = false;
  for (const auto& child : root->children()) {
    if (child->name().find(".rex-") != std::string::npos) {
      saw_artifact = true;
    }
  }
  CHECK(!saw_artifact);
  CHECK(device.ResolvePath("save.dat") != nullptr);
}

TEST_CASE("write marker: present exactly while a write session is open", "[filesystem][atomic_write]") {
  TempDir dir("marker");
  WriteHostFile(dir.path / "save.dat", "OLD");
  const fs::path marker = dir.path.parent_path() / "marker-test.rex-writing";
  std::error_code ec;
  fs::remove(marker, ec);

  HostPathDevice device("\\Device\\AtomicTest5\\", dir.path, false);
  device.set_write_marker_path(marker);
  REQUIRE(device.Initialize());
  CHECK(!fs::exists(marker));

  auto* entry = device.ResolvePath("save.dat");
  REQUIRE(entry != nullptr);
  rex::filesystem::File* file = nullptr;
  REQUIRE(entry->Open(FileAccess::kGenericRead | FileAccess::kGenericWrite, &file) == 0);

  // A write session is open: the marker must be on disk NOW - this is what
  // survives a kill and triggers quarantine at the next mount.
  CHECK(fs::exists(marker));

  file->Destroy();
  CHECK(!fs::exists(marker));

  fs::remove(marker, ec);
}
