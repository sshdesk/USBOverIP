cmake_minimum_required(VERSION 3.20)
project(usbip LANGUAGES CXX)

add_executable(usbip)

target_sources(usbip
    PRIVATE
        attach.cpp
        ctrl_c_guard.cpp
        detach.cpp
        list.cpp
        port.cpp
        strings.cpp
        usbip.cpp
        usbip.rc
)

set_target_properties(usbip PROPERTIES
    COMPILE_WARNING_AS_ERROR ON
)

target_compile_features(usbip
    PRIVATE
        cxx_std_23   # <-- Change this back to 23 to enable std::println
        c_std_17
)

target_include_directories(usbip
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_compile_definitions(usbip
    PRIVATE
        _CONSOLE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        "$<$<CONFIG:Release>:NDEBUG>"
)

target_link_libraries(usbip
    PRIVATE
        ws2_32
        libusbip
)

# Keep the target options explicitly bound right here
if(MSVC)
    target_compile_options(usbip
        PRIVATE
            /W4
            /Zc:__cplusplus   
    )
else()
    target_compile_options(usbip
        PRIVATE
            /W4
    )
endif()

target_link_options(usbip
    PRIVATE
        /PDBALTPATH:%_PDB%
)
