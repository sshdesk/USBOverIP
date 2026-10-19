# Remove the old build directory if it exists
if (Test-Path build) {
    Remove-Item -Recurse -Force build
}

# Create the new debug build directory
New-Item -ItemType Directory -Path build/Debug -Force

# Configure the project with CMake for Visual Studio 2022
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POSITION_INDEPENDENT_CODE=ON

# Build the project
cmake --build build --config Debug
