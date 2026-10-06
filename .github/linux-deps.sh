#!/usr/bin/env bash
# What the Linux jobs build with, on the release baseline (Ubuntu 22.04,
# glibc 2.35, GCC 12's libstdc++): clang and lld 21 from apt.llvm.org, as
# GCC's LTO spent 90 minutes linking the game, SDL's headers as its
# docs/README-linux.md lists them, and Xinerama for raylib's GLFW. git is
# installed before this, for checkout.
set -e
apt-get install -y wget ca-certificates
wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key
echo "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-21 main" > /etc/apt/sources.list.d/llvm.list
apt-get update
apt-get install -y build-essential g++-12 clang-21 lld-21 python3.11 python3.11-venv pkg-config \
  libasound2-dev libpulse-dev libaudio-dev libfribidi-dev libjack-dev libsndio-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev \
  libxtst-dev libxinerama-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev \
  libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev \
  libusb-1.0-0-dev libpipewire-0.3-dev libwayland-dev wayland-protocols libdecor-0-dev liburing-dev
