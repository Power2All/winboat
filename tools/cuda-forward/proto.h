/*
 * Copyright 2026 WinBoat contributors
 * SPDX-License-Identifier: MIT
 */

/* The forwarding's wire format, shared by the guest's nvcuda.dll and the host's server. A connection (one per guest
 * thread: CUDA's current context is per thread) starts with a hello; then each call is a request and a reply:
 *
 *   hello    u32 magic, u32 version, u64 process token (the guest process: its threads share one host process)
 *   request  u32 payload size, u32 function (FN_* from gen/ids.h, OP_* below), payload
 *   reply    u32 payload size, i32 CUresult, payload
 *
 * Payloads are the parameters in order, each as gen.py's kinds describe (raw bytes for values; a present flag
 * ahead of pointers; u32 length + bytes for strings, u64 count + elements for arrays). Both sides are x86-64 and
 * lay out the API's types alike. */

#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FWD_MAGIC 0x57424355u   /* "UCBW" */
#define FWD_VERSION 1
#define FWD_PORT 7150
#define FWD_MAX_MESSAGE (1u << 30)

/* the hand-written calls' own requests (gen/ids.h numbers the generated ones from 1) */
enum {
   OP_RESOLVE = 0x10000,   /* string symbol, i32 version, u64 flags -> string exported name */
   OP_ERROR_NAME,          /* i32 result, i32 string kind (0 name, 1 description) -> string */
   OP_MEMORY_TYPE,         /* u64 address -> u32 CU_MEMORYTYPE_DEVICE or 0 (not device memory: the guest's) */
   OP_FUNC_PARAMS,         /* u64 function -> u32 count, count x (u64 offset, u64 size) */
   OP_COPY_HTOD,           /* u64 dst, u64 stream (~0: synchronous), bytes -> */
   OP_COPY_DTOH,           /* u64 src, u64 size, u64 stream (~0: synchronous) -> bytes */
   OP_COPY_DTOD,           /* u64 dst, u64 src, u64 size, u64 stream (~0: synchronous) -> */
   OP_COPY_2D,             /* CUDA_MEMCPY2D with the guest side's rows as bytes, u32 flavor, u64 stream -> rows */
   OP_LOAD_IMAGE,          /* u32 what (0 module, 1 fatbin module, 2 library), bytes -> u64 handle */
   OP_LAUNCH,              /* u32 flavor, u64 function, u32 grid[3], u32 block[3], u32 shared memory, u64 stream,
                              u32 attribute count, attributes (CUlaunchAttribute each), parameter bytes -> */
   OP_FUNC_NAME,           /* u64 function, u32 kernel -> string */
   OP_POINTER_ATTRIBUTE,   /* i32 attribute, u64 pointer, u32 size -> bytes */
};

/* A message being written */
typedef struct {
   unsigned char *p;
   size_t len, cap;
   int bad;
} wbuf;

static inline void
w_put(wbuf *w, const void *data, size_t n)
{
   if (w->len + n > w->cap) {
      size_t cap = w->cap ? w->cap : 256;
      while (cap < w->len + n)
         cap *= 2;
      unsigned char *p = (unsigned char *)realloc(w->p, cap);
      if (!p) {
         w->bad = 1;
         return;
      }
      w->p = p;
      w->cap = cap;
   }
   if (n)
      memcpy(w->p + w->len, data, n);
   w->len += n;
}

static inline void w_u8(wbuf *w, uint8_t v) { w_put(w, &v, 1); }
static inline void w_u32(wbuf *w, uint32_t v) { w_put(w, &v, 4); }
static inline void w_u64(wbuf *w, uint64_t v) { w_put(w, &v, 8); }
static inline void w_flag(wbuf *w, int present) { w_u8(w, present ? 1 : 0); }

static inline void
w_str(wbuf *w, const char *s)
{
   uint32_t n = s ? (uint32_t)strlen(s) + 1 : 0xffffffffu;
   w_u32(w, n);
   if (s)
      w_put(w, s, n);
}

/* one object behind a pointer, NULL travels as absent */
static inline void
w_obj(wbuf *w, const void *p, size_t size)
{
   w_flag(w, p != NULL);
   if (p)
      w_put(w, p, size);
}

static inline void
w_arr(wbuf *w, const void *p, uint64_t count, size_t elem)
{
   w_flag(w, p != NULL);
   w_u64(w, count);
   if (p)
      w_put(w, p, (size_t)(count * elem));
}

/* A message being read: running past its end (a malformed message) sets bad, and the reads return zeros */
typedef struct {
   const unsigned char *p;
   size_t len, pos;
   int bad;
} rbuf;

static inline const void *
r_take(rbuf *r, size_t n)
{
   if (r->bad || n > r->len - r->pos) {
      r->bad = 1;
      return NULL;
   }
   const void *p = r->p + r->pos;
   r->pos += n;
   return p;
}

static inline void
r_get(rbuf *r, void *data, size_t n)
{
   const void *p = r_take(r, n);
   if (p)
      memcpy(data, p, n);
   else if (n)
      memset(data, 0, n);
}

static inline uint8_t r_u8(rbuf *r) { uint8_t v; r_get(r, &v, 1); return v; }
static inline uint32_t r_u32(rbuf *r) { uint32_t v; r_get(r, &v, 4); return v; }
static inline uint64_t r_u64(rbuf *r) { uint64_t v; r_get(r, &v, 8); return v; }
static inline int r_flag(rbuf *r) { return r_u8(r) != 0; }

/* a string in place (NULL when absent or not terminated) */
static inline const char *
r_str(rbuf *r)
{
   uint32_t n = r_u32(r);
   if (n == 0xffffffffu)
      return NULL;
   const char *s = (const char *)r_take(r, n);
   if (!s || !n || s[n - 1]) {
      r->bad = 1;
      return NULL;
   }
   return s;
}

/* one object: into t (and t returned) when present, NULL when absent */
static inline void *
r_obj(rbuf *r, void *t, size_t size)
{
   if (!r_flag(r))
      return NULL;
   r_get(r, t, size);
   return t;
}

/* an input array in place (NULL when absent) */
static inline const void *
r_arr(rbuf *r, uint64_t *count, size_t elem)
{
   int present = r_flag(r);
   *count = r_u64(r);
   if (!present)
      return NULL;
   if (elem && *count > FWD_MAX_MESSAGE / elem) {
      r->bad = 1;
      return NULL;
   }
   return r_take(r, (size_t)(*count * elem));
}
