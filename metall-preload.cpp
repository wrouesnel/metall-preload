#include <atomic>
#include <iostream>
#include <cstdint>
#include <dlfcn.h>

#include <boost/filesystem.hpp>
#include <metall/metall.hpp>

namespace {
    thread_local std::int64_t busy = 0;
    std::atomic_bool initialized{false};
    boost::filesystem::path diskCache;
    metall::manager *manager;
    // Track allocations which went to real malloc vs metall
    // std::set<void*> allocs;
    std::map<void*,size_t> metall_allocs;

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
} // namespace

void __attribute__((constructor)) startup() {
    std::cout << "Metall-Preload: Memory at " << diskCache.c_str() << "\n" ;
}
//
// void __attribute__((destructor)) shutdown() {
//     std::cout << "Metall-Preload: Shutdown\n";
//     if (!diskCache.empty())
//     {
//         boost::filesystem::remove_all(diskCache);
//     }
// }

extern "C" void* malloc(size_t size) {
    static auto* next
        = reinterpret_cast<decltype(malloc)*>(dlsym(RTLD_NEXT, "malloc"));

    if (!initialized || busy > 0) {
        void* ptr = next(size);
        // track the allocation for dispatch
        // allocs.insert(ptr);
        return ptr;
    }

    ++busy;
    void* ptr = manager->allocate(size);
    metall_allocs[ptr] = size;
    --busy;
    return ptr;
}

extern "C" void free(void* ptr) {
    static auto* next
        = reinterpret_cast<decltype(free)*>(dlsym(RTLD_NEXT, "free"));

    if (!initialized || busy > 0 /*|| allocs.contains(ptr)*/) {
        // track the allocation for dispatch
        // allocs.erase(ptr);
        return next(ptr);
    }

    ++busy;
    // free the block
    manager->deallocate(ptr);
    metall_allocs.erase(ptr);
    --busy;
}

extern "C" void* realloc(void *ptr, size_t new_size) {
    static auto* next
        = reinterpret_cast<decltype(realloc)*>(dlsym(RTLD_NEXT, "realloc"));

    if (!initialized || busy > 0 /*|| allocs.contains(ptr)*/) {
        void* nptr = next(ptr, new_size);
        return nptr;
    }

    ++busy;
    void* nptr = manager->allocate(new_size);
    // metall_allocs[nptr] = new_size;
    // get the existing allocation
    size_t old = metall_allocs[ptr];
    // copy it to the new location
    memcpy(nptr, ptr, old);
    // free the old block
    manager->deallocate(ptr);
    // metall_allocs.erase(ptr);
    --busy;
    return ptr;
}