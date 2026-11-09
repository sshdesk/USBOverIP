# Create the build directory if it doesn't exist
New-Item -ItemType Directory -Path "build" -Force

# Generate the Visual Studio 2022 project files
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Debug -DRTC_STATIC=1 -DCMAKE_POSITION_INDEPENDENT_CODE=ON

# Compile the project
cmake --build build --config Debug


