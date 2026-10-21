// Exercise the real runtime filesystem, without game code or a player save.
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace vfs = rex::filesystem;
using rex::X_STATUS;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  try {
    Check(argc == 2, "Pass a disposable scratch directory");
    const auto root = fs::absolute(argv[1]);
    fs::create_directories(root);
    // Each run owns a fresh directory. Keep it for inspection after a failure.
    unsigned index = 0;
    fs::path session;
    do { session = root / ("run-" + std::to_string(index++)); }
    while (!fs::create_directory(session));

    auto device = std::make_unique<vfs::HostPathDevice>("photo:", session, false);
    Check(device->Initialize(), "Mount scratch photo device");
    auto* directory = device->ResolvePath("");
    Check(directory != nullptr, "Resolve photo root");
    Check(directory->CreateEntry("Normal", vfs::kFileAttributeDirectory), "Create normal photo");

    // Portable deterministic host error: a file occupies a requested directory.
    // The file was created after mounting, so it is absent from the VFS cache.
    { std::ofstream(session / "Blocked") << "keep this file"; }
    Check(!directory->CreateEntry("Blocked", vfs::kFileAttributeDirectory),
          "Directory creation must return failure instead of throwing");
    std::string contents;
    { std::ifstream input(session / "Blocked"); std::getline(input, contents); }
    Check(contents == "keep this file", "Failed create must preserve existing data");
    Check(!directory->GetChild("Blocked"), "Failed create must not add a cached directory");

    // A directory can appear after the device's initial scan, before the create.
    fs::create_directory(session / "Appeared");
    Check(directory->CreateEntry("Appeared", vfs::kFileAttributeDirectory),
          "An existing host directory is a successful create, not an I/O error");

    vfs::VirtualFileSystem filesystem;
    filesystem.RegisterDevice(std::move(device));
    vfs::File* file = nullptr;
    vfs::FileAction action{};
#ifdef _WIN32
    // NtCreateFile permits ':' inside an ANSI path, but Windows cannot create
    // this directory. This is the same create-directory request as the log.
    const auto status = filesystem.OpenFile(nullptr, "photo:\\invalid:name",
        vfs::FileDisposition::kCreate, 0x00100001, true, false, &file, &action);
    Check(status == X_STATUS_ACCESS_DENIED && !file, "Return guest failure for invalid host name");
#endif
    const auto status_ok = filesystem.OpenFile(nullptr, "photo:\\AfterFailure",
        vfs::FileDisposition::kOpenIf, 0x00100001, true, false, &file, &action);
    Check(status_ok == X_STATUS_SUCCESS && file, "Next photo directory still opens after failure");
    file->Destroy();
    std::cout << "Photo directory failures return safely; existing data preserved\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
