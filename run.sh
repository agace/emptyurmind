#!/bin/bash

set -e

TARGET="bewatermyfriend"
VIDEO=${1:-emptyurmind.mp4}

echo ">>> Checking dependencies..."

if ! command -v pkg-config >/dev/null 2>&1; then
    echo "pkg-config not found. Attempting to install..."
    if [ -f /etc/debian_version ]; then
        sudo apt install -y pkg-config libavformat-dev libavcodec-dev libavutil-dev \
            libswscale-dev libswresample-dev libsdl2-dev libncurses-dev
    elif [ -f /etc/fedora-release ]; then
        sudo dnf install -y pkg-config ffmpeg-devel SDL2-devel ncurses-devel
    elif [ -f /etc/arch-release ]; then
        sudo pacman -S --needed pkgconf ffmpeg sdl2 ncurses
    else
        echo "Unsupported distro. Please install FFmpeg, SDL2, ncurses, and pkg-config manually."
        exit 1
    fi
else
    echo "pkg-config found"
fi

echo ">>> Cleaning old build..."
make clean || true

echo ">>> Building project..."
make

echo ">>> Running $TARGET with video: $VIDEO"
./$TARGET "$VIDEO"

