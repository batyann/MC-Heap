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
#include "heap.h"
#define _XOPEN_SOURCE 600
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>

typedef uint32_t U32;
typedef uint16_t U16;
typedef uint8_t  U8 ;

#define PRINTF(...) printf(__VA_ARGS__)

#ifdef MAX_PERF
   #define ASSERT(x)  { }
#else
   #define ASSERT(x) assert(x)
   #define DEBUG_BUILD
#endif

#define BASE_SIZES_COUNT (105)
#define BASE_SIZE_MAX (0xF0000000U)
#define BASE_SIZE_MIN (0x00000010U)

#define CLZ(x) __builtin_clz(x)
#define CTZ(x) __builtin_ctz(x)

#define MAIN_BASE_SIZE_COUNT 7U

#define unlikely(x) __builtin_expect(!!(x), 0)

/* -------------------------------------------------------------------------- */
#ifndef MAX_PERF
static const bool is_base_size(U32 const size)
{
   U32 const clz = CLZ(size) & 0x1CU;
   return ((0xF0000000U >> clz) & size) == size;
}
#endif
/* -------------------------------------------------------------------------- */
static const U32 closest_base_size(U32 const from)
{
   if (unlikely(from < BASE_SIZE_MIN)) {
      return BASE_SIZE_MIN;
   }
   if (unlikely(from > BASE_SIZE_MAX)) {
      return 0;
   }
   ASSERT(0 != from);
   U32 const clz = CLZ(from) & 0x1CU;
   ASSERT(clz < 28);
   U32 const lsbits = 0x0FFFFFFFU >> clz;
   return ((from + lsbits) & ~lsbits);
}
/* -------------------------------------------------------------------------- */
static const U32 base_size_to_index(U32 const size)
{
   ASSERT(0 != size);
   ASSERT(is_base_size(size));
   U32 const ctz = CTZ(size) & 0x1CU;
   U32 const tmp = (ctz >> 2);
   return (tmp << 4) - tmp + (size >> ctz) - 16;
}
/* -------------------------------------------------------------------------- */
static const U32 base_size_from_index(U32 const index)
{
   ASSERT(index < BASE_SIZES_COUNT);
   U32 const index_div15 = ((index << 7) + (index << 3) + index) >> 11; 
   U32 const index_rem15 = index - ((index_div15 << 4) - index_div15);
   return (index_rem15 + 1) << ((index_div15 + 1) << 2);
}
/* -------------------------------------------------------------------------- */
typedef enum {
   eSTATUS_FREE       = 0x02U,
   eSTATUS_ALLOC      = 0x00U,
   eSTATUS_ALLOC_HEAD = 0x01U,
   eSTATUS_SPLIT      = 0x03U,
 #ifdef DEBUG_BUILD
   eSTATUS_INVALID    = 0xFFU,
 #endif
} eChunkStatus;

#define ALL_FREE  0xAAAAAAAAU
#define ALL_ALLOC 0x00000000U

typedef struct _chunk {
   struct _chunk *prev;
   struct _chunk *next;
} chunk;
_Static_assert(sizeof(chunk) <= 16, "FIXME");

#define HEADS_BITS_SIZE (((BASE_SIZES_COUNT + 31) >> 5))
struct heap_st {
   U32 headsbits[HEADS_BITS_SIZE];
   U32*bitfield[MAIN_BASE_SIZE_COUNT];
   U8 *hdata;
   U32 hsize;
   U32 hdcnt;
   U32 bscnt;
   chunk*heads[0];
};
static U32 next_available_head_index(heap*const h, U32 const size)
{
   U32 const next_size = closest_base_size(size);
   if (unlikely(0 == next_size)) {
      return BASE_SIZES_COUNT;
   }
   U32 const index = base_size_to_index(next_size);
   U32 const start_idx = index >> 5;
   U32 x = h->headsbits[start_idx] << (index & 0x1FU);
   if (0 == x) {
      U32 const idx = start_idx << 5;
      ASSERT(start_idx <= 2);
      x = h->headsbits[start_idx + 1];
      if (0 == x) {
         ASSERT(start_idx <= 1);
         x = h->headsbits[start_idx + 2];
         if (0 == x) {
            ASSERT(0 == start_idx);
            x = h->headsbits[start_idx + 3];
            ASSERT(0 != x);
            return idx + 96 + CLZ(x);
         }
         return idx + 64 + CLZ(x);
      }
      return idx + 32 + CLZ(x);
   }
   return index + CLZ(x);
}
/* -------------------------------------------------------------------------- */
static inline U32 count_leading_allocs(U32 const bits)
{
   return CLZ(bits) >> 1;
}
/* -------------------------------------------------------------------------- */
static eChunkStatus chunk_get_status(U32 const*const bf, U32 const idx)
{
   U32 const sub = ~idx & 15u;
   return (bf[idx >> 4] >> (sub << 1)) & 0x03U;
}
/* -------------------------------------------------------------------------- */
/* function to grab the number of bytes available from a given pointer
 * provided it's from within a heap allocated buffer */
static U32 heap_get_alloc_size(heap const*const h, void const*const p)
{
   U8 const*const a = (__typeof(a))p;
   U32 const A = (__typeof(A))a;
   U8*const base = h->hdata;
   if (unlikely(a < base || a >= base + h->hsize || 0 != (A & 0x0FU))) {
      return 0;
   }
   U32 const reladdr = a - base;
   ASSERT(0 != A);
   U32 const ctz = CTZ(A);
   ASSERT(ctz >= 4);
   U32 lvl = (ctz >> 2) - 1;
   U32 shift = (lvl + 1) << 2, idx;
   for ( ;; --lvl, shift -= 4) {
      idx = reladdr >> shift;
      if (eSTATUS_ALLOC_HEAD == chunk_get_status(h->bitfield[lvl], idx)) {
         break;
      }
      if (unlikely(0 == lvl)) {
         return 0;
      }
      ASSERT(0 != lvl && shift >= 4);
   }
   U32 const sub = idx & 0x0FU;
   if (unlikely(15 == sub)) {
      return 1 << shift;
   }
   U32 const bits = h->bitfield[lvl][idx >> 4] << ((sub + 1) << 1);
   if (unlikely(0 == bits)) {
      return (16 - sub) << shift;
   }
   U32 allocs = count_leading_allocs(bits) + 1;
   ASSERT(sub + allocs < 16);

   U32 size = allocs << shift;

   while (eSTATUS_SPLIT == chunk_get_status(h->bitfield[lvl], idx + allocs)) {
      ASSERT(0 != lvl && shift >= 4);
      lvl -= 1;
      shift -= 4;
      idx = (reladdr + size) >> shift;
      U32 const bf = h->bitfield[lvl][idx >> 4];
      ASSERT(0 != bf);
      allocs = count_leading_allocs(bf);
      size += allocs << shift;
   }
   return size;
}
/* -------------------------------------------------------------------------- */
#ifdef DEBUG_BUILD
static U32 heap_get_address_status_priv(heap const*const h, void const*const a,
                                       U32 const idx, eChunkStatus const prev_status)
{
   U8 const*const address = (__typeof(address))a;

   ASSERT(idx < h->bscnt);

   U32 const index = (address - h->hdata) >> ((idx + 1) << 2);
   eChunkStatus status = chunk_get_status(h->bitfield[idx],index);

   if (status == eSTATUS_FREE && idx < h->bscnt - 1) {
      return heap_get_address_status_priv(h,a,idx + 1,status);
   }

   if (status == eSTATUS_SPLIT) {
      ASSERT(idx != 0);
      return prev_status;
   }

   if (status == eSTATUS_ALLOC_HEAD &&
         ((address - h->hdata) & ((16 << (idx << 2)) - 1)) != 0) {
      status = eSTATUS_ALLOC;
   }

   return status;
}
/* -------------------------------------------------------------------------- */
eChunkStatus heap_get_address_status(heap const*const h, void const*const a)
{
   U8 const*const address = (__typeof(address))a;
   if (address < h->hdata || address >= h->hdata + h->hsize ||
         ((U32)address & (BASE_SIZE_MIN - 1)) != 0) {
      return eSTATUS_INVALID;
   }

   return heap_get_address_status_priv(h,a,0,eSTATUS_INVALID);
}
#endif
/* -------------------------------------------------------------------------- */
static U32 needed_bitfield_count(U32 const size, U32 const index)
{
   U32 const count = size >> ((index + 1) << 2);
   return (count + 15) >> 4;
}
/* -------------------------------------------------------------------------- */
static U32 total_bitfield_count(U32 const size)
{
   U32 cnt = 0;
   for (U32 i = 0; i < MAIN_BASE_SIZE_COUNT; i++) {
      cnt += needed_bitfield_count(size, i);
   }
   return cnt;
}
/* -------------------------------------------------------------------------- */
void heap_destroy(heap *h)
{
   ASSERT(h != NULL);
   free(h->bitfield[0]);
   free(h);
   return;
}
/* -------------------------------------------------------------------------- */
static inline void bf_set_b11(U32*const bf, U32 const index)
{
   U32 const sub = index & 0xFU;
   bf[index >> 4] |= 0xC0000000U >> (sub << 1);
}
/* -------------------------------------------------------------------------- */
static inline void bf_set_b00_multi(U32*const bf, U32 const index, U32 const cnt)
{
   ASSERT(0 != cnt && cnt <= 16);
   U32 const sub = index & 0xFU;
   ASSERT(sub + cnt <= 16);
   U32 const shf = 32 - (cnt << 1);
   U32 const msk = 0xFFFFFFFFU >> shf;
   ASSERT(shf >= (sub << 1));
   bf[index >> 4] &= ~(msk << (shf - (sub << 1)));
}
/* -------------------------------------------------------------------------- */
static inline void
bf_set_bxx_multi(U32*const bf, U32 const index, U32 const cnt, U32 const pattern)
{
   ASSERT(0 != cnt && cnt <= 16);
   U32 const sub = index & 0xFU;
   ASSERT(sub + cnt <= 16);
   U32 const shf = 32 - (cnt << 1);
   ASSERT(shf >= (sub << 1));
   U32 const msk = (0xFFFFFFFFU >> shf) << (shf - (sub << 1));
   U32 const idx = index >> 4;
   U32 const bit = bf[idx] & ~msk;
   bf[idx] = bit | (msk & pattern);
}
/* -------------------------------------------------------------------------- */
static inline void bf_set_b10_multi(U32*const bf, U32 const index, U32 const cnt)
{
   bf_set_bxx_multi(bf, index, cnt, 0xAAAAAAAAU);
}
/* -------------------------------------------------------------------------- */
static inline void bf_set_b01_multi(U32*const bf, U32 const index, U32 const cnt)
{
   bf_set_bxx_multi(bf, index, cnt, 0x55555555U);
}
/* -------------------------------------------------------------------------- */
static inline void bf_set_b01(U32*const bf, U32 const index)
{
   U32 const sub = index & 0xFU;
   bf[index >> 4] &= ~(0x80000000U >> (sub << 1));
   bf[index >> 4] |=   0x40000000U >> (sub << 1);
}
/* -------------------------------------------------------------------------- */
static inline void bf_set_free_multi(U32*const bf, U32 const index, U32 const cnt)
{
   bf_set_b10_multi(bf, index, cnt);
}
/* -------------------------------------------------------------------------- */
static inline void bf_set_split(U32*const bf, U32 const index)
{
   bf_set_b11(bf, index);
}
/* -------------------------------------------------------------------------- */
static void bf_set_alloc_multi(U32*const bf, U32 const index, U32 const cnt)
{
   bf_set_b00_multi(bf, index, cnt);
}
/* -------------------------------------------------------------------------- */
static void bf_set_alloc_head(U32*const bf, U32 const index)
{
   bf_set_b01(bf, index);
}
/* -------------------------------------------------------------------------- */
static void bf_set_alloc_head_multi(U32*const bf, U32 const index, U32 const cnt)
{
   bf_set_b01_multi(bf, index, cnt);
}
/* -------------------------------------------------------------------------- */
static void update_prev(heap const*const h, chunk*const c, chunk const*const p)
{
   c->prev = (__typeof(c->prev))p;

#ifdef DEBUG_BUILD
   if (NULL != p) {
      for (U32 i = 0; i < h->hdcnt; i++) {
         ASSERT(h->heads[i] != c);
      }
   }
#endif
}
/* -------------------------------------------------------------------------- */
static void update_next(heap const*const h, chunk*const c, chunk const*const n)
{
   c->next = (__typeof(c->next))n;
}
/* -------------------------------------------------------------------------- */
static void update_head(heap*const h, U32 const index, chunk const*const c)
{
   ASSERT(index < BASE_SIZES_COUNT);

   if (NULL != c) {
   #ifdef DEBUG_BUILD
      for (U32 i = 0; i < h->hdcnt; i++) {
         ASSERT(h->heads[i] != c);
      }
   #endif

      ASSERT(NULL == c->prev);
      h->headsbits[index >> 5] |=  (0x80000000U >> (index & 31));
   } else {
      h->headsbits[index >> 5] &= ~(0x80000000U >> (index & 31));
   }

   ASSERT(index < h->hdcnt);
   h->heads[index] = (chunk*)c;
}
/* -------------------------------------------------------------------------- */
#if 0
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_spinlock_t spl;
#endif
static void heap_lock(heap *h)
{
   #if 0
   pthread_mutex_lock(&mtx);
   pthread_spin_lock(&spl);
   #endif
}
static void heap_unlock(heap *h)
{
   #if 0
   pthread_mutex_unlock(&mtx);
   pthread_spin_unlock(&spl);
   #endif
}
/* -------------------------------------------------------------------------- */
/* Allocate! */
void*heap_alloc(heap*const h, U32 const sz)
{
   U32 lvl_needed_sz;
   const U32 base = (U32)h->hdata;

   if (unlikely(0 == sz)) {
      return NULL;
   }

   U32 needed_sz = (sz + BASE_SIZE_MIN - 1) & ~(BASE_SIZE_MIN - 1);

   heap_lock(h);

   U32 const index = next_available_head_index(h, needed_sz);
   ASSERT(index <= BASE_SIZES_COUNT);
   if (unlikely(index == BASE_SIZES_COUNT)) {
      return NULL;
   }
   U32 const found_sz = base_size_from_index(index);
   ASSERT(found_sz >= needed_sz);

   ASSERT(index < h->hdcnt);
   chunk*c = h->heads[index];
   ASSERT(NULL != c);

   if (NULL != c->next) {
      update_prev(h, c->next, NULL);
   }

   update_head(h, index, c->next);

   U32 const extra_sz = found_sz - needed_sz;

   U32 bs_level = (CTZ(found_sz) >> 2) - 1;
   ASSERT(bs_level < MAIN_BASE_SIZE_COUNT);
   U32 shift = (bs_level << 2) + 4;

   /* the combined number of iterations for both 'for' loops in this
    * function is 7 max, hence the 0(1) complexity */
   for (;; --bs_level, shift -= 4) {
      U32 const lvl_remain_sz = (extra_sz >> shift) & 0x0FU;

      if (0 != lvl_remain_sz) {
         U32 const head = (bs_level << 4) - bs_level + lvl_remain_sz - 1;
         ASSERT(head < h->hdcnt);
         chunk*const hd = h->heads[head];
         update_next(h, c, hd);
         update_prev(h, c, NULL);
         update_head(h, head, c);

         if (NULL != hd) {
            ASSERT(NULL == hd->prev);
            update_prev(h, hd, c);
         }

         c = (chunk*)((U8*)c + (lvl_remain_sz << shift));
      }

      lvl_needed_sz = (needed_sz >> shift) /* & 0x0FU */;
      if (0 != lvl_needed_sz) {
         break;
      }

      ASSERT(0 != bs_level);

      U32 const split = ((U32)c - base) >> shift;
      bf_set_split(h->bitfield[bs_level], split);
   }

   U32 main_bs = 1 << shift;
   ASSERT(is_base_size(main_bs));
   ASSERT(lvl_needed_sz < 16);

   void*const result = c;
   bf_set_alloc_head(h->bitfield[bs_level],((U32)c - base) >> shift);
   c = (chunk*)((U8*)c + main_bs);

   U32 const cnt = lvl_needed_sz - 1;
   if (0 != cnt) {
      bf_set_alloc_multi(h->bitfield[bs_level], ((U32)c - base) >> shift, cnt);
      c = (chunk*)((U8*)c + (main_bs * cnt));
   }

   needed_sz -= lvl_needed_sz << shift;
   if (0 != needed_sz && 0 != bs_level) {
      U32 const split = ((U32)c - base) >> shift;
      bf_set_split(h->bitfield[bs_level], split);
   }

   if (0 != bs_level) for (bs_level = bs_level - 1;; --bs_level) {
      shift -= 4;
      main_bs >>= 4;
      lvl_needed_sz = (needed_sz >> shift) /* & 0x0FU */;
      ASSERT(shift != 0);
      ASSERT(lvl_needed_sz < 16);
      ASSERT(is_base_size(main_bs));

      if (0 != lvl_needed_sz) {
         bf_set_alloc_multi(
               h->bitfield[bs_level], ((U32)c - base) >> shift, lvl_needed_sz);
         c = (chunk*)((U8*)c + (main_bs * lvl_needed_sz));
      }

      needed_sz -= lvl_needed_sz << shift;

      U32 const lvl_remain_sz = (extra_sz >> shift) & 0x0FU;
      if (0 != lvl_remain_sz) {
         U32 const head = (bs_level << 4) - bs_level + lvl_remain_sz - 1;
         chunk *new = c;

         if (0 != bs_level && 0 != needed_sz) {
            new = (chunk*)((U8*)new + main_bs);
         }

         ASSERT(head < h->hdcnt);
         chunk*const hd = h->heads[head];
         update_next(h, new, hd);
         update_prev(h, new, NULL);
         update_head(h, head, new);

         if (NULL != hd) {
            ASSERT(NULL == hd->prev);
            update_prev(h, hd, new);
         }
      }

      if (0 == needed_sz) {
         break; 
      }
      if (0 == bs_level) {
         break;
      }

      U32 const split = ((U32)c - base) >> shift;
      bf_set_split(h->bitfield[bs_level], split);
   }

   heap_unlock(h);
   return result;
}
/* -------------------------------------------------------------------------- */
static inline void
chunk_remove_from_list(heap*const h, chunk const*const c, U32 const h_idx)
{
   if (NULL != c->next) {
      update_prev(h, c->next, c->prev);
   }

   if (NULL != c->prev) {
      update_next(h, c->prev, c->next);
   } else {
      ASSERT(h_idx < h->hdcnt);
      ASSERT(h->heads[h_idx] == c);
      update_head(h, h_idx, c->next);
   }
}
/* -------------------------------------------------------------------------- */
static inline void
new_head(heap*const h, chunk*const c, U32 const lvl15, U32 const tot)
{
   U32 const hidx = lvl15 + tot - 1;
   ASSERT(hidx < h->hdcnt);
   chunk*const hd = h->heads[hidx];
   update_next(h, c, hd);
   update_prev(h, c, NULL);
   update_head(h, hidx, c);
   if (NULL != hd) {
      ASSERT(hd->prev == NULL);
      update_prev(h, hd, c);
   }
}
/* -------------------------------------------------------------------------- */
void heap_free(heap*const h, void*const address)
{
   U8 const*const a = (__typeof(a))address;
   U32 const A = (__typeof(A))a;
   U8*const base = h->hdata;
   if (unlikely(a < base || a >= base + h->hsize || 0 != (A & 0x0FU))) {
      fprintf(stderr,"ERR: %p is not an allocated address.\n", address);
      return;
   }
   U32 const reladdr = a - base;
   ASSERT(A >= 4);
   U32 lvl = (CTZ(A) >> 2) - 1;
   ASSERT(lvl < 7);
   U32 shift = (lvl + 1) << 2, idx;
   for (;; --lvl, shift -= 4) {
      idx = reladdr >> shift;
      if (eSTATUS_ALLOC_HEAD == chunk_get_status(h->bitfield[lvl], idx)) {
         break;
      }
      if (unlikely(0 == lvl)) {
         fprintf(stderr, "ERR: %p is not an allocated address.\n", address);
         return;
      }
      ASSERT(0 != lvl && shift >= 4);
   }

   U32 const head_lvl = lvl;
   U32 tot_size = 0;
   U32 const sidx = idx & 0x0FU;
   if (unlikely(15 == sidx)) {
      tot_size = 1 << shift;
   } else {
      U32 const*bs_lvl = h->bitfield[lvl];
      U32 const bits = bs_lvl[idx >> 4] << ((sidx + 1) << 1);
      if (unlikely(0 == bits)) {
         ASSERT(0 != sidx);
         tot_size = (16 - sidx) << shift;
      } else {
         U32 allocs = count_leading_allocs(bits) + 1;
         ASSERT(sidx + allocs < 16);
         tot_size = allocs << shift;
         while (eSTATUS_SPLIT == chunk_get_status(bs_lvl, idx + allocs)) {
            ASSERT(0 != lvl);
            lvl -= 1;
            bs_lvl = h->bitfield[lvl];
            ASSERT(shift >= 4);
            shift -= 4;
            idx = (reladdr + tot_size) >> shift;
            allocs = count_leading_allocs(bs_lvl[idx >> 4]);
            tot_size += allocs << shift;
         }
      }
   }
   ASSERT(tot_size == heap_get_alloc_size(h, address));
   ASSERT(0 != tot_size);
   ASSERT(lvl == (CTZ(tot_size) >> 2) - 1);
   U32 sub_empty = 0;
   U32 const bottom_addr = reladdr + tot_size;
   while (lvl < head_lvl) {
      U32 const base_size = (tot_size >> shift) & 0x0Fu;
      U32 const bsize_sub = base_size + sub_empty;
      ASSERT(0 != bsize_sub);
      U32 const index = (bottom_addr >> shift) - base_size;
      ASSERT(0 == (index & 0x0Fu));
      U32*const bf_lvl = h->bitfield[lvl];
      U32 next = 0;
      U32 const lvl15 = (lvl << 4) - lvl;
      U32 new_bf = 0;
      U32 const bsize_sub2 = bsize_sub << 1;
      if (16 != bsize_sub) {
         U32 const stat = bf_lvl[index >> 4];
         next = (CLZ((stat << bsize_sub2) ^ ALL_FREE)) >> 1;
         if (0 != next) {
            chunk const*const n = (chunk*)(base + ((index + bsize_sub) << shift));
            chunk_remove_from_list(h, n, lvl15 + next - 1);
         }
         U32 const nmask = (0x40000000u >> (bsize_sub2 - 2)) - 1;
         new_bf |= stat & nmask;
      }
      new_bf |= ALL_FREE << (32 - bsize_sub2);
      bf_lvl[index >> 4] = new_bf;
      U32 const tot = next + bsize_sub;
      if (unlikely(16 == tot)) {
         sub_empty = 1;
      } else {
         chunk*const c = (chunk*)(base + (index << shift));
         new_head(h, c, lvl15, tot);
         sub_empty = 0;
         ASSERT(0 != (tot_size >> (shift + 4)));
         lvl += CTZ(tot_size >> (shift + 4)) >> 2;
      }
      lvl += 1;
      shift = (lvl + 1) << 2;
      ASSERT(lvl <= head_lvl);
   }
   ASSERT(lvl == head_lvl);
   for (U32 base_size = (tot_size >> shift) & 0x0Fu;;) {
      U32 const bsize_sub = base_size + sub_empty;
      ASSERT(0 != bsize_sub);
      U32 const idx = reladdr >> shift;
      U32 const sub = idx & 0x0Fu;
      U32*const bf_lvl = h->bitfield[lvl];
      U32 const lvl15 = (lvl << 4) - lvl;
      U32 prev = 0, next = 0;
      U32 const stat = bf_lvl[idx >> 4];
      U32 new_bf = 0;
      U32 const inxt = (sub + bsize_sub) << 1;
      if (32 != inxt) {
         _Static_assert(0 != ALL_FREE, "FIXME");
         next = (CLZ((stat << inxt) ^ ALL_FREE)) >> 1;
         if (0 != next) {
            chunk const*const n = (chunk*)(base + ((idx + bsize_sub) << shift));
            chunk_remove_from_list(h, n, lvl15 + next - 1);
         }
         ASSERT(0 != inxt);
         U32 const nmask = (0x40000000u >> (inxt - 2)) - 1;
         new_bf |= stat & nmask;
      }
      if (0 != sub) {
         prev = (CTZ((stat >> ((16 - sub) << 1)) ^ ALL_FREE)) >> 1;
         if (0 != prev) {
            ASSERT(prev <= sub);
            chunk*const p = (chunk*)(base + ((idx - prev) << shift));
            chunk_remove_from_list(h, p, lvl15 + prev - 1);
         }
         ASSERT(prev <= sub);
         ASSERT(sub <= 15);
         U32 const pmask = 0xFFFFFFFCu << ((15 - sub) << 1);
         new_bf |= stat & pmask;
      }
      new_bf |= (ALL_FREE >> (32 - (bsize_sub << 1))) << (32 - inxt);
      bf_lvl[idx >> 4] = new_bf;
      U32 const tot = next + prev + bsize_sub;
      ASSERT(tot <= 16 && (tot != 16 || lvl < MAIN_BASE_SIZE_COUNT));
      if (tot != 16) {
         chunk*const c = (chunk*)(base + ((idx - prev) << shift));
         new_head(h, c, lvl15, tot);
         break;
      }
      sub_empty = 1;
      lvl += 1;
      ASSERT(shift < 28);
      shift += 4;
      base_size = 0;
   }
   ASSERT(heap_get_address_status(h, address) == eSTATUS_FREE);
   heap_unlock(h);
   return;
}
/* -------------------------------------------------------------------------- */
#if 0
void heap_free1(heap*const h, void*const address)
{
   if (NULL == address) {
      return;
   }

   U8 const*const a = (__typeof(a))address;
   U32 sub_empty = 0;

   U8*const base = h->hdata;

   if (unlikely(a < base || a >= base + h->hsize)) {
      fprintf(stderr,"0x%08X is not an address within heap boundaries.\n",
               (U32)address);
      return;
   }

   U32 const size = heap_get_alloc_size(h,address);
   if (unlikely(0 == size)) {
      fprintf(stderr,"0x%08X is not an allocated address.\n", (U32)address);
      return;
   }

   U8 const*addr = (__typeof(addr))address + size;

   heap_lock(h);

   U32 const lvl_head = 6 - (CLZ(size) >> 2);
   bool done = false;
   for (U32 lvl = 0; lvl < MAIN_BASE_SIZE_COUNT && !done; ++lvl) {
      U32 const shift = (lvl + 1) << 2;
      U32 const sz_shft = size >> shift;
      if (0 == sz_shft && 0 == sub_empty) {
         break;
      }
      U32 const bs = sz_shft & 0x0F;
      U32 const base_size = bs + sub_empty;
      if (0 == base_size) {
         continue;
      }
      addr -= bs << shift;

      U32 const index = (addr - base) >> shift;
      U32 const sub = index & 0x0F;
      U32 const stat = h->bitfield[lvl][index >> 4] ^ ~ALL_FREE;
      bf_set_free_multi(h->bitfield[lvl], index, base_size);
      U32 prev = 0, next = 0;
      U32 const inxt = sub + base_size;
      U32 const lvl15 = (lvl << 4) - lvl;
      if (16 != inxt) {
         next = (CLZ(~(stat << (inxt << 1)))) >> 1;
      }
      if (lvl >= lvl_head && 0 != sub) {
         prev = (CTZ(~(stat >> ((16 - sub) << 1)))) >> 1;
         done = prev == 0;
      }
      ASSERT(base_size + prev + next <= 16);

      if (0 != next) {
         chunk const*const n = (chunk*)(addr + (base_size << shift));
         chunk_remove_from_list(h, n, lvl15 + next - 1);
      }

      if (0 != prev) {
         ASSERT(prev <= sub);
         chunk*const p = (chunk*)(addr - (prev << shift));
         chunk_remove_from_list(h, p, lvl15 + prev - 1);
         addr = (__typeof(addr))p;
      }

      U32 const tot = next + prev + base_size;
      if (tot == 16) {
         ASSERT(lvl != MAIN_BASE_SIZE_COUNT);
         sub_empty = 1;
         ASSERT(!done);
      } else {
         chunk*const c = (chunk*)addr;
         U32 const hidx = lvl15 + tot - 1;
         ASSERT(hidx < h->hdcnt);
         update_next(h, c, h->heads[hidx]);
         update_prev(h, c, NULL);
         sub_empty = 0;

         update_head(h, hidx, c);

         if (NULL != c->next) {
            ASSERT(c->next->prev == NULL);
            update_prev(h, c->next, c);
         }
      }
   }

   ASSERT(heap_get_address_status(h, address) == eSTATUS_FREE);

   heap_unlock(h);

   return;
}
#endif
/* -------------------------------------------------------------------------- */
static void set_bf_ptr(U32 const index, U32 const lvl_bf_count, heap*const H,
                       U32 const start, void*const mem, U32 const size)
{
   if (0 != lvl_bf_count) {
      H->bitfield[index] = &(((U32*)mem)[start]);
      H->bscnt = index + 1;
      U32 const lvl_chunk_cnt = size >> ((index + 1) << 2);
      for (U32 i = 0; i < (lvl_chunk_cnt >> 4); i++) {
         H->bitfield[index][i] = ALL_FREE;
      }
      if (0 != (lvl_chunk_cnt & 0x0FU)) {
         U32 const idx = lvl_chunk_cnt & ~0x0FU;
         U32 const sub = lvl_chunk_cnt &  0x0FU;
         bf_set_free_multi(H->bitfield[index], idx, sub);
         bf_set_alloc_head_multi(H->bitfield[index], idx + sub, 16 - sub);
      }
   } else {
      H->bitfield[index] = NULL;
   }
}
/* -------------------------------------------------------------------------- */
static void populate_heads(heap *const h, void const*const data, U32 const size)
{
   ASSERT((size & (BASE_SIZE_MIN - 1)) == 0);

   U32 used_size = closest_base_size(size);
   U32 i = base_size_to_index(used_size);

   if (used_size != size) {
      ASSERT(i != 0);
      i = i - 1;
      used_size = base_size_from_index(i);
      ASSERT(used_size < size);
   }

   ASSERT(i < h->hdcnt);
   ASSERT(h->heads[i] == NULL);
   h->heads[i] = (chunk*)data;
   h->headsbits[i >> 5] |= 0x80000000U >> (i & 31);
   h->heads[i]->prev = NULL;
   h->heads[i]->next = NULL;

   if (used_size != size) {
      return populate_heads(h, (U8*)data + used_size, size - used_size);
   }

   return;
}
/* -------------------------------------------------------------------------- */
/* Never been tested with size > UINT32_MAX (4GB). That will likely not work. */
heap*heap_create(U8*const address, U32 const size)
{
   heap *new_heap = NULL;

   if (0 == size || 0 != (size & (BASE_SIZE_MIN - 1))) {
      fprintf(stderr, "heap size must be multiple of %u bytes.\n", BASE_SIZE_MIN);
      return NULL;
   }

   U32 const cs = CLZ(size) & 0x1CU;
   U32 const largest = 0x10000000 >> cs;
   if (0 != ((U32)address & (largest - 1))) {
      fprintf(stderr, "heap with size %u must be aligned on 0x%08X\n",
               size, largest);
      return NULL;
   }
   U32 const hd_cnt = ((24 - cs) >> 2) * 15 + ((size >> (28 - cs)) & 0x0FU);
   /* we use an externally allocated buffer for the book-keeping */
   new_heap = (heap*)malloc(sizeof(*new_heap) + (hd_cnt * sizeof(chunk*)));

   if (NULL == new_heap) {
      fprintf(stderr, "couldn't alloc %zu bytes for the heap.\n", sizeof(heap));
      return NULL;
   }

   new_heap->hdcnt = hd_cnt;

   U32 const tot_bf_count = total_bitfield_count(size);
   void*const mem_bf = malloc(tot_bf_count * sizeof(U32));

   PRINTF("This %u bytes heap requires %zu bytes for its base "
          "structure plus %zu bytes (%.2f%%) for book-keeping."
          "There are %u base sizes.\n",
          size, sizeof(*new_heap) + (hd_cnt * sizeof(chunk*)),
          tot_bf_count * sizeof(U32),
          100.0 * (tot_bf_count * sizeof(U32)) / size, hd_cnt);

   for (U32 i = 0, start = 0; i < MAIN_BASE_SIZE_COUNT; ++i) {
      U32 const nbc = needed_bitfield_count(size, i);
      set_bf_ptr(i, nbc, new_heap, start, mem_bf, size);
      start += nbc;
   }

   for (U32 i = 0; i < hd_cnt; i++) {
      new_heap->heads[i] = NULL;
   }

   new_heap->headsbits[0] = 0;
   new_heap->headsbits[1] = 0;
   new_heap->headsbits[2] = 0;
   new_heap->headsbits[3] = 0x007FFFFFU;
   ASSERT(hd_cnt <= BASE_SIZES_COUNT);

   populate_heads(new_heap, address, size);

   new_heap->hdata = address;
   new_heap->hsize = size;

   return new_heap;
}
/* -------------------------------------------------------------------------- */
