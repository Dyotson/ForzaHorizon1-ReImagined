// The VFS turns a host refusal of a guest-created directory into an error status instead of
// letting std::filesystem throw through NtCreateFile into guest code (patch 0097, #211).
// Links the runtime; no game files needed.
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/system/xtypes.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using namespace rex::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

// NtCreateFile(FILE_CREATE, FILE_DIRECTORY_FILE), as the game's CreateDirectoryA makes it.
static rex::X_STATUS CreateDirectory(VirtualFileSystem& vfs, std::string_view path) {
  File* file = nullptr;
  FileAction action = FileAction::kDoesNotExist;
  auto status = vfs.OpenFile(nullptr, path, FileDisposition::kCreate,
                             FileAccess::kFileReadData, true, false, &file, &action);
  if (file) file->Destroy();
  return status;
}

int main() {
  const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
  const auto root = temporary / ("fh1-vfs-host-errors-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  int rc = 0;
  try {
    std::filesystem::create_directories(root);
    VirtualFileSystem vfs;
    auto device = std::make_unique<HostPathDevice>("\\Device\\Photo", root, false, true);
    CHECK(device->Initialize());
    CHECK(vfs.RegisterDevice(std::move(device)));
    CHECK(vfs.RegisterSymbolicLink("photo:", "\\Device\\Photo"));

    CHECK(XSUCCEEDED(CreateDirectory(vfs, "photo:\\PHOTO2")));
    CHECK(std::filesystem::is_directory(root / "PHOTO2"));

    // A file the VFS has not seen where the directory goes: every host refuses.
    FILE* blocker = OpenFile(root / "blocker", "wb");
    CHECK(blocker);
    fclose(blocker);
    CHECK(XFAILED(CreateDirectory(vfs, "photo:\\blocker")));
    CHECK(XFAILED(CreateDirectory(vfs, "photo:\\blocker\\sub")));
    // A name past every host's limit (NAME_MAX 255; MAX_PATH without long-path support).
    CHECK(XFAILED(CreateDirectory(vfs, "photo:\\" + std::string(260, 'x'))));
#ifdef _WIN32
    // NtCreateFile lets ':' through; Windows refuses it in a name (ERROR_DIRECTORY).
    CHECK(XFAILED(CreateDirectory(vfs, "photo:\\Ferrari:458")));
#endif
    // Still creates what the host accepts, after the refusals.
    CHECK(XSUCCEEDED(CreateDirectory(vfs, "photo:\\PHOTO2\\sub")));
    CHECK(std::filesystem::is_directory(root / "PHOTO2" / "sub"));
    std::cout << "vfs host errors: ok\n";
  } catch (const std::exception& e) {
    std::cerr << "vfs host errors: FAILED: " << e.what() << "\n";
    rc = 1;
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return rc;
}
