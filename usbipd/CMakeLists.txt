cmake_minimum_required(VERSION 3.15)
project(usbip_server_c C)

set(CMAKE_C_STANDARD 11)

include_directories(include)

set(SOURCES
    src/main.c
    src/usb_device_hid.c
    src/usb_device_passthrough.c
    src/usbip_server.c
)

add_executable(usbip_server ${SOURCES})

if(WIN32)
    target_link_libraries(usbip_server ws2_32 advapi32 setupapi cfgmgr32)
else()
    target_link_libraries(usbip_server pthread)
endif()
