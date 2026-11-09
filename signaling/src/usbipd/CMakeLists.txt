cmake_minimum_required(VERSION 3.15)
# 1. Changed language from C to CXX (C++)
project(usbip_server_cpp CXX)


set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# Dynamically target the 'bin' folder in the source root
set(OUTPUT_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_BIN_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_BIN_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_BIN_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${OUTPUT_BIN_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${OUTPUT_BIN_DIR})

include_directories(include)

set(SOURCES
    src/main.cpp
    src/usb_device_hid.cpp
    src/usb_device_passthrough.cpp
    src/usbip_server.cpp
)

add_executable(usbip_server ${SOURCES})

if(WIN32)
    target_link_libraries(usbip_server ws2_32 advapi32 setupapi cfgmgr32)
else()
    target_link_libraries(usbip_server pthread)
endif()
