cmake_minimum_required(VERSION 3.20)
project(WetrixPC C CXX)

# The PC port: the app around the recompiled game -- window, input, renderer
# selection (f3d, Wetter's soft and hard), the debug console. It builds
# natively on Windows and Linux; ../r36s builds it for the R36S.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# The game: `wetrix_game` (recompiled code, audio microcode, game shims) and the
# runtime it runs on, from the recompilation next to wetrix-n64-ports/. It also sets
# WETRIX_RUNTIME_DIR and WETRIX_IMAGE.
set(WETRIX_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/wetrix-n64-recompilation"
    CACHE PATH "The Wetrix recompilation (wetrix-n64-recompilation)")
if(NOT EXISTS "${WETRIX_RECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "No recompilation in ${WETRIX_RECOMP_DIR}.\n"
        "Use the build scripts (they run tools/fetch_deps.py), or pass -DWETRIX_RECOMP_DIR=<folder>.")
endif()
add_subdirectory("${WETRIX_RECOMP_DIR}" recomp)

# ---------------------------------------------------------------------------
# SDL2: the system's (MSYS2's mingw-w64-ucrt-x86_64-SDL2 on Windows, the builder
# image's on aarch64), found through pkg-config.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(wetrix
    src/main.cpp
    src/crash_handler.cpp
    src/stall_probe.cpp
    src/render_select.cpp
    src/debug_server.cpp
    src/sampler.cpp
    src/soft_context.cpp
    src/wetrix_run.cpp)
target_include_directories(wetrix PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Wetter: the soft and hard renderers (--renderer soft / hard). src/soft_context.cpp
# and HardContext in src/render_select.cpp adapt them to ultramodern. Next to
# wetrix-n64-ports/ by default.
set(WETTER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/wetter" CACHE PATH "Wetter renderer library")
if(NOT EXISTS "${WETTER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "No Wetter in ${WETTER_DIR}.\n"
        "Use the build scripts (they run tools/fetch_deps.py), or pass -DWETTER_DIR=<folder>.")
endif()
add_subdirectory("${WETTER_DIR}" "${CMAKE_BINARY_DIR}/wetter")
target_link_libraries(wetrix PRIVATE wetter)

# ---------------------------------------------------------------------------
# f3d, the Fast3D-lineage renderer (OpenGL ES 3.0 / GL 3.3 core). It is a
# plain static library, so the same sources build for the R36S. Selected at runtime with WETRIX_RENDERER / --renderer; see
# include/render_select.h.
# ---------------------------------------------------------------------------
add_library(f3d STATIC
    fast3d/f3d_core.cpp
    fast3d/f3d_gl.cpp
    fast3d/f3d_disasm.cpp
    fast3d/gfx_cc.cpp
    fast3d/glad/glad.c)
target_include_directories(f3d PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/fast3d"
    PRIVATE
    "${WETRIX_RUNTIME_DIR}/thirdparty/xxHash"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/stb")
target_include_directories(f3d PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(wetrix PRIVATE f3d)

target_include_directories(wetrix PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/stb"
    ${SDL2_INCLUDE_DIRS})

target_link_libraries(wetrix PRIVATE wetrix_game ${SDL2_LIBRARIES})
target_link_directories(wetrix PRIVATE ${SDL2_LIBRARY_DIRS})
if(NOT WIN32)
    target_link_libraries(wetrix PRIVATE pthread dl)
endif()

# dbghelp supplies StackWalk64, which is how the crash handler unwinds the stack
# without a debugger present. See src/crash_handler.cpp.
if(WIN32)
    target_link_libraries(wetrix PRIVATE dbghelp ws2_32)
endif()

# ---------------------------------------------------------------------------
# Make the build directory runnable by double-click.
#
# The exe imports the MinGW C++ runtime and SDL2, none of which are on a normal
# Windows PATH, so a double-click otherwise dies instantly with exit code 127 and
# no message. Copying them beside the exe is what lets it be launched from
# Explorer; main.cpp pins the working directory and librecomp's config path to
# the same folder, so the game image copied in below is found there too.
# ---------------------------------------------------------------------------
if(WIN32)
    get_filename_component(WETRIX_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET wetrix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WETRIX_MINGW_BIN_DIR}/SDL2.dll"
            "${WETRIX_MINGW_BIN_DIR}/libgcc_s_seh-1.dll"
            "${WETRIX_MINGW_BIN_DIR}/libstdc++-6.dll"
            "${WETRIX_MINGW_BIN_DIR}/libwinpthread-1.dll"
            "$<TARGET_FILE_DIR:wetrix>"
        COMMENT "Copying runtime DLLs next to wetrix.exe so it can be launched directly")
endif()

# Put the game image beside the exe, on every platform: that is where librecomp
# looks for it (config_path / GameEntry::stored_filename()), and main.cpp pins
# config_path to the exe's folder so a double-click works. A copy, not a
# derivation -- the image already exists in the recompilation, and if it were absent
# the exe would have to provision itself on first launch from a tree with no ROM
# in it, which is the slow path rather than the normal one.
add_custom_command(TARGET wetrix POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WETRIX_IMAGE}"
        "$<TARGET_FILE_DIR:wetrix>/wetrix.z64"
    COMMENT "Copying wetrix.z64 (the game image) next to wetrix.exe")

if(NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set_target_properties(wetrix PROPERTIES OUTPUT_NAME "wetrix.aarch64")
endif()
