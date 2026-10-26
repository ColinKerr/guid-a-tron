#---------------------------------------------------------------------------------------------
# guid-a-tron — an independent CLI that gives a real iModel's history federationGuids.
#
# It links **nothing from iTwin**. SQLite comes from the public amalgamation, downloaded and
# checksummed by cmake/dependencies.cmake and compiled here with the flags a .bim needs
# (listed below, each with why). The LZMA SDK is vendored under
# third_party/lzma, copied from the iTwin source so that a rewritten changeset is byte-identical
# to one BeSQLite's own writer would produce. JSON is one downloaded header.
#
#
# Use scripts/build.sh rather than invoking cmake directly.
#---------------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.24)

project(GuidATron LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/dependencies.cmake)

# --- SQLite, from the public amalgamation ---------------------------------------------------
# The flags are PUBLIC, not PRIVATE: several of them decide what sqlite3.h declares — the session
# API is inside `#if defined(SQLITE_ENABLE_SESSION) && defined(SQLITE_ENABLE_PREUPDATE_HOOK)` —
# so a consumer compiled without them sees a different SQLite from the one it links.
#
# iTwin's own list is in `imodel-native/iModelCore/BeSQLite/SQLite/bentley-sqlite.c`. This is that
# list with the entries this tool has no use for removed, and each one's reason beside it.
add_library(guid_a_tron_sqlite STATIC "${SQLITE_DIR}/sqlite3.c")
target_include_directories(guid_a_tron_sqlite PUBLIC "${SQLITE_DIR}")
target_compile_definitions(guid_a_tron_sqlite PUBLIC
    # Mandatory to open a real .bim at all: a briefcase holds virtual tables, and a module that is
    # not compiled in makes its table unusable. Every file checked has `dgn_SpatialIndex USING
    # rtree(...)` and `dgn_fts_idx using fts5(...)`. FTS4 and GEOPOLY are here because iTwin's
    # build has them, so a file written by that build may use them.
    SQLITE_ENABLE_RTREE=1
    SQLITE_ENABLE_GEOPOLY=1
    SQLITE_ENABLE_FTS4=1
    SQLITE_ENABLE_FTS5=1

    # Limits, which have to be at least as generous as the build that wrote the file. 2200 columns
    # is the one that matters, it is taken from ECDb.
    SQLITE_MAX_COLUMN=2200
    SQLITE_MAX_LENGTH=2147483647
    SQLITE_MAX_VARIABLE_NUMBER=20000
    SQLITE_MAX_EXPR_DEPTH=3000

    # The session extension, which the public amalgamation carries and which only the unit tests
    # use: they author changesets with `sqlite3session_*` and read them back with
    # `sqlite3changeset_*`, so the inputs to the rewriter and the assertions about its output come
    # from the reference implementation of the format rather than from the code under test. The
    # tool's own commands parse and emit changeset bytes directly and never call the session API.
    SQLITE_ENABLE_SESSION=1
    SQLITE_ENABLE_PREUPDATE_HOOK=1
    SESSIONS_STRM_CHUNK_SIZE=65536

    # Fidelity with the build that wrote these files, so that what this tool writes is encoded and
    # enforced the same way. NULL_TRIM changes the on-disk record encoding (trailing NULL columns
    # are omitted); DEFAULT_FOREIGN_KEYS changes whether a write is checked.
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_ENABLE_NULL_TRIM=1
    SQLITE_DEFAULT_CACHE_SIZE=8000
    SQLITE_USE_URI=1
    SQLITE_ENABLE_MATH_FUNCTIONS=1
    SQLITE_ENABLE_COLUMN_METADATA=1)
# Deliberately not set, from the list `bentley-sqlite.c` carries:
#   SQLITE_ENABLE_ZIPVFS       a licensed extension whose source is not public; CloudSqlite only
#   SQLITE_OMIT_AUTOINIT       iTwin calls sqlite3_initialize() itself; letting SQLite do it keeps
#                              this tool's process setup to nothing at all
#   SQLITE_DEBUG, _IOTRACE     assertions and tracing, slow and unnecessary here
#   SQLITE_ENABLE_DBSTAT_VTAB, _STMTVTAB, _BYTECODE_VTAB, _NORMALIZE, _INTROSPECTION_PRAGMAS
#                              introspection for a SQL console; nothing in a .bim refers to them
#   SQLITE_OMIT_DEPRECATED     removes declarations for no gain here
#   SQLITE_API=<visibility>    DLL export plumbing; this is a static library
# Also absent: `shathree.c` and `blockcachevfs.c`, which iTwin compiles beside the amalgamation.
#
# Bentley's copy of the amalgamation differs from this one in exactly three ways, measured by
# diffing the two files: two extra lines in the header comment, a SOURCE_ID ending `alt1` instead
# of `9bcc`, and one functional change — `if( (rc&0xff)==SQLITE_CONSTRAINT )` where the public
# source has `if( rc==SQLITE_CONSTRAINT )`, inside `sqlite3changeset_apply`'s constraint-retry
# path next to `sessionRetryConstraints`. Nothing in this tool applies a changeset in anger, and a
# conflict-free apply never reaches that line. See ../IMPLEMENTATION.md.
target_compile_options(guid_a_tron_sqlite PRIVATE -w)
find_package(Threads REQUIRED)
target_link_libraries(guid_a_tron_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# --- The LZMA SDK, vendored from the iTwin source -------------------------------------------
# _7ZIP_ST is what iTwin's own lzma manifest passes on every platform but Windows, and it is why
# the multi-threading translation units are not here.
add_library(guid_a_tron_lzma STATIC
    third_party/lzma/Alloc.c
    third_party/lzma/LzmaLib.c
    third_party/lzma/LzmaEnc.c
    third_party/lzma/LzmaDec.c
    third_party/lzma/Lzma2Enc.c
    third_party/lzma/Lzma2Dec.c
    third_party/lzma/LzFind.c)
target_include_directories(guid_a_tron_lzma PUBLIC third_party/lzma)
target_compile_definitions(guid_a_tron_lzma PUBLIC _7ZIP_ST)
target_compile_options(guid_a_tron_lzma PRIVATE -w)

# --- The tool -------------------------------------------------------------------------------
add_library(guid_a_tron_core STATIC
    src/Sqlite.cpp
    src/Sha1.cpp
    src/Guid.cpp
    src/Files.cpp
    src/Lzma.cpp
    src/ChangesetFile.cpp
    src/ChangesetRewrite.cpp
    src/GuidATron.cpp)
target_include_directories(guid_a_tron_core PUBLIC src "${JSON_DIR}")
target_link_libraries(guid_a_tron_core PUBLIC guid_a_tron_sqlite guid_a_tron_lzma)

add_executable(guid-a-tron src/main.cpp)
target_link_libraries(guid-a-tron PRIVATE guid_a_tron_core)

add_executable(guid-a-tron-tests tests/tests.cpp)
target_link_libraries(guid-a-tron-tests PRIVATE guid_a_tron_core)

enable_testing()
add_test(NAME guid-a-tron-tests COMMAND guid-a-tron-tests)
