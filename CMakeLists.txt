cmake_minimum_required(VERSION 3.20)
include(FetchContent)

project(metall_preload LANGUAGES CXX)

# CMake 3.30 dropped its own FindBoost; use Boost's CMake config where possible.
if (POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# Metall is header-only but needs Boost headers (no compiled Boost libraries).
find_package(Boost 1.80 REQUIRED)

# Just install Metall header files and avoid building executables in the repository.
set(JUST_INSTALL_METALL_HEADER TRUE)
# A pinned source tarball, so builds need neither git nor a moving branch.
FetchContent_Declare(
        Metall
        URL https://github.com/LLNL/metall/archive/3aa4b9a5e2f3ac7836beba755b818726f9665440.tar.gz
        URL_HASH SHA256=54cacd8073635773107ebb821beecd1a9b614331a74d287e64fa3e5c470fad5f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(Metall)

find_package(Threads REQUIRED)

# Main library
add_library(metall_preload SHARED metall-preload.cpp)
target_link_libraries(metall_preload PRIVATE Metall::Metall Boost::headers Threads::Threads ${CMAKE_DL_LIBS})

# The whole segment is created up front as sparse files (see metall-preload.cpp),
# so use large blocks to keep the number of files and descriptors small.
target_compile_definitions(metall_preload PRIVATE METALL_SEGMENT_BLOCK_SIZE=\(1ULL<<35ULL\))

# Only the interposed allocator entry points are exported.
set_target_properties(metall_preload PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
target_link_options(metall_preload PRIVATE
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map
        -Wl,-z,nodelete
        -Wl,--no-undefined)

# Inside nix, the dynamic linker binds our libstdc++ dependency to the one nix
# was built with, and in every other program to the system's. Linking the C++
# runtime statically (with its symbols kept local by exports.map) means the
# library only needs glibc, so one build works with any nix version.
option(METALL_PRELOAD_STATIC_RUNTIME "Link libstdc++ and libgcc statically" ON)
if (METALL_PRELOAD_STATIC_RUNTIME)
    target_link_options(metall_preload PRIVATE -static-libstdc++ -static-libgcc)
endif ()
