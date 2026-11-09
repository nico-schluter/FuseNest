cmake_minimum_required(VERSION 3.25)
project(FuseNest VERSION 2.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Layout
#
#   src/        pure source. Nothing in here ships.
#   packaging/  the non-code parts of the deliverable: manifest and icon.
#   dist/       GENERATED. dist/FuseNest is the add-in: point Fusion here, and
#               it is also exactly what gets zipped or wrapped in a .bundle.
#
# Fusion finds an add-in by looking for <folder>/<Name>.manifest and loading
# <Name>.dylib (or .dll) beside it, so the folder name, manifest name and binary
# name must all agree.
# ---------------------------------------------------------------------------
set(ADDIN_NAME FuseNest)
set(DIST_DIR "${CMAKE_SOURCE_DIR}/dist/${ADDIN_NAME}")

# ---------------------------------------------------------------------------
# Fusion C++ API.
#
# Fusion re-copies include/ and lib/ to this hash-free path on every update, so
# unlike the TypeScript API's tsconfig.json these settings survive updates
# untouched. lib/*.dylib are LINK STUBS: core.dylib's LC_ID_DYLIB is
# @rpath/Neutron/XInterface10.dylib, so we link the stub and dyld binds to
# Fusion's live libraries at load using the host process's rpath.
# ---------------------------------------------------------------------------
if(APPLE)
    set(FUSION_API "$ENV{HOME}/Library/Application Support/Autodesk/Autodesk Fusion 360/API/CPP")
    set(ADDIN_SUFFIX ".dylib")
elseif(WIN32)
    file(TO_CMAKE_PATH "$ENV{APPDATA}/Autodesk/Autodesk Fusion 360/API/CPP" FUSION_API)
    set(ADDIN_SUFFIX ".dll")
endif()

if(NOT EXISTS "${FUSION_API}/include/Core/CoreAll.h")
    message(FATAL_ERROR "Fusion C++ API not found at:\n  ${FUSION_API}\n"
                        "Launch Fusion once so it stages the API, then re-run CMake.")
endif()

# Only what we actually call. Each extra module is another dylib bound at load.
set(FUSION_API_MODULES core fusion)

foreach(mod IN LISTS FUSION_API_MODULES)
    if(APPLE)
        list(APPEND FUSION_API_LIBS "${FUSION_API}/lib/${mod}.dylib")
    else()
        list(APPEND FUSION_API_LIBS "${FUSION_API}/lib/${mod}.lib")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The add-in binary.
#
# Sources are listed explicitly: a file(GLOB) without CONFIGURE_DEPENDS silently
# misses newly added files and links a stale object.
# ---------------------------------------------------------------------------
add_library(${ADDIN_NAME} SHARED
    src/entry.cpp
    src/log.cpp
    src/message.cpp
    src/nest_command.cpp
    src/nest_request.cpp
    src/paths.cpp
    src/preview.cpp
    src/profile.cpp
    src/solver.cpp
    src/settings.cpp
    src/solver_input.cpp
    src/timing.cpp
    src/transform4.cpp
    src/triangulate.cpp
    src/writeback.cpp
)

set_target_properties(${ADDIN_NAME} PROPERTIES
    PREFIX ""                                   # no "lib" prefix
    SUFFIX "${ADDIN_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "${DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${DIST_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${DIST_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${DIST_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${DIST_DIR}"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden                # only run/stop are exported
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(${ADDIN_NAME} SYSTEM PRIVATE "${FUSION_API}/include")
target_link_libraries(${ADDIN_NAME} PRIVATE ${FUSION_API_LIBS})

# ---------------------------------------------------------------------------
# Windows/MSVC settings. Every one of these is a link or decode failure rather
# than a preference.
#
# CRT: Rust's x86_64-pc-windows-msvc target links the DYNAMIC CRT -- crt-static
# is off by default there -- and the kernel is always built --release. CMake's
# MSVC default is /MD for Release but /MDd for Debug and RelWithDebInfo gets
# /MD, so a Debug configure would hand /MDd objects to a /MD archive and link
# dies on duplicate CRT symbols and an _ITERATOR_DEBUG_LEVEL mismatch. Pin it
# for every configuration instead of relying on which preset someone used.
#
# /utf-8: MSVC otherwise decodes source as the system codepage. The message
# tests hold non-ASCII literals deliberately -- that is the point of the path
# cases -- and a mis-decoded literal turns a real escaping test into a pass.
#
# NOMINMAX: windows.h defines min and max as macros and breaks std::min.
# WIN32_LEAN_AND_MEAN: drops winsock1, which collides with the winsock2 that
# Rust's std brings in through ws2_32.
#
# /EHsc is set EXPLICITLY rather than inherited. The first Windows build had
# every translation unit warning C4530 -- "C++ exception handler used, but
# unwind semantics are not enabled" -- which means the flag was not reaching
# the compiler, whatever CMake's default is supposed to be. That is not a
# warning to silence: entry.cpp and solver.cpp between them have nine try/catch
# blocks, they sit on the add-in entry point and the Rust FFI boundary, and
# without unwind semantics those handlers do not run destructors on the way
# out. Stating the exception model here makes it independent of what a default
# happens to be.
#
# /bigobj because the two Fusion umbrella headers come in through a PCH and
# nest_command.cpp is already large enough to be worth not finding out.
#
# Deliberately NOT here: /permissive- and a warning level. Both are strictness
# choices rather than build requirements, they are applied to the Fusion
# headers too, and neither can be judged from a machine that cannot compile
# this. They belong with the -Wall -Wextra -Werror work, where the point is to
# read what they say.
# ---------------------------------------------------------------------------
if(MSVC)
    set_target_properties(${ADDIN_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    target_compile_options(${ADDIN_NAME} PRIVATE /EHsc /utf-8 /bigobj)
    target_compile_definitions(${ADDIN_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# The journal, the settings file and any diagnostic dumps go to a per-user
# directory resolved at RUN time (see src/paths.cpp) -- a compiled-in path meant
# that on any machine but the build machine every write silently failed. Only
# the version is baked in, for the log header and the diagnostic report.
target_compile_definitions(${ADDIN_NAME} PRIVATE FUSENEST_VERSION="${PROJECT_VERSION}")

# The two umbrella headers are several seconds of compile on their own. Paying
# that once takes incremental rebuilds under a second.
target_precompile_headers(${ADDIN_NAME} PRIVATE
    "${FUSION_API}/include/Core/CoreAll.h"
    "${FUSION_API}/include/Fusion/FusionAll.h"
)

# ---------------------------------------------------------------------------
# Stage the non-code parts of the deliverable.
#
# configure_file registers a dependency, so editing the manifest re-runs CMake
# and re-copies -- the staged copy cannot silently drift from the source of
# truth in packaging/.
# ---------------------------------------------------------------------------
# The manifest carries the version, so it is CONFIGURED rather than copied:
# the version now has exactly one source, project() above. Two hand-edited
# copies of a version number is a drift waiting to happen, and the one that
# matters is the one Fusion reads, which is not the one anybody looks at.
#
# The "id" is the app's store identity, and FuseNest already has one -- see
# FUSENEST_APP_ID by the bundle target. It is the same GUID as the
# PackageContents UpgradeCode: unbraced here, braced there.
if(FUSENEST_APP_ID)
    set(FUSENEST_MANIFEST_ID_LINE "    \"id\": \"${FUSENEST_APP_ID}\",\n")
else()
    set(FUSENEST_MANIFEST_ID_LINE "")
endif()
configure_file("packaging/${ADDIN_NAME}.manifest.in" "${DIST_DIR}/${ADDIN_NAME}.manifest" @ONLY)
configure_file("packaging/AddInIcon.svg" "${DIST_DIR}/AddInIcon.svg" COPYONLY)

# The F1 page and the third-party notices it links to. Staged the same way, so
# a build is a complete add-in folder: Command::helpFile is set to an absolute
# path under here at run time, and a missing file means no help button rather
# than a button that opens nothing. notices.html is GENERATED -- see
# tools/gen_notices.py -- and regenerating it after a dependency change is a
# deliberate step, not something a build should do silently behind the
# packager's back.
configure_file("packaging/resources/help.html" "${DIST_DIR}/resources/help.html" COPYONLY)
configure_file("packaging/resources/notices.html" "${DIST_DIR}/resources/notices.html" COPYONLY)

# Make an accidental single-architecture release build impossible to miss: an
# arm64-only add-in loads fine here and silently fails on Intel Macs.
if(APPLE)
    add_custom_command(TARGET ${ADDIN_NAME} POST_BUILD
        COMMAND lipo -info "$<TARGET_FILE:${ADDIN_NAME}>"
        COMMENT "Architectures in the built add-in:"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Two deliverables.
#
#   package-bundle  the Autodesk App Store submission, and what ships.
#   package-zip     a sideload zip, for your own testing and beta builds only.
#
# package-zip is NOT the publication route. A downloaded-and-unzipped add-in
# carries com.apple.quarantine, and a quarantined ad-hoc-signed dylib is
# refused outright by a host with library validation -- which Fusion has -- with
# "library load disallowed by system policy". Terminal unzip propagates the
# attribute just as Archive Utility does, and clearing it with xattr -dr was
# measured NOT to be enough; the bytes had to be rewritten to a fresh file. The
# App Store installer never applies the attribute, which is why the bundle is
# the route and why no certificate is needed for it.
# ---------------------------------------------------------------------------
add_custom_target(package-zip
    COMMAND ${CMAKE_COMMAND} -E tar "cf"
            "${CMAKE_SOURCE_DIR}/dist/${ADDIN_NAME}-${PROJECT_VERSION}.zip"
            --format=zip -- "${DIST_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
    DEPENDS ${ADDIN_NAME}
    COMMENT "Packaging dist/${ADDIN_NAME} -> dist/${ADDIN_NAME}-${PROJECT_VERSION}.zip"
    VERBATIM)

# ---------------------------------------------------------------------------
# The Rust nesting kernel (sparrow), linked in as a static archive.
#
# A staticlib means everything Rust needs ends up inside the add-in: no second
# binary, nothing to dlopen, no runtime dependency on Fusion's layout. The FFI
# surface is a handful of extern "C" functions, so Fusion's vtable ABI and the
# solver never touch each other -- a Fusion update cannot break the kernel,
# only the thin C++ layer that calls the Fusion API.
#
# WARNING: linking Rust pulls in thread-local storage, and dyld REFUSES to
# unload any image containing TLS (dlclose returns success and lies). From here
# on, Stop + rebuild + Run keeps running the OLD code -- Fusion must be
# restarted after every rebuild. Statics also survive Stop/Run, so reset state
# in run() rather than relying on a fresh load.
# ---------------------------------------------------------------------------
find_program(CARGO cargo REQUIRED)

set(RUST_DIR "${CMAKE_SOURCE_DIR}/nest_kernel")
# CONFIGURE_DEPENDS matters: without it the list is fixed at configure time, so
# a NEW .rs file never triggers a cargo rebuild and the add-in silently links a
# stale archive. That cost a struct-layout mismatch to find once already.
file(GLOB RUST_SOURCES CONFIGURE_DEPENDS "${RUST_DIR}/src/*.rs" "${RUST_DIR}/Cargo.toml")

# Which architectures to build, and the Rust triple and archive name for each.
#
# macOS: the dist preset asks for two and they are lipo'd together; dev builds
# the host one only, so ccache stays useful.
#
# Windows: exactly one, x86_64. Fusion ships win64 only, so an arm64 Windows
# host still wants the x86_64 add-in -- it runs under emulation inside an
# emulated Fusion. A native aarch64 slice would be a DLL Fusion cannot load.
if(APPLE)
    set(_archs ${CMAKE_OSX_ARCHITECTURES})
    if(NOT _archs)
        set(_archs ${CMAKE_HOST_SYSTEM_PROCESSOR})
    endif()
    set(_rust_lib_name "libnest_kernel.a")
elseif(WIN32)
    set(_archs "x86_64")
    # MSVC names a static library without the lib prefix and with .lib, so the
    # path cargo writes is NOT the one the macOS branch looks for. Getting this
    # wrong produces a missing custom-command output, which Ninja reports as a
    # confusing "no rule to make target" rather than anything about Rust.
    set(_rust_lib_name "nest_kernel.lib")
endif()

set(_rust_slices "")
foreach(arch IN LISTS _archs)
    if(APPLE AND arch STREQUAL "arm64")
        set(triple "aarch64-apple-darwin")
    elseif(APPLE AND arch STREQUAL "x86_64")
        set(triple "x86_64-apple-darwin")
    elseif(WIN32 AND arch STREQUAL "x86_64")
        set(triple "x86_64-pc-windows-msvc")
    else()
        message(FATAL_ERROR "No Rust target mapping for architecture '${arch}'")
    endif()

    set(slice "${RUST_DIR}/target/${triple}/release/${_rust_lib_name}")
    add_custom_command(
        OUTPUT "${slice}"
        COMMAND "${CARGO}" build --release --target ${triple}
        WORKING_DIRECTORY "${RUST_DIR}"
        DEPENDS ${RUST_SOURCES}
        COMMENT "cargo build --release --target ${triple}"
        VERBATIM)
    list(APPEND _rust_slices "${slice}")
endforeach()

# One slice is already the archive; two need lipo. Guarding on the count rather
# than on APPLE keeps the macOS dev build off lipo as well, and lipo exists on
# no other platform.
list(LENGTH _rust_slices _slice_count)
if(_slice_count GREATER 1)
    set(RUST_KERNEL_LIB "${CMAKE_CURRENT_BINARY_DIR}/libnest_kernel.a")
    add_custom_command(
        OUTPUT "${RUST_KERNEL_LIB}"
        COMMAND lipo -create ${_rust_slices} -output "${RUST_KERNEL_LIB}"
        DEPENDS ${_rust_slices}
        COMMENT "Combining Rust slices into ${RUST_KERNEL_LIB}"
        VERBATIM)
else()
    set(RUST_KERNEL_LIB "${_rust_slices}")
endif()

add_custom_target(rust_kernel DEPENDS "${RUST_KERNEL_LIB}")
add_dependencies(${ADDIN_NAME} rust_kernel)
target_link_libraries(${ADDIN_NAME} PRIVATE "${RUST_KERNEL_LIB}")

# What Rust's std needs from the system, which a staticlib cannot tell the
# linker itself.
#
# An .a/.lib carries no link directives, so every library std depends on is an
# unresolved symbol at DLL-link time unless it is named here. On macOS the
# frameworks std uses are already pulled in by the Fusion libraries and libc++;
# on Windows nothing else drags them in.
#
# This list is measured, not guessed. Run on 2026-11-09 against the pinned
# 1.97.0 toolchain, the compiler reported exactly:
#
#   native-static-libs: kernel32.lib ntdll.lib userenv.lib ws2_32.lib
#                       dbghelp.lib /defaultlib:msvcrt
#
# so the five below are the whole requirement -- an earlier guess had seven
# more that are not needed. The /defaultlib:msvcrt in that output is also the
# direct confirmation of the dynamic-CRT pin above: that is Rust telling the
# linker which CRT its objects expect.
#
# Re-check after any toolchain bump or dependency change; this is toolchain
# output, not a constant:
#
#   cd nest_kernel
#   cargo rustc --release --target x86_64-pc-windows-msvc \
#       --crate-type staticlib -- --print native-static-libs
if(WIN32)
    target_link_libraries(${ADDIN_NAME} PRIVATE
        kernel32 ntdll userenv ws2_32 dbghelp)
endif()

# ---------------------------------------------------------------------------
# Tests.
#
# Only the geometry that does not touch Fusion. That is the point of the
# target: everything else in the add-in can be exercised only by restarting
# Fusion and looking at it, so the code that CAN be checked from a terminal is
# kept free of Fusion headers and checked here.
#
#   cmake --build --preset dev --target run-tests
# ---------------------------------------------------------------------------
add_executable(triangulate_test EXCLUDE_FROM_ALL
    tests/triangulate_test.cpp
    src/triangulate.cpp
)
target_include_directories(triangulate_test PRIVATE src)
target_compile_features(triangulate_test PRIVATE cxx_std_20)

add_executable(transform4_test EXCLUDE_FROM_ALL
    tests/transform4_test.cpp
    src/transform4.cpp
)
target_include_directories(transform4_test PRIVATE src)
target_compile_features(transform4_test PRIVATE cxx_std_20)

# Not geometry, but the same rule applies: settings recovery is checkable from
# a terminal, and it is the one piece of start-up code whose failure mode is the
# add-in not loading at all.
add_executable(settings_test EXCLUDE_FROM_ALL
    tests/settings_test.cpp
    src/settings.cpp
    src/paths.cpp
)
target_include_directories(settings_test PRIVATE src)
target_compile_features(settings_test PRIVATE cxx_std_20)

# Nor is this geometry: a message body is HTML whether or not it meant to be,
# so a part name with an ampersand in it is a correctness question.
add_executable(message_test EXCLUDE_FROM_ALL
    tests/message_test.cpp
    src/message.cpp
)
target_include_directories(message_test PRIVATE src)
target_compile_features(message_test PRIVATE cxx_std_20)

# The same CRT and codepage pinning as the add-in. message_test is the one
# that genuinely needs /utf-8 -- its non-ASCII literals ARE the test, and MSVC
# decoding them as codepage 1252 would turn the escaping check into a pass --
# but a CRT mismatch between test binaries is the same link failure as above.
if(MSVC)
    foreach(t triangulate_test transform4_test settings_test message_test)
        set_target_properties(${t} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        target_compile_options(${t} PRIVATE /EHsc /utf-8)
        # settings_test compiles paths.cpp, which includes windows.h.
        target_compile_definitions(${t} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endforeach()
endif()

add_custom_target(run-tests
    COMMAND triangulate_test
    COMMAND transform4_test
    COMMAND settings_test
    COMMAND message_test
    DEPENDS triangulate_test transform4_test settings_test message_test
    COMMENT "Running geometry, settings and message tests"
)


# ---------------------------------------------------------------------------
# `cmake --build --preset dist --target package-bundle`
#
#   dist/FuseNest.bundle/
#     PackageContents.xml
#     Contents/              <- exactly dist/FuseNest
#
# PackageContents.xml is GENERATED, from packaging/PackageContents.xml.in --
# which is a transcription of the file Autodesk shipped inside FuseNest 1.0.0,
# so the schema is observed rather than invented, and AppVersion cannot go
# stale the way a hand-retained copy does. The guidelines require a new version
# number on every submission, which makes staleness the likeliest mistake.
#
# If Autodesk ever hands back a different file, drop it at
# packaging/PackageContents.xml and it is staged verbatim instead.
#
# ONE GUID, and FuseNest already has one. From the shipped bundle: the add-in
# manifest's "id" and the PackageContents UpgradeCode are the same value. That
# value IS the existing store listing, so building with it makes this an update
# to it; a different GUID would publish a second, unrelated app.
#
# ProductCode is not a GUID at all -- the shipped file carries a literal "*"
# and Autodesk substitutes a per-release code. There is nothing to set.
# ---------------------------------------------------------------------------
set(FUSENEST_APP_ID "42821570-7F9C-4B2F-A4E2-4652C90D2924" CACHE STRING
    "The app's store identity, from FuseNest 1.0.0's shipped bundle. Changing it publishes a new app instead of updating the existing listing.")

# Publisher-facing strings, kept separate from the add-in's own author field:
# renaming a published listing is a decision about the listing, not about the
# build. These are what 1.0.0 shipped, so the defaults are "leave it alone".
set(FUSENEST_STORE_NAME "FuseNest 2D Nesting" CACHE STRING
    "Listing name. Must match the existing listing to be seen as an update.")
set(FUSENEST_STORE_AUTHOR "Nico Schlueter" CACHE STRING
    "Author as the existing listing records it.")
set(FUSENEST_STORE_COMPANY "Ortus Lab" CACHE STRING
    "CompanyDetails name as the existing listing records it.")

set(BUNDLE_DIR "${CMAKE_SOURCE_DIR}/dist/${ADDIN_NAME}.bundle")

if(EXISTS "${CMAKE_SOURCE_DIR}/packaging/PackageContents.xml")
    # The retained file from a previous submission. Staged verbatim: its GUIDs
    # are not ours to regenerate, and the version inside it is bumped by hand
    # as part of preparing the submission.
    set(_package_contents "${CMAKE_SOURCE_DIR}/packaging/PackageContents.xml")
elseif(FUSENEST_APP_ID)
    configure_file("packaging/PackageContents.xml.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/PackageContents.xml" @ONLY)
    set(_package_contents "${CMAKE_CURRENT_BINARY_DIR}/PackageContents.xml")
else()
    set(_package_contents "")
endif()

add_custom_target(package-bundle
    COMMAND ${CMAKE_COMMAND}
            -D "DIST_DIR=${DIST_DIR}"
            -D "BUNDLE_DIR=${BUNDLE_DIR}"
            -D "ADDIN_NAME=${ADDIN_NAME}"
            -D "VERSION=${PROJECT_VERSION}"
            -D "PACKAGE_CONTENTS=${_package_contents}"
            -P "${CMAKE_SOURCE_DIR}/packaging/make_bundle.cmake"
    DEPENDS ${ADDIN_NAME}
    COMMENT "Assembling ${BUNDLE_DIR}"
    VERBATIM)
