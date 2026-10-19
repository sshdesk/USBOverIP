cmake_minimum_required(VERSION 3.20)
project(usbip LANGUAGES CXX)


add_library(usbip STATIC)

 
# but you can add it back if it contains string tables or dialogs needed by the lib)
target_sources(usbip
    PRIVATE
        attach.cpp
        ctrl_c_guard.cpp
        detach.cpp
        list.cpp
        port.cpp
        strings.cpp
        usbip.cpp
)

set_target_properties(usbip PROPERTIES
    COMPILE_WARNING_AS_ERROR ON
)


target_compile_features(usbip
    PUBLIC
        cxx_std_23
    PRIVATE
        c_std_17
)



target_include_directories(usbip
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
    PRIVATE
        "${CMAKE_SOURCE_DIR}/usbip/include"
)


target_compile_definitions(usbip
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    PRIVATE
        "$<$<CONFIG:Release>:NDEBUG>"
)


target_link_libraries(usbip
    PUBLIC
        ws2_32 libusbip
)

target_compile_options(usbip
    PRIVATE
        /W4
)
