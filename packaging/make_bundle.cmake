# Assembles dist/FuseNest.bundle. Run by the package-bundle target, not by hand:
#
#   cmake -D DIST_DIR=... -D BUNDLE_DIR=... -D ADDIN_NAME=... -D VERSION=... \
#         -D PACKAGE_CONTENTS=... -D WINDOWS_DLL=... -P packaging/make_bundle.cmake
#
# This is a script rather than a few add_custom_command lines because of the
# checks at the end. A bundle is assembled on one machine and the other
# platform's binary arrives from the other, so "it built" and "it is complete"
# are different questions -- and worse, "both binaries are here" and "both
# binaries are the SAME SOURCE" are different questions again. Nothing else in
# the build is in a position to ask either one.

foreach(v DIST_DIR BUNDLE_DIR ADDIN_NAME VERSION)
    if(NOT ${v})
        message(FATAL_ERROR "make_bundle.cmake: -D ${v} is required")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${DIST_DIR}")
    message(FATAL_ERROR "No staged add-in at ${DIST_DIR}. Build the add-in first.")
endif()

# A clean Contents, so a file deleted from packaging/ does not live on inside an
# old bundle.
file(REMOVE_RECURSE "${BUNDLE_DIR}/Contents")
file(MAKE_DIRECTORY "${BUNDLE_DIR}/Contents")
file(COPY "${DIST_DIR}/" DESTINATION "${BUNDLE_DIR}/Contents")

# The foreign binary is an explicit INPUT, not something left lying in the build
# output directory hoping to be noticed. dist/<name> is generated and nothing on
# this machine produces a .dll, so a .dll sitting in it would be an unexplained
# artifact that no build validates and no rebuild refreshes.
if(WINDOWS_DLL)
    if(NOT EXISTS "${WINDOWS_DLL}")
        message(FATAL_ERROR
            "FUSENEST_WINDOWS_DLL points at nothing:\n  ${WINDOWS_DLL}")
    endif()
    file(COPY "${WINDOWS_DLL}" DESTINATION "${BUNDLE_DIR}/Contents")
    get_filename_component(_dll_name "${WINDOWS_DLL}" NAME)
    if(NOT _dll_name STREQUAL "${ADDIN_NAME}.dll")
        file(RENAME "${BUNDLE_DIR}/Contents/${_dll_name}"
                    "${BUNDLE_DIR}/Contents/${ADDIN_NAME}.dll")
    endif()
endif()

if(PACKAGE_CONTENTS)
    file(COPY "${PACKAGE_CONTENTS}" DESTINATION "${BUNDLE_DIR}")
    get_filename_component(_pc_name "${PACKAGE_CONTENTS}" NAME)
    if(NOT _pc_name STREQUAL "PackageContents.xml")
        file(RENAME "${BUNDLE_DIR}/${_pc_name}" "${BUNDLE_DIR}/PackageContents.xml")
    endif()
else()
    file(REMOVE "${BUNDLE_DIR}/PackageContents.xml")
endif()

# --- provenance -------------------------------------------------------------
# The stamp is a literal compiled into the binary (see FUSENEST_BUILD_STAMP in
# CMakeLists.txt), so it travels with the file through a zip and an email.
# file(STRINGS) reads it without needing `strings`, which Windows has not got.
function(read_stamp binary out)
    set(${out} "" PARENT_SCOPE)
    if(NOT EXISTS "${binary}")
        return()
    endif()
    file(STRINGS "${binary}" _hits REGEX "${VERSION}\\+[A-Za-z0-9._-]+")
    foreach(h IN LISTS _hits)
        if(h MATCHES "(${VERSION}\\+[A-Za-z0-9._-]+)")
            set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

set(_dylib "${BUNDLE_DIR}/Contents/${ADDIN_NAME}.dylib")
set(_dll "${BUNDLE_DIR}/Contents/${ADDIN_NAME}.dll")
read_stamp("${_dylib}" _mac_stamp)
read_stamp("${_dll}" _win_stamp)

set(_missing "")
message(STATUS "${ADDIN_NAME} ${VERSION} -> ${BUNDLE_DIR}")

if(EXISTS "${_dylib}")
    execute_process(COMMAND lipo -info "${_dylib}"
                    OUTPUT_VARIABLE _archs OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _lipo_rc)
    if(_lipo_rc EQUAL 0)
        string(REGEX REPLACE "^.*are: *" "" _archs "${_archs}")
        string(REGEX REPLACE "^.*is architecture: *" "" _archs "${_archs}")
    else()
        set(_archs "?")
    endif()
    message(STATUS "  macOS    ${ADDIN_NAME}.dylib  [${_archs}]  ${_mac_stamp}")
else()
    list(APPEND _missing "${ADDIN_NAME}.dylib (macOS)")
endif()

if(EXISTS "${_dll}")
    message(STATUS "  Windows  ${ADDIN_NAME}.dll     ${_win_stamp}")
else()
    list(APPEND _missing "${ADDIN_NAME}.dll (Windows)")
endif()

if(_missing)
    string(REPLACE ";" "\n           " _missing_text "${_missing}")
    message(WARNING
        "Bundle is INCOMPLETE -- missing:\n           ${_missing_text}\n"
        "  The manifest declares supportedOS \"windows|mac\", so a submission "
        "needs both binaries.\n"
        "  Build the other platform and pass its binary in:\n"
        "    cmake --preset dist -D FUSENEST_WINDOWS_DLL=/path/to/FuseNest.dll")
endif()

# Same source, or two different commits wearing the same version number? The
# manifest stamps both with ${VERSION} regardless, so without this the mismatch
# ships silently and shows up as one platform behaving like last week.
if(_mac_stamp AND _win_stamp)
    if(NOT _mac_stamp STREQUAL _win_stamp)
        message(WARNING
            "The two binaries are from DIFFERENT builds:\n"
            "    macOS    ${_mac_stamp}\n"
            "    Windows  ${_win_stamp}\n"
            "  Both are labelled ${VERSION} by the manifest, so nothing "
            "downstream will notice. Rebuild whichever is behind.")
    else()
        message(STATUS "  provenance: both from ${_mac_stamp}")
    endif()
elseif(EXISTS "${_dylib}" AND EXISTS "${_dll}")
    message(WARNING
        "Cannot verify the two binaries match: at least one carries no "
        "build stamp.\n"
        "    macOS    ${_mac_stamp}\n"
        "    Windows  ${_win_stamp}\n"
        "  A binary built before FUSENEST_BUILD_STAMP existed carries no "
        "marker. Rebuild both and the check becomes automatic.")
endif()

foreach(_s "${_mac_stamp}" "${_win_stamp}")
    if(_s MATCHES "-dirty$")
        message(WARNING
            "Built from a DIRTY tree: ${_s}\n"
            "  Uncommitted changes are in this binary, so the commit it names "
            "does not describe it. Fine for testing; not for a submission.")
    endif()
endforeach()

if(NOT PACKAGE_CONTENTS)
    message(WARNING
        "No PackageContents.xml in the bundle, so this is NOT submittable.\n"
        "  FUSENEST_APP_ID is empty. It defaults to the GUID of the existing "
        "store listing, so an empty value means someone cleared it on "
        "purpose.\n"
        "  Re-configure with -D FUSENEST_APP_ID=42821570-7F9C-4B2F-A4E2-4652C90D2924 "
        "to restore it, or drop a file at packaging/PackageContents.xml to "
        "stage that verbatim instead.")
endif()
