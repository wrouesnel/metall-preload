#include <atomic>
#include <iostream>

#include <boost/filesystem.hpp>
#include <metall/metall.hpp>

namespace {
    //thread_local std::int64_t busy = 0;
    std::atomic_bool initialized{false};
    boost::filesystem::path diskCache;
    metall::manager *manager;
} // namespace

const struct Initialization {
    Initialization() {
        diskCache = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("metall-preload-%%%%-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(diskCache);
        manager = new metall::manager(metall::create_only, diskCache.c_str());

        initialized = true;
    }

    ~Initialization() {
        initialized = false;
        if (!diskCache.empty())
        {
            boost::filesystem::remove_all(diskCache);
        }
    }
} _;

extern "C" void* malloc(size_t size) {
    return manager->allocate(size);
}

extern "C" void free(void* ptr) {
    return manager->deallocate(ptr);
}
