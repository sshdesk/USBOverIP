cmake_minimum_required(VERSION 3.16)
project(WebRtcFirebaseSignaling VERSION 1.0 LANGUAGES CXX)

# Coordinated standard (Aligned to 17 to prevent Conan package warnings)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)



set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_ASSERTIONS")


find_package(OpenSSL REQUIRED)
find_package(google-webrtc REQUIRED)
find_package(spdlog REQUIRED)

find_package(SDL2 REQUIRED)

find_package(SDL2_ttf QUIET)
if(NOT SDL2_ttf_FOUND)
    # Manual fallback configuration layout if parsing paths directly
    find_path(SDL2_TTF_INCLUDE_DIR SDL_ttf.h PATH_SUFFIXES SDL2)
    find_library(SDL2_TTF_LIBRARY NAMES SDL2_ttf)
else()
    set(SDL2_TTF_INCLUDE_DIR ${SDL2_TTF_INCLUDE_DIRS})
    set(SDL2_TTF_LIBRARY ${SDL2_TTF_LIBRARIES})
endif()


set(FIREBASE_CPP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/firebase_cpp_sdk")
add_subdirectory(${FIREBASE_CPP_SDK_DIR} bin/ EXCLUDE_FROM_ALL)


add_executable(webrtc_signaling_app 
    main.cpp 
    signaling.cpp
    webrtc.cpp
    webrtc_stats.cpp

)

target_include_directories(webrtc_signaling_app PRIVATE 
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
)


target_link_libraries(webrtc_signaling_app PRIVATE 
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARY}
)


if(WIN32)
    target_compile_definitions(webrtc_signaling_app PRIVATE WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_definitions(webrtc_signaling_app PRIVATE WEBRTC_POSIX)
endif()


find_package(Threads REQUIRED)

if(WIN32)
    target_link_libraries(webrtc_signaling_app
        PRIVATE
        google-webrtc::google-webrtc
        spdlog::spdlog
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        
        firebase_firestore
        firebase_database
        firebase_auth
        firebase_app
        
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
        
        firebase_firestore
        firebase_database
        firebase_auth
        firebase_app
        
        dl rt m pthread
    )
endif()
