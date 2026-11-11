# *********************************************************************
# Zane — a high-performance JavaScript engine
#
# Top-level CMake build. This file replaces the old pipeline:
#   - tools/setup.py  -> missing dependencies are downloaded & extracted
#                        into deps/ automatically (ZANE_DOWNLOAD_DEPS)
#   - build.ps1       -> all compile/link logic (see the sections below)
#
# Primary target: Windows / MSVC (see `build.ps1` wrapper for the exact
# configure/build commands). Generated executable: <build>/bin/zane.exe
# *********************************************************************

cmake_minimum_required(VERSION 3.24)
project(Zane VERSION 0.0.1 LANGUAGES C CXX)

include(ExternalProject)

# ---------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------
option(ZANE_DOWNLOAD_DEPS
       "Automatically download missing dependencies into deps/ (replaces tools/setup.py)"
       ON)
option(ZANE_CHECK_STYLE
       "Run tools/check_style.py before every zane build (was build.ps1 Step 0)"
       ON)

set(ZANE_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps")

# zlib / brotli / zstd are compiled from source in-tree; keep them static
set(BUILD_SHARED_LIBS OFF)
# /MT for every target in every configuration (matches the old build.ps1)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# ---------------------------------------------------------------------
# Dependency acquisition (ex-tools/setup.py)
#
# For each dependency: if deps/<folder> already exists it is reused;
# otherwise the release archive is downloaded and extracted into deps/
# (the same URLs/versions the old setup.py used). Note src code includes
# deps headers through relative paths, so downloads must land in the
# source tree, not in the build tree.
# ---------------------------------------------------------------------
function(zane_ensure_dependency dep_name dep_folder dep_url)
  set(dep_path "${ZANE_DEPS_DIR}/${dep_folder}")
  if(EXISTS "${dep_path}")
    message(STATUS "[Zane] deps/${dep_folder} - present, skipping download")
    return()
  endif()

  if(NOT ZANE_DOWNLOAD_DEPS)
    message(FATAL_ERROR "[Zane] deps/${dep_folder} is missing and ZANE_DOWNLOAD_DEPS=OFF. "
                        "Enable the option, or place the dependency at ${dep_path}.")
  endif()

  message(STATUS "[Zane] deps/${dep_folder} missing - downloading ${dep_url} ...")

  if(dep_url MATCHES "[.]tar[.]gz$")
    set(archive_name "${dep_name}.tar.gz")
  else()
    set(archive_name "${dep_name}.zip")
  endif()
  set(archive_file "${CMAKE_BINARY_DIR}/deps-download/${archive_name}")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/deps-download")

  file(DOWNLOAD "${dep_url}" "${archive_file}"
       STATUS download_status SHOW_PROGRESS TLS_VERIFY ON)
  list(GET download_status 0 download_code)
  if(NOT download_code EQUAL 0)
    list(GET download_status 1 download_error)
    message(FATAL_ERROR "[Zane] Failed to download ${dep_url}: ${download_error}")
  endif()

  file(ARCHIVE_EXTRACT INPUT "${archive_file}" DESTINATION "${ZANE_DEPS_DIR}")
  file(REMOVE "${archive_file}")

  if(EXISTS "${dep_path}")
    # The archive was already laid out as deps/<dep_folder>
    message(STATUS "[Zane] deps/${dep_folder} ready.")
    return()
  endif()

  # The archives extract a versioned top folder (e.g. zlib-1.3.2). Match by
  # name prefix: this also heals leftovers from earlier failed runs instead of
  # erroring out on them, and ignores *_install directories.
  file(GLOB extracted "${ZANE_DEPS_DIR}/${dep_folder}*")
  set(candidates)
  foreach(entry IN LISTS extracted)
    get_filename_component(entry_name "${entry}" NAME)
    if(IS_DIRECTORY "${entry}"
       AND NOT entry_name STREQUAL dep_folder
       AND NOT entry_name MATCHES "^${dep_folder}_"
       AND NOT entry_name MATCHES "^[.]")
      list(APPEND candidates "${entry}")
    endif()
  endforeach()

  if(candidates)
    list(GET candidates 0 extracted_dir)
    message(STATUS "[Zane] Renaming ${extracted_dir} -> ${dep_path}")
    file(RENAME "${extracted_dir}" "${dep_path}")
    message(STATUS "[Zane] deps/${dep_folder} ready.")
  else()
    file(GLOB deps_entries "${ZANE_DEPS_DIR}/*")
    list(JOIN deps_entries "  " deps_entries_str)
    message(FATAL_ERROR "[Zane] Extraction of ${dep_url} produced no usable folder for "
                        "deps/${dep_folder}. Current contents of deps/: ${deps_entries_str}")
  endif()
endfunction()

if(WIN32)
  zane_ensure_dependency(zlib    zlib    "https://github.com/madler/zlib/releases/download/v1.3.2/zlib132.zip")
  zane_ensure_dependency(trantor trantor "https://github.com/an-tao/trantor/archive/refs/tags/v1.5.26.zip")
  zane_ensure_dependency(brotli  brotli  "https://github.com/google/brotli/archive/refs/tags/v1.2.0.zip")
  zane_ensure_dependency(zstd    zstd    "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz")
  zane_ensure_dependency(drogon  drogon  "https://codeload.github.com/drogonframework/drogon/zip/refs/tags/v1.9.13")
  zane_ensure_dependency(jsoncpp jsoncpp "https://codeload.github.com/open-source-parsers/jsoncpp/zip/refs/tags/1.9.6")
endif()

# ---------------------------------------------------------------------
# In-tree libraries
# ---------------------------------------------------------------------

# All static archives land in <build>/lib so paths stay deterministic
# (drogon's external build below receives an absolute ZLIB_LIBRARY path).
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${_cfg}" _CFG)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/lib")
endforeach()

# --- zlib (target: zlibstatic) ---
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${ZANE_DEPS_DIR}/zlib ${CMAKE_BINARY_DIR}/deps-in-tree/zlib)
# zlibstatic is named zs.lib on Windows; drop the debug postfix so that the
# same file name is used for every configuration (drogon needs one path).
set_target_properties(zlibstatic PROPERTIES DEBUG_POSTFIX "")

# --- brotli (targets: brotlicommon, brotlienc, brotlidec) ---
set(BROTLI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(${ZANE_DEPS_DIR}/brotli ${CMAKE_BINARY_DIR}/deps-in-tree/brotli)

# --- zstd (custom static target; mirrors the source list of the old build.ps1,
#         including the Zane-specific zstd_preSplit.c) ---
set(ZSTD_SOURCES
    ${ZANE_DEPS_DIR}/zstd/lib/common/debug.c
    ${ZANE_DEPS_DIR}/zstd/lib/common/entropy_common.c
    ${ZANE_DEPS_DIR}/zstd/lib/common/error_private.c
    ${ZANE_DEPS_DIR}/zstd/lib/common/fse_decompress.c
    ${ZANE_DEPS_DIR}/zstd/lib/common/pool.c
    ${ZANE_DEPS_DIR}/zstd/lib/common/threading.c
    ${ZANE_DEPS_DIR}/zstd/lib/common/xxhash.c
    ${ZANE_DEPS_DIR}/zstd/lib/common/zstd_common.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/fse_compress.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/hist.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/huf_compress.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_compress.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_compress_literals.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_compress_sequences.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_compress_superblock.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_double_fast.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_fast.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_lazy.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_ldm.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_opt.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstdmt_compress.c
    ${ZANE_DEPS_DIR}/zstd/lib/compress/zstd_preSplit.c
    ${ZANE_DEPS_DIR}/zstd/lib/decompress/huf_decompress.c
    ${ZANE_DEPS_DIR}/zstd/lib/decompress/zstd_ddict.c
    ${ZANE_DEPS_DIR}/zstd/lib/decompress/zstd_decompress.c
    ${ZANE_DEPS_DIR}/zstd/lib/decompress/zstd_decompress_block.c)
add_library(zstd_static STATIC ${ZSTD_SOURCES})
target_include_directories(zstd_static PRIVATE
    ${ZANE_DEPS_DIR}/zstd/lib
    ${ZANE_DEPS_DIR}/zstd/lib/common
    ${ZANE_DEPS_DIR}/zstd/lib/compress
    ${ZANE_DEPS_DIR}/zstd/lib/decompress)

# ---------------------------------------------------------------------
# trantor / jsoncpp / drogon (install-based, ex-build.ps1 steps 3.5-3.7)
#
# If deps/<name>_install already exists, the installed library is used
# directly; otherwise ExternalProject builds & installs it with the exact
# cmake arguments the old build.ps1 used.
# ---------------------------------------------------------------------

# Forward CMAKE_BUILD_TYPE to external projects for single-config generators
set(ZANE_EP_BUILD_TYPE_ARGS)
if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
  list(APPEND ZANE_EP_BUILD_TYPE_ARGS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()

# --- trantor ---
set(ZANE_TRANTOR_LIB "${ZANE_DEPS_DIR}/trantor_install/lib/trantor.lib")
if(NOT EXISTS "${ZANE_TRANTOR_LIB}")
  message(STATUS "[Zane] trantor not installed - building via ExternalProject (first build only)")
  # The install dir is gone (e.g. deps/ deleted) but the ExternalProject stamp
  # files would skip re-install. With the NMake generator the stamps live in
  # <build>/zane_<name>_ep-prefix (resolved from the build dir, not from the
  # EP's BINARY_DIR), so wipe that instead to force a rebuild from scratch.
  file(REMOVE_RECURSE
      "${CMAKE_BINARY_DIR}/deps/trantor"
      "${CMAKE_BINARY_DIR}/zane_trantor_ep-prefix")
  ExternalProject_Add(zane_trantor_ep
      SOURCE_DIR "${ZANE_DEPS_DIR}/trantor"
      BINARY_DIR "${CMAKE_BINARY_DIR}/deps/trantor"
      CMAKE_ARGS
          "${ZANE_EP_BUILD_TYPE_ARGS}"
          "-DCMAKE_INSTALL_PREFIX=${ZANE_DEPS_DIR}/trantor_install"
          "-DBUILD_TESTING=OFF"
          "-DCMAKE_CXX_STANDARD=17"
          "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
          "-DCMAKE_C_FLAGS_RELEASE=/MT"  "-DCMAKE_CXX_FLAGS_RELEASE=/MT"
          "-DCMAKE_C_FLAGS_DEBUG=/MTd"   "-DCMAKE_CXX_FLAGS_DEBUG=/MTd"
          "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded")
endif()

# --- jsoncpp ---
set(ZANE_JSONCPP_LIB "${ZANE_DEPS_DIR}/jsoncpp_install/lib/jsoncpp.lib")
if(NOT EXISTS "${ZANE_JSONCPP_LIB}")
  message(STATUS "[Zane] jsoncpp not installed - building via ExternalProject ...")
  file(REMOVE_RECURSE
      "${CMAKE_BINARY_DIR}/deps/jsoncpp"
      "${CMAKE_BINARY_DIR}/zane_jsoncpp_ep-prefix")
  ExternalProject_Add(zane_jsoncpp_ep
      SOURCE_DIR "${ZANE_DEPS_DIR}/jsoncpp"
      BINARY_DIR "${CMAKE_BINARY_DIR}/deps/jsoncpp"
      CMAKE_ARGS
          "${ZANE_EP_BUILD_TYPE_ARGS}"
          "-DCMAKE_INSTALL_PREFIX=${ZANE_DEPS_DIR}/jsoncpp_install"
          "-DBUILD_SHARED_LIBS=OFF"
          "-DJSONCPP_WITH_TESTS=OFF"
          "-DJSONCPP_WITH_POST_BUILD_UNITTEST=OFF"
          "-DJSONCPP_WITH_PKGCONFIG_SUPPORT=OFF"
          "-DJSONCPP_STATIC_WINDOWS_RUNTIME=ON"
          "-DCMAKE_CXX_STANDARD=17"
          "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
          "-DCMAKE_C_FLAGS_RELEASE=/MT"  "-DCMAKE_CXX_FLAGS_RELEASE=/MT"
          "-DCMAKE_C_FLAGS_DEBUG=/MTd"   "-DCMAKE_CXX_FLAGS_DEBUG=/MTd"
          "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded")
endif()

# --- drogon ---
set(ZANE_DROGON_LIB "${ZANE_DEPS_DIR}/drogon_install/lib/drogon.lib")
if(NOT EXISTS "${ZANE_DROGON_LIB}")
  message(STATUS "[Zane] drogon not installed - building via ExternalProject ...")
  file(REMOVE_RECURSE
      "${CMAKE_BINARY_DIR}/deps/drogon"
      "${CMAKE_BINARY_DIR}/zane_drogon_ep-prefix")
  set(zane_drogon_deps)
  if(TARGET zane_trantor_ep)
    list(APPEND zane_drogon_deps zane_trantor_ep)
  endif()
  if(TARGET zane_jsoncpp_ep)
    list(APPEND zane_drogon_deps zane_jsoncpp_ep)
  endif()
  list(APPEND zane_drogon_deps zlibstatic)
  ExternalProject_Add(zane_drogon_ep
      SOURCE_DIR "${ZANE_DEPS_DIR}/drogon"
      BINARY_DIR "${CMAKE_BINARY_DIR}/deps/drogon"
      DEPENDS ${zane_drogon_deps}
      CMAKE_ARGS
          "${ZANE_EP_BUILD_TYPE_ARGS}"
          "-DCMAKE_INSTALL_PREFIX=${ZANE_DEPS_DIR}/drogon_install"
          "-DUSE_SUBMODULE=OFF"
          "-DBUILD_CTL=OFF"
          "-DBUILD_EXAMPLES=OFF"
          "-DBUILD_ORM=OFF"
          "-DBUILD_TESTING=OFF"
          "-DBUILD_YAML_CONFIG=OFF"
          "-DBUILD_BROTLI=OFF"
          "-DCMAKE_PREFIX_PATH=${ZANE_DEPS_DIR}/trantor_install"
          # NOTE: no semicolon-separated lists here - ExternalProject splits
          # ';' into separate command-line arguments. jsoncpp is passed via
          # CMAKE_INCLUDE_PATH/CMAKE_LIBRARY_PATH (single values) instead.
          "-DCMAKE_INCLUDE_PATH=${ZANE_DEPS_DIR}/jsoncpp_install/include"
          "-DCMAKE_LIBRARY_PATH=${ZANE_DEPS_DIR}/jsoncpp_install/lib"
          "-DCMAKE_CXX_STANDARD=17"
          "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
          "-DZLIB_INCLUDE_DIR=${ZANE_DEPS_DIR}/zlib"
          "-DZLIB_LIBRARY=${CMAKE_BINARY_DIR}/lib/zs.lib"
          "-DCMAKE_C_FLAGS_RELEASE=/MT"  "-DCMAKE_CXX_FLAGS_RELEASE=/MT"
          "-DCMAKE_C_FLAGS_DEBUG=/MTd"   "-DCMAKE_CXX_FLAGS_DEBUG=/MTd"
          "-DCMAKE_POSITION_INDEPENDENT_CODE=ON")
endif()

# Imported targets for the installed libraries (headers flow through the
# INTERFACE_INCLUDE_DIRECTORIES when zane links them). Ensure the include
# dirs exist at configure time: CMake validates them during generation, and
# ExternalProject installs the real headers there later.
file(MAKE_DIRECTORY
    "${ZANE_DEPS_DIR}/trantor_install/include"
    "${ZANE_DEPS_DIR}/jsoncpp_install/include"
    "${ZANE_DEPS_DIR}/drogon_install/include")

add_library(zane_trantor STATIC IMPORTED GLOBAL)
set_target_properties(zane_trantor PROPERTIES
    IMPORTED_LOCATION "${ZANE_TRANTOR_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZANE_DEPS_DIR}/trantor_install/include")
if(TARGET zane_trantor_ep)
  add_dependencies(zane_trantor zane_trantor_ep)
endif()

add_library(zane_jsoncpp STATIC IMPORTED GLOBAL)
set_target_properties(zane_jsoncpp PROPERTIES
    IMPORTED_LOCATION "${ZANE_JSONCPP_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZANE_DEPS_DIR}/jsoncpp_install/include")
if(TARGET zane_jsoncpp_ep)
  add_dependencies(zane_jsoncpp zane_jsoncpp_ep)
endif()

add_library(zane_drogon STATIC IMPORTED GLOBAL)
set_target_properties(zane_drogon PROPERTIES
    IMPORTED_LOCATION "${ZANE_DROGON_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZANE_DEPS_DIR}/drogon_install/include")
if(TARGET zane_drogon_ep)
  add_dependencies(zane_drogon zane_drogon_ep)
endif()

# ---------------------------------------------------------------------
# V8 — prebuilt monolith (headers + library from the v8-zane build)
# ---------------------------------------------------------------------
set(ZANE_V8_DIR "${CMAKE_CURRENT_SOURCE_DIR}/V8")
set(ZANE_V8_LIB "${ZANE_V8_DIR}/out.gn/x64.release/obj/v8_monolith.lib")
if(NOT EXISTS "${ZANE_V8_LIB}")
  message(FATAL_ERROR "[Zane] V8 monolith not found at ${ZANE_V8_LIB}. "
                      "Copy the v8-zane build output into the V8/ directory "
                      "(see https://github.com/Zane-JS/v8-zane).")
endif()
add_library(zane_v8 STATIC IMPORTED GLOBAL)
set_target_properties(zane_v8 PROPERTIES
    IMPORTED_LOCATION "${ZANE_V8_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZANE_V8_DIR}/include")

# ---------------------------------------------------------------------
# zane executable (ex-build.ps1 steps 3.8 + 4)
# ---------------------------------------------------------------------
set(ZANE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/temporal_shims.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/console.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/timer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/node/fs/fs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/node/path/path.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/node/os/os.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/node/process/process.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/node/util/util.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/node/buffer/buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/node/zlib/zlib.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/node/events/events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/node/stream/stream.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/builtin/builtin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/builtin/main_thread_dispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/builtin/server/request.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/builtin/server/response.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module/builtin/server/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/http/http_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/http/http2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/http/http3.cpp)

if(WIN32)
  list(APPEND ZANE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/asset/zane.rc)
endif()

# exe lands in <build>/bin for every configuration (must be set before the target)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${_cfg}" _CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/bin")
endforeach()

add_executable(zane ${ZANE_SOURCES})

target_include_directories(zane PRIVATE
    src
    libs/http
    "${ZANE_DEPS_DIR}/zlib"
    "${ZANE_DEPS_DIR}/brotli/c/include"
    "${ZANE_DEPS_DIR}/zstd/lib")
target_include_directories(zane SYSTEM PRIVATE "${ZANE_V8_DIR}/include")

# Same compile options as the old build.ps1 (core sources only, incl. /analyze)
target_compile_options(zane PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/std:c++23preview>
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>
    $<$<CXX_COMPILER_ID:MSVC>:/EHsc>
    $<$<CXX_COMPILER_ID:MSVC>:/GS>
    $<$<CXX_COMPILER_ID:MSVC>:/analyze>
    $<$<CXX_COMPILER_ID:MSVC>:/external:W0>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GL>)

target_compile_definitions(zane PRIVATE
    V8_COMPRESS_POINTERS
    $<$<CONFIG:Debug>:Zane_DEBUG_VERBOSE>)

if(MSVC)
  target_link_options(zane PRIVATE $<$<CONFIG:Release>:/LTCG>)
endif()

target_link_libraries(zane PRIVATE
    zane_v8
    zane_drogon
    zane_trantor
    zane_jsoncpp
    zlibstatic
    zstd_static
    brotlienc
    brotlidec
    brotlicommon
    winmm dbghelp shlwapi user32 iphlpapi
    advapi32 shell32 ole32 uuid rpcrt4 ntdll userenv
    ws2_32 crypt32)

# V8 needs its ICU data file next to the executable
add_custom_command(TARGET zane POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZANE_V8_DIR}/out.gn/x64.release/icudtl.dat"
            "$<TARGET_FILE_DIR:zane>")

# ---------------------------------------------------------------------
# Python-based dev tools (style check runs as part of the default build,
# matching build.ps1 Step 0; shim regeneration stays an explicit target)
# ---------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_FOUND AND ZANE_CHECK_STYLE)
  add_custom_command(TARGET zane PRE_BUILD
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_style.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/src"
      COMMENT "[Zane] Checking coding style (tools/check_style.py)")
elseif(NOT Python3_FOUND)
  message(WARNING "[Zane] Python3 not found - coding-style check and the "
                  "update-shims target are disabled.")
endif()

if(Python3_FOUND)
  add_custom_target(update-shims
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_shims.py"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Regenerate src/temporal_shims.cpp from the V8 monolith symbols")
endif()

# ---------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------
message(STATUS "")
message(STATUS "[Zane] Build configuration summary:")
message(STATUS "  Dependencies dir : ${ZANE_DEPS_DIR}")
message(STATUS "  V8 monolith      : ${ZANE_V8_LIB}")
message(STATUS "  Output           : ${CMAKE_BINARY_DIR}/bin/zane.exe")
message(STATUS "")