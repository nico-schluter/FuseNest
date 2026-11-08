cmake_minimum_required(VERSION 3.25)
project(FuseNest VERSION 0.1.0 LANGUAGES CXX)

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
configure_file("packaging/${ADDIN_NAME}.manifest" "${DIST_DIR}/${ADDIN_NAME}.manifest" COPYONLY)
configure_file("packaging/AddInIcon.svg" "${DIST_DIR}/AddInIcon.svg" COPYONLY)

# Make an accidental single-architecture release build impossible to miss: an
# arm64-only add-in loads fine here and silently fails on Intel Macs.
if(APPLE)
    add_custom_command(TARGET ${ADDIN_NAME} POST_BUILD
        COMMAND lipo -info "$<TARGET_FILE:${ADDIN_NAME}>"
        COMMENT "Architectures in the built add-in:"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# `cmake --build --preset dist --target package-zip` produces the sideload
# artifact: a zip of dist/FuseNest that a user drops into their AddIns folder.
#
# For the Autodesk App Store this folder instead becomes FuseNest.bundle/Contents
# next to a PackageContents.xml. Note the published manifest drops the
# sourcemac/sourcewindows keys and gains an "id" GUID assigned at submission.
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

set(_archs ${CMAKE_OSX_ARCHITECTURES})
if(NOT _archs)
    set(_archs ${CMAKE_HOST_SYSTEM_PROCESSOR})
endif()

set(_rust_slices "")
foreach(arch IN LISTS _archs)
    if(arch STREQUAL "arm64")
        set(triple "aarch64-apple-darwin")
    elseif(arch STREQUAL "x86_64")
        set(triple "x86_64-apple-darwin")
    else()
        message(FATAL_ERROR "No Rust target mapping for architecture '${arch}'")
    endif()

    set(slice "${RUST_DIR}/target/${triple}/release/libnest_kernel.a")
    add_custom_command(
        OUTPUT "${slice}"
        COMMAND "${CARGO}" build --release --target ${triple}
        WORKING_DIRECTORY "${RUST_DIR}"
        DEPENDS ${RUST_SOURCES}
        COMMENT "cargo build --release --target ${triple}"
        VERBATIM)
    list(APPEND _rust_slices "${slice}")
endforeach()

set(RUST_KERNEL_LIB "${CMAKE_CURRENT_BINARY_DIR}/libnest_kernel.a")
add_custom_command(
    OUTPUT "${RUST_KERNEL_LIB}"
    COMMAND lipo -create ${_rust_slices} -output "${RUST_KERNEL_LIB}"
    DEPENDS ${_rust_slices}
    COMMENT "Combining Rust slices into ${RUST_KERNEL_LIB}"
    VERBATIM)

add_custom_target(rust_kernel DEPENDS "${RUST_KERNEL_LIB}")
add_dependencies(${ADDIN_NAME} rust_kernel)
target_link_libraries(${ADDIN_NAME} PRIVATE "${RUST_KERNEL_LIB}")

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
target_compile_features(triangulate_test PRIVATE cxx_std_17)

add_executable(transform4_test EXCLUDE_FROM_ALL
    tests/transform4_test.cpp
    src/transform4.cpp
)
target_include_directories(transform4_test PRIVATE src)
target_compile_features(transform4_test PRIVATE cxx_std_17)

# Not geometry, but the same rule applies: settings recovery is checkable from
# a terminal, and it is the one piece of start-up code whose failure mode is the
# add-in not loading at all.
add_executable(settings_test EXCLUDE_FROM_ALL
    tests/settings_test.cpp
    src/settings.cpp
    src/paths.cpp
)
target_include_directories(settings_test PRIVATE src)
target_compile_features(settings_test PRIVATE cxx_std_17)

add_custom_target(run-tests
    COMMAND triangulate_test
    COMMAND transform4_test
    COMMAND settings_test
    DEPENDS triangulate_test transform4_test settings_test
    COMMENT "Running geometry and settings tests"
)
