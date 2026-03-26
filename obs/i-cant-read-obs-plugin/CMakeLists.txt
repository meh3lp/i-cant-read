cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

function(add_spirv_embed target glsl_file symbol)
    get_filename_component(abs ${glsl_file} ABSOLUTE)
    set(spv "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.spv")
    set(embed_c "${CMAKE_CURRENT_BINARY_DIR}/${symbol}_spv.c")
    set(embed_h "${CMAKE_CURRENT_BINARY_DIR}/${symbol}_spv.h")

    # glslc: compile GLSL compute shader to SPIR-V
    add_custom_command(
        OUTPUT  ${spv}
        COMMAND ${GLSLC} -fshader-stage=compute
                         -O                          # optimise
                         --target-env=opengl         # GL SPIR-V dialect
                         ${abs} -o ${spv}
        DEPENDS ${abs}
        COMMENT "SPIR-V: ${glsl_file}"
        VERBATIM
    )

    # Embed raw SPIR-V bytes as a C uint32_t array.
    # SPIR-V words are 32-bit; we read raw bytes and let the C compiler
    # handle alignment via the uint32_t[] declaration.
    add_custom_command(
        OUTPUT  ${embed_c} ${embed_h}
        COMMAND ${CMAKE_COMMAND}
                    -DSPV=${spv}
                    -DSYMBOL=${symbol}
                    -DEMBED_C=${embed_c}
                    -DEMBED_H=${embed_h}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/embed_spirv.cmake
        DEPENDS ${spv}
        COMMENT "Embedding ${symbol}.spv"
        VERBATIM
    )

    target_sources(${target} PRIVATE ${embed_c})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endfunction()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.c
    src/settings.c
    src/settings.h
    src/settings-dialog.cpp
    src/settings-dialog.hpp
    src/dhash.c
    src/dhash.h
    src/gate-manager.c
    src/gate-manager.h
    src/stb_image_write.h
    src/gates.c
    src/gates.h
    src/gl.c)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # exposes glad/glad.h and KHR/khrplatform.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

find_program(GLSLC glslc REQUIRED
    HINTS
        /run/current-system/sw/bin
        $ENV{HOME}/.nix-profile/bin
)
add_spirv_embed(${CMAKE_PROJECT_NAME} data/shaders/gray_lap.comp spv_gray_lap)
add_spirv_embed(${CMAKE_PROJECT_NAME} data/shaders/dilate.comp   spv_dilate)
add_spirv_embed(${CMAKE_PROJECT_NAME} data/shaders/diff.comp     spv_diff)
add_spirv_embed(${CMAKE_PROJECT_NAME} data/shaders/reduce.comp   spv_reduce)
