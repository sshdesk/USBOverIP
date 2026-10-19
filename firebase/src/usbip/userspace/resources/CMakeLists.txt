cmake_minimum_required(VERSION 3.20)
project(resources)

add_library(resources SHARED)

# This ensures that both Debug and Release configurations dump directly 
# into the top-level build tree root (e.g., build-cache-windows_debug/Debug/)
set_target_properties(resources PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    # Define paths for the input message file and the generated output targets
    set(MC_INPUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/messages.mc")
    set(MC_OUTPUT_BIN "${CMAKE_CURRENT_BINARY_DIR}/MSG00409.bin")
    set(MC_OUTPUT_RC  "${CMAKE_CURRENT_BINARY_DIR}/messages.rc")
    set(MC_OUTPUT_H   "${CMAKE_CURRENT_BINARY_DIR}/messages.h")

    # Custom command to invoke mc.exe before compilation begins
    add_custom_command(
        OUTPUT "${MC_OUTPUT_BIN}" "${MC_OUTPUT_RC}" "${MC_OUTPUT_H}"
        COMMAND mc.exe -r "${CMAKE_CURRENT_BINARY_DIR}" -h "${CMAKE_CURRENT_BINARY_DIR}" "${MC_INPUT_FILE}"
        DEPENDS "${MC_INPUT_FILE}"
        COMMENT "Compiling message file: ${MC_INPUT_FILE}"
    )

    # Let the Resource Compiler find the generated .bin file inside the binary directory
    target_compile_options(resources PRIVATE "$<$<COMPILE_LANGUAGE:RC>:/i${CMAKE_CURRENT_BINARY_DIR}>")
    
    # Direct the resource definition file to point to the freshly generated version if required
    set(RESOURCE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources.rc")
else()
    set(RESOURCE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources.rc")
endif()

target_sources(resources
    PRIVATE
        ${RESOURCE_SOURCES}
        # Add generated targets to the dependency tree so CMake builds them sequentially
        $<$<BOOL:${MSVC}>:${MC_OUTPUT_BIN}>
)

set_target_properties(resources PROPERTIES
    COMPILE_WARNING_AS_ERROR ON
)

target_compile_features(resources
    PRIVATE
        cxx_std_23
        c_std_17
)

target_compile_definitions(resources
    PRIVATE
        RESOURCES_EXPORTS
        _WINDOWS
        _USRDLL
        UNICODE
        _UNICODE
        "$<$<CONFIG:Debug>:_DEBUG>"
        "$<$<CONFIG:Release>:NDEBUG>"
)

if(MSVC)
    target_compile_options(resources
        PRIVATE
            /W4
            /Zc:__cplusplus
    )
else()
    target_compile_options(resources
        PRIVATE
            /W4
    )
endif()

target_link_options(resources
    PRIVATE
        /PDBALTPATH:%_PDB%
        $<$<CXX_COMPILER_ID:MSVC>:/NOENTRY>  
)
