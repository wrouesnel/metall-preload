# metall preload

This is an LD_PRELOAD library which interposes `malloc` to use the [LLNL metall](https://github.com/llnl/metall)
persistent memory allocator instead.

This allows for _enormous_ memory efficient allocations on RAM constrained hardware.