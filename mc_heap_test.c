/*
 * Copyright (c) 2010-2021 Yann Poupet
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIEDi
 *    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *    ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "mc_heap.c"
/* -------------------------------------------------------------------------- */
static void __attribute((unused)) test_alloc_inc(heap *H,U32 step)
{
   U32 allocated = 0,idx = 0,cur_size = 0,i;
   void **pointers;
   bool up = true;
   pointers = (void**)malloc((H->hsize / step) * sizeof(void*));
   ASSERT(pointers != NULL);

   while (true) {
      if (up) {
         cur_size += step;
      }

      if (0 == cur_size) {
         break;
      }

      pointers[idx] = heap_alloc(H,cur_size);

      if (pointers[idx] == NULL) {
         up = false;
         cur_size -= step;
      } else {
         memset(pointers[idx],0xA5,8); /* overwrite chunk's next & prev */
         allocated += cur_size;
         idx++;
         ASSERT(idx < H->hsize / step);
      }
   }

   for (i = 0; i < idx; i++) {
      heap_free(H,pointers[i]);
      ASSERT(heap_get_address_status(H,pointers[i]) == eSTATUS_FREE);
   }

   free(pointers);
   return;
}
/* -------------------------------------------------------------------------- */
static void test_mixed_sizes(heap*const H)
{
   /* allocate multiple sets of 16,32,64,128,16 bytes */
   U32 const set_size = 5;
   U32 const set_count = 1024*1024;
   void**pointers = malloc(set_count * set_size * sizeof(void*));
   ASSERT(NULL != pointers);
   for (U32 i = 0; i < set_count; i++) {
      pointers[(i*set_size) + 0] = heap_alloc(H, 16);
      ASSERT(NULL != pointers[(i*set_size) + 0]);
      pointers[(i*set_size) + 1] = heap_alloc(H, 32);
      ASSERT(NULL != pointers[(i*set_size) + 1]);
      pointers[(i*set_size) + 2] = heap_alloc(H, 64);
      ASSERT(NULL != pointers[(i*set_size) + 2]);
      pointers[(i*set_size) + 3] = heap_alloc(H, 128);
      ASSERT(NULL != pointers[(i*set_size) + 3]);
      pointers[(i*set_size) + 4] = heap_alloc(H, 16);
      ASSERT(NULL != pointers[(i*set_size) + 4]);
   }
   PRINTF("allocated %u times 16+32+64+128+16 bytes\n", set_count);
   for (U32 i = 0; i < set_count; i++) {
      /* free in "random" order */
      heap_free(H, pointers[(i*set_size) + ((i + 0) % set_size)]);
      heap_free(H, pointers[(i*set_size) + ((i + 1) % set_size)]);
      heap_free(H, pointers[(i*set_size) + ((i + 2) % set_size)]);
      heap_free(H, pointers[(i*set_size) + ((i + 3) % set_size)]);
      heap_free(H, pointers[(i*set_size) + ((i + 4) % set_size)]);
   }
   PRINTF("freed them all.\n");
   /* another test */
   {
      void*a = heap_alloc(H, 16+256+4096);
      void*b = heap_alloc(H, 16);
      ASSERT(NULL != a);
      ASSERT(NULL != b);
      heap_free(H, a);
      heap_free(H, b);
   }

   /* double check the heap is completely freed */
   void*all = heap_alloc(H, H->hsize);
   ASSERT(NULL != all);
   heap_free(H, all);
   free(pointers);
}
/* -------------------------------------------------------------------------- */
static void test_alloc_all(heap*const H, U32 const elem_size)
{
   U32 size = (elem_size + BASE_SIZE_MIN - 1) & ~(BASE_SIZE_MIN - 1);
   U32 i;
   U32 alloc_count = 0;
   void **pointers;
   /* e.g. we cannot allocate (H->size/48) times 48B because a 256B chunk can
    * contain 5 times 48B allocations with 16B remaining.
    * If for example we want to allocate items of 4097 bytes, they all have to
    * be aligned on 4096 bytes. Therefore, only 8 allocs of 4096 bytes can be
    * made per chunk of 64kB
    */
   static const U32 bslist[MAIN_BASE_SIZE_COUNT] = {
                           16, 256, 4096, 65536, 1048576, 16777216, 268435456 };
   U32 align_idx = 0;
   for (U32 x = 0; x < MAIN_BASE_SIZE_COUNT; x++) {
      if (size < bslist[x]) {
         ASSERT(0 != x);
         align_idx = x - 1;
         break;
      }
   }
   U32 const next_bs = bslist[align_idx + 1];
   alloc_count = (next_bs / ((size + bslist[align_idx] - 1) &
                                   ~(bslist[align_idx] - 1))) * (H->hsize / next_bs);
   pointers = (void**)malloc(alloc_count * sizeof(void*));
   ASSERT(pointers != NULL);
   for (i = 0; i < alloc_count; i++) {
      pointers[i] = heap_alloc(H,elem_size);
      ASSERT(NULL != pointers[i]);
   #ifdef DEBUG_BUILD
      memset(pointers[i],0xA5,elem_size); /* overwrite chunk's next & prev */
   #endif
      ASSERT(heap_get_alloc_size(H,pointers[i]) == size);
      ASSERT(heap_get_address_status(H,pointers[i]) == eSTATUS_ALLOC_HEAD);
   }
   ASSERT(heap_alloc(H,elem_size) == NULL);
   PRINTF("Allocated %u times %u bytes.\n",alloc_count,elem_size);
   for (i = 0; i < alloc_count; i++) {
      heap_free(H,pointers[i]);
      ASSERT(heap_get_address_status(H,pointers[i]) == eSTATUS_FREE);
   }
   PRINTF("Freed them all.\n");
   free(pointers);
   return;
}
/* -------------------------------------------------------------------------- */
#ifdef MAX_PERF
void *test_alloc(void *arg)
{
   U32 i;
   heap *H = (heap*)arg;

#if 1
   for (i = 0; i < (6 * 1024 * 1024); i++)
   {
      size_t size = (i & 0x3F) + 1;
      (void)heap_alloc(H,size);
   }

   return NULL;
#endif

   for (i = 0; i < H->hsize >> 4; i++)
   {
      (void)heap_alloc(H,16);
   }

#if 0
   PRINTF("Allocated %u times 16 bytes.\n",i);
#endif

#if 0
   for (i = 0; i < H->size >> 4; i++)
   {
      heap_free(H,H->hdata + (i << 4));
   }
#endif

   return NULL;
}
#endif
/* -------------------------------------------------------------------------- */
static void*memalign(U32 const alignmt, U32 const size)
{
   void*ptr;
   if (0 != posix_memalign(&ptr, alignmt, size)) {
      ptr = NULL;
   }
   return ptr;
}
/* -------------------------------------------------------------------------- */
   static U32 const base_size_list[BASE_SIZES_COUNT] = {
      0x00000010U,0x00000020U,0x00000030U,0x00000040U,0x00000050U,0x00000060U,0x00000070U,
      0x00000080U,0x00000090U,0x000000A0U,0x000000B0U,0x000000C0U,0x000000D0U,0x000000E0U,
      0x000000F0U,0x00000100U,0x00000200U,0x00000300U,0x00000400U,0x00000500U,0x00000600U,
      0x00000700U,0x00000800U,0x00000900U,0x00000A00U,0x00000B00U,0x00000C00U,0x00000D00U,
      0x00000E00U,0x00000F00U,0x00001000U,0x00002000U,0x00003000U,0x00004000U,0x00005000U,
      0x00006000U,0x00007000U,0x00008000U,0x00009000U,0x0000A000U,0x0000B000U,0x0000C000U,
      0x0000D000U,0x0000E000U,0x0000F000U,0x00010000U,0x00020000U,0x00030000U,0x00040000U,
      0x00050000U,0x00060000U,0x00070000U,0x00080000U,0x00090000U,0x000A0000U,0x000B0000U,
      0x000C0000U,0x000D0000U,0x000E0000U,0x000F0000U,0x00100000U,0x00200000U,0x00300000U,
      0x00400000U,0x00500000U,0x00600000U,0x00700000U,0x00800000U,0x00900000U,0x00A00000U,
      0x00B00000U,0x00C00000U,0x00D00000U,0x00E00000U,0x00F00000U,0x01000000U,0x02000000U,
      0x03000000U,0x04000000U,0x05000000U,0x06000000U,0x07000000U,0x08000000U,0x09000000U,
      0x0A000000U,0x0B000000U,0x0C000000U,0x0D000000U,0x0E000000U,0x0F000000U,0x10000000U,
      0x20000000U,0x30000000U,0x40000000U,0x50000000U,0x60000000U,0x70000000U,0x80000000U,
      0x90000000U,0xA0000000U,0xB0000000U,0xC0000000U,0xD0000000U,0xE0000000U,0xF0000000U,
   };
/* -------------------------------------------------------------------------- */
static void closest_base_size_index_UT(void)
{
   U32 idx = 0;
   /* very slow */
   for (U32 i = 0; i < BASE_SIZE_MAX; i++) {
      ASSERT(closest_base_size(i) == base_size_list[idx]);
      if (i == base_size_list[idx]) ++idx;
   }
}
/* -------------------------------------------------------------------------- */
static void base_size_to_index_UT(void)
{
   for (U32 i = 0; i < BASE_SIZES_COUNT; i++) {
      #if 0
      PRINTF("base_size_to_index(%u)=%u\n",base_size_list[i],
                        base_size_to_index(base_size_list[i]));
      #endif
      ASSERT(i == base_size_to_index(base_size_list[i]));
   }
}
/* -------------------------------------------------------------------------- */
static void base_size_from_index_UT(void)
{
   for (U32 i = 0; i < BASE_SIZES_COUNT; i++) {
      ASSERT(base_size_list[i] == base_size_from_index(i));
   }
}
/* -------------------------------------------------------------------------- */
static void __attribute((unused)) UT(void)
{
   PRINTF("closest_base_size_index_UT\n");
   closest_base_size_index_UT();
   PRINTF("base_size_to_index_UT\n");
   base_size_to_index_UT();
   PRINTF("base_size_from_index_UT\n");
   base_size_from_index_UT();
}
/* -------------------------------------------------------------------------- */
int main(int argc,char *argv[])
{
#if 0
   U32 const SZ = 18*1024;
   void*const mem = memalign(64*1024, SZ);
   heap_destroy(heap_create(mem, SZ));
   free(mem);
   return 0;
#endif
#if 0
   UT();
   PRINTF("UT OK.\n");
   return 0;
#endif
#if 1
   const U32 SIZE = 256 * 1024 * 1024;
   void *data = memalign(SIZE, SIZE);
   heap *H1 = heap_create(data, SIZE);

   #if 0
   void*ptr = heap_alloc(H1, (16*4096)+(15*256)+16);
   PRINTF("ptr=%p\n", ptr);
   heap_free(H1, ptr);
   PRINTF("...\n");
   void*ptr1 = heap_alloc(H1, 16);
   PRINTF("ptr1=%p\n", ptr1);
   void*ptr2 = heap_alloc(H1, 16);
   PRINTF("ptr2=%p\n", ptr2);
   heap_free(H1, ptr1);
   PRINTF("...\n");
   heap_free(H1, ptr2);

   return 0;
   #endif
   #if 0
   test_alloc_all(H1, 16);
   test_alloc_all(H1, 16);
   test_alloc_all(H1, 16);
   test_alloc_all(H1, 16);
   test_alloc_all(H1, 16);
   test_alloc_all(H1, 16);
   test_alloc_all(H1, 16);
   test_alloc_all(H1, 16);
   return 0;
   #endif
   #if 1
   {
      #if 0
      void*big = heap_alloc(H1, 4096+16);   
      while (NULL != heap_alloc(H1, 16));
      heap_free(H1, big);
      return 0;
      #endif
   }
   test_mixed_sizes(H1);
   test_alloc_all(H1, (16*4096)+(15*256)+16);
   test_alloc_all(H1, 16);
   test_alloc_all(H1, 24);
   test_alloc_all(H1, 32);
   test_alloc_all(H1, 48);
   test_alloc_all(H1, 61);
   test_alloc_all(H1, 65);
   test_alloc_all(H1, 79);
   test_alloc_all(H1, 80);
   test_alloc_all(H1, 81);
   #endif
   test_alloc_all(H1, 16+256+4096);
   test_alloc_all(H1, 345);
   //test_alloc_inc(H1,16);
#else
  #if 0
   #define MAX_THREADS (1)
   U32 T;
   const U32 SIZE = 128 * 1024 * 1024;
   void *data = memalign(SIZE,SIZE);
   pthread_t Threads[MAX_THREADS];
   heap *H[MAX_THREADS];

   for (T = 0; T < MAX_THREADS; T++)
   {
      H[T] = heap_create(data, SIZE);

      if (pthread_create(&Threads[T],NULL,test_alloc,(void*)H[T]) != 0)
      {
         perror("pthread_create");
         break;
      }
   }

   while (T > 0)
   {
      (void)pthread_join(Threads[--T],NULL);
   }
  #else
   const U32 SIZE = 256 * 1024 * 1024;
   void *data = memalign(SIZE,SIZE);
   heap *H1 = heap_create(data, SIZE);
   test_alloc(H1);
  #endif
#endif

   return 0;
}
