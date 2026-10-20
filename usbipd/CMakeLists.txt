cmake_minimum_required(VERSION 3.15)
# 1. Changed language from C to CXX (C++)
project(usbip_server_cpp CXX)



# This project uses the Windows VBoxUSB/VBoxUSBMon driver stack and Win32 APIs.
if(NOT WIN32)
    message(FATAL_ERROR "This USB/IP passthrough server requires Windows and the VBoxUSB/VBoxUSBMon driver stack.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(usbip_server
    src/main.cpp
    src/usb_device_hid.cpp
    src/usb_device_passthrough.cpp
    src/usbip_server.cpp
)

target_include_directories(usbip_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(usbip_server PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)




if(MSVC)

    target_compile_options(usbip_server PRIVATE /W4 /EHsc)
    

    target_compile_options(usbip_server PRIVATE 
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/O2>
        $<$<CONFIG:MinSizeRel>:/O1>
    )
else()
    target_compile_options(usbip_server PRIVATE -Wall -Wextra -O2)
endif()




target_link_libraries(usbip_server PRIVATE
    ws2_32
    advapi32
    setupapi
    cfgmgr32
)

set_target_properties(usbip_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Keep multi-configuration Visual Studio generators consistent too.
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(usbip_server PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_BINARY_DIR}/bin"
    )
endforeach()
