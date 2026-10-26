# Game Title (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The hooks use fixed addresses in this exact image, so reject a different dump before code generation.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/supported_xbe.txt)
    file(STRINGS config/supported_xbe.txt GAME_SHA1 REGEX "^sha1=")
    string(REPLACE "sha1=" "" GAME_SHA1 "${GAME_SHA1}")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe")
        message(FATAL_ERROR "Put your extracted game in assets/ (missing default.xbe); see DEPENDENCIES.md")
    endif()
    file(SHA1 "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe" ACTUAL_GAME_SHA1)
    if(NOT ACTUAL_GAME_SHA1 STREQUAL GAME_SHA1)
        message(FATAL_ERROR "assets/default.xbe is not the supported build: ${ACTUAL_GAME_SHA1}; expected ${GAME_SHA1}")
    endif()
endif()

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(MC3RemixXBE default.xbe game.toml ${GAME_PARTITIONS} game)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    set(HEADLIGHT_ANCHORS "${CMAKE_CURRENT_SOURCE_DIR}/src/game/headlight_anchors.h")
    add_custom_command(
        OUTPUT "${HEADLIGHT_ANCHORS}"
        COMMAND "${Python3_EXECUTABLE}" tools/generate_headlight_anchors.py assets/assets_x.dat_2
        DEPENDS tools/generate_headlight_anchors.py assets/assets_x.dat_2
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Reading headlight anchors from your game files"
        VERBATIM)
    add_custom_target(mc3_headlight_anchors DEPENDS "${HEADLIGHT_ANCHORS}")
    add_dependencies(MC3RemixXBE mc3_headlight_anchors)
    if(NOT XBE_HEADLESS)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/assets/mc3_motion_blur_override.bin"
            COMMAND "${Python3_EXECUTABLE}" tools/generate_blur_override.py assets/assets_x.dat
            DEPENDS tools/generate_blur_override.py tools/generate_headlight_anchors.py assets/assets_x.dat
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
        add_custom_target(mc3_blur_tune DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/mc3_motion_blur_override.bin")
        add_dependencies(MC3RemixXBE mc3_blur_tune)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/assets/mc3_road_override.bin"
            COMMAND "${Python3_EXECUTABLE}" tools/generate_road_override.py assets/assets_x.dat
            DEPENDS tools/generate_road_override.py tools/generate_headlight_anchors.py assets/assets_x.dat
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
        add_custom_target(mc3_road_tune DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/mc3_road_override.bin")
        add_dependencies(MC3RemixXBE mc3_road_tune)
        set(GLARE_SIGNATURES "${CMAKE_CURRENT_BINARY_DIR}/mc3_light_glare.bin")
        add_custom_command(OUTPUT "${GLARE_SIGNATURES}"
            COMMAND "${Python3_EXECUTABLE}" tools/generate_light_glare.py assets/assets_x.dat "${GLARE_SIGNATURES}"
            DEPENDS tools/generate_light_glare.py tools/generate_headlight_anchors.py assets/assets_x.dat
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
        add_custom_target(mc3_light_glare DEPENDS "${GLARE_SIGNATURES}")
        add_dependencies(MC3RemixXBE mc3_light_glare)
    endif()
endif()
