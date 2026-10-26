#---------------------------------------------------------------------------------------------
# The two dependencies this tool downloads, pinned by URL and SHA-256.
#
# SQLite comes from the public amalgamation. 
# The release is the same one imodel-native vendors (3.53.4); the flags are applied
# in CMakeLists.txt, which says why each one is there.
#
# nlohmann/json is one header.
#
# The LZMA SDK is **not** here: it is vendored under third_party/lzma, because the encoder's output
# differs between SDK versions and this tool's output is compared byte-for-byte against what
# BeSQLite's writer produces. See third_party/lzma/README.md.
#
# Downloads land in third_party/download (gitignored) and are reused, so only a first build needs
# the network. GUID_A_TRON_DEPS_DIR relocates the cache, e.g. to a shared or pre-populated one.
#---------------------------------------------------------------------------------------------

set(GUID_A_TRON_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/download"
    CACHE PATH "Where the pinned SQLite and nlohmann/json downloads are cached")

set(SQLITE_VERSION "3530400")  # 3.53.4, the release imodel-native vendors
set(SQLITE_URL "https://sqlite.org/2026/sqlite-amalgamation-${SQLITE_VERSION}.zip")
set(SQLITE_SHA256 "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d")

set(JSON_VERSION "3.11.3")
set(JSON_URL "https://github.com/nlohmann/json/releases/download/v${JSON_VERSION}/json.hpp")
set(JSON_SHA256 "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6")

file(MAKE_DIRECTORY "${GUID_A_TRON_DEPS_DIR}")

# --- SQLite ---------------------------------------------------------------------------------
set(SQLITE_DIR "${GUID_A_TRON_DEPS_DIR}/sqlite-amalgamation-${SQLITE_VERSION}")
if(NOT EXISTS "${SQLITE_DIR}/sqlite3.c")
    set(_zip "${GUID_A_TRON_DEPS_DIR}/sqlite-amalgamation-${SQLITE_VERSION}.zip")
    if(NOT EXISTS "${_zip}")
        message(STATUS "guid-a-tron: downloading ${SQLITE_URL}")
        file(DOWNLOAD "${SQLITE_URL}" "${_zip}" EXPECTED_HASH "SHA256=${SQLITE_SHA256}"
             STATUS _status TLS_VERIFY ON SHOW_PROGRESS)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _status 1 _message)
            file(REMOVE "${_zip}")
            message(FATAL_ERROR "guid-a-tron: cannot download the SQLite amalgamation: ${_message}\n"
                                "Fetch ${SQLITE_URL} by hand into ${GUID_A_TRON_DEPS_DIR} and build again.")
        endif()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${GUID_A_TRON_DEPS_DIR}")
endif()
if(NOT EXISTS "${SQLITE_DIR}/sqlite3.c")
    message(FATAL_ERROR "guid-a-tron: ${SQLITE_DIR}/sqlite3.c is missing after extraction")
endif()

# --- nlohmann/json --------------------------------------------------------------------------
set(JSON_DIR "${GUID_A_TRON_DEPS_DIR}/nlohmann-json-${JSON_VERSION}")
if(NOT EXISTS "${JSON_DIR}/nlohmann/json.hpp")
    message(STATUS "guid-a-tron: downloading ${JSON_URL}")
    file(DOWNLOAD "${JSON_URL}" "${JSON_DIR}/nlohmann/json.hpp" EXPECTED_HASH "SHA256=${JSON_SHA256}"
         STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _status 1 _message)
        file(REMOVE "${JSON_DIR}/nlohmann/json.hpp")
        message(FATAL_ERROR "guid-a-tron: cannot download nlohmann/json: ${_message}\n"
                            "Fetch ${JSON_URL} into ${JSON_DIR}/nlohmann/ by hand and build again.")
    endif()
endif()
