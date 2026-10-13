cmake_minimum_required(VERSION 3.12)
project(SDL_GUI_App CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


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


add_executable(sdl_gui main.cpp)


target_include_directories(sdl_gui PRIVATE 
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIR}
)


target_link_libraries(sdl_gui PRIVATE 
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARY}
)


if(NOT WIN32)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(sdl_gui PRIVATE SDL2::SDL2main)
    else()
        # Fallback method: Manually locate the system libSDL2main library asset if target fails
        find_library(SDL2MAIN_LIB NAMES SDL2main)
        if(SDL2MAIN_LIB)
            target_link_libraries(sdl_gui PRIVATE ${SDL2MAIN_LIB})
        endif()
    endif()
endif()

# Platform-specific build configuration tweaks (Mac App Bundles vs Windows binaries)
if(WIN32)
    set_target_properties(sdl_gui PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    set_target_properties(sdl_gui PROPERTIES MACOSX_BUNDLE TRUE)
endif()
