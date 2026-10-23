/*
 * cudafwd-server: runs the CUDA driver API and NVENC calls of a guest's nvcuda.dll / nvEncodeAPI64.dll on the host's
 * libcuda.so.1 / libnvidia-encode.so.1 (wire format: proto.h).
 *
 *   cudafwd-server <listen address> [port]      CF_LOG=1 logs every call
 *
 * One host process per guest process (forked when its anchor connection arrives, before CUDA is loaded), one thread
 * per guest thread. The server trusts its guest: CUDA's unified addressing lets device pointer arguments reach host
 * memory, so a guest process can read and write its own host process' memory. Each guest process has a process of its
 * own, and the listen address should be the guest-facing one only.
 * ponytail: no device pointer validation; track each process' allocations and check pointer arguments if the guest
 * can't be trusted with its host process.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"
#define CUDA_ERROR_NOT_SUPPORTED 801 /* not in ffnvcodec's subset of cuda.h */
#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/nvEncodeAPI.h>

static int log_calls;
#define LOG(...) fprintf(stderr, "cudafwd: " __VA_ARGS__)

/* --- sockets --- */

static int read_all(int fd, void *p, size_t n)
{
   for (uint8_t *b = p; n;) {
      ssize_t got = recv(fd, b, n, 0);
      if (got <= 0) {
         if (got < 0 && errno == EINTR)
            continue;
         return -1;
      }
      b += got;
      n -= (size_t)got;
   }
   return 0;
}

static int write_all(int fd, const void *p, size_t n)
{
   for (const uint8_t *b = p; n;) {
      ssize_t put = send(fd, b, n, MSG_NOSIGNAL);
      if (put <= 0) {
         if (put < 0 && errno == EINTR)
            continue;
         return -1;
      }
      b += put;
      n -= (size_t)put;
   }
   return 0;
}

/* --- the functions guests resolve --- */

typedef int (*fn16)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*custom_fn)(cf_rd *req, cf_buf *reply);

struct fn {
   char name[64];
   char *desc;
   void *fn;
   custom_fn custom;
};

static struct fn fns[1024];
static int nfns;
static pthread_mutex_t fns_lock = PTHREAD_MUTEX_INITIALIZER;
static void *libcuda, *libnvenc;
static NV_ENCODE_API_FUNCTION_LIST nvenc;
static int nvenc_status = -1; /* NV_ENC_SUCCESS once loaded */

static void *cuda_sym(const char *name)
{
   if (!libcuda && !(libcuda = dlopen("libcuda.so.1", RTLD_NOW)))
      return NULL;
   return dlsym(libcuda, name);
}

static int load_nvenc(void)
{
   if (nvenc_status == NV_ENC_SUCCESS)
      return 1;
   if (!libnvenc && !(libnvenc = dlopen("libnvidia-encode.so.1", RTLD_NOW)))
      return 0;
   NVENCSTATUS (*create)(NV_ENCODE_API_FUNCTION_LIST *) = dlsym(libnvenc, "NvEncodeAPICreateInstance");
   if (!create)
      return 0;
   nvenc.version = NV_ENCODE_API_FUNCTION_LIST_VER;
   nvenc_status = create(&nvenc);
   if (nvenc_status != NV_ENC_SUCCESS)
      LOG("NvEncodeAPICreateInstance: %d (the host driver doesn't have NVENC API %d.%d)\n", nvenc_status,
          NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
   return nvenc_status == NV_ENC_SUCCESS;
}

#define NVENC_FN(f) { #f, offsetof(NV_ENCODE_API_FUNCTION_LIST, f) }
static const struct { const char *name; size_t offset; } nvenc_fns[] = {
   NVENC_FN(nvEncOpenEncodeSession), NVENC_FN(nvEncGetEncodeGUIDCount), NVENC_FN(nvEncGetEncodeProfileGUIDCount),
   NVENC_FN(nvEncGetEncodeProfileGUIDs), NVENC_FN(nvEncGetEncodeGUIDs), NVENC_FN(nvEncGetInputFormatCount),
   NVENC_FN(nvEncGetInputFormats), NVENC_FN(nvEncGetEncodeCaps), NVENC_FN(nvEncGetEncodePresetCount),
   NVENC_FN(nvEncGetEncodePresetGUIDs), NVENC_FN(nvEncGetEncodePresetConfig), NVENC_FN(nvEncInitializeEncoder),
   NVENC_FN(nvEncCreateInputBuffer), NVENC_FN(nvEncDestroyInputBuffer), NVENC_FN(nvEncCreateBitstreamBuffer),
   NVENC_FN(nvEncDestroyBitstreamBuffer), NVENC_FN(nvEncEncodePicture), NVENC_FN(nvEncLockBitstream),
   NVENC_FN(nvEncUnlockBitstream), NVENC_FN(nvEncLockInputBuffer), NVENC_FN(nvEncUnlockInputBuffer),
   NVENC_FN(nvEncGetEncodeStats), NVENC_FN(nvEncGetSequenceParams), NVENC_FN(nvEncMapInputResource),
   NVENC_FN(nvEncUnmapInputResource), NVENC_FN(nvEncDestroyEncoder), NVENC_FN(nvEncInvalidateRefFrames),
   NVENC_FN(nvEncOpenEncodeSessionEx), NVENC_FN(nvEncRegisterResource), NVENC_FN(nvEncUnregisterResource),
   NVENC_FN(nvEncReconfigureEncoder), NVENC_FN(nvEncCreateMVBuffer), NVENC_FN(nvEncDestroyMVBuffer),
   NVENC_FN(nvEncGetEncodePresetConfigEx),
};

static void *nvenc_sym(const char *name)
{
   if (!strcmp(name, "NvEncodeAPIGetMaxSupportedVersion")) {
      if (!libnvenc)
         libnvenc = dlopen("libnvidia-encode.so.1", RTLD_NOW);
      return libnvenc ? dlsym(libnvenc, name) : NULL;
   }
   for (size_t i = 0; i < sizeof(nvenc_fns) / sizeof(nvenc_fns[0]); i++)
      if (!strcmp(name, nvenc_fns[i].name))
         return load_nvenc() ? *(void **)((char *)&nvenc + nvenc_fns[i].offset) : NULL;
   return NULL;
}

/* --- generic forwarding (desc: proto.h) --- */

static void generic(const struct fn *f, cf_rd *rd, cf_buf *reply)
{
   uint64_t a[CF_MAX_ARGS] = {0};
   struct { void *p; size_t n; } outs[CF_MAX_ARGS];
   void *frees[CF_MAX_ARGS];
   int na = 0, nouts = 0, nfrees = 0;
   const char *d = f->desc;
   cf_tok t;
   while (rd->ok && cf_next_tok(&d, &t) && na < CF_MAX_ARGS - 1) {
      switch (t.kind) {
      case 'u': a[na++] = cf_get32(rd); break;
      case 'v': a[na++] = cf_get64(rd); break;
      case 'g': a[na++] = cf_get64(rd); a[na++] = cf_get64(rd); break;
      case 'z':
         if (cf_get8(rd)) {
            uint32_t n = cf_get32(rd);
            const char *s = cf_take(rd, n);
            if (!s || !n || s[n - 1])
               rd->ok = 0;
            a[na++] = (uintptr_t)s;
         } else {
            a[na++] = 0;
         }
         break;
      default: { /* i o x b r */
         int out = t.kind == 'o' || t.kind == 'x' || t.kind == 'r';
         if (!cf_get8(rd)) {
            a[na++] = 0;
            if (out)
               outs[nouts++].p = NULL;
            break;
         }
         size_t n = t.n;
         if (t.kind == 'b' || t.kind == 'r')
            n = cf_get64(rd);
         if (n > CF_MAX_MESSAGE) {
            rd->ok = 0;
            break;
         }
         void *p = calloc(1, n ? n : 1);
         frees[nfrees++] = p;
         if (t.kind == 'i' || t.kind == 'x' || t.kind == 'b')
            cf_get(rd, p, n);
         a[na++] = (uintptr_t)p;
         if (out) {
            outs[nouts].p = p;
            outs[nouts++].n = n;
         }
      }
      }
   }
   int result = rd->ok ? ((fn16)f->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11],
                                       a[12], a[13], a[14], a[15])
                       : CUDA_ERROR_INVALID_VALUE;
   cf_put32(reply, (uint32_t)result);
   for (int i = 0; i < nouts; i++)
      if (outs[i].p)
         cf_put(reply, outs[i].p, outs[i].n);
   for (int i = 0; i < nfrees; i++)
      free(frees[i]);
}

/* --- CUDA functions with host pointers or variable layouts --- */

static void put_string(cf_buf *b, const char *s)
{
   uint32_t n = s ? (uint32_t)strlen(s) + 1 : 0;
   cf_put32(b, n);
   cf_put(b, s, n);
}

/* (u32 error) -> result, string */
static void custom_error_string(cf_rd *rd, cf_buf *reply, const char *which)
{
   CUresult (*fn)(CUresult, const char **) = cuda_sym(which);
   const char *s = NULL;
   int result = fn ? fn((CUresult)cf_get32(rd), &s) : CUDA_ERROR_NOT_SUPPORTED;
   cf_put32(reply, (uint32_t)result);
   put_string(reply, result == CUDA_SUCCESS ? s : NULL);
}
static void custom_cuGetErrorName(cf_rd *rd, cf_buf *reply) { custom_error_string(rd, reply, "cuGetErrorName"); }
static void custom_cuGetErrorString(cf_rd *rd, cf_buf *reply) { custom_error_string(rd, reply, "cuGetErrorString"); }

/* (v function) -> result, u32 count, (u32 offset, u32 size) per parameter: the guest packs kernel arguments by it */
static void custom_cfFuncParams(cf_rd *rd, cf_buf *reply)
{
   CUresult (*info)(CUfunction, size_t, size_t *, size_t *) = cuda_sym("cuFuncGetParamInfo");
   CUfunction f = (CUfunction)(uintptr_t)cf_get64(rd);
   if (!info) {
      cf_put32(reply, CUDA_ERROR_NOT_SUPPORTED);
      return;
   }
   cf_buf params = {0};
   uint32_t count = 0;
   size_t offset, size;
   while (info(f, count, &offset, &size) == CUDA_SUCCESS) {
      cf_put32(&params, (uint32_t)offset);
      cf_put32(&params, (uint32_t)size);
      count++;
   }
   cf_put32(reply, CUDA_SUCCESS);
   cf_put32(reply, count);
   cf_put(reply, params.data, params.size);
   free(params.data);
}

/* (v f, u grid xyz, u block xyz, u shared, v stream, u32 count, count x (u32 size, bytes) | 0xffffffff, u64 size,
 * bytes: the packed "extra" buffer) -> result */
static void custom_cuLaunchKernel(cf_rd *rd, cf_buf *reply)
{
   CUresult (*launch)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, CUstream,
                      void **, void **) = cuda_sym("cuLaunchKernel");
   CUfunction f = (CUfunction)(uintptr_t)cf_get64(rd);
   unsigned g[7];
   for (int i = 0; i < 7; i++)
      g[i] = cf_get32(rd);
   CUstream stream = (CUstream)(uintptr_t)cf_get64(rd);
   uint32_t count = cf_get32(rd);
   int result;
   if (count == 0xffffffffu) {
      size_t size = cf_get64(rd);
      void *buffer = (void *)cf_take(rd, size);
      void *extra[] = {(void *)1 /* CU_LAUNCH_PARAM_BUFFER_POINTER */, buffer, (void *)2 /* ..._SIZE */, &size,
                       (void *)0 /* CU_LAUNCH_PARAM_END */};
      result = rd->ok ? launch(f, g[0], g[1], g[2], g[3], g[4], g[5], g[6], stream, NULL, extra)
                      : CUDA_ERROR_INVALID_VALUE;
   } else {
      void **params = calloc(count + 1, sizeof(void *));
      for (uint32_t i = 0; i < count && rd->ok; i++) {
         uint32_t size = cf_get32(rd);
         params[i] = (void *)cf_take(rd, size);
      }
      result = rd->ok ? launch(f, g[0], g[1], g[2], g[3], g[4], g[5], g[6], stream, params, NULL)
                      : CUDA_ERROR_INVALID_VALUE;
      free(params);
   }
   cf_put32(reply, (uint32_t)result);
}

/* (CUDA_MEMCPY2D, u8 async, v stream, host source rows packed when the source is host memory) -> result, the
 * destination rows packed when it is host memory. The guest made host sides tightly packed (pitch = width, x/y 0). */
static void custom_memcpy2d(cf_rd *rd, cf_buf *reply)
{
   CUresult (*copy)(const CUDA_MEMCPY2D *) = cuda_sym("cuMemcpy2D_v2");
   CUresult (*copy_async)(const CUDA_MEMCPY2D *, CUstream) = cuda_sym("cuMemcpy2DAsync_v2");
   CUresult (*sync)(CUstream) = cuda_sym("cuStreamSynchronize");
   CUDA_MEMCPY2D c;
   cf_get(rd, &c, sizeof(c));
   int async = cf_get8(rd);
   CUstream stream = (CUstream)(uintptr_t)cf_get64(rd);
   size_t size = c.WidthInBytes * c.Height;
   void *dst = NULL;
   if (c.srcMemoryType == CU_MEMORYTYPE_HOST)
      c.srcHost = cf_take(rd, size);
   if (c.dstMemoryType == CU_MEMORYTYPE_HOST && size <= CF_MAX_MESSAGE)
      c.dstHost = dst = malloc(size ? size : 1);
   int result = CUDA_ERROR_INVALID_VALUE;
   if (rd->ok && (c.srcMemoryType != CU_MEMORYTYPE_HOST || c.srcHost) &&
       (c.dstMemoryType != CU_MEMORYTYPE_HOST || dst)) {
      /* Pageable host memory: an upload returns once the data is staged, a download is made to wait for the stream */
      result = async ? copy_async(&c, stream) : copy(&c);
      if (result == CUDA_SUCCESS && async && dst)
         result = sync(stream);
   }
   cf_put32(reply, (uint32_t)result);
   if (c.dstMemoryType == CU_MEMORYTYPE_HOST)
      cf_put(reply, dst, result == CUDA_SUCCESS ? size : 0);
   free(dst);
}

/* --- NVENC functions with pointers in their structures --- */

/* A structure with nested inputs: u32 nblobs, nblobs x (u32 size, bytes), u32 npatches, npatches x (u32 blob,
 * u32 offset, u32 blob it points to). Blob 0 is the structure; a patch stores a blob's address in another at offset.
 * Returns blob 0 (NULL on a malformed message); *all receives every allocation, freed with free_blobs. */
static void *get_blobs(cf_rd *rd, void ***all, uint32_t *count, size_t *size0)
{
   uint32_t n = cf_get32(rd);
   if (!rd->ok || !n || n > 64)
      return NULL;
   void **blobs = calloc(n, sizeof(void *));
   uint32_t *sizes = calloc(n, sizeof(uint32_t));
   for (uint32_t i = 0; i < n && rd->ok; i++) {
      sizes[i] = cf_get32(rd);
      const void *p = cf_take(rd, sizes[i]);
      if (p && (blobs[i] = calloc(1, sizes[i] + 8)))
         memcpy(blobs[i], p, sizes[i]);
   }
   uint32_t patches = cf_get32(rd);
   for (uint32_t i = 0; i < patches && rd->ok; i++) {
      uint32_t blob = cf_get32(rd), offset = cf_get32(rd), target = cf_get32(rd);
      if (blob >= n || target >= n || (uint64_t)offset + sizeof(void *) > sizes[blob])
         rd->ok = 0;
      else
         memcpy((char *)blobs[blob] + offset, &blobs[target], sizeof(void *));
   }
   *all = blobs;
   *count = n;
   if (size0)
      *size0 = sizes[0];
   free(sizes);
   return rd->ok ? blobs[0] : NULL;
}

static void free_blobs(void **blobs, uint32_t n)
{
   for (uint32_t i = 0; blobs && i < n; i++)
      free(blobs[i]);
   free(blobs);
}

/* (v encoder, blobs) -> result: nvEncInitializeEncoder, nvEncReconfigureEncoder, nvEncEncodePicture */
static void custom_nvenc_blobs(cf_rd *rd, cf_buf *reply, size_t fn_offset)
{
   void *encoder = (void *)(uintptr_t)cf_get64(rd);
   void **blobs;
   uint32_t n = 0;
   void *params = get_blobs(rd, &blobs, &n, NULL);
   NVENCSTATUS (*fn)(void *, void *) = load_nvenc() ? *(void **)((char *)&nvenc + fn_offset) : NULL;
   cf_put32(reply, !fn ? NV_ENC_ERR_UNIMPLEMENTED : params ? fn(encoder, params) : NV_ENC_ERR_INVALID_PARAM);
   free_blobs(blobs, n);
}
#define NVENC_BLOBS(f) \
   static void custom_##f(cf_rd *rd, cf_buf *reply) \
   { custom_nvenc_blobs(rd, reply, offsetof(NV_ENCODE_API_FUNCTION_LIST, f)); }
NVENC_BLOBS(nvEncInitializeEncoder)
NVENC_BLOBS(nvEncReconfigureEncoder)
NVENC_BLOBS(nvEncEncodePicture)

/* Input buffers: the size of a locked one comes from its format and height (recorded when created) */
struct input_buffer {
   void *handle, *locked;
   NV_ENC_BUFFER_FORMAT format;
   uint32_t height, pitch;
};
static struct input_buffer inputs[256];
static pthread_mutex_t inputs_lock = PTHREAD_MUTEX_INITIALIZER;

static struct input_buffer *find_input(void *handle)
{
   for (int i = 0; i < 256; i++)
      if (inputs[i].handle == handle)
         return &inputs[i];
   return NULL;
}

static size_t input_size(const struct input_buffer *b)
{
   size_t rows = b->height;
   switch (b->format) {
   case NV_ENC_BUFFER_FORMAT_NV12: case NV_ENC_BUFFER_FORMAT_YV12: case NV_ENC_BUFFER_FORMAT_IYUV:
   case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
      rows = b->height * 3 / 2; break;
   case NV_ENC_BUFFER_FORMAT_YUV444: case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
      rows = b->height * 3; break;
   case NV_ENC_BUFFER_FORMAT_NV16: case NV_ENC_BUFFER_FORMAT_P210:
      rows = b->height * 2; break;
   default: break; /* packed RGB/AYUV: one plane */
   }
   return rows * b->pitch;
}

/* (v encoder, NV_ENC_CREATE_INPUT_BUFFER) -> result, the structure */
static void custom_nvEncCreateInputBuffer(cf_rd *rd, cf_buf *reply)
{
   void *encoder = (void *)(uintptr_t)cf_get64(rd);
   NV_ENC_CREATE_INPUT_BUFFER c;
   cf_get(rd, &c, sizeof(c));
   c.pSysMemBuffer = NULL;
   int result = !rd->ok ? NV_ENC_ERR_INVALID_PARAM : load_nvenc() ? nvenc.nvEncCreateInputBuffer(encoder, &c)
                                                                  : NV_ENC_ERR_UNIMPLEMENTED;
   if (result == NV_ENC_SUCCESS) {
      pthread_mutex_lock(&inputs_lock);
      struct input_buffer *b = find_input(NULL);
      if (b)
         *b = (struct input_buffer){c.inputBuffer, NULL, c.bufferFmt, c.height, 0};
      pthread_mutex_unlock(&inputs_lock);
      if (!b) {
         nvenc.nvEncDestroyInputBuffer(encoder, c.inputBuffer);
         result = NV_ENC_ERR_OUT_OF_MEMORY;
      }
   }
   cf_put32(reply, (uint32_t)result);
   cf_put(reply, &c, sizeof(c));
}

/* (v encoder, v input buffer) -> result */
static void custom_nvEncDestroyInputBuffer(cf_rd *rd, cf_buf *reply)
{
   void *encoder = (void *)(uintptr_t)cf_get64(rd), *handle = (void *)(uintptr_t)cf_get64(rd);
   pthread_mutex_lock(&inputs_lock);
   struct input_buffer *b = handle ? find_input(handle) : NULL;
   if (b)
      memset(b, 0, sizeof(*b));
   pthread_mutex_unlock(&inputs_lock);
   cf_put32(reply, load_nvenc() ? nvenc.nvEncDestroyInputBuffer(encoder, handle) : NV_ENC_ERR_UNIMPLEMENTED);
}

/* (v encoder, NV_ENC_LOCK_INPUT_BUFFER) -> result, the structure, u64 size the guest writes on unlock */
static void custom_nvEncLockInputBuffer(cf_rd *rd, cf_buf *reply)
{
   void *encoder = (void *)(uintptr_t)cf_get64(rd);
   NV_ENC_LOCK_INPUT_BUFFER l;
   cf_get(rd, &l, sizeof(l));
   pthread_mutex_lock(&inputs_lock);
   struct input_buffer *b = rd->ok && l.inputBuffer ? find_input(l.inputBuffer) : NULL;
   int result = !b ? NV_ENC_ERR_INVALID_PARAM : nvenc.nvEncLockInputBuffer(encoder, &l);
   uint64_t size = 0;
   if (result == NV_ENC_SUCCESS) {
      b->locked = l.bufferDataPtr;
      b->pitch = l.pitch;
      size = input_size(b);
   }
   pthread_mutex_unlock(&inputs_lock);
   cf_put32(reply, (uint32_t)result);
   cf_put(reply, &l, sizeof(l));
   cf_put64(reply, size);
}

/* (v encoder, v input buffer, u64 size, bytes) -> result */
static void custom_nvEncUnlockInputBuffer(cf_rd *rd, cf_buf *reply)
{
   void *encoder = (void *)(uintptr_t)cf_get64(rd), *handle = (void *)(uintptr_t)cf_get64(rd);
   uint64_t size = cf_get64(rd);
   const void *data = cf_take(rd, size);
   pthread_mutex_lock(&inputs_lock);
   struct input_buffer *b = handle ? find_input(handle) : NULL;
   int result = NV_ENC_ERR_INVALID_PARAM;
   if (b && b->locked) {
      size_t limit = input_size(b);
      if (data)
         memcpy(b->locked, data, size < limit ? size : limit);
      b->locked = NULL;
      result = nvenc.nvEncUnlockInputBuffer(encoder, handle);
   }
   pthread_mutex_unlock(&inputs_lock);
   cf_put32(reply, (uint32_t)result);
}

/* (v encoder, NV_ENC_LOCK_BITSTREAM, u8 wants slice offsets) -> result, the structure, the bitstream bytes, the
 * slice offsets (numSlices of them) */
static void custom_nvEncLockBitstream(cf_rd *rd, cf_buf *reply)
{
   void *encoder = (void *)(uintptr_t)cf_get64(rd);
   NV_ENC_LOCK_BITSTREAM l;
   cf_get(rd, &l, sizeof(l));
   int slices = cf_get8(rd);
   static __thread uint32_t offsets[4096];
   l.sliceOffsets = slices ? offsets : NULL;
   l.outputStatsPtr = NULL;
   int result = !rd->ok ? NV_ENC_ERR_INVALID_PARAM : load_nvenc() ? nvenc.nvEncLockBitstream(encoder, &l)
                                                                  : NV_ENC_ERR_UNIMPLEMENTED;
   cf_put32(reply, (uint32_t)result);
   cf_put(reply, &l, sizeof(l));
   uint32_t size = result == NV_ENC_SUCCESS ? l.bitstreamSizeInBytes : 0;
   cf_put32(reply, size);
   cf_put(reply, l.bitstreamBufferPtr, size);
   uint32_t n = result == NV_ENC_SUCCESS && slices ? (l.numSlices < 4096 ? l.numSlices : 4096) : 0;
   cf_put32(reply, n);
   cf_put(reply, offsets, n * 4);
}

/* (v encoder, NV_ENC_SEQUENCE_PARAM_PAYLOAD) -> result, the structure, u32 size, bytes */
static void custom_nvEncGetSequenceParams(cf_rd *rd, cf_buf *reply)
{
   void *encoder = (void *)(uintptr_t)cf_get64(rd);
   NV_ENC_SEQUENCE_PARAM_PAYLOAD p;
   cf_get(rd, &p, sizeof(p));
   uint32_t size = 0, cap = p.inBufferSize < 65536 ? p.inBufferSize : 65536;
   uint8_t *buffer = calloc(1, cap ? cap : 1);
   p.spsppsBuffer = buffer;
   p.outSPSPPSPayloadSize = &size;
   int result = !rd->ok ? NV_ENC_ERR_INVALID_PARAM : load_nvenc() ? nvenc.nvEncGetSequenceParams(encoder, &p)
                                                                  : NV_ENC_ERR_UNIMPLEMENTED;
   if (size > cap)
      size = cap;
   cf_put32(reply, (uint32_t)result);
   cf_put(reply, &p, sizeof(p));
   cf_put32(reply, size);
   cf_put(reply, buffer, size);
   free(buffer);
}

/* (v encoder) -> 0, string */
static void custom_nvEncGetLastErrorString(cf_rd *rd, cf_buf *reply)
{
   void *encoder = (void *)(uintptr_t)cf_get64(rd);
   cf_put32(reply, 0);
   put_string(reply, load_nvenc() ? nvenc.nvEncGetLastErrorString(encoder) : NULL);
}

/* (v encoder, u8 has input, v input stream, u8 has output, v output stream) -> result. NVENC keeps the pointers. */
static void custom_nvEncSetIOCudaStreams(cf_rd *rd, cf_buf *reply)
{
   void *encoder = (void *)(uintptr_t)cf_get64(rd);
   /* ponytail: kept for the process' lifetime (16 bytes per call; programs set them once per encoder) */
   uint64_t *streams = calloc(2, sizeof(uint64_t));
   int has_in = cf_get8(rd);
   streams[0] = cf_get64(rd);
   int has_out = cf_get8(rd);
   streams[1] = cf_get64(rd);
   cf_put32(reply, !rd->ok ? NV_ENC_ERR_INVALID_PARAM
                   : load_nvenc() ? nvenc.nvEncSetIOCudaStreams(encoder, has_in ? &streams[0] : NULL,
                                                                 has_out ? &streams[1] : NULL)
                                  : NV_ENC_ERR_UNIMPLEMENTED);
}

static const struct { const char *name; custom_fn fn; } customs[] = {
#define C(f) { #f, custom_##f }
   C(cuGetErrorName), C(cuGetErrorString), C(cfFuncParams), C(cuLaunchKernel),
   { "cuMemcpy2D_v2", custom_memcpy2d }, { "cuMemcpy2DAsync_v2", custom_memcpy2d },
   C(nvEncInitializeEncoder), C(nvEncReconfigureEncoder), C(nvEncEncodePicture), C(nvEncCreateInputBuffer),
   C(nvEncDestroyInputBuffer), C(nvEncLockInputBuffer), C(nvEncUnlockInputBuffer), C(nvEncLockBitstream),
   C(nvEncGetSequenceParams), C(nvEncGetLastErrorString), C(nvEncSetIOCudaStreams),
#undef C
};

/* "name\0desc\0" -> id (index + 1), -1 if the host has no such function */
static int resolve(cf_rd *rd)
{
   const char *name = (const char *)rd->p, *end = (const char *)rd->end;
   const char *desc = memchr(name, 0, (size_t)(end - name));
   if (!desc || !memchr(desc + 1, 0, (size_t)(end - desc - 1)) || strlen(name) >= sizeof(fns[0].name))
      return -1;
   desc++;
   pthread_mutex_lock(&fns_lock);
   int id = -1;
   for (int i = 0; i < nfns; i++)
      if (!strcmp(fns[i].name, name) && !strcmp(fns[i].desc, desc)) {
         id = i + 1;
         goto done;
      }
   struct fn f = {0};
   strcpy(f.name, name);
   for (size_t i = 0; i < sizeof(customs) / sizeof(customs[0]); i++)
      if (!strcmp(name, customs[i].name))
         f.custom = customs[i].fn;
   if (!f.custom && !(f.fn = !strncmp(name, "nvEnc", 5) || !strncmp(name, "NvEnc", 5) ? nvenc_sym(name)
                                                                                       : cuda_sym(name)))
      goto done;
   if (nfns < (int)(sizeof(fns) / sizeof(fns[0]))) {
      f.desc = strdup(desc);
      fns[nfns++] = f;
      id = nfns;
   }
done:
   pthread_mutex_unlock(&fns_lock);
   if (log_calls)
      LOG("resolve %s (%s): %d\n", name, desc, id);
   return id;
}

/* --- a guest thread --- */

static void *serve(void *arg)
{
   int fd = (int)(intptr_t)arg;
   cf_buf req = {0}, reply = {0};
   for (;;) {
      uint32_t head[2];
      if (read_all(fd, head, sizeof(head)) || head[0] > CF_MAX_MESSAGE)
         break;
      req.size = 0;
      cf_grow(&req, head[0]);
      if (read_all(fd, req.data, head[0]))
         break;
      cf_rd rd = {req.data, req.data + head[0], 1};
      reply.size = 0;
      cf_put32(&reply, 0); /* the size, filled in below */
      struct timespec t0;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      const char *name = "resolve";
      if (head[1] == 0) {
         cf_put32(&reply, (uint32_t)resolve(&rd));
      } else if (head[1] <= (uint32_t)nfns) {
         const struct fn *f = &fns[head[1] - 1];
         name = f->name;
         if (f->custom)
            f->custom(&rd, &reply);
         else
            generic(f, &rd, &reply);
      } else {
         cf_put32(&reply, CUDA_ERROR_INVALID_VALUE);
      }
      if (log_calls && head[1]) {
         struct timespec t1;
         clock_gettime(CLOCK_MONOTONIC, &t1);
         LOG("%s: %d (%ld us)\n", name, (int)*(uint32_t *)(reply.data + 4),
             (long)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000));
      }
      uint32_t size = (uint32_t)reply.size - 4;
      memcpy(reply.data, &size, 4);
      if (write_all(fd, reply.data, reply.size))
         break;
   }
   close(fd);
   free(req.data);
   free(reply.data);
   return NULL;
}

static void start_thread(int fd)
{
   int one = 1;
   setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
   pthread_t t;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   if (pthread_create(&t, &attr, serve, (void *)(intptr_t)fd))
      close(fd);
   pthread_attr_destroy(&attr);
}

/* A guest process' host process: the anchor's thread, then the other connections passed over ctl */
static void *anchor(void *arg)
{
   serve(arg);
   _exit(0); /* the guest process is gone: the driver frees its contexts, memory and encoders with ours */
}

static void child(int anchor_fd, int ctl)
{
   pthread_t t;
   int one = 1;
   setsockopt(anchor_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
   if (pthread_create(&t, NULL, anchor, (void *)(intptr_t)anchor_fd))
      _exit(1);
   for (;;) {
      char byte;
      char control[CMSG_SPACE(sizeof(int))];
      struct iovec iov = {&byte, 1};
      struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                           .msg_controllen = sizeof(control)};
      if (recvmsg(ctl, &msg, 0) <= 0)
         pause(); /* the server went away: keep serving until the anchor closes */
      struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
      if (c && c->cmsg_type == SCM_RIGHTS) {
         int fd;
         memcpy(&fd, CMSG_DATA(c), sizeof(fd));
         start_thread(fd);
      }
   }
}

/* --- the server: hellos, one child per guest process --- */

static struct { uint64_t session; pid_t pid; int ctl; } sessions[256];
static int listener;

static void reap(void)
{
   pid_t pid;
   while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
      for (int i = 0; i < 256; i++)
         if (sessions[i].pid == pid) {
            close(sessions[i].ctl);
            memset(&sessions[i], 0, sizeof(sessions[i]));
         }
}

static int send_fd(int ctl, int fd)
{
   char byte = 0, control[CMSG_SPACE(sizeof(int))];
   struct iovec iov = {&byte, 1};
   struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
   struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
   c->cmsg_level = SOL_SOCKET;
   c->cmsg_type = SCM_RIGHTS;
   c->cmsg_len = CMSG_LEN(sizeof(int));
   memcpy(CMSG_DATA(c), &fd, sizeof(fd));
   return sendmsg(ctl, &msg, MSG_NOSIGNAL) == 1 ? 0 : -1;
}

static void accept_one(int fd)
{
   struct timeval timeout = {5, 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
   struct cf_hello hello;
   if (read_all(fd, &hello, sizeof(hello)) || hello.magic != CF_MAGIC || hello.version != CF_VERSION) {
      close(fd);
      return;
   }
   timeout.tv_sec = 0;
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
   reap();
   int slot = -1;
   for (int i = 0; i < 256; i++)
      if (sessions[i].pid && sessions[i].session == hello.session)
         slot = i;
   if (hello.kind == CF_HELLO_THREAD) {
      if (slot < 0 || send_fd(sessions[slot].ctl, fd))
         LOG("a thread of an unknown guest process (%llx)\n", (unsigned long long)hello.session);
      close(fd);
      return;
   }
   if (slot < 0)
      for (int i = 0; i < 256 && slot < 0; i++)
         if (!sessions[i].pid)
            slot = i;
   int pair[2];
   if (slot < 0 || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) {
      close(fd);
      return;
   }
   pid_t pid = fork();
   if (pid == 0) {
      close(listener);
      close(pair[0]);
      for (int i = 0; i < 256; i++)
         if (sessions[i].pid)
            close(sessions[i].ctl);
      child(fd, pair[1]);
   }
   close(pair[1]);
   close(fd);
   if (pid < 0) {
      close(pair[0]);
      return;
   }
   if (sessions[slot].pid)
      close(sessions[slot].ctl); /* a reused session: the old process ends with its own anchor */
   sessions[slot].session = hello.session;
   sessions[slot].pid = pid;
   sessions[slot].ctl = pair[0];
   if (log_calls)
      LOG("guest process %llx: host process %d\n", (unsigned long long)hello.session, pid);
}

int main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: %s <listen address> [port]\n", argv[0]);
      return 2;
   }
   log_calls = getenv("CF_LOG") && *getenv("CF_LOG") == '1';
   struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(argc > 2 ? atoi(argv[2]) : CF_PORT)};
   if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
      fprintf(stderr, "%s: not an IPv4 address\n", argv[1]);
      return 2;
   }
   int s = listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
   setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) || listen(s, 64)) {
      perror("cudafwd: listen");
      return 1;
   }
   LOG("listening on %s:%d\n", argv[1], ntohs(addr.sin_port));
   for (;;) {
      int fd = accept4(s, NULL, NULL, 0);
      if (fd >= 0)
         accept_one(fd);
   }
}
