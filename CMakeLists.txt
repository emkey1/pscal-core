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
