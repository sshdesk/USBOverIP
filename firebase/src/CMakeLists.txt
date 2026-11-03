cmake_minimum_required(VERSION 3.16)
project(usbipd VERSION 1.0 LANGUAGES CXX)

# Coordinated standard (Aligned to 17 to prevent Conan package warnings)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_ASSERTIONS")

# Find Conan Packages
find_package(OpenSSL REQUIRED)
find_package(google-webrtc REQUIRED)
find_package(spdlog REQUIRED)
find_package(Opus REQUIRED CONFIG)
find_package(ZLIB REQUIRED CONFIG)
find_package(SDL2 REQUIRED CONFIG)
find_package(SDL2_ttf REQUIRED CONFIG)
find_package(firebase-cpp-sdk REQUIRED CONFIG)

if(WIN32)
    link_libraries(dbghelp.lib)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
# -------------------------------------------------------------------------

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
    guiElectron
    usbipd/include
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
    firebase-cpp-sdk::firebase-cpp-sdk
    ws2_32 winmm secur32 crypt32 iphlpapi shlwapi user32 gdi32
    ZLIB::ZLIB 
)

add_executable(gui_usbip WIN32 gui/gui.cpp gui/main.cpp gui/logsync.cpp  usbip_commands.cpp usbipd_commands.cpp
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

target_include_directories(gui_usbip PRIVATE
    usbipd/include  gui  ./
    usbip/include/usbip  usbip/userspace/usbip usbip/userspace
)

if(WIN32)
    target_compile_definitions(gui_usbip PRIVATE WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_options(gui_usbip PRIVATE "/IGNORE:4099")
else()
    target_compile_definitions(gui_usbip PRIVATE WEBRTC_POSIX)
endif()


target_link_libraries(gui_usbip PRIVATE 
    google-webrtc::google-webrtc 
    openssl::openssl 
    Opus::opus 
    spdlog::spdlog 
    usbip
    firebase-cpp-sdk::firebase-cpp-sdk
    ws2_32 winmm secur32 crypt32 iphlpapi shlwapi user32 gdi32
    ZLIB::ZLIB 
    SDL2::SDL2
    SDL2::SDL2main
    sdl_ttf::sdl_ttf
)

add_subdirectory(usbip)

set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Local install prefix" FORCE)

# Copy assets to build directory
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/assets")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/gui/assets" DESTINATION "${CMAKE_BINARY_DIR}")
else()
    message(WARNING "gui/assets folder not found in source directory: ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/google-services.json")
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(FB_OUTPUT_CONFIG "Release")
    else()
        set(FB_OUTPUT_CONFIG "Debug")
    endif()
    
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/google-services.json"
        "${CMAKE_BINARY_DIR}/${FB_OUTPUT_CONFIG}/google-services.json"
        COPYONLY
    )
    
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

# -------------------------------------------------------------------------
# Shared Workspace Bin Folder & WiX Packaging Configuration
# -------------------------------------------------------------------------
set(WORKSPACE_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin")

# --- 1. guiElectron Staging Commands (Shared bin) ---
add_custom_command(TARGET guiElectron POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WORKSPACE_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:guiElectron>"
        "${WORKSPACE_BIN_DIR}/guiElectron.exe"
    COMMENT "Copying guiElectron.exe to ${WORKSPACE_BIN_DIR}"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/google-services.json")
    add_custom_command(TARGET guiElectron POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/google-services.json"
            "${WORKSPACE_BIN_DIR}/google-services.json"
        COMMENT "Copying google-services.json to ${WORKSPACE_BIN_DIR}"
    )
endif()

add_custom_command(TARGET guiElectron POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:devnode>"
        "${WORKSPACE_BIN_DIR}/devnode.exe"
    COMMENT "Copying devnode.exe to ${WORKSPACE_BIN_DIR}"
)
add_dependencies(guiElectron devnode)


# --- 2. gui_usbip Staging Commands (Shared bin) ---
add_custom_command(TARGET gui_usbip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WORKSPACE_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:gui_usbip>"
        "${WORKSPACE_BIN_DIR}/gui_usbip.exe"
    COMMENT "Copying gui_usbip.exe to ${WORKSPACE_BIN_DIR}"
)



add_custom_command(TARGET gui_usbip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/usbip/drivers"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:devnode>"
        "${CMAKE_CURRENT_SOURCE_DIR}/usbip/drivers/devnode.exe"
    COMMENT "Copying devnode.exe to usbip/drivers..."
)
add_dependencies(gui_usbip devnode)

# Copy assets directory to ./bin/assets
add_custom_command(TARGET gui_usbip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/assets"
        "${WORKSPACE_BIN_DIR}/assets"
    COMMENT "Copying gui/assets to ${WORKSPACE_BIN_DIR}/assets"
)


# --- 3. WiX MSI Builders ---
find_program(DOTNET dotnet)

if(DOTNET)
    message(STATUS "Found dotnet CLI. Setting up WiX .NET packaging targets.")

    # Target 1: USBIPElectron.msi (uses guiElectron.exe)
    set(WIX_SOURCE_ELECTRON "${CMAKE_CURRENT_SOURCE_DIR}/installer.wxs")
    set(WIX_OUTPUT_MSI_ELECTRON "${WORKSPACE_BIN_DIR}/USBIPElectron.msi")

    add_custom_command(
        OUTPUT ${WIX_OUTPUT_MSI_ELECTRON}
        COMMAND ${DOTNET} wix build "${WIX_SOURCE_ELECTRON}" -arch x64 -b "${CMAKE_CURRENT_SOURCE_DIR}" -b "${WORKSPACE_BIN_DIR}" -o "${WIX_OUTPUT_MSI_ELECTRON}"
        DEPENDS ${WIX_SOURCE_ELECTRON} guiElectron devnode
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building USBIPElectron.msi..."
    )

    # Target 2: USBIPGui.msi (uses gui_usbip.exe)
    set(WIX_SOURCE_GUI "${CMAKE_CURRENT_SOURCE_DIR}/gui_installer.wxs")
    set(WIX_OUTPUT_MSI_GUI "${WORKSPACE_BIN_DIR}/USBIPGui.msi")

    add_custom_command(
        OUTPUT ${WIX_OUTPUT_MSI_GUI}
        COMMAND ${DOTNET} wix build "${WIX_SOURCE_GUI}" -arch x64 -b "${CMAKE_CURRENT_SOURCE_DIR}" -b "${WORKSPACE_BIN_DIR}" -o "${WIX_OUTPUT_MSI_GUI}"
        DEPENDS ${WIX_SOURCE_GUI} gui_usbip devnode
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building USBIPGui.msi..."
    )

    # Combined master target to generate both MSIs into /bin
    add_custom_target(package_all_installers ALL DEPENDS ${WIX_OUTPUT_MSI_ELECTRON} ${WIX_OUTPUT_MSI_GUI})
else()
    message(WARNING "dotnet CLI not found. MSI installer targets will not be available.")
endif()
