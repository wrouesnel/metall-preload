cmake_minimum_required(VERSION 3.20)
include(FetchContent)

project(metall_preload LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# Metall is header-only but needs Boost headers (no compiled Boost libraries).
find_package(Boost 1.80 REQUIRED)

# Just install Metall header files and avoid building executables in the repository.
set(JUST_INSTALL_METALL_HEADER TRUE)
FetchContent_Declare(
        Metall
        GIT_REPOSITORY https://github.com/LLNL/metall.git
        GIT_TAG 3aa4b9a5
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
