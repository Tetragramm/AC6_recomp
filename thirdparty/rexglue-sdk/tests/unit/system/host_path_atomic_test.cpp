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
#include <rex/system/xam/content_manager.h>

// White-box seams for the flush-before-commit drill (src-internal headers).
#include "../../../src/native/filesystem/devices/host_path_entry.h"
#include "../../../src/native/filesystem/devices/host_path_file.h"

namespace fs = std::filesystem;
using rex::filesystem::FileAccess;
using rex::filesystem::HostPathDevice;
using rex::system::XContentType;
using rex::system::xam::ContentManager;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;

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
  // survives a kill and decides quarantine at the next mount. Before any
  // commit rename its phase must be "writing": a kill here leaves every
  // committed file untouched.
  CHECK(fs::exists(marker));
  CHECK(HostPathDevice::ReadWriteMarkerPhase(marker) ==
        rex::filesystem::kWriteMarkerPhaseWriting);

  file->Destroy();
  CHECK(!fs::exists(marker));

  fs::remove(marker, ec);
}

TEST_CASE("write marker: phase flips to 'committing' at the first commit rename and never back",
          "[filesystem][atomic_write]") {
  TempDir dir("marker-phase");
  WriteHostFile(dir.path / "a.dat", "OLD-A");
  WriteHostFile(dir.path / "b.dat", "OLD-B");
  const fs::path marker = dir.path.parent_path() / "marker-phase-test.rex-writing";
  std::error_code ec;
  fs::remove(marker, ec);

  HostPathDevice device("\\Device\\AtomicTest6\\", dir.path, false);
  device.set_write_marker_path(marker);
  REQUIRE(device.Initialize());

  // Two sessions in one burst, so the marker outlives the first commit.
  auto* entry_a = device.ResolvePath("a.dat");
  auto* entry_b = device.ResolvePath("b.dat");
  REQUIRE(entry_a != nullptr);
  REQUIRE(entry_b != nullptr);
  rex::filesystem::File* file_a = nullptr;
  rex::filesystem::File* file_b = nullptr;
  REQUIRE(entry_a->Open(FileAccess::kGenericRead | FileAccess::kGenericWrite, &file_a) == 0);
  REQUIRE(entry_b->Open(FileAccess::kGenericRead | FileAccess::kGenericWrite, &file_b) == 0);
  CHECK(HostPathDevice::ReadWriteMarkerPhase(marker) ==
        rex::filesystem::kWriteMarkerPhaseWriting);

  const std::string new_a = "NEW-A";
  size_t written = 0;
  REQUIRE(file_a->WriteSync({reinterpret_cast<const uint8_t*>(new_a.data()), new_a.size()}, 0,
                            &written) == 0);

  // First commit of the burst: the container is now mutating, and the burst
  // is still open (b.dat's session) - a kill from here on may tear it.
  file_a->Destroy();
  CHECK(fs::exists(marker));
  CHECK(HostPathDevice::ReadWriteMarkerPhase(marker) ==
        rex::filesystem::kWriteMarkerPhaseCommitting);

  // The remaining session closes without a commit: monotonic - the phase
  // never returns to "writing"; the whole burst ending deletes the marker.
  file_b->Destroy();
  CHECK(!fs::exists(marker));

  fs::remove(marker, ec);
}

#if defined(_WIN32)
TEST_CASE("write marker: a marker that cannot be deleted is rewritten to phase 'complete'",
          "[filesystem][atomic_write]") {
  TempDir dir("marker-held");
  WriteHostFile(dir.path / "save.dat", "OLD");
  const fs::path marker = dir.path.parent_path() / "marker-held-test.rex-writing";
  std::error_code ec;
  fs::remove(marker, ec);

  HostPathDevice device("\\Device\\AtomicTest7\\", dir.path, false);
  device.set_write_marker_path(marker);
  REQUIRE(device.Initialize());

  auto* entry = device.ResolvePath("save.dat");
  REQUIRE(entry != nullptr);
  rex::filesystem::File* file = nullptr;
  REQUIRE(entry->Open(FileAccess::kGenericRead | FileAccess::kGenericWrite, &file) == 0);
  REQUIRE(fs::exists(marker));

  const std::string new_content = "NEW!";
  size_t written = 0;
  REQUIRE(file->WriteSync({reinterpret_cast<const uint8_t*>(new_content.data()),
                           new_content.size()},
                          0, &written) == 0);

  {
    // Simulate an antivirus holding the marker: an MSVC ifstream shares
    // read/write but not delete, so deletion fails while rewriting the
    // content stays possible - exactly the scanner behaviour in the field.
    std::ifstream holder(marker, std::ios::binary);
    REQUIRE(holder.is_open());

    file->Destroy();

    // The commit itself went through; the undeletable marker was rewritten
    // to "complete" so the next mount keeps the container.
    CHECK(ReadHostFile(dir.path / "save.dat") == "NEW!");
    CHECK(fs::exists(marker));
    CHECK(HostPathDevice::ReadWriteMarkerPhase(marker) ==
          rex::filesystem::kWriteMarkerPhaseComplete);
  }

  fs::remove(marker, ec);
}
#endif  // defined(_WIN32)

namespace {

// Records that Flush() ran, and whether the real file was still untouched at
// that moment - i.e. the flush landed BEFORE the commit renames.
class FlushProbeHandle : public rex::filesystem::FileHandle {
 public:
  FlushProbeHandle(const fs::path& temp_path, const fs::path& real_path, bool* flushed,
                   bool* real_untouched_at_flush)
      : FileHandle(temp_path),
        real_path_(real_path),
        flushed_(flushed),
        real_untouched_at_flush_(real_untouched_at_flush) {}

  bool Read(size_t, void*, size_t, size_t*) override { return false; }
  bool Write(size_t, const void*, size_t, size_t*) override { return false; }
  bool SetLength(size_t) override { return false; }
  void Flush() override {
    *flushed_ = true;
    *real_untouched_at_flush_ = (ReadHostFile(real_path_) == "OLD");
  }

 private:
  fs::path real_path_;
  bool* flushed_;
  bool* real_untouched_at_flush_;
};

}  // namespace

TEST_CASE("atomic write: the temp is flushed to stable storage before the commit renames",
          "[filesystem][atomic_write]") {
  TempDir dir("flush");
  WriteHostFile(dir.path / "save.dat", "OLD");

  HostPathDevice device("\\Device\\AtomicTest8\\", dir.path, false);
  REQUIRE(device.Initialize());
  auto* entry = device.ResolvePath("save.dat");
  REQUIRE(entry != nullptr);

  // The finished temp, as the guest's last write left it (written after
  // Initialize - mount sweeps stale temps).
  WriteHostFile(dir.path / "save.dat.rex-tmp", "NEW!");

  bool flushed = false;
  bool real_untouched_at_flush = false;
  auto* file = new rex::filesystem::HostPathFile(
      FileAccess::kGenericWrite, static_cast<rex::filesystem::HostPathEntry*>(entry),
      std::make_unique<FlushProbeHandle>(dir.path / "save.dat.rex-tmp", dir.path / "save.dat",
                                         &flushed, &real_untouched_at_flush),
      dir.path / "save.dat.rex-tmp", /*started_dirty=*/true);

  // Close = flush + commit. Rename atomicity only covers which name points
  // at which file - the flush is what gets the BYTES to stable storage
  // before the temp becomes the real file (power loss would otherwise leave
  // a correctly-named file of zeros).
  file->Destroy();

  CHECK(flushed);
  CHECK(real_untouched_at_flush);
  CHECK(ReadHostFile(dir.path / "save.dat") == "NEW!");
  CHECK(ReadHostFile(dir.path / "save.dat.rex-bak") == "OLD");
}

namespace {

// Fixture for the mount-time torn-container decision table. Lays out a
// content root with one populated extracted package and a stale marker whose
// content the drill controls, then runs the same check the content manager
// runs at XamContentCreate/XamContentOpen time.
struct MountFixture {
  TempDir root;
  fs::path package_dir;
  fs::path marker;
  ContentManager manager;
  XCONTENT_AGGREGATE_DATA data{};

  MountFixture()
      : root("mount-check"),
        // content_root/xuid/title_id/content_type/name - the layout
        // ResolvePackagePath produces for xuid 1, an explicit title id, and
        // a saved-game package.
        package_dir(root.path / "0000000000000001" / "12345678" / "00000001" / "sav_test"),
        marker(fs::path(package_dir) += ".rex-writing"),
        manager(nullptr, root.path) {
    fs::create_directories(package_dir);
    WriteHostFile(package_dir / "gamedata.dat", "SAVED");
    data.content_type = XContentType::kSavedGame;
    data.title_id = 0x12345678;
    data.xuid = 0;  // resolves against the xuid argument below
    data.set_display_name(u"drill");
    data.set_file_name("sav_test");
  }

  void RunMountCheck() { manager.QuarantineTornPackage(1, data); }

  bool PackageIntact() const {
    return fs::is_directory(package_dir) &&
           ReadHostFile(package_dir / "gamedata.dat") == "SAVED";
  }

  size_t QuarantinedCount() const {
    std::error_code ec;
    size_t count = 0;
    for (fs::directory_iterator it(root.path / "quarantine", ec), end; !ec && it != end; ++it) {
      ++count;
    }
    return count;
  }
};

}  // namespace

TEST_CASE("mount check: a stale 'writing' marker proves consistency - cleared, not quarantined",
          "[filesystem][atomic_write]") {
  MountFixture fx;
  // The dead session never started a commit rename: every file in the
  // package is still its previous version.
  WriteHostFile(fx.marker, "writing\n");

  fx.RunMountCheck();

  CHECK(fx.PackageIntact());
  CHECK(!fs::exists(fx.marker));
  CHECK(fx.QuarantinedCount() == 0);
}

TEST_CASE("mount check: a stale 'complete' marker proves consistency - cleared, not quarantined",
          "[filesystem][atomic_write]") {
  MountFixture fx;
  // The burst finished every commit; only the marker's own deletion failed
  // (the blocked-delete rewrite path wrote this phase).
  WriteHostFile(fx.marker, "complete\n");

  fx.RunMountCheck();

  CHECK(fx.PackageIntact());
  CHECK(!fs::exists(fx.marker));
  CHECK(fx.QuarantinedCount() == 0);
}

TEST_CASE("mount check: 'committing', unreadable or unrecognized markers quarantine",
          "[filesystem][atomic_write]") {
  MountFixture fx;

  SECTION("phase 'committing': a commit rename ran, the container may be torn") {
    WriteHostFile(fx.marker, "committing\n");
  }
  SECTION("unrecognized content proves nothing - conservative default") {
    WriteHostFile(fx.marker, "Write in progress. Legacy prose, no phase word.\n");
  }
  SECTION("empty marker proves nothing - conservative default") {
    WriteHostFile(fx.marker, "");
  }

  fx.RunMountCheck();

  // Moved aside - never deleted - and the marker is gone, so the game
  // recreates the package cleanly.
  CHECK(!fs::exists(fx.package_dir));
  CHECK(!fs::exists(fx.marker));
  CHECK(fx.QuarantinedCount() == 1);
}
