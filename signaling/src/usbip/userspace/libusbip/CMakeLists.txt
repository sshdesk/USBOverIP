cmake_minimum_required(VERSION 3.20)
project(libusbip LANGUAGES CXX)

add_library(libusbip STATIC)

target_sources(libusbip
    PRIVATE
        libusbip.rc
        src/device_speed.cpp
        src/dllmain.cpp
        src/file_ver.cpp
        src/format_message.cpp
        src/output.cpp
        src/persistent.cpp
        src/proto_op.cpp
        src/remote.cpp
        src/strconv.cpp
        src/usb_ids.cpp
        src/vhci.cpp
        src/win_socket.cpp
)

set_target_properties(libusbip PROPERTIES
    COMPILE_WARNING_AS_ERROR ON
)

target_compile_features(libusbip
    PRIVATE
        cxx_std_23
        c_std_17
)

target_include_directories(libusbip
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_compile_definitions(libusbip
    PRIVATE
        USBIP_EXPORTS
        _WINDOWS
        _USRDLL
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        "$<$<CONFIG:Release>:NDEBUG>"
)

target_link_libraries(libusbip
    PRIVATE
        Version
        ws2_32
        CfgMgr32
)

target_compile_options(libusbip
    PRIVATE
        /W4
)
