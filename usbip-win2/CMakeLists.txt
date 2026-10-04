cmake_minimum_required(VERSION 3.20)
project(usbip_win2_userspace LANGUAGES C CXX)

# Force all output binaries (.exe, .dll) and debug symbols (.pdb) into a shared 'bin' folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")


add_subdirectory(userspace/resources)
add_subdirectory(userspace/libusbip)
add_subdirectory(userspace/devnode)
add_subdirectory(userspace/usbip)
