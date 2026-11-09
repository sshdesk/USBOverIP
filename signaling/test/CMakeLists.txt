cmake_minimum_required(VERSION 3.16)
project(WebRtcAmplifySignaling VERSION 1.0 LANGUAGES CXX)

# Coordinated standard (Aligned to 17 to prevent Conan package warnings)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_ASSERTIONS")

find_package(OpenSSL REQUIRED)
find_package(google-webrtc REQUIRED)
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

# Include cpp-httplib (assuming it's added via subdirectory or fetched)
# add_subdirectory(cpp-httplib)

add_executable(webrtc_signaling_app 
    main.cpp 
    signaling.cpp
    webrtc.cpp
    webrtc_stats.cpp
    tcptunnel.cpp
)

if(WIN32)
    target_compile_definitions(webrtc_signaling_app PRIVATE WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_definitions(webrtc_signaling_app PRIVATE WEBRTC_POSIX)
endif()

if(WIN32)
    target_link_libraries(webrtc_signaling_app
        PRIVATE
        google-webrtc::google-webrtc
        spdlog::spdlog
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        ws2_32 winmm secur32 crypt32 iphlpapi shlwapi user32 gdi32
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)
    pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)

    target_link_libraries(webrtc_signaling_app 
        PRIVATE 
        google-webrtc::google-webrtc
        spdlog::spdlog
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        PkgConfig::SECRET
        PkgConfig::GLIB
        anl
        dl rt m pthread
    )
endif()