#!/bin/bash
set -e # Exit immediately if a command exits with a non-zero status

echo "============ Starting SDL GUI Build (Bash) ============"

# 1. Setup Build and Asset Directories
PROJECT_ROOT=$(pwd)
BUILD_DIR="$PROJECT_ROOT/build"
ASSETS_DIR="$PROJECT_ROOT/assets"

mkdir -p "$BUILD_DIR"
mkdir -p "$ASSETS_DIR"

# 2. Fetch a fallback font into project assets if it doesn't exist
if [ ! -f "$ASSETS_DIR/arial.ttf" ]; then
    echo "No font asset found. Attempting to copy a system font..."
    if [ -f "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" ]; then
        cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf "$ASSETS_DIR/arial.ttf"
    elif [ -f "/System/Library/Fonts/Supplemental/Arial.ttf" ]; then # macOS fallback
        cp "/System/Library/Fonts/Supplemental/Arial.ttf" "$ASSETS_DIR/arial.ttf"
    else
        echo "Warning: System font fallback not found. Please place a font at assets/arial.ttf"
    fi
fi

# 3. Configure and Build Code via CMake
cd "$BUILD_DIR"
echo "Configuring CMake project..."
cmake ..

echo "Compiling application..."
cmake --build . --config Debug

# 4. Copy Assets directly next to the output executable
echo "Deploying assets to output target folder..."
# Handles standard Makefile layout vs multi-configuration generators (like Xcode on Mac)
if [ -d "Debug" ]; then
    mkdir -p "Debug/assets"
    cp "$ASSETS_DIR/arial.ttf" "Debug/assets/"
else
    mkdir -p "assets"
    cp "$ASSETS_DIR/arial.ttf" "assets/"
fi

echo "============ Build Finished Successfully! ============"
echo "To run your app, execute: ./build/sdl_gui"

