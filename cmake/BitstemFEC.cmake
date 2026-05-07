# BitstemFEC.cmake — locate, extract, and expose the bitstem-fec codec
# binary distribution as the imported target `bitstem::fec`.
#
# bitstem-fec is shipped as platform-specific tarballs vendored under
# `lib/bitstem-fec/<platform>/`. The exact version is pinned in
# `lib/bitstem-fec/VERSION`; this script builds the expected tarball
# filename from (pin, distro, arch, microarch) and hard-fails at
# configure time if the file isn't present. See
# `lib/bitstem-fec/README.md` for the update workflow.

if(TARGET bitstem::fec)
    return()
endif()

set(_BSF_DIST_DIR "${PROJECT_SOURCE_DIR}/lib/bitstem-fec")
set(_BSF_VERSION_FILE "${_BSF_DIST_DIR}/VERSION")
if(NOT EXISTS "${_BSF_VERSION_FILE}")
    message(FATAL_ERROR
        "BitstemFEC: ${_BSF_VERSION_FILE} not found. "
        "Have the bitstem-fec tarballs been vendored?")
endif()
file(STRINGS "${_BSF_VERSION_FILE}" BSF_VERSION LIMIT_COUNT 1)
if(NOT BSF_VERSION)
    message(FATAL_ERROR "BitstemFEC: ${_BSF_VERSION_FILE} is empty.")
endif()

# Host platform / distro / arch / microarch resolution.
if(WIN32)
    set(_BSF_PLATFORM "windows")
elseif(APPLE)
    set(_BSF_PLATFORM "macos")
elseif(UNIX)
    set(_BSF_PLATFORM "linux")
else()
    message(FATAL_ERROR "BitstemFEC: unsupported host platform")
endif()

set(_BSF_DISTRO "")
if(_BSF_PLATFORM STREQUAL "linux")
    if(EXISTS "/etc/os-release")
        file(READ "/etc/os-release" _BSF_OS_RELEASE)
        # VERSION_CODENAME is the canonical Ubuntu/Debian short tag
        # (e.g. "noble", "jammy"). On distros that don't set it (some
        # RHEL derivatives) the lookup fails — at which point the user
        # has to ship a matching tarball or set BSF_DISTRO by hand.
        string(REGEX MATCH "VERSION_CODENAME=([A-Za-z0-9_.-]+)"
                _ "${_BSF_OS_RELEASE}")
        set(_BSF_DISTRO "${CMAKE_MATCH_1}")
    endif()
    if(NOT _BSF_DISTRO)
        message(FATAL_ERROR
            "BitstemFEC: could not detect Linux distro codename. "
            "Set -DBSF_DISTRO=<codename> manually.")
    endif()
endif()

# Architecture: only x86_64 today. ARM64 / Apple Silicon when the
# bitstem-fec pipeline starts producing them.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(_BSF_ARCH "x86_64")
else()
    message(FATAL_ERROR
        "BitstemFEC: unsupported CPU arch '${CMAKE_SYSTEM_PROCESSOR}'")
endif()

# x86-64 microarch level (https://en.wikipedia.org/wiki/X86-64#Microarchitecture_levels).
# v3 = AVX2 baseline; matches what the bitstem-fec pipeline builds today.
set(BSF_MICROARCH "v3" CACHE STRING
    "x86-64 microarch level for bitstem-fec (v1/v2/v3/v4)")
# Caller override for distro detection (useful in containers that
# don't ship /etc/os-release, or for cross-builds against a host with
# a different runtime).
set(BSF_DISTRO "${_BSF_DISTRO}" CACHE STRING
    "Linux distro codename for bitstem-fec tarball selection")

# Build expected tarball name from the (version, distro, arch, microarch)
# tuple. The expected filename below MUST exist verbatim under
# lib/bitstem-fec/<platform>/.
if(_BSF_PLATFORM STREQUAL "linux")
    set(_BSF_TARBALL_NAME
        "bitstem-fec-shared-${BSF_VERSION}-${BSF_DISTRO}-${_BSF_ARCH}-${BSF_MICROARCH}.tar.gz")
elseif(_BSF_PLATFORM STREQUAL "macos")
    set(_BSF_TARBALL_NAME
        "bitstem-fec-shared-${BSF_VERSION}-macos-${_BSF_ARCH}-${BSF_MICROARCH}.tar.gz")
elseif(_BSF_PLATFORM STREQUAL "windows")
    # Windows ships a .zip with a .dll inside; same name+pin convention.
    set(_BSF_TARBALL_NAME
        "bitstem-fec-shared-${BSF_VERSION}-windows-${_BSF_ARCH}-${BSF_MICROARCH}.zip")
endif()

set(_BSF_TARBALL_PATH "${_BSF_DIST_DIR}/${_BSF_PLATFORM}/${_BSF_TARBALL_NAME}")
if(NOT EXISTS "${_BSF_TARBALL_PATH}")
    message(FATAL_ERROR
        "BitstemFEC: expected tarball ${_BSF_TARBALL_PATH} is missing.\n"
        "Either drop the matching binary into ${_BSF_DIST_DIR}/${_BSF_PLATFORM}/, "
        "bump lib/bitstem-fec/VERSION to a tag that has one, or override "
        "-DBSF_MICROARCH / -DBSF_DISTRO to a tuple that does.")
endif()

# Extract once into the build tree; subsequent configure runs hit the
# stamp file and skip. The tarball's top-level directory is its own
# basename without the .tar.gz suffix — we use that as the extraction
# target so reconfigures with a bumped VERSION don't collide with old
# extractions.
# Tarball "stem" (filename minus .tar.gz / .zip) — matches the
# top-level directory name produced by the bitstem-fec packaging
# pipeline. Can't use NAME_WE here because the version string contains
# dots and pluses (`0.9.0+main.99a6ce0`) that would be misinterpreted
# as suffixes.
string(REGEX REPLACE "\\.(tar\\.gz|tgz|zip)$" "" _BSF_TARBALL_BASE
       "${_BSF_TARBALL_NAME}")
set(_BSF_EXTRACT_ROOT "${CMAKE_BINARY_DIR}/_deps/bitstem-fec")
set(_BSF_PREFIX_DIR   "${_BSF_EXTRACT_ROOT}/${_BSF_TARBALL_BASE}")
set(_BSF_STAMP        "${_BSF_PREFIX_DIR}/.cmake-extracted")

if(NOT EXISTS "${_BSF_STAMP}")
    message(STATUS "BitstemFEC: extracting ${_BSF_TARBALL_NAME}")
    file(MAKE_DIRECTORY "${_BSF_EXTRACT_ROOT}")
    file(ARCHIVE_EXTRACT
        INPUT       "${_BSF_TARBALL_PATH}"
        DESTINATION "${_BSF_EXTRACT_ROOT}")
    file(TOUCH "${_BSF_STAMP}")
endif()

# Resolve the real soname-versioned shared library inside the extracted
# tree. The tarball ships libfec.so → libfec.so.<MAJOR> →
# libfec.so.<X.Y.Z>; we point IMPORTED_LOCATION at the fully-versioned
# file so cmake's install/copy logic can resolve it deterministically.
file(GLOB _BSF_SO_CANDIDATES
    "${_BSF_PREFIX_DIR}/lib/libfec.so.[0-9]*.[0-9]*.[0-9]*"
    "${_BSF_PREFIX_DIR}/lib/libfec.dylib"
    "${_BSF_PREFIX_DIR}/bin/fec.dll")
list(LENGTH _BSF_SO_CANDIDATES _BSF_SO_COUNT)
if(_BSF_SO_COUNT EQUAL 0)
    message(FATAL_ERROR
        "BitstemFEC: extracted ${_BSF_TARBALL_NAME} but no shared library "
        "found under ${_BSF_PREFIX_DIR}.")
endif()
list(GET _BSF_SO_CANDIDATES 0 _BSF_SO_PATH)

set(_BSF_INCLUDE_DIR "${_BSF_PREFIX_DIR}/include")
if(NOT EXISTS "${_BSF_INCLUDE_DIR}/bitstem/fec/fec.hpp")
    message(FATAL_ERROR
        "BitstemFEC: ${_BSF_INCLUDE_DIR}/bitstem/fec/fec.hpp not found "
        "after extraction.")
endif()

add_library(bitstem::fec SHARED IMPORTED GLOBAL)
set_target_properties(bitstem::fec PROPERTIES
    IMPORTED_LOCATION             "${_BSF_SO_PATH}"
    INTERFACE_INCLUDE_DIRECTORIES "${_BSF_INCLUDE_DIR}")
if(_BSF_PLATFORM STREQUAL "linux")
    set_target_properties(bitstem::fec PROPERTIES
        IMPORTED_SONAME "libfec.so.0")
endif()

# Surface the resolved version + path so the consumer can inject them
# into compile flags / install rules without re-globbing.
set(BITSTEM_FEC_VERSION   "${BSF_VERSION}"     CACHE INTERNAL "")
set(BITSTEM_FEC_LIBRARY   "${_BSF_SO_PATH}"    CACHE INTERNAL "")
set(BITSTEM_FEC_PREFIX    "${_BSF_PREFIX_DIR}" CACHE INTERNAL "")

message(STATUS
    "BitstemFEC: ${BSF_VERSION} (${_BSF_PLATFORM}/${BSF_DISTRO}/${_BSF_ARCH}/${BSF_MICROARCH}) "
    "-> ${_BSF_SO_PATH}")
