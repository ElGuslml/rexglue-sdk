#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>

using rex::X_STATUS;
using rex::filesystem::Entry;
using rex::filesystem::FileAccess;
using rex::filesystem::FileAction;
using rex::filesystem::FileDisposition;
using rex::filesystem::HostPathDevice;
using rex::filesystem::HostPathEntry;
using rex::filesystem::VirtualFileSystem;

namespace {

// A temporary folder of the test's, removed with it.
class ScopedFolder {
 public:
  explicit ScopedFolder(std::string_view name) {
    root_ = std::filesystem::temp_directory_path() /
            ("rexglue_vfs_overlay_" + std::string(name) + "_" + std::to_string(counter_++));
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
    std::filesystem::create_directories(root_, ec);
  }
  ~ScopedFolder() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  const std::filesystem::path& root() const { return root_; }

  void Write(const std::filesystem::path& relative, std::string_view content) const {
    std::filesystem::create_directories((root_ / relative).parent_path());
    FILE* file = rex::filesystem::OpenFile(root_ / relative, "wb");
    REQUIRE(file != nullptr);
    fwrite(content.data(), 1, content.size(), file);
    fclose(file);
  }

 private:
  static int counter_;
  std::filesystem::path root_;
};

int ScopedFolder::counter_ = 0;

// The whole content of a guest file, or "<missing>".
std::string ReadGuest(VirtualFileSystem& vfs, std::string_view path) {
  rex::filesystem::File* file = nullptr;
  FileAction action = FileAction::kDoesNotExist;
  if (vfs.OpenFile(nullptr, path, FileDisposition::kOpen, FileAccess::kGenericRead, false, true, &file,
                   &action) != X_STATUS_SUCCESS ||
      !file) {
    return "<missing>";
  }
  std::array<uint8_t, 64> buffer{};
  size_t read = 0;
  file->ReadSync(std::span<uint8_t>(buffer), 0, &read);
  file->Destroy();
  return std::string(reinterpret_cast<const char*>(buffer.data()), read);
}

}  // namespace

TEST_CASE("VFS overlay shadows, adds files and adds directories", "[filesystem][vfs][overlay]") {
  ScopedFolder game("game");
  game.Write("data/a.txt", "BASE");
  game.Write("data/keep.txt", "KEEP");
  ScopedFolder overlay("mod");
  overlay.Write("DATA/A.TXT", "OVERLAY!!");  // another casing: matched case-insensitively
  overlay.Write("data/new.txt", "NEW");
  overlay.Write("newdir/inner.txt", "INNER");

  VirtualFileSystem vfs;
  auto device = std::make_unique<HostPathDevice>("\\Game", game.root(), true);
  REQUIRE(device->Initialize());
  REQUIRE(device->MergeOverlay(overlay.root()));
  CHECK_FALSE(device->MergeOverlay(overlay.root() / "not_there"));
  REQUIRE(vfs.RegisterDevice(std::move(device)));
  REQUIRE(vfs.RegisterSymbolicLink("game:", "\\Game"));

  // The shadowed file: the overlay's content and size, the game's name.
  CHECK(ReadGuest(vfs, "game:\\data\\a.txt") == "OVERLAY!!");
  Entry* shadowed = vfs.ResolvePath("game:\\data\\a.txt");
  REQUIRE(shadowed != nullptr);
  CHECK(shadowed->size() == 9);
  CHECK(shadowed->name() == "a.txt");
  CHECK(static_cast<HostPathEntry*>(shadowed)->from_overlay());

  // What the game has and the overlay does not, unchanged.
  CHECK(ReadGuest(vfs, "game:\\data\\keep.txt") == "KEEP");
  CHECK_FALSE(static_cast<HostPathEntry*>(vfs.ResolvePath("game:\\data\\keep.txt"))->from_overlay());

  // A file and a directory added, found in any casing.
  CHECK(ReadGuest(vfs, "game:\\DATA\\New.txt") == "NEW");
  CHECK(ReadGuest(vfs, "game:\\NewDir\\INNER.txt") == "INNER");

  // The listing: the game's two files and the one added, once each.
  Entry* data = vfs.ResolvePath("game:\\data");
  REQUIRE(data != nullptr);
  CHECK(data->children().size() == 3);

  // An overlay's file is not the guest's to move.
  CHECK(shadowed->Rename("game:\\data\\moved.txt") == X_STATUS_ACCESS_DENIED);
  CHECK(std::filesystem::exists(overlay.root() / "DATA" / "A.TXT"));
}

TEST_CASE("VFS without an overlay renames and deletes as before", "[filesystem][vfs][overlay]") {
  ScopedFolder store("store");
  store.Write("save.dat", "SAVE");

  VirtualFileSystem vfs;
  auto device = std::make_unique<HostPathDevice>("\\Store", store.root(), false, true);
  REQUIRE(device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(device)));
  REQUIRE(vfs.RegisterSymbolicLink("store:", "\\Store"));

  Entry* entry = vfs.ResolvePath("store:\\save.dat");
  REQUIRE(entry != nullptr);
  CHECK_FALSE(static_cast<HostPathEntry*>(entry)->from_overlay());

  // Renamed under the device's own host path.
  REQUIRE(entry->Rename("store:\\renamed.dat") == X_STATUS_SUCCESS);
  CHECK_FALSE(std::filesystem::exists(store.root() / "save.dat"));
  CHECK(std::filesystem::exists(store.root() / "renamed.dat"));
  CHECK(ReadGuest(vfs, "store:\\renamed.dat") == "SAVE");

  // Deleted at its parent's host path and its name.
  REQUIRE(vfs.DeletePath("store:\\renamed.dat"));
  CHECK_FALSE(std::filesystem::exists(store.root() / "renamed.dat"));
  CHECK(ReadGuest(vfs, "store:\\renamed.dat") == "<missing>");
}
