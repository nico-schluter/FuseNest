# Assembles dist/FuseNest.bundle. Run by the package-bundle target, not by hand:
#
#   cmake -D DIST_DIR=... -D BUNDLE_DIR=... -D ADDIN_NAME=... -D VERSION=... \
#         -D PACKAGE_CONTENTS=... -P packaging/make_bundle.cmake
#
# This is a script rather than a few add_custom_command lines because the part
# worth having is the check at the end: which platform binaries actually made it
# in. A bundle is assembled on one machine and the other platform's binary is
# copied in from the other, so "it built" and "it is complete" are different
# questions, and only the second one matters at submission time.

foreach(v DIST_DIR BUNDLE_DIR ADDIN_NAME VERSION)
    if(NOT ${v})
        message(FATAL_ERROR "make_bundle.cmake: -D ${v} is required")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${DIST_DIR}")
    message(FATAL_ERROR "No staged add-in at ${DIST_DIR}. Build the add-in first.")
endif()

# A clean Contents, so a file deleted from packaging/ does not live on inside an
# old bundle. dist/<name> is the merge point for the two platform binaries --
# see the README -- so nothing is lost by rebuilding Contents from it.
file(REMOVE_RECURSE "${BUNDLE_DIR}/Contents")
file(MAKE_DIRECTORY "${BUNDLE_DIR}/Contents")
file(COPY "${DIST_DIR}/" DESTINATION "${BUNDLE_DIR}/Contents")

if(PACKAGE_CONTENTS)
    file(COPY "${PACKAGE_CONTENTS}" DESTINATION "${BUNDLE_DIR}")
    get_filename_component(_pc_name "${PACKAGE_CONTENTS}" NAME)
    if(NOT _pc_name STREQUAL "PackageContents.xml")
        file(RENAME "${BUNDLE_DIR}/${_pc_name}" "${BUNDLE_DIR}/PackageContents.xml")
    endif()
else()
    file(REMOVE "${BUNDLE_DIR}/PackageContents.xml")
endif()

# --- report -----------------------------------------------------------------
set(_dylib "${BUNDLE_DIR}/Contents/${ADDIN_NAME}.dylib")
set(_dll "${BUNDLE_DIR}/Contents/${ADDIN_NAME}.dll")
set(_missing "")

message(STATUS "${ADDIN_NAME} ${VERSION} -> ${BUNDLE_DIR}")

if(EXISTS "${_dylib}")
    # An arm64-only dylib loads fine on the build machine and silently fails on
    # every Intel Mac, so the architectures are stated rather than assumed.
    execute_process(COMMAND lipo -info "${_dylib}"
                    OUTPUT_VARIABLE _archs OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _lipo_rc)
    if(_lipo_rc EQUAL 0)
        message(STATUS "  macOS   ${ADDIN_NAME}.dylib -- ${_archs}")
    else()
        message(STATUS "  macOS   ${ADDIN_NAME}.dylib")
    endif()
else()
    list(APPEND _missing "${ADDIN_NAME}.dylib (macOS)")
endif()

if(EXISTS "${_dll}")
    message(STATUS "  Windows ${ADDIN_NAME}.dll")
else()
    list(APPEND _missing "${ADDIN_NAME}.dll (Windows)")
endif()

if(_missing)
    string(REPLACE ";" "\n           " _missing_text "${_missing}")
    message(WARNING
        "Bundle is INCOMPLETE -- missing:\n           ${_missing_text}\n"
        "  The manifest declares supportedOS \"windows|mac\", so a submission "
        "needs both binaries. Build the other platform, copy its binary into\n"
        "  ${DIST_DIR}\n  and re-run this target.")
endif()

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
