# pscal-core — the shared PSCAL runtime (VM, bytecode compiler, AST, lexer,
# symbol table, builtin backend). Builds standalone; front ends consume it.
#
# Minimal config (SDL / curl / sqlite OFF). The optional graphics/network/db
# features are added behind options as the extraction matures.
cmake_minimum_required(VERSION 3.18)
project(pscal_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall)
if(UNIX AND NOT APPLE)
    add_compile_definitions(_GNU_SOURCE _POSIX_C_SOURCE=200809L)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    add_compile_options(-Wno-nullability-completeness)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # PSCAL_VALUE_FIELD (core/types.h) keeps a `const Value` _Generic branch
    # for compilers that do not drop qualifiers during lvalue conversion;
    # clang does drop them and warns the branch is unreachable.
    add_compile_options(-Wno-unreachable-code-generic-assoc)
endif()

option(PSCAL_SDL "Enable the SDL graphics/audio backend (sdl/gl/audio + 3D)" OFF)

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

find_package(Threads REQUIRED)

# Generate pscal_paths.h (install-path fallbacks).
set(PSCAL_INSTALL_ROOT "/usr/local/pscal" CACHE PATH "Default PSCAL install root")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file("${SRC}/core/pscal_paths.h.in" "${CMAKE_BINARY_DIR}/generated/pscal_paths.h" @ONLY)

include_directories(
    "${CMAKE_BINARY_DIR}/generated"
    "${SRC}"
    "${SRC}/third_party/yyjson"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/nextvi"
)

add_compile_definitions(
    PROGRAM_VERSION="0.0.0"
    PSCAL_GIT_TAG="untagged"
    ENABLE_EXT_BUILTIN_MATH
    ENABLE_EXT_BUILTIN_STRINGS
    ENABLE_EXT_BUILTIN_SYSTEM
    ENABLE_EXT_BUILTIN_USER
    ENABLE_EXT_BUILTIN_YYJSON
    ENABLE_EXT_BUILTIN_3D
    ENABLE_EXT_BUILTIN_GRAPHICS
)

set(PSCAL_CORE_SOURCES
    ${SRC}/core/globals.c ${SRC}/core/type_registry.c ${SRC}/core/types.c
    ${SRC}/core/utils.c ${SRC}/core/list.c ${SRC}/core/preproc.c ${SRC}/core/version.c
    ${SRC}/ast/ast.c ${SRC}/ast/closure_registry.c
    ${SRC}/symbol/symbol.c
    ${SRC}/lexer/lexer.c
    ${SRC}/backend_ast/builtin.c ${SRC}/backend_ast/builtin_network_api_stub.c
    ${SRC}/backend_ast/shell_runtime_stub.c ${SRC}/backend_ast/thread_wrappers.c
    ${SRC}/compiler/bytecode.c ${SRC}/compiler/compiler.c
    ${SRC}/core/cache.c
    ${SRC}/common/runtime_params.c ${SRC}/common/runtime_tty.c ${SRC}/common/runtime_clipboard.c
    ${SRC}/common/env_snapshot.c ${SRC}/common/pscal_hosts.c ${SRC}/common/path_truncate.c
    ${SRC}/common/path_virtualization.c ${SRC}/common/pscal_terminal_host.c
    ${SRC}/common/frontend_kind.c ${SRC}/common/runtime_paths.c ${SRC}/common/pascal_state.c
    ${SRC}/core/diagmap.c
    ${SRC}/vm/vm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/noise/noise.c
    ${SRC}/runtime/terrain/terrain_generator.c ${SRC}/runtime/shaders/shader_common.c
    ${SRC}/runtime/shaders/terrain/terrain_shader.c ${SRC}/runtime/shaders/sky/sky_dome.c
    ${SRC}/runtime/shaders/sky/cloud_layer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/nextvi/vi.c
    ${SRC}/ext_builtins/register.c ${SRC}/ext_builtins/registry.c
    ${SRC}/ext_builtins/query_builtin.c ${SRC}/ext_builtins/dump.c
    ${SRC}/ext_builtins/math/chudnovsky.c ${SRC}/ext_builtins/math/factorial.c
    ${SRC}/ext_builtins/math/fibonacci.c ${SRC}/ext_builtins/math/mandelbrot.c
    ${SRC}/ext_builtins/math/register.c
    ${SRC}/ext_builtins/strings/register.c ${SRC}/ext_builtins/strings/atoi.c
    ${SRC}/ext_builtins/strings/parse.c
    ${SRC}/ext_builtins/system/fileexists.c ${SRC}/ext_builtins/system/getcurrentdir.c
    ${SRC}/ext_builtins/system/getpid.c ${SRC}/ext_builtins/system/realtimeclock.c
    ${SRC}/ext_builtins/system/swap.c ${SRC}/ext_builtins/system/register.c
    ${SRC}/ext_builtins/yyjson/yyjson_builtins.c ${SRC}/ext_builtins/yyjson/register.c
    ${SRC}/ext_builtins/user/register.c ${SRC}/ext_builtins/user/landscape.c
    ${SRC}/ext_builtins/user/sierpinski_threads.c
    ${SRC}/ext_builtins/threed/register.c ${SRC}/ext_builtins/threed/balls3d.c
    ${SRC}/ext_builtins/graphics/register.c
    ${SRC}/third_party/yyjson/yyjson.c
)

set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/third-party/nextvi/vi.c PROPERTIES
    COMPILE_DEFINITIONS "main=nextvi_main_entry"
    COMPILE_OPTIONS "-Wno-gnu-folding-constant")

add_library(pscal_core_static STATIC ${PSCAL_CORE_SOURCES})
target_compile_definitions(pscal_core_static PRIVATE PSCAL_NO_CLI_ENTRYPOINTS)
target_link_libraries(pscal_core_static PUBLIC Threads::Threads m)

# ---- Optional SDL graphics/audio backend -------------------------------------
# Compiles the SDL-gated sources (sdl/gl/audio + the platform 3D backend) and
# links SDL2 + OpenGL (plus the Metal/Quartz/Foundation frameworks on Apple). The
# `SDL` define and the libraries are PUBLIC, so any consumer that links
# pscal_core_static (e.g. aether) inherits both. Mirrors the umbrella recipe.
if(PSCAL_SDL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PC_SDL2       REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(PC_SDL2_IMAGE REQUIRED IMPORTED_TARGET SDL2_image)
    pkg_check_modules(PC_SDL2_TTF   REQUIRED IMPORTED_TARGET SDL2_ttf)
    pkg_check_modules(PC_SDL2_MIXER REQUIRED IMPORTED_TARGET SDL2_mixer)
    find_package(OpenGL REQUIRED)

    set(_PSCAL_SDL_SRCS
        ${SRC}/backend_ast/sdl.c
        ${SRC}/backend_ast/sdl3d.c
        ${SRC}/backend_ast/gl.c
        ${SRC}/backend_ast/audio.c
        ${SRC}/common/smallclue_applets_stub.c)
    if(APPLE)
        enable_language(OBJC)
        list(APPEND _PSCAL_SDL_SRCS
            ${SRC}/backend_ast/graphics_3d_backend_ios.c
            ${SRC}/backend_ast/graphics_3d_metal_apple.m)
    else()
        list(APPEND _PSCAL_SDL_SRCS
            ${SRC}/backend_ast/graphics_3d_backend_gl.c)
    endif()
    target_sources(pscal_core_static PRIVATE ${_PSCAL_SDL_SRCS})

    target_compile_definitions(pscal_core_static PUBLIC SDL)
    target_link_libraries(pscal_core_static PUBLIC
        PkgConfig::PC_SDL2 PkgConfig::PC_SDL2_IMAGE
        PkgConfig::PC_SDL2_TTF PkgConfig::PC_SDL2_MIXER
        OpenGL::GL)
    if(APPLE)
        target_link_libraries(pscal_core_static PUBLIC
            "-framework Metal" "-framework QuartzCore" "-framework Foundation")
    endif()
    message(STATUS "pscal-core: SDL backend ENABLED (sdl/gl/audio + 3D)")
endif()

# ---- Install ----------------------------------------------------------------
# pscal-core is the shared runtime library. Installing it makes the static lib +
# public headers available to external consumers. Guard to standalone builds so
# that front ends which FetchContent pscal-core do not drag its lib/headers/docs
# into their own `cmake --install`.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(GNUInstallDirs)
    install(TARGETS pscal_core_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY ${SRC}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pscal-core
            FILES_MATCHING PATTERN "*.h")
    install(FILES ${CMAKE_BINARY_DIR}/generated/pscal_paths.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pscal-core)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs/
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
