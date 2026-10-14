"""Pinned downloads. Every archive is checked against its sha256 before it is unpacked."""

LLVM_VERSION = "23.1.3"
MIN_SYSTEM_CLANG = 21   # the project is tested with 23; older than 21 lacks C++23 library parts
CMAKE_VERSION = "4.4.4"
NINJA_VERSION = "1.13.2"
XWIN_VERSION = "0.10.0"
SDK_TAG = "nightly-20261002-bd833a2a"
SDK_VERSION = "0.10.0.24"
_GH = "https://github.com"

# name -> (url, sha256, kind). The portable toolchains are official release archives.
DOWNLOADS = {
    "llvm-linux": (
        f"{_GH}/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/LLVM-{LLVM_VERSION}-Linux-X64.tar.xz",
        "ca9fe385a5d9e2d88132dc0e23a00e9f87f38fa75f67207b5e457d208e71d013", "tar"),
    "llvm-windows": (
        f"{_GH}/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/clang+llvm-{LLVM_VERSION}-x86_64-pc-windows-msvc.tar.xz",
        "9ee6a2192fbb30c776415af529927f76ca6b5d1d55699bb1315f9878e610b5b8", "tar"),
    "cmake-linux": (
        f"{_GH}/Kitware/CMake/releases/download/v{CMAKE_VERSION}/cmake-{CMAKE_VERSION}-linux-x86_64.tar.gz",
        "e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb", "tar"),
    "cmake-windows": (
        f"{_GH}/Kitware/CMake/releases/download/v{CMAKE_VERSION}/cmake-{CMAKE_VERSION}-windows-x86_64.zip",
        "bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536", "zip"),
    "ninja-linux": (
        f"{_GH}/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-linux.zip",
        "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6", "zip"),
    "ninja-windows": (
        f"{_GH}/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-win.zip",
        "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65", "zip"),
    "xwin-windows": (
        f"{_GH}/Jake-Shadle/xwin/releases/download/{XWIN_VERSION}/xwin-{XWIN_VERSION}-x86_64-pc-windows-msvc.tar.gz",
        "96e83665ef5c1406aabd0be603a8d08e67e604f36fda630b6807ee948a614e26", "tar"),
    # The ReXGlue SDK nightly the project's patches are made for (BSD-3-Clause).
    "sdk-linux": (
        f"{_GH}/rexglue/rexglue-sdk/releases/download/{SDK_TAG}/rexglue-sdk-{SDK_VERSION}-dev.gbd833a2-linux-amd64.zip",
        "0a56565221a4e75f91fc71f787d9299a0d0298127b970c8a88a292f8d3a3befe", "zip"),
}

# Rough download sizes in MB, for the confirmation text.
SIZES_MB = {"llvm-linux": 2016, "llvm-windows": 901, "cmake-linux": 65, "cmake-windows": 54,
            "ninja-linux": 1, "ninja-windows": 1, "xwin-windows": 3, "sdk-linux": 122}

MS_LICENSE_URL = "https://go.microsoft.com/fwlink/?LinkId=2086102"
