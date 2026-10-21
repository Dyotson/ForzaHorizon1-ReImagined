#!/usr/bin/env bash
# CI helper (release.yml, sdk-linux): everything the SDK build needs on a fresh Ubuntu 22.04 runner.
# The overlay is built on 22.04 so its binaries need glibc 2.35 only. 22.04 has no C++23 standard
# library headers (g++ 12) and CMake 3.22 (the SDK wants 3.25), so g++-14 comes from the
# ubuntu-toolchain-r PPA and CMake, ninja and LLVM are the launcher's pinned downloads.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive
# Launchpad occasionally returns 503 for individual packages. APT retries each
# transfer; repeat the command too so already downloaded packages stay cached.
apt_get() {
    local attempt
    for attempt in 1 2 3; do
        if $SUDO apt-get -o Acquire::Retries=3 "$@"; then
            return 0
        fi
        [ "$attempt" -lt 3 ] || return 1
        echo "APT failed (attempt $attempt/3); retrying in $((attempt * 10)) seconds" >&2
        sleep "$((attempt * 10))"
    done
}
apt_get update
apt_get install -y software-properties-common ca-certificates
$SUDO add-apt-repository --no-update -y ppa:ubuntu-toolchain-r/test
apt_get update
# A cached SDK overlay still needs C++23 headers for the stock-header HUD check.
# Avoid installing the SDK's complete build dependencies for that compile alone.
if [ "${1:-}" = --compiler-only ]; then
    apt_get install -y g++-14
    exit 0
fi
# The -dev packages of PulseAudio and ALSA (and D-Bus, fribidi, libthai) matter at run time too: SDL3 inside
# the runtime only gets those audio and input backends if its headers are present when it is built;
# without them the game finds no audio device and stops (KeBugCheckEx).
apt_get install -y g++-14 make ninja-build python3 git patchelf pkg-config \
    libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev libxfixes-dev libxcursor-dev libxi-dev \
    libxrandr-dev libxss-dev libxrender-dev libxtst-dev x11proto-dev libwayland-dev \
    libxkbcommon-dev libdrm-dev libgl1-mesa-dev libvulkan-dev \
    libpulse-dev libasound2-dev libdbus-1-dev libfribidi-dev libthai-dev
python3 "$HERE/ci_fetch.py" cmake-linux "$HOME/cmake" --strip 1
python3 "$HERE/ci_fetch.py" llvm-linux "$HOME/llvm" --strip 1
python3 "$HERE/ci_fetch.py" sdk-linux "$HOME/rexglue-sdk-nightly/0.10.0.24"
