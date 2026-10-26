# SPDX-License-Identifier: MIT
# Copyright (c) Kirikiri SDL2 Developers

cmake_minimum_required(VERSION 3.12)

if(IOS AND (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET))
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.0)
endif()

if((NOT IOS) AND (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET))
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.14)
endif()

if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
set(CMAKE_POLICY_VERSION_MINIMUM 3.12)
endif()

project(krkrsdl2)

# Disable install for subprojects
if(NOT DEFINED _DISABLE_INSTALLATION)
set(_DISABLE_INSTALLATION FALSE)
macro(install)
if (NOT _DISABLE_INSTALLATION)
_install(${ARGN})
endif()
endmacro()
endif()

if (${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.13")
cmake_policy(SET CMP0077 NEW)
endif()
option(OPTION_ENABLE_ASYNC_IMAGE_LOAD "Enable async image load. Not applicable to Emscripten build." ON)
option(OPTION_ENABLE_CANVAS "Enable Canvas interface. Requires OpenGL ES or ANGLE at runtime." OFF)
option(OPTION_ENABLE_CASEINSENSITIVITY "Enable case insensitivity. May malfunction if directories in the path are not listable." ON)
option(OPTION_ENABLE_SDL_THREADS "Enable SDL threads. May be more useful on platforms that do not have a good implementation of std::thread." OFF)
option(OPTION_ENABLE_EXTERNAL_PLUGINS "Enable external plugins. This will make Plugins.link load a plugin instead of a no-op." ON)
option(OPTION_BUILD_MACOS_BUNDLE "Build an application bundle for macOS." ON)
option(OPTION_DEBUG_PROBES "Compile in the ad-hoc stderr probes (font/audio/input diagnostics)." OFF)
option(KRKRSDL2_EMSCRIPTEN_ENABLE_JS_FALLBACK "Enable JavaScript fallback for the Emscripten build." OFF)
option(KRKRSDL2_EMSCRIPTEN_ENABLE_BIGINT "Enable BigInt support for the Emscripten build." ON)
option(KRKRSDL2_EMSCRIPTEN_ENABLE_WASM_SIMD "Enable WebAssembly SIMD for the Emscripten build." OFF)
option(KRKRSDL2_EMSCRIPTEN_ENABLE_THREADS "Enable threads for the Emscripten build." OFF)
option(KRKRSDL2_EMSCRIPTEN_ENABLE_SOURCE_MAPS "Enable threads for the Emscripten build." OFF)
set(KRKRSDL2_EMSCRIPTEN_INITIAL_MEMORY "134217728" CACHE STRING "Initial memory usage for the Emscripten build.")
option(KRKRSDL2_BUILD_ENABLE_WARNINGS "Enable warnings when building the project." OFF)
# Build as the engine module of the onscripter-en libretro core: the core's
# engine_kirikiri_cabi.cpp expects an unversioned library named
# onscripter-en_engine_krkrsdl2.<ext>, loaded by path at run time. This is OFF
# by default so a plain checkout builds upstream's standalone application
# unchanged; our libretro build turns it on explicitly
# (-DKRKRSDL2_LIBRETRO=ON). Turning it on also defines KRKRSDL2_LIBRETRO, which
# switches the SDL2 backend to the captured-audio/silent-device path the core
# needs (extern "C" control entry points, ring-buffer audio, dummy video).
option(KRKRSDL2_LIBRETRO "Build as a libretro core engine module" OFF)
if(KRKRSDL2_LIBRETRO)
# Two names, one meaning, and the split is deliberate:
#
#   KRKRSDL2_LIBRETRO  the CMake OPTION, and the marker the engine's existing
#                      sources test. Kept because the option is passed by name
#                      from outside (-DKRKRSDL2_LIBRETRO=ON, in the Makefile and
#                      in buildroot), so renaming it would churn every build
#                      recipe for no behavioural gain.
#   __LIBRETRO__       the C marker this whole tree shares. Defined here so the
#                      engine's code can test ONE name for "we are inside the
#                      vnovel core" — the same one the core, the two ONScripter
#                      engines and the sdl-libretro glue already use — instead
#                      of inventing a second vocabulary for the same fact. New
#                      code should test __LIBRETRO__.
add_compile_definitions(KRKRSDL2_LIBRETRO __LIBRETRO__)
endif()

# Compiled-in plugin shims (src/core/base/sdl2/BuiltinShims.h). ON lets
# overrides.txt name a shim as `@builtin:<name>` and have it resolved inside the
# engine instead of dlopen'ing a file, so the shim travels with the binary and
# cannot be the wrong architecture or absent. The libretro build turns it on;
# a standalone build may too, so it is not derived from KRKRSDL2_LIBRETRO.
option(KRKRSDL2_BUILTIN_SHIMS "Compile the plugin shims into the engine" OFF)
if(KRKRSDL2_BUILTIN_SHIMS)
add_compile_definitions(KRKRSDL2_BUILTIN_SHIMS)
endif()

# The development wire protocol (the text stream out, navigation intents in).
# The shim that publishes the flow/edge records must agree with the core: with
# this OFF its publish calls are local no-ops (src/shims/textstream_shim.cpp), so a
# build that defines it for the core and not here would listen on the socket and
# never emit anything. The core's Makefile passes -DVNOVEL_DEVWIRE when it builds
# with DEVWIRE=1, which CMake takes as this cache variable.
option(VNOVEL_DEVWIRE "Compile the development wire protocol into the engine" OFF)
if(VNOVEL_DEVWIRE)
add_compile_definitions(VNOVEL_DEVWIRE)
endif()

# mingw: prefix holding the static libogg/libvorbis the Windows DLL links
# (built by tools/build-mingw-vorbis.sh). Kept as a variable rather than a
# hardcoded /tmp path so a clean machine can place it anywhere.
set(KRKRSDL2_MINGW_VORBIS_PREFIX "" CACHE PATH
	"Prefix with the static libogg/libvorbis built for mingw (tools/build-mingw-vorbis.sh). Empty means <repo>/build/mingw-deps/vorbis, which is where that script installs by default.")

if((NOT DEFINED CMAKE_BUILD_TYPE) OR (NOT CMAKE_BUILD_TYPE) OR (CMAKE_BUILD_TYPE STREQUAL ""))
if(${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten")
set(CMAKE_BUILD_TYPE MinSizeRel)
else()
set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
endif()

if(NOT DEFINED KRKRSDL2_REPO_DIR)
set(KRKRSDL2_REPO_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if((OPTION_ENABLE_ASYNC_IMAGE_LOAD) AND (NOT (${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten")))
set(EVAL_ENABLE_ASYNC_IMAGE_LOAD ON)
else()
set(EVAL_ENABLE_ASYNC_IMAGE_LOAD OFF)
endif()

# At the moment, iOS does not support case insensitivity properly with our code
# due to the fact that some folders cannot be listed
if((OPTION_ENABLE_CASEINSENSITIVITY) AND (NOT IOS))
set(EVAL_ENABLE_CASEINSENSITIVITY ON)
else()
set(EVAL_ENABLE_CASEINSENSITIVITY OFF)
endif()

if((OPTION_ENABLE_SDL_THREADS) OR (VITA OR NINTENDO_SWITCH))
set(EVAL_ENABLE_SDL_THREADS ON)
else()
set(EVAL_ENABLE_SDL_THREADS OFF)
endif()

if((OPTION_ENABLE_EXTERNAL_PLUGINS) AND (NOT (VITA OR NINTENDO_SWITCH)))
set(EVAL_ENABLE_EXTERNAL_PLUGINS ON)
else()
set(EVAL_ENABLE_EXTERNAL_PLUGINS OFF)
endif()

# Ad-hoc stderr probes (font/audio/input diagnostics). Off by default.
if(OPTION_DEBUG_PROBES)
list(APPEND KRKRSDL2_DEFINITIONS TVP_DEBUG_PROBES)
endif()

if(NOT KRKRSDL2_ENABLE_PRECOMPILED_HEADERS)
if(WIN32)
set(KRKRSDL2_ENABLE_PRECOMPILED_HEADERS OFF)
else()
set(KRKRSDL2_ENABLE_PRECOMPILED_HEADERS ON)
endif()
endif()

# Don't code sign by default.
set(KRKRSDL2_IOS_CODE_SIGN_IDENTITY "" CACHE STRING "The code sign identity to use when building the IPA.")

if(KRKRSDL2_IOS_CODE_SIGN_IDENTITY STREQUAL "")
set(KRKRSDL2_IOS_CODE_SIGNING_ENABLED NO)
else()
set(KRKRSDL2_IOS_CODE_SIGNING_ENABLED YES)
endif()

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-fno-delete-null-pointer-checks KRKRSDL2_COMPILER_SUPPORTS_NO_DELETE_NULL_POINTER_CHECKS)

if(IOS)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/out/library)
set(CMAKE_XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
endif()

if(WIN32)
set(WIN32_RESOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/win32 CACHE FILEPATH "The path to the resources for Win32.")
endif()

if(APPLE AND IOS)
set(IOS_RESOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/ios CACHE FILEPATH "The path to the resources for iOS.")
endif()

if(APPLE AND NOT IOS)
set(MACOS_RESOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/macos CACHE FILEPATH "The path to the resources for macOS.")
add_compile_options(-Wno-declaration-after-statement)
endif()

if(VITA)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Kirikiri SDL2" CACHE STRING "The application name to use for the VPK.")
set(VITA_TITLEID  "KRKR00001"     CACHE STRING "The title ID to use for the VPK.")
set(VITA_VERSION  "02.00"         CACHE STRING "The version to use for the VPK.")
set(VITA_RESOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/vita CACHE FILEPATH "The path to the resources for Vita.")
endif()

if(NINTENDO_SWITCH)
set(SWITCH_APP_NAME "Kirikiri SDL2" CACHE STRING "The application name to use for the NRO.")
set(SWITCH_AUTHOR   "uyjulian"      CACHE STRING "The author to use for the NRO.")
set(SWITCH_VERSION  "v2.0.0"        CACHE STRING "The version to use for the NRO.")
set(SWITCH_RESOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/nswitch CACHE FILEPATH "The path to the resources for Switch.")
endif()

set(EMBED_DATA_PATH ${CMAKE_CURRENT_SOURCE_DIR}/data CACHE FILEPATH "The path to the data file or directory.")
get_filename_component(EMBED_DATA_PATH_BASENAME ${EMBED_DATA_PATH} NAME)

set(KRKRSDL2_SRC "")
set(KRKRSDL2_INCLUDES "")
set(KRKRSDL2COMMON_SRC "")
set(KRKRSDL2COMMON_INCLUDES "")

file(STRINGS src/config/src_list/kirikirisdl2/sources.txt KIRIKIRISDL2_SRC)
file(STRINGS src/config/src_list/kirikirisdl2/sources_sse2.txt KIRIKIRISDL2_SSE2_SRC)
file(STRINGS src/config/src_list/kirikirisdl2/sources_avx2.txt KIRIKIRISDL2_AVX2_SRC)
file(STRINGS src/config/src_list/kirikirisdl2/includes.txt KIRIKIRISDL2_INCLUDES)

list(APPEND KRKRSDL2COMMON_SRC ${KIRIKIRISDL2_SRC})
list(APPEND KRKRSDL2COMMON_SRC ${KIRIKIRISDL2_SSE2_SRC})
list(APPEND KRKRSDL2COMMON_SRC ${KIRIKIRISDL2_AVX2_SRC})
list(APPEND KRKRSDL2COMMON_INCLUDES ${KIRIKIRISDL2_INCLUDES})

set(KRKRSDL2OPUS_SRC "")
list(APPEND KRKRSDL2OPUS_SRC ${KRKRSDL2COMMON_SRC})
list(FILTER KRKRSDL2OPUS_SRC INCLUDE REGEX "^external/krkrz/external/opus")
set_source_files_properties(${KRKRSDL2OPUS_SRC} PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/external/krkrz/external/opus/silk;${CMAKE_CURRENT_SOURCE_DIR}/external/krkrz/external/opus/silk/fixed;${CMAKE_CURRENT_SOURCE_DIR}/external/krkrz/external/opus/silk/float")

list(APPEND KRKRSDL2COMMON_SRC src/support/roundevenf.c)

if(${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten")
list(APPEND KRKRSDL2COMMON_SRC src/plugins/kremscripten.cpp)
endif()

list(APPEND KRKRSDL2_DEFINITIONS
	TVP_COMPILING_KRKRSDL2
	TVP_OPUS_DECODER_IMPLEMENT
	TVP_LOG_TO_COMMANDLINE_CONSOLE
	HAVE_CONFIG_H
	FT2_BUILD_LIBRARY
	USE_ALLOCA
	TVP_TEXT_READ_ANSI_MBCS
	USE_OBSOLETE_FUNCTIONS
	HAVE_LRINT
	FLOAT_APPROX
	PNG_ARM_NEON_OPT=0
)

# Build stamp, printed by krkrsdl2_pre_init_platform so a log proves which
# module binary is loaded. Bump it on any engine change. Can be overridden from
# the command line (-DKRKRSDL2_BUILD_STAMP=...) by a packaging recipe.
if(NOT DEFINED KRKRSDL2_BUILD_STAMP)
set(KRKRSDL2_BUILD_STAMP "dev")
endif()
list(APPEND KRKRSDL2_DEFINITIONS
	ENGINE_BUILD_STAMP="${KRKRSDL2_BUILD_STAMP}"
)

if(${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten")
list(APPEND KRKRSDL2_LDFLAGS
	-sDISABLE_EXCEPTION_CATCHING=0
	-sEXIT_RUNTIME=1
	-sALLOW_MEMORY_GROWTH=1
	-sINITIAL_MEMORY=${KRKRSDL2_EMSCRIPTEN_INITIAL_MEMORY}
	-sFORCE_FILESYSTEM=1
	-sASSERTIONS=0
	-sSTACK_OVERFLOW_CHECK=0
	-sMODULARIZE=1
	-sEXPORT_NAME="instantiate_krkrsdl2_module"
	-sEXPORTED_RUNTIME_METHODS=['FS','IDBFS']
	-sUSE_SDL=2
	-lidbfs.js
	-lopenal
	--bind
	--no-heap-copy
	-fPIC
)
list(APPEND KRKRSDL2_CFLAGS
	-sDISABLE_EXCEPTION_CATCHING=0
	-sUSE_SDL=2
	-fPIC
)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
list(APPEND KRKRSDL2_LDFLAGS
	-O3
)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
list(APPEND KRKRSDL2_LDFLAGS
	-O2
)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
list(APPEND KRKRSDL2_LDFLAGS
	-Os
)
endif()
if(EVAL_ENABLE_EXTERNAL_PLUGINS)
list(APPEND KRKRSDL2_LDFLAGS
	-sMAIN_MODULE=1
)
endif()
if(${KRKRSDL2_EMSCRIPTEN_ENABLE_WASM_SIMD} EQUAL ON)
list(APPEND KRKRSDL2_CFLAGS
	-msimd128
	-msse4.2
)
endif()
if(${KRKRSDL2_EMSCRIPTEN_ENABLE_THREADS} EQUAL ON)
list(APPEND KRKRSDL2_CFLAGS
	-sUSE_PTHREADS=1
	-sPROXY_TO_PTHREAD=1
	-sOFFSCREEN_FRAMEBUFFER=1
)
endif()
if(${KRKRSDL2_EMSCRIPTEN_ENABLE_JS_FALLBACK} EQUAL ON)
list(APPEND KRKRSDL2_LDFLAGS
	-sWASM=2
)
endif()
if(${KRKRSDL2_EMSCRIPTEN_ENABLE_BIGINT} EQUAL ON)
list(APPEND KRKRSDL2_LDFLAGS
	-sWASM_BIGINT=1
)
endif()
if(${KRKRSDL2_EMSCRIPTEN_ENABLE_THREADS} EQUAL ON)
list(APPEND KRKRSDL2_LDFLAGS
	-sUSE_PTHREADS=1
	-sPROXY_TO_PTHREAD=1
	-sOFFSCREEN_FRAMEBUFFER=1
)
endif()
if(${KRKRSDL2_EMSCRIPTEN_ENABLE_SOURCE_MAPS} EQUAL ON)
list(APPEND KRKRSDL2_LDFLAGS
	-g4
	--source-map-base http://127.0.0.1:8080/
)
endif()
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
list(APPEND KRKRSDL2_LDFLAGS
	-pthread
)
list(APPEND KRKRSDL2_CFLAGS
	-pthread
)
list(APPEND KRKRSDL2_LIBS
	pthread
)
endif()

# Ogg Vorbis decoding (wuvorbis.dll equivalent) via libvorbisfile.
# Enabled only when the library is available; VorbisCodecDecoder.cpp compiles
# to nothing otherwise and TVPCreateWaveDecoder simply won't offer .ogg — which
# is what made KiriKiri BGM silent: the Windows build had no vorbis at all.
if(NOT (${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten"))
if(MINGW)
# Windows: vorbis is looked up in the prefix built by
# tools/build-mingw-vorbis.sh, not via pkg-config (the cross pkg-config does
# not see it). Direct paths, not find_path/find_library: the mingw toolchain
# sets CMAKE_FIND_ROOT_PATH_MODE_{INCLUDE,LIBRARY} ONLY, which re-roots those
# searches and would reject a prefix outside the sysroot. The libraries
# themselves are appended to the single static section in _MIN_STATIC below.
#
# An empty setting means the in-tree default, so a plain configure works and a
# reboot cannot silently disable the decoder (the old /tmp default did exactly
# that).
if(KRKRSDL2_MINGW_VORBIS_PREFIX STREQUAL "")
set(KRKRSDL2_MINGW_VORBIS_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../build/mingw-deps/vorbis")
endif()
set(_vorbis_inc "${KRKRSDL2_MINGW_VORBIS_PREFIX}/include")
if(EXISTS "${_vorbis_inc}/vorbis/vorbisfile.h"
   AND EXISTS "${KRKRSDL2_MINGW_VORBIS_PREFIX}/lib/libvorbisfile.a")
list(APPEND KRKRSDL2COMMON_INCLUDES ${_vorbis_inc})
list(APPEND KRKRSDL2_DEFINITIONS TVP_VORBIS_DECODER_IMPLEMENT)
message(STATUS "krkrsdl2: Ogg Vorbis decoder enabled (${KRKRSDL2_MINGW_VORBIS_PREFIX})")
else()
message(STATUS "krkrsdl2: Ogg Vorbis NOT available in ${KRKRSDL2_MINGW_VORBIS_PREFIX} (build it with tools/build-mingw-vorbis.sh)")
endif()
else()
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
pkg_check_modules(VORBISFILE QUIET IMPORTED_TARGET vorbisfile)
if(VORBISFILE_FOUND)
list(APPEND KRKRSDL2_LIBS
	PkgConfig::VORBISFILE
)
list(APPEND KRKRSDL2_DEFINITIONS
	TVP_VORBIS_DECODER_IMPLEMENT
)
message(STATUS "krkrsdl2: Ogg Vorbis decoder enabled (libvorbisfile ${VORBISFILE_VERSION})")
else()
message(STATUS "krkrsdl2: Ogg Vorbis decoder NOT available (no vorbisfile)")
endif()
endif()
endif()
endif()

# Font discovery via fontconfig (Linux/BSD): the system's own font database, so
# user/distribution font directories, aliases and substitutions are honoured
# without a hardcoded path list. Optional — without it the portable directory
# walk in FontDiscovery.cpp is used instead.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
pkg_check_modules(FONTCONFIG QUIET IMPORTED_TARGET fontconfig)
if(FONTCONFIG_FOUND)
list(APPEND KRKRSDL2_LIBS
	PkgConfig::FONTCONFIG
)
list(APPEND KRKRSDL2_DEFINITIONS
	KRKRSDL2_HAVE_FONTCONFIG
)
message(STATUS "krkrsdl2: font discovery via fontconfig ${FONTCONFIG_VERSION}")
else()
message(STATUS "krkrsdl2: fontconfig not found; using directory scan for fonts")
endif()
endif()

# FreeType: use the SYSTEM library, not the vendored krkrz fork.
# The vendored copy is FreeType 2.6.3 (2016), which fails to parse the game's
# Noto Sans CJK fonts (FT_Err_Invalid_Table, err=8) — they register with
# faces=0, and any path that falls back to the default font ("Noto Sans CJK
# JP") then throws (e.g. the save screen). The sources.txt list no longer
# compiles the vendored freetype; link the system one instead.
if(NOT (${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten"))
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
if(MINGW)
# Archival build: the Windows DLL must be self-contained. Linking an import
# library instead would make krkrsdl2.dll import libfreetype-6.dll (and, through
# it, zlib/bzip2/brotli), adding DLLs the package has to ship. So the whole
# non-system closure is resolved from static archives.
#
# The flags are gathered in _MIN_STATIC and appended ONCE, after the targets
# (see below). Two reasons they cannot be spread across the per-library blocks:
#  - CMake emits a target's PUBLIC link libraries more than once, which breaks
#    the -Bstatic / -Bdynamic pairing: the later copy falls inside a -Bdynamic
#    region and silently resolves the import library instead. That is exactly
#    how libstdc++-6.dll / libwinpthread-1.dll crept back into the imports.
#  - ld matches left to right, so every static archive and the compiler
#    runtime must appear after everything that references them.
# vorbis (the -L path comes from pkg-config) and the shared libs' private deps
# are covered here too; onscripter-en learned the same ordering rule.
# pkg_check_modules must run FIRST: it is what populates the *_LDFLAGS used below.
pkg_check_modules(FREETYPE2 QUIET IMPORTED_TARGET freetype2)
if(NOT FREETYPE2_FOUND)
message(FATAL_ERROR "krkrsdl2: system freetype2 not found (required)")
endif()
list(APPEND KRKRSDL2COMMON_INCLUDES ${FREETYPE2_INCLUDE_DIRS})
message(STATUS "krkrsdl2: FreeType ${FREETYPE2_VERSION} (system, static)")
list(APPEND _MIN_STATIC
	-L${KRKRSDL2_MINGW_VORBIS_PREFIX}/lib -lvorbisfile -lvorbis -logg
	${FREETYPE2_STATIC_LDFLAGS} -lwinpthread -lstdc++
)
else()
pkg_check_modules(FREETYPE2 QUIET IMPORTED_TARGET freetype2)
if(FREETYPE2_FOUND)
list(APPEND KRKRSDL2_LIBS
	PkgConfig::FREETYPE2
)
message(STATUS "krkrsdl2: FreeType ${FREETYPE2_VERSION} (system)")
else()
message(FATAL_ERROR "krkrsdl2: system freetype2 not found (required)")
endif()
endif()
endif()
endif()

if(KRKRSDL2_BUILD_ENABLE_WARNINGS)
list(APPEND KRKRSDL2_CFLAGS
	-Wall
)
else()
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
list(APPEND KRKRSDL2_CFLAGS
	-Wno-everything
)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
list(APPEND KRKRSDL2_CFLAGS
	-w
)
endif()
endif()

if(WIN32)
list(APPEND KRKRSDL2_DEFINITIONS
	UNICODE
	_UNICODE
)
list(APPEND KRKRSDL2_LDFLAGS
	-static
	-static-libstdc++
	-static-libgcc
	-municode
	-Wl,--subsystem,windows
)
# Export the standalone libretro ABI under mingw. Besides defining the exports,
# the .def sidesteps a PE limit: --export-all-symbols over a -static link
# (~23k symbols after --exclude-libs) overflows the ordinary export table and
# ld falls back to a low-ordinal VMA layout, pushing the image's base past the
# 32-bit DLL address limit (0x7fffffff). A bounded list keeps the image loadable
# and documents the core↔engine contract in one reviewable file.
if(MINGW)
list(APPEND KRKRSDL2_LDFLAGS
	-Wl,${CMAKE_CURRENT_SOURCE_DIR}/src/config/onscripter-en_engine_krkrsdl2.def
)
endif()
list(APPEND KRKRSDL2_LIBS
	uuid
	mpr
	dmoguids
	mfplat
	strmiids
	mf
	propsys
	mfuuid
	amstrmid
	dxguid
	windowscodecs
)
if((${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686") OR (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "amd64"))
list(APPEND KRKRSDL2_LIBS
	quartz
)
endif()
if((${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686") OR (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "amd64"))
set_source_files_properties(${KIRIKIRISDL2_SSE2_SRC} PROPERTIES COMPILE_FLAGS -msse2)
set_source_files_properties(${KIRIKIRISDL2_AVX2_SRC} PROPERTIES COMPILE_FLAGS -mavx2)
endif()
list(APPEND KRKRSDL2COMMON_SRC
	src/core/sound/sdl2/tvpsnd.c

	src/core/environ/win32/WindowsUtil.cpp

	external/krkrz/movie/win32/TVPVideoOverlay.cpp

	external/krkrz/movie/win32/BufferRenderer.cpp
	external/krkrz/movie/win32/CDemuxOutputPin.cpp
	external/krkrz/movie/win32/CDemuxSource.cpp
	external/krkrz/movie/win32/CMediaSeekingProxy.cpp
	external/krkrz/movie/win32/CVMRCustomAllocatorPresenter9.cpp
	external/krkrz/movie/win32/CWMAllocator.cpp
	external/krkrz/movie/win32/CWMBuffer.cpp
	external/krkrz/movie/win32/CWMReader.cpp
	external/krkrz/movie/win32/DShowException.cpp
	external/krkrz/movie/win32/IBufferRenderer_i.c
	external/krkrz/movie/win32/IRendererBufferAccess_i.c
	external/krkrz/movie/win32/IRendererBufferVideo_i.c
	external/krkrz/movie/win32/MFByteStream.cpp
	external/krkrz/movie/win32/MFPlayer.cpp
	external/krkrz/movie/win32/PlayWindow.cpp
	external/krkrz/movie/win32/init_guid_movie.c
	external/krkrz/movie/win32/asyncio.cpp
	external/krkrz/movie/win32/asyncrdr.cpp
	external/krkrz/movie/win32/dslayerd.cpp
	external/krkrz/movie/win32/dsmixer.cpp
	external/krkrz/movie/win32/dsmovie.cpp
	external/krkrz/movie/win32/dsoverlay.cpp
	external/krkrz/movie/win32/krlmovie.cpp
	external/krkrz/movie/win32/krmmovie.cpp
	external/krkrz/movie/win32/krmovie.cpp

	external/krkrz/external/baseclasses/amextra.cpp
	external/krkrz/external/baseclasses/amfilter.cpp
	external/krkrz/external/baseclasses/amvideo.cpp
	external/krkrz/external/baseclasses/arithutil.cpp
	external/krkrz/external/baseclasses/combase.cpp
	external/krkrz/external/baseclasses/cprop.cpp
	external/krkrz/external/baseclasses/ctlutil.cpp
	external/krkrz/external/baseclasses/ddmm.cpp
	external/krkrz/external/baseclasses/dllentry.cpp
	external/krkrz/external/baseclasses/dllsetup.cpp
	external/krkrz/external/baseclasses/mtype.cpp
	external/krkrz/external/baseclasses/outputq.cpp
	external/krkrz/external/baseclasses/perflog.cpp
	external/krkrz/external/baseclasses/pstream.cpp
	external/krkrz/external/baseclasses/pullpin.cpp
	external/krkrz/external/baseclasses/refclock.cpp
	external/krkrz/external/baseclasses/renbase.cpp
	external/krkrz/external/baseclasses/schedule.cpp
	external/krkrz/external/baseclasses/seekpt.cpp
	external/krkrz/external/baseclasses/source.cpp
	external/krkrz/external/baseclasses/strmctl.cpp
	external/krkrz/external/baseclasses/sysclock.cpp
	external/krkrz/external/baseclasses/transfrm.cpp
	external/krkrz/external/baseclasses/transip.cpp
	external/krkrz/external/baseclasses/videoctl.cpp
	external/krkrz/external/baseclasses/vtrans.cpp
	external/krkrz/external/baseclasses/winctrl.cpp
	external/krkrz/external/baseclasses/winutil.cpp
	external/krkrz/external/baseclasses/wxdebug.cpp
	external/krkrz/external/baseclasses/wxlist.cpp
	external/krkrz/external/baseclasses/wxutil.cpp
)
list(APPEND KRKRSDL2COMMON_INCLUDES
	src/core/visual/win32
	src/core/environ/win32
	external/krkrz/movie/win32
	external/krkrz/external/baseclasses
)
endif()

list(APPEND KRKRSDL2_CXXFLAGS ${KRKRSDL2_CFLAGS})

list(APPEND KRKRSDL2_CXXFLAGS
	-Wno-non-virtual-dtor
)

if(EVAL_ENABLE_ASYNC_IMAGE_LOAD)
list(APPEND KRKRSDL2_DEFINITIONS
	KRKRSDL2_ENABLE_ASYNC_IMAGE_LOAD
)
endif()

if(OPTION_ENABLE_CANVAS)
list(APPEND KRKRSDL2_DEFINITIONS
	KRKRZ_ENABLE_CANVAS
)
list(APPEND KRKRSDL2COMMON_SRC
	external/krkrz/visual/opengl/CanvasIntf.cpp
	external/krkrz/visual/opengl/DrawCycleTimer.cpp
	external/krkrz/visual/opengl/GLFrameBufferObject.cpp
	external/krkrz/visual/opengl/GLShaderUtil.cpp
	external/krkrz/visual/opengl/Matrix32Intf.cpp
	external/krkrz/visual/opengl/Matrix44Intf.cpp
	external/krkrz/visual/opengl/OffscreenIntf.cpp
	external/krkrz/visual/opengl/ShaderProgramIntf.cpp
	external/krkrz/visual/opengl/TextureIntf.cpp
	external/krkrz/visual/opengl/VertexBinderIntf.cpp
	external/krkrz/visual/opengl/VertexBufferIntf.cpp
	src/core/visual/sdl2/opengl/OpenGLScreenSDL2.cpp
)
list(APPEND KRKRSDL2COMMON_INCLUDES
	src/core/visual/sdl2/opengl
	external/krkrz/external/angle/include
	external/krkrz/external/glm
)
if(${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten")
list(APPEND KRKRSDL2_LDFLAGS
	-sMAX_WEBGL_VERSION=2
	-sFULL_ES3=1
)
endif()
endif()

if(EVAL_ENABLE_CASEINSENSITIVITY)
list(APPEND KRKRSDL2_DEFINITIONS
	KRKRZ_CASE_INSENSITIVE
)
endif()

if(EVAL_ENABLE_SDL_THREADS)
list(APPEND KRKRSDL2_DEFINITIONS
	KRKRZ_USE_SDL_THREADS
)
endif()

if(EVAL_ENABLE_EXTERNAL_PLUGINS)
list(APPEND KRKRSDL2_DEFINITIONS
	KRKRSDL2_ENABLE_PLUGINS
)
endif()

if(KRKRSDL2_COMPILER_SUPPORTS_NO_DELETE_NULL_POINTER_CHECKS)
list(APPEND KRKRSDL2_CXXFLAGS
	-fno-delete-null-pointer-checks
)
endif()

if(ANDROID)
list(APPEND KRKRSDL2_LIBS
	android
)
endif()

if(IOS)
set(SDL_SHARED_ENABLED_BY_DEFAULT OFF)
set(SDL_SHARED OFF)
set(HAVE_GCC_WDECLARATION_AFTER_STATEMENT OFF CACHE BOOL "Intentionally disabled for SDL on iOS: can declaration-after-statement warning be used" FORCE)
set(HAVE_GCC_WERROR_DECLARATION_AFTER_STATEMENT OFF CACHE BOOL "Intentionally disabled for SDL on iOS: can declaration-after-statement error warning be used" FORCE)
add_subdirectory(external/SDL)
find_library(SDL2 NAMES SDL2-static SDL2main)
list(APPEND KRKRSDL2_LIBS
	SDL2-static
	SDL2main
)
list(APPEND KRKRSDL2_DEFINITIONS
	USE_SDL_MAIN
)
get_target_property(SDL2_INCLUDE_DIRS SDL2-static INCLUDE_DIRECTORIES)
set(SDL2_LIBRARIES SDL2-static)
elseif((APPLE AND NOT IOS) OR (${CMAKE_SYSTEM_NAME} STREQUAL "Linux"))
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
list(APPEND KRKRSDL2_LIBS PkgConfig::SDL2)
set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
set(SDL2_LIBRARIES ${SDL2_LIBRARIES})
set(SDL_SHARED_ENABLED_BY_DEFAULT ON)
set(SDL_STATIC_ENABLED_BY_DEFAULT OFF)

elseif(WIN32)
set(SDL_SHARED_ENABLED_BY_DEFAULT OFF)
set(SDL_SHARED OFF)
set(HAVE_WINDOWS_GAMING_INPUT_H NO)
add_subdirectory(external/SDL)
find_library(SDL2 NAMES SDL2-static)
list(APPEND KRKRSDL2_LIBS
	SDL2-static
)
get_target_property(SDL2_INCLUDE_DIRS SDL2-static INCLUDE_DIRECTORIES)
set(SDL2_LIBRARIES SDL2-static)
elseif(VITA)
set(SDL_SHARED_ENABLED_BY_DEFAULT OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/SDL)
find_library(SDL2 NAMES SDL2-static)

list(APPEND KRKRSDL2_LIBS
	stdc++
	pthread
	stdc++

	SDL2-static
	SceAppUtil_stub
	SceShellSvc_stub
	m
	z
)
get_target_property(SDL2_INCLUDE_DIRS SDL2-static INCLUDE_DIRECTORIES)
set(SDL2_LIBRARIES SDL2-static)
elseif(NINTENDO_SWITCH)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

list(APPEND KRKRSDL2_LIBS
	PkgConfig::SDL2
)
else()
find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

list(APPEND KRKRSDL2_LIBS
	${SDL2_LIBRARIES}
)
endif()

if(NOT (${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten"))
set(XNASONG OFF CACHE BOOL "Intentionally disabled for FAudio: enable XNA_Song.c" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Intentionally disabled for FAudio: Build shared libraries" FORCE)
set(_DISABLE_INSTALLATION_OLD ${_DISABLE_INSTALLATION})
set(_DISABLE_INSTALLATION TRUE)
add_subdirectory(external/FAudio)
set_target_properties(FAudio PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
target_compile_definitions(FAudio PRIVATE KRKRSDL2_LIBRETRO)
list(APPEND KRKRSDL2_LIBS
	FAudio
)
list(APPEND KRKRSDL2_DEFINITIONS
	TVP_FAUDIO_IMPLEMENT
	KRKRSDL2_LIBRETRO
)
endif()

if(VITA)
list(APPEND KRKRSDL2_SRC
	${KRKRSDL2COMMON_SRC}
)
list(APPEND KRKRSDL2_INCLUDES
	${KRKRSDL2COMMON_INCLUDES}
)
else()
set(KRKRSDL2COMMON_NAME krkrsdl2common)

add_library(${KRKRSDL2COMMON_NAME} STATIC)
if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
set_target_properties(${KRKRSDL2COMMON_NAME} PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
endif()
set_target_properties(${KRKRSDL2COMMON_NAME} PROPERTIES C_STANDARD 11)
set_target_properties(${KRKRSDL2COMMON_NAME} PROPERTIES CXX_STANDARD 14)
target_sources(${KRKRSDL2COMMON_NAME} PRIVATE ${KRKRSDL2COMMON_SRC})
target_compile_definitions(${KRKRSDL2COMMON_NAME} PUBLIC ${KRKRSDL2_DEFINITIONS})
target_compile_options(${KRKRSDL2COMMON_NAME} PUBLIC $<$<COMPILE_LANGUAGE:C>:${KRKRSDL2_CFLAGS}>)
target_compile_options(${KRKRSDL2COMMON_NAME} PUBLIC $<$<COMPILE_LANGUAGE:CXX>:${KRKRSDL2_CXXFLAGS}>)
target_include_directories(${KRKRSDL2COMMON_NAME} PUBLIC ${KRKRSDL2COMMON_INCLUDES})
if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.13.0")
target_link_options(${KRKRSDL2COMMON_NAME} PUBLIC ${KRKRSDL2_LDFLAGS})
endif()
target_link_libraries(${KRKRSDL2COMMON_NAME} ${KRKRSDL2_LIBS})
if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.16.0")
if(KRKRSDL2_ENABLE_PRECOMPILED_HEADERS)
target_precompile_headers(${KRKRSDL2COMMON_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sdl2/SDLPrecompiledHeader.h)
endif()
endif()

list(APPEND KRKRSDL2_LIBS
	${KRKRSDL2COMMON_NAME}
)
endif()

list(APPEND KRKRSDL2_SRC
	src/core/sdl2/SDLEntrypoint.cpp
)

set(KRKRSDL2_NAME krkrsdl2)

if(ANDROID)
add_library(${KRKRSDL2_NAME} SHARED)
set_target_properties(${KRKRSDL2_NAME} PROPERTIES OUTPUT_NAME "main")
else()
add_library(${KRKRSDL2_NAME} SHARED)
if(KRKRSDL2_LIBRETRO)
# As a libretro engine module the name is a contract with the core:
# KRKR_ENGINE_SONAME in engine_kirikiri_cabi.cpp loads
# onscripter-en_engine_krkrsdl2.<ext> by path. The name is unversioned and,
# on PE, unprefixed, so it is exactly that string. Only a libretro build
# gets it; upstream's standalone names (tvpwin32/tvpwin64, krkrsdl2, main)
# are unchanged otherwise.
set_target_properties(${KRKRSDL2_NAME} PROPERTIES OUTPUT_NAME "onscripter-en_engine_krkrsdl2")
# No "lib" prefix, matching the core's other engine modules
# (onscripter-en_engine_ons.so / _pons.so): the name is a contract, identical
# in shape across platforms.
set_target_properties(${KRKRSDL2_NAME} PROPERTIES PREFIX "")
elseif(NOT ANDROID)
set_target_properties(${KRKRSDL2_NAME} PROPERTIES OUTPUT_NAME "krkrsdl2")
if(WIN32)
if("${CMAKE_SIZEOF_VOID_P}" STREQUAL "8")
set_target_properties(${KRKRSDL2_NAME} PROPERTIES OUTPUT_NAME "tvpwin64")
else()
set_target_properties(${KRKRSDL2_NAME} PROPERTIES OUTPUT_NAME "tvpwin32")
endif()
endif()
endif()
endif()
if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
set_target_properties(${KRKRSDL2_NAME} PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
endif()
set_target_properties(${KRKRSDL2_NAME} PROPERTIES C_STANDARD 11)
set_target_properties(${KRKRSDL2_NAME} PROPERTIES CXX_STANDARD 14)
target_sources(${KRKRSDL2_NAME} PRIVATE ${KRKRSDL2_SRC})
# ENABLE_EXPORTS adds --export-all-symbols on PE, which would override the .def
# on mingw; leave it to the platforms that do not pass an explicit export list.
if(NOT MINGW)
set_target_properties(${KRKRSDL2_NAME} PROPERTIES ENABLE_EXPORTS ON)
endif()
target_include_directories(${KRKRSDL2_NAME} PUBLIC ${KRKRSDL2_INCLUDES})
if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.13.0")
target_link_options(${KRKRSDL2_NAME} PUBLIC ${KRKRSDL2_LDFLAGS})
endif()
target_link_libraries(${KRKRSDL2_NAME} ${KRKRSDL2_LIBS})
if(MINGW AND _MIN_STATIC)
# One -Bstatic region for the entire non-system closure, last on the command
# line so every reference above resolves from an archive. See _MIN_STATIC.
target_link_libraries(${KRKRSDL2_NAME} -Wl,-Bstatic ${_MIN_STATIC} -Wl,-Bdynamic)
endif()
if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.16.0")
if(KRKRSDL2_ENABLE_PRECOMPILED_HEADERS)
target_precompile_headers(${KRKRSDL2_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sdl2/SDLPrecompiledHeader.h)
endif()
endif()

if(WIN32)
enable_language("RC")
target_sources(${KRKRSDL2_NAME} PRIVATE
	${WIN32_RESOURCES_ROOT}/krkrsdl2.rc
)
endif()

if(IOS)
set_target_properties(${KRKRSDL2_NAME} PROPERTIES
	XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${KRKRSDL2_IOS_CODE_SIGN_IDENTITY}"
	XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "${KRKRSDL2_IOS_CODE_SIGNING_ENABLED}"
	XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "${KRKRSDL2_IOS_CODE_SIGNING_ENABLED}"
	BUNDLE TRUE
	MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/config/iOSBundleInfo.plist.in
	MACOSX_BUNDLE TRUE
	MACOSX_BUNDLE_BUNDLE_NAME "Kirikiri SDL2"
	MACOSX_BUNDLE_BUNDLE_VERSION "0.0.0"
	MACOSX_BUNDLE_COPYRIGHT "(KIRIKIRI core) (C) W.Dee and contributors All Rights Reserved. This software is based in part on the work of Independent JPEG Group. For details: Run this program with '-about' option."
	MACOSX_BUNDLE_GUI_IDENTIFIER pw.uyjulian.krkrsdl2
	MACOSX_BUNDLE_LONG_VERSION_STRING "Development version"
	MACOSX_BUNDLE_SHORT_VERSION_STRING "dev"
	XCODE_ATTRIBUTE_ENABLE_BITCODE NO
	XCODE_ATTRIBUTE_SKIP_INSTALL NO
	XCODE_ATTRIBUTE_INSTALL_PATH "/Applications"
	XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
	XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_LAUNCHIMAGE_NAME "LaunchImage"
)
set_property(
	SOURCE ${EMBED_DATA_PATH} ${IOS_RESOURCES_ROOT}/Images.xcassets
	PROPERTY MACOSX_PACKAGE_LOCATION "Resources"
)
target_sources(${KRKRSDL2_NAME} PRIVATE 
	${EMBED_DATA_PATH}
	${IOS_RESOURCES_ROOT}/Images.xcassets
)
endif()

if(OPTION_BUILD_MACOS_BUNDLE)
if(APPLE AND NOT IOS)
set_target_properties(${KRKRSDL2_NAME} PROPERTIES
	BUNDLE TRUE
	MACOSX_BUNDLE TRUE
	MACOSX_BUNDLE_BUNDLE_NAME "Kirikiri SDL2"
	MACOSX_BUNDLE_BUNDLE_VERSION "0.0.0"
	MACOSX_BUNDLE_COPYRIGHT "(KIRIKIRI core) (C) W.Dee and contributors All Rights Reserved. This software is based in part on the work of Independent JPEG Group. For details: Run this program with '-about' option."
	MACOSX_BUNDLE_GUI_IDENTIFIER pw.uyjulian.krkrsdl2
	MACOSX_BUNDLE_ICON_FILE "krkrsdl2.icns"
	MACOSX_BUNDLE_LONG_VERSION_STRING "Development version"
	MACOSX_BUNDLE_SHORT_VERSION_STRING "dev"
	XCODE_ATTRIBUTE_SKIP_INSTALL NO
	XCODE_ATTRIBUTE_INSTALL_PATH "/Applications"
)
set_property(
	SOURCE ${EMBED_DATA_PATH} ${MACOS_RESOURCES_ROOT}/krkrsdl2.icns
	PROPERTY MACOSX_PACKAGE_LOCATION "Resources"
)
target_sources(${KRKRSDL2_NAME} PRIVATE 
	${EMBED_DATA_PATH}
	${MACOS_RESOURCES_ROOT}/krkrsdl2.icns
)
endif()
endif()

if(VITA)
vita_create_self(${KRKRSDL2_NAME}.self ${KRKRSDL2_NAME})

set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -s CATEGORY=gdb -d ATTRIBUTE2=12")

vita_create_vpk(${KRKRSDL2_NAME}.vpk ${VITA_TITLEID} ${KRKRSDL2_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE ${VITA_RESOURCES_ROOT}/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${VITA_RESOURCES_ROOT}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE ${VITA_RESOURCES_ROOT}/sce_sys/livearea/contents/icon.png sce_sys/livearea/contents/icon.png
    FILE ${VITA_RESOURCES_ROOT}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    FILE ${EMBED_DATA_PATH} ${EMBED_DATA_PATH_BASENAME}
)
endif()

if(NINTENDO_SWITCH)
add_custom_target(${KRKRSDL2_NAME}.nacp
    COMMAND $ENV{DEVKITPRO}/tools/bin/nacptool
        --create
        ${SWITCH_APP_NAME}
        ${SWITCH_AUTHOR}
        ${SWITCH_VERSION}
        ${KRKRSDL2_NAME}.nacp
)
add_custom_target(romfs ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/romfs
	COMMAND ${CMAKE_COMMAND} -E create_symlink ${EMBED_DATA_PATH} ${CMAKE_CURRENT_BINARY_DIR}/romfs/${EMBED_DATA_PATH_BASENAME}
)
add_custom_target(${KRKRSDL2_NAME}.nro ALL
    COMMAND $ENV{DEVKITPRO}/tools/bin/elf2nro
        $<TARGET_FILE:${KRKRSDL2_NAME}>
        ${KRKRSDL2_NAME}.nro
        --icon=${SWITCH_RESOURCES_ROOT}/icon.jpg
        --nacp=${KRKRSDL2_NAME}.nacp
        --romfsdir=${CMAKE_CURRENT_BINARY_DIR}/romfs
    DEPENDS ${KRKRSDL2_NAME}.nacp romfs ${SWITCH_RESOURCES_ROOT}/icon.jpg
)
add_dependencies(${KRKRSDL2_NAME}.nro ${KRKRSDL2_NAME})
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
install(TARGETS ${KRKRSDL2_NAME} DESTINATION bin)
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten")
configure_file(
	src/config/index.html.in
	index.html
	@ONLY
	NEWLINE_STYLE UNIX
)
endif()

get_directory_property(HAS_PARENT_SCOPE PARENT_DIRECTORY)
if(HAS_PARENT_SCOPE)
# Share certain variables to projects that include this one
set(KRKRSDL2_NAME ${KRKRSDL2_NAME} PARENT_SCOPE)
set(KRKRSDL2_REPO_DIR ${KRKRSDL2_REPO_DIR} PARENT_SCOPE)
endif()
