cmake_minimum_required(VERSION 4.2)
include(FetchContent)

project(metall_preload LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(Boost 1.80 COMPONENTS filesystem)
if (NOT Boost_FOUND)
    message(STATUS "Download or search previously downloaded Boost using FetchContent")
    FetchContent_Declare(Boost
            URL https://boostorg.jfrog.io/artifactory/main/release/1.80.0/source/boost_1_80_0.tar.bz2)
    FetchContent_MakeAvailable(Boost)
    find_package(Boost 1.80 REQUIRED REQUIRED COMPONENTS filesystem)
endif ()

# Download and setup Metall.
if (NOT BUILD_C)
    # Just install Metall header files and avoid building executables in the repository.
    set(JUST_INSTALL_METALL_HEADER TRUE)
endif ()
FetchContent_Declare(
        Metall
        GIT_REPOSITORY https://github.com/LLNL/metall.git
        GIT_TAG master
)
FetchContent_MakeAvailable(Metall)

# Main library
add_library(metall_preload SHARED metall-preload.cpp)

# Metall CXX API
find_package(Threads)
target_include_directories(metall_preload PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(metall_preload PRIVATE stdc++fs)
target_link_libraries(metall_preload PRIVATE Threads::Threads)
target_link_libraries(metall_preload PRIVATE Metall::Metall)

# Our boost usage
target_link_libraries(metall_preload PRIVATE Boost::filesystem)