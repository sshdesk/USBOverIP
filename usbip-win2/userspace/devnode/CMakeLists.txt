cmake_minimum_required(VERSION 3.20)
project(devnode LANGUAGES CXX)

add_executable(devnode)

target_sources(devnode
    PRIVATE
        devnode.rc
        main.cpp
)

set_target_properties(devnode PROPERTIES
    COMPILE_WARNING_AS_ERROR ON
)

target_compile_features(devnode
    PRIVATE
        cxx_std_23
        c_std_17
)

target_include_directories(devnode
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_compile_definitions(devnode
    PRIVATE
        _CONSOLE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        "$<$<CONFIG:Release>:NDEBUG>"
)

target_link_libraries(devnode
    PRIVATE
        shlwapi
        Setupapi
        Newdev
        libusbip
)

target_compile_options(devnode
    PRIVATE
        /W4
)

target_link_options(devnode
    PRIVATE
        /PDBALTPATH:%_PDB%
)

