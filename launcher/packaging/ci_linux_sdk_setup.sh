#!/usr/bin/env bash
# CI helper (release.yml, sdk-linux): everything the SDK build needs on a fresh Ubuntu 22.04 runner.
# The overlay is built on 22.04 so its binaries need glibc 2.35 only. 22.04 has no C++23 standard
# library headers (g++ 12) and CMake 3.22 (the SDK wants 3.25), so g++-14 comes from the
# ubuntu-toolchain-r PPA and CMake, ninja and LLVM are the launcher's pinned downloads.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update
$SUDO apt-get install -y software-properties-common ca-certificates
$SUDO add-apt-repository -y ppa:ubuntu-toolchain-r/test
$SUDO apt-get update
$SUDO apt-get install -y g++-14 make ninja-build python3 git patchelf pkg-config \
    libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev libxfixes-dev libxcursor-dev libxi-dev \
    libxrandr-dev libxss-dev libxrender-dev libxtst-dev x11proto-dev libwayland-dev \
    libxkbcommon-dev libdrm-dev libgl1-mesa-dev libvulkan-dev
python3 "$HERE/ci_fetch.py" cmake-linux "$HOME/cmake" --strip 1
python3 "$HERE/ci_fetch.py" llvm-linux "$HOME/llvm" --strip 1
python3 "$HERE/ci_fetch.py" sdk-linux "$HOME/rexglue-sdk-nightly/0.10.0.24"
