cmake_minimum_required(VERSION 3.20)
project(usbip_meta LANGUAGES C CXX) # Changed name slightly to avoid sub-target clash

# Force all nested targets inside this subdirectory to build statically
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static compilation" FORCE)

add_subdirectory(userspace/resources)
add_subdirectory(userspace/libusbip)
add_subdirectory(userspace/devnode)
add_subdirectory(userspace/usbip)
