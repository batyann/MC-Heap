# MC-Heap
Heap allocator (malloc, free, ...) for microcontrollers, and more (should work everywhere)

MC-Heap is a heap allocator (malloc, free, ...) that was designed for microcontrollers (currently running on ARM Cortex M4), but it can be used anywhere - I use it for testing on a macbook. The main limitation when not used on a microcontroller is that it's not been designed to grow dynamically.

MC-Heap is *fast and predictable*: most operations are 0(1). A malloc takes ~180 cycles typically (armv7em), regardless of the size of the allocation, the size of the heap, and the number of allocations already performed. Same goes for free().

MC-Heap is *efficient*: it uses only ~1.5% of the heap size for its internal book-keeping.

MC-Heap is *small*: less than 1000 LOCs.

MC-Heap is *secure*: it allows you to query how much data can be accessed from a given pointer, provided the pointer if from within an allocated memory block.

e.g.

    char * x = MC_HEAP_Alloc(256); /* we assume we don't get NULL */
    U32 avail = MC_Heap_Avail(&x[16]); /* will return 240 */
     
  This could be used for ex. in a custom memcpy function to double check if enough data can be read and written from/to passed pointers to prevent buffer overflows.

MC-Heap allocations are always rounded to 16 bytes. Allocating 1 bytes will give you 16, allocating 1000 bytes will give you 1008.

MC-Heap allocations are always aligned on the largest nibble of the size: when size is 0x00**1**002FF, the returned memory block is aligned on 0x00100000; when size is 0x0000**F**FF0, the returned memory block is aligned on 0x00001000
    
MC-Heap uses a best-fit allocation.

MC-Heap automatically coalesces memory blocks at free() time: no need to run a coalescing task on a regular basis.
