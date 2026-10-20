@echo off
rem Remove the old build directory if it exists
if exist build (
    rmdir /s /q build
)

rem Create the new debug build directory
mkdir build\Debug

rem Configure the project with CMake for Visual Studio 2022
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POSITION_INDEPENDENT_CODE=ON

rem Build the project
cmake --build build --config Debug
