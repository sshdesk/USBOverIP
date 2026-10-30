cmake_minimum_required(VERSION 3.16)
project(usbipd VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_ASSERTIONS")

find_package(OpenSSL REQUIRED)
find_package(google-webrtc REQUIRED)
find_package(spdlog REQUIRED)

# Firebase C++ SDK Configuration
set(FIREBASE_CPP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../firebasesdk/third_party/firebase_cpp_sdk/firebase_cpp_sdk")
add_subdirectory(${FIREBASE_CPP_SDK_DIR} bin/ EXCLUDE_FROM_ALL)

if(WIN32)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(FB_CONFIG_TYPE "Release")
    else()
        set(FB_CONFIG_TYPE "Debug")
    endif()

    set(FB_CORRECT_LIB_DIR "${FIREBASE_CPP_SDK_DIR}/libs/windows/VS2019/MT/x64/${FB_CONFIG_TYPE}")

    set_target_properties(firebase_firestore PROPERTIES IMPORTED_LOCATION "${FB_CORRECT_LIB_DIR}/firebase_firestore.lib")
    set_target_properties(firebase_database  PROPERTIES IMPORTED_LOCATION "${FB_CORRECT_LIB_DIR}/firebase_database.lib")
    set_target_properties(firebase_auth      PROPERTIES IMPORTED_LOCATION "${FB_CORRECT_LIB_DIR}/firebase_auth.lib")
    set_target_properties(firebase_app       PROPERTIES IMPORTED_LOCATION "${FB_CORRECT_LIB_DIR}/firebase_app.lib")

    link_libraries(dbghelp.lib)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Opus REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(ZLIB REQUIRED CONFIG)

# Headless executable (WITHOUT WIN32 keyword so stdout/stdin pipes work properly)
add_executable(guiElectron
    guiElectron/main.cpp
    usbip_commands.cpp
    usbipd_commands.cpp
    webrtc_usbipd.cpp
    webrtc_usbip.cpp
    signaling.cpp
    webrtc_stats.cpp
    vbox_service_manager.cpp
    tcptunnel.cpp
    usbip/userspace/usbip/usbip.rc

    # USBIPD Source Files
    usbipd/src/usb_device_hid.cpp  
    usbipd/src/usb_device_passthrough.cpp  
    usbipd/src/usbip_server.cpp  
)

target_include_directories(guiElectron PRIVATE
    gui
    usbipd/include
    guiElectron
    usbip/include/usbip
    usbip/userspace/usbip
    usbip/userspace
    ./
)

if(WIN32)
    target_compile_definitions(guiElectron PRIVATE WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_options(guiElectron PRIVATE "/IGNORE:4099")
else()
    target_compile_definitions(guiElectron PRIVATE WEBRTC_POSIX)
endif()

target_link_libraries(guiElectron PRIVATE 
    google-webrtc::google-webrtc 
    openssl::openssl 
    Opus::opus 
    spdlog::spdlog 
    usbip
    firebase_firestore
    firebase_database
    firebase_auth
    firebase_app
    ws2_32 winmm secur32 crypt32 iphlpapi shlwapi user32 gdi32
    ZLIB::ZLIB 
)

add_subdirectory(usbip)
