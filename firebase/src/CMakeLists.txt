cmake_minimum_required(VERSION 3.16)
project(usbipd VERSION 1.0 LANGUAGES CXX)

# Coordinated standard (Aligned to 17 to prevent Conan package warnings)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_ASSERTIONS")

find_package(OpenSSL REQUIRED)
find_package(google-webrtc REQUIRED)
find_package(spdlog REQUIRED)

# Include the Firebase C++ SDK Subdirectory
set(FIREBASE_CPP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../firebasesdk/third_party/firebase_cpp_sdk/firebase_cpp_sdk")
add_subdirectory(${FIREBASE_CPP_SDK_DIR} bin/ EXCLUDE_FROM_ALL)


if(WIN32)
    # Automatically resolve configuration subfolder type (fallback to Debug if not specified)
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
# -------------------------------------------------------------------------




find_package(Opus REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(ZLIB REQUIRED CONFIG)
find_package(SDL2 REQUIRED CONFIG)
find_package(SDL2_ttf REQUIRED CONFIG)


add_executable(gui_usbip WIN32 gui/gui_usbip.cpp)



target_link_libraries(gui_usbip PRIVATE 
    google-webrtc::google-webrtc 
    openssl::openssl 
    Opus::opus 
    spdlog::spdlog 
    ZLIB::ZLIB 
    SDL2::SDL2
    SDL2::SDL2main
    sdl_ttf::sdl_ttf
)


add_executable(usbipd 
    main.cpp 
    signaling.cpp
    webrtc_stats.cpp
    vbox_service_manager.cpp

    # USBIPD Source Files
    usbipd/src/usb_device_hid.cpp  
    usbipd/src/usb_device_passthrough.cpp  
    usbipd/src/usbip_server.cpp  
)


# Add include directories for USBIPD headers
target_include_directories(usbipd PRIVATE
    usbipd/include
)

if(WIN32)
    target_compile_definitions(usbipd PRIVATE WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN)
    
    # Suppress the noisy LNK4099 warnings regarding missing internal libcurl PDB files
    target_link_options(usbipd PRIVATE "/IGNORE:4099")
else()
    target_compile_definitions(usbipd PRIVATE WEBRTC_POSIX)
endif()

find_package(Threads REQUIRED)

if(WIN32)
    target_link_libraries(usbipd
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

    target_link_libraries(usbipd 
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


add_subdirectory(usbip)

add_executable(usbip-win2 
    mainusbip.cpp
    signaling.cpp
    tcptunnel.cpp
    webrtc_stats.cpp
    usbip/userspace/usbip/usbip.rc
)




if(WIN32)
    target_compile_definitions(usbip-win2 PRIVATE WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN)
    
    # Suppress the noisy LNK4099 warnings regarding missing internal libcurl PDB files
    target_link_options(usbip-win2 PRIVATE "/IGNORE:4099")
else()
    target_compile_definitions(usbip-win2 PRIVATE WEBRTC_POSIX)
endif()


target_include_directories(usbip-win2 PRIVATE
    usbip/include/usbip  usbip/userspace/usbip
)


find_package(Threads REQUIRED)

if(WIN32)
    target_link_libraries(usbip-win2
        PRIVATE
        google-webrtc::google-webrtc
        spdlog::spdlog
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        usbip
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

    target_link_libraries(usbip-win2 
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




# AUTOMATED FIX: Copy libusbip.dll to the executable folder post-build
if(WIN32)
    add_custom_command(TARGET usbip-win2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:libusbip>"
        "$<TARGET_FILE_DIR:usbip-win2>/"
    )
endif()



set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Local install prefix" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/google-services.json")
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(FB_OUTPUT_CONFIG "Release")
    else()
        set(FB_OUTPUT_CONFIG "Debug")
    endif()
    
    # Copy to config-specific build directory
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/google-services.json"
        "${CMAKE_BINARY_DIR}/${FB_OUTPUT_CONFIG}/google-services.json"
        COPYONLY
    )
    
    # Copy to root build directory
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/google-services.json"
        "${CMAKE_BINARY_DIR}/google-services.json"
        COPYONLY
    )
    
    install(
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/google-services.json"
        DESTINATION bin
    )
else()
    message(WARNING "google-services.json not found in source directory: ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Copy and Install Python Scripts from ../script directory
set(PYTHON_SCRIPTS
    port_forwarding_counter.py
    port_forwarding_test_16KB.py
    usb-ip.py
)

set(PYTHON_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../script")

foreach(SCRIPT ${PYTHON_SCRIPTS})
    set(SCRIPT_PATH "${PYTHON_SCRIPTS_DIR}/${SCRIPT}")
    if(EXISTS "${SCRIPT_PATH}")
        if(NOT DEFINED FB_OUTPUT_CONFIG)
            if(CMAKE_BUILD_TYPE STREQUAL "Release")
                set(FB_OUTPUT_CONFIG "Release")
            else()
                set(FB_OUTPUT_CONFIG "Debug")
            endif()
        endif()
        
        configure_file(
            "${SCRIPT_PATH}"
            "${CMAKE_BINARY_DIR}/${FB_OUTPUT_CONFIG}/${SCRIPT}"
            COPYONLY
        )
        
        configure_file(
            "${SCRIPT_PATH}"
            "${CMAKE_BINARY_DIR}/${SCRIPT}"
            COPYONLY
        )
        
        install(
            FILES "${SCRIPT_PATH}"
            DESTINATION bin
        )
    else()
        message(WARNING "${SCRIPT} not found in script directory: ${PYTHON_SCRIPTS_DIR}")
    endif()
endforeach()

install(TARGETS usbipd
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION bin
)
