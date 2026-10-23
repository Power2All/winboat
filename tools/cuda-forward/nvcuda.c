/*
 * nvcuda.dll for the guest: the CUDA driver API, forwarded to cudafwd-server on the host (proto.h), which runs it on
 * the host's NVIDIA driver. Device pointers, contexts, streams and modules are the host's values; host memory is
 * copied across. 64-bit only. The server is WINBOAT_CUDA_SERVER (address[:port]) or the default gateway, port 7737;
 * without one, cuInit returns CUDA_ERROR_NO_DEVICE, as on a machine without an NVIDIA GPU.
 *
 * The functions are ffmpeg's (nv-codec-headers' dynlink_loader.h) for now; the CUDA runtime's private tables
 * (cuGetExportTable) are not there yet, so programs built with cudart don't start.
 */
#define CF_BUILDING_NVCUDA
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#include "transport.h"
#include <ffnvcodec/dynlink_cuda.h>

#define CUDA_ERROR_NOT_SUPPORTED ((CUresult)801)

/* --- transport --- */

struct thread_state {
   SOCKET s;
   cf_buf req, reply;
};

static DWORD tls = TLS_OUT_OF_INDEXES;
static CRITICAL_SECTION anchor_lock;
static SOCKET anchor_socket = INVALID_SOCKET;
static int unavailable; /* no server: not asked again in this process */
static struct sockaddr_in server;
static uint64_t session;

static void find_server(void)
{
   WSADATA wsa;
   WSAStartup(MAKEWORD(2, 2), &wsa);
   server.sin_family = AF_INET;
   server.sin_port = htons(CF_PORT);
   char env[64];
   DWORD n = GetEnvironmentVariableA("WINBOAT_CUDA_SERVER", env, sizeof(env));
   if (n && n < sizeof(env)) {
      char *colon = strchr(env, ':');
      if (colon) {
         *colon = 0;
         server.sin_port = htons((u_short)atoi(colon + 1));
      }
      inet_pton(AF_INET, env, &server.sin_addr);
   } else {
      MIB_IPFORWARDROW row;
      if (GetBestRoute(inet_addr("1.1.1.1"), 0, &row) == NO_ERROR)
         server.sin_addr.s_addr = row.dwForwardNextHop;
   }
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   session = ((uint64_t)GetCurrentProcessId() << 32) ^ (uint64_t)t.QuadPart;
}

static int send_all(SOCKET s, const void *p, size_t n)
{
   for (const char *b = p; n;) {
      int put = send(s, b, n > INT_MAX ? INT_MAX : (int)n, 0);
      if (put <= 0)
         return -1;
      b += put;
      n -= (size_t)put;
   }
   return 0;
}

static int recv_all(SOCKET s, void *p, size_t n)
{
   for (char *b = p; n;) {
      int got = recv(s, b, n > INT_MAX ? INT_MAX : (int)n, 0);
      if (got <= 0)
         return -1;
      b += got;
      n -= (size_t)got;
   }
   return 0;
}

/* Connects with a short timeout (a dropped SYN would otherwise stall a program probing for CUDA) and says hello */
static SOCKET connect_server(uint32_t kind)
{
   if (!server.sin_addr.s_addr)
      return INVALID_SOCKET;
   SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (s == INVALID_SOCKET)
      return s;
   u_long nonblocking = 1;
   ioctlsocket(s, FIONBIO, &nonblocking);
   connect(s, (struct sockaddr *)&server, sizeof(server));
   fd_set writable, failed;
   FD_ZERO(&writable);
   FD_SET(s, &writable);
   FD_ZERO(&failed);
   FD_SET(s, &failed);
   struct timeval timeout = {1, 0};
   nonblocking = 0;
   if (select(0, NULL, &writable, &failed, &timeout) != 1 || !FD_ISSET(s, &writable) ||
       ioctlsocket(s, FIONBIO, &nonblocking)) {
      closesocket(s);
      return INVALID_SOCKET;
   }
   BOOL one = TRUE;
   setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
   struct cf_hello hello = {CF_MAGIC, CF_VERSION, kind, 0, session};
   if (send_all(s, &hello, sizeof(hello))) {
      closesocket(s);
      return INVALID_SOCKET;
   }
   return s;
}

/* This thread's connection; the process' anchor first (it keeps the host process alive while threads come and go) */
static struct thread_state *state(void)
{
   struct thread_state *st = TlsGetValue(tls);
   if (!st) {
      st = calloc(1, sizeof(*st));
      st->s = INVALID_SOCKET;
      TlsSetValue(tls, st);
   }
   if (st->s == INVALID_SOCKET && !unavailable) {
      EnterCriticalSection(&anchor_lock);
      if (anchor_socket == INVALID_SOCKET && !unavailable) {
         find_server();
         anchor_socket = connect_server(CF_HELLO_ANCHOR);
         unavailable = anchor_socket == INVALID_SOCKET;
      }
      LeaveCriticalSection(&anchor_lock);
      if (!unavailable)
         st->s = connect_server(CF_HELLO_THREAD);
   }
   return st->s == INVALID_SOCKET ? NULL : st;
}

/* Sends st->req as call id; the reply lands in st->reply */
static int call(struct thread_state *st, uint32_t id, cf_rd *reply)
{
   uint32_t head[2] = {(uint32_t)st->req.size, id}, size;
   if (send_all(st->s, head, sizeof(head)) || send_all(st->s, st->req.data, st->req.size) ||
       recv_all(st->s, &size, 4) || size > CF_MAX_MESSAGE) {
      closesocket(st->s);
      st->s = INVALID_SOCKET;
      return -1;
   }
   st->reply.size = 0;
   cf_grow(&st->reply, size);
   if (recv_all(st->s, st->reply.data, size)) {
      closesocket(st->s);
      st->s = INVALID_SOCKET;
      return -1;
   }
   *reply = (cf_rd){st->reply.data, st->reply.data + size, 1};
   return 0;
}

/* name -> id: > 0, -1 if the host lacks it, 0 without a server */
static int resolve(const char *name, const char *desc)
{
   struct thread_state *st = state();
   if (!st)
      return 0;
   st->req.size = 0;
   cf_put(&st->req, name, strlen(name) + 1);
   cf_put(&st->req, desc, strlen(desc) + 1);
   cf_rd rd;
   if (call(st, 0, &rd))
      return 0;
   int id = (int)cf_get32(&rd);
   return rd.ok && id ? id : 0;
}

CF_API int cf_generic(int *id, const char *name, const char *desc, ...)
{
   if (!*id && !(*id = resolve(name, desc)))
      return CF_ERR_TRANSPORT;
   if (*id < 0)
      return CF_ERR_MISSING;
   uint64_t v[CF_MAX_ARGS];
   cf_tok t[CF_MAX_ARGS];
   int n = 0;
   va_list ap;
   va_start(ap, desc);
   for (const char *d = desc; n < CF_MAX_ARGS && cf_next_tok(&d, &t[n]); n++)
      v[n] = t[n].kind == 'u' ? va_arg(ap, unsigned) : va_arg(ap, uint64_t);
   va_end(ap);

   struct thread_state *st = state();
   if (!st)
      return CF_ERR_TRANSPORT;
   cf_buf *b = &st->req;
   b->size = 0;
   for (int i = 0; i < n; i++) {
      void *p = (void *)(uintptr_t)v[i];
      size_t size = t[i].kind == 'b' || t[i].kind == 'r' ? (t[i].n < (unsigned)n ? v[t[i].n] : 0) * t[i].mul : t[i].n;
      switch (t[i].kind) {
      case 'u': cf_put32(b, (uint32_t)v[i]); break;
      case 'v': cf_put64(b, v[i]); break;
      case 'g': cf_put(b, p, 16); break;
      case 'z':
         cf_put8(b, p != NULL);
         if (p) {
            cf_put32(b, (uint32_t)strlen(p) + 1);
            cf_put(b, p, strlen(p) + 1);
         }
         break;
      default: /* i o x b r */
         cf_put8(b, p != NULL);
         if (!p)
            break;
         if (t[i].kind == 'b' || t[i].kind == 'r')
            cf_put64(b, size);
         if (t[i].kind == 'i' || t[i].kind == 'x' || t[i].kind == 'b')
            cf_put(b, p, size);
      }
   }
   cf_rd rd;
   if (call(st, (uint32_t)*id, &rd))
      return CF_ERR_TRANSPORT;
   int result = (int)cf_get32(&rd);
   for (int i = 0; i < n; i++) {
      void *p = (void *)(uintptr_t)v[i];
      if (!p || (t[i].kind != 'o' && t[i].kind != 'x' && t[i].kind != 'r'))
         continue;
      cf_get(&rd, p, t[i].kind == 'r' ? (t[i].n < (unsigned)n ? v[t[i].n] : 0) * t[i].mul : t[i].n);
   }
   return rd.ok ? result : CF_ERR_TRANSPORT;
}

CF_API int cf_custom(int *id, const char *name, const cf_buf *req, int *result, cf_rd *reply)
{
   if (!*id && !(*id = resolve(name, "")))
      return CF_ERR_TRANSPORT;
   if (*id < 0)
      return CF_ERR_MISSING;
   struct thread_state *st = state();
   if (!st)
      return CF_ERR_TRANSPORT;
   st->req.size = 0;
   cf_put(&st->req, req->data, req->size);
   if (call(st, (uint32_t)*id, reply))
      return CF_ERR_TRANSPORT;
   *result = (int)cf_get32(reply);
   return reply->ok ? 0 : CF_ERR_TRANSPORT;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
   if (reason == DLL_PROCESS_ATTACH) {
      tls = TlsAlloc();
      InitializeCriticalSection(&anchor_lock);
   } else if (reason == DLL_THREAD_DETACH || reason == DLL_PROCESS_DETACH) {
      struct thread_state *st = TlsGetValue(tls);
      if (st) {
         if (st->s != INVALID_SOCKET)
            closesocket(st->s);
         free(st->req.data);
         free(st->reply.data);
         free(st);
         TlsSetValue(tls, NULL);
      }
      if (reason == DLL_PROCESS_DETACH && !reserved && anchor_socket != INVALID_SOCKET)
         closesocket(anchor_socket); /* FreeLibrary: the host process goes with it */
   }
   return TRUE;
}

/* --- the CUDA driver API --- */

static CUresult cu(int r)
{
   return r == CF_ERR_TRANSPORT ? CUDA_ERROR_UNKNOWN : r == CF_ERR_MISSING ? CUDA_ERROR_NOT_SUPPORTED : (CUresult)r;
}

/* Declared through nv-codec-headers' function types, so a wrong signature doesn't compile */
#define FN(type, name) __declspec(dllexport) type name; __declspec(dllexport) CUresult CUDAAPI name
#define FWD(desc, ...) do { static int id_; return cu(cf_generic(&id_, __func__, desc, ##__VA_ARGS__)); } while (0)

FN(tcuInit, cuInit)(unsigned int flags)
{
   static int id;
   int r = cf_generic(&id, __func__, "u", flags);
   return r == CF_ERR_TRANSPORT || r == CF_ERR_MISSING ? CUDA_ERROR_NO_DEVICE : (CUresult)r;
}

FN(tcuDriverGetVersion, cuDriverGetVersion)(int *v) { FWD("o4", v); }
FN(tcuDeviceGetCount, cuDeviceGetCount)(int *count) { FWD("o4", count); }
FN(tcuDeviceGet, cuDeviceGet)(CUdevice *device, int ordinal) { FWD("o4 u", device, ordinal); }
FN(tcuDeviceGetAttribute, cuDeviceGetAttribute)(int *pi, CUdevice_attribute attrib, CUdevice dev) { FWD("o4 u u", pi, attrib, dev); }
FN(tcuDeviceGetName, cuDeviceGetName)(char *name, int len, CUdevice dev) { FWD("r1 u u", name, len, dev); }
FN(tcuDeviceGetUuid, cuDeviceGetUuid)(CUuuid *uuid, CUdevice dev) { FWD("o16 u", uuid, dev); }
FN(tcuDeviceGetUuid_v2, cuDeviceGetUuid_v2)(CUuuid *uuid, CUdevice dev) { FWD("o16 u", uuid, dev); }
FN(tcuDeviceGetLuid, cuDeviceGetLuid)(char *luid, unsigned int *mask, CUdevice dev) { FWD("o8 o4 u", luid, mask, dev); }
FN(tcuDeviceGetByPCIBusId, cuDeviceGetByPCIBusId)(CUdevice *dev, const char *id) { FWD("o4 z", dev, id); }
FN(tcuDeviceGetPCIBusId, cuDeviceGetPCIBusId)(char *id, int len, CUdevice dev) { FWD("r1 u u", id, len, dev); }
FN(tcuDeviceComputeCapability, cuDeviceComputeCapability)(int *major, int *minor, CUdevice dev) { FWD("o4 o4 u", major, minor, dev); }
__declspec(dllexport) CUresult CUDAAPI cuDeviceTotalMem_v2(size_t *bytes, CUdevice dev) { FWD("o8 u", bytes, dev); }

FN(tcuCtxCreate_v2, cuCtxCreate_v2)(CUcontext *pctx, unsigned int flags, CUdevice dev) { FWD("o8 u u", pctx, flags, dev); }
FN(tcuCtxSynchronize, cuCtxSynchronize)(void) { FWD(""); }
FN(tcuCtxGetStreamPriorityRange, cuCtxGetStreamPriorityRange)(int *least, int *greatest) { FWD("o4 o4", least, greatest); }
FN(tcuCtxGetCurrent, cuCtxGetCurrent)(CUcontext *pctx) { FWD("o8", pctx); }
FN(tcuCtxSetLimit, cuCtxSetLimit)(CUlimit limit, size_t value) { FWD("u v", limit, value); }
FN(tcuCtxPushCurrent_v2, cuCtxPushCurrent_v2)(CUcontext ctx) { FWD("v", ctx); }
FN(tcuCtxPopCurrent_v2, cuCtxPopCurrent_v2)(CUcontext *pctx) { FWD("o8", pctx); }
FN(tcuCtxDestroy_v2, cuCtxDestroy_v2)(CUcontext ctx) { FWD("v", ctx); }
FN(tcuCtxGetDevice, cuCtxGetDevice)(CUdevice *device) { FWD("o4", device); }
__declspec(dllexport) CUresult CUDAAPI cuCtxSetCurrent(CUcontext ctx) { FWD("v", ctx); }
FN(tcuDevicePrimaryCtxRetain, cuDevicePrimaryCtxRetain)(CUcontext *pctx, CUdevice dev) { FWD("o8 u", pctx, dev); }
FN(tcuDevicePrimaryCtxRelease, cuDevicePrimaryCtxRelease)(CUdevice dev) { FWD("u", dev); }
FN(tcuDevicePrimaryCtxSetFlags, cuDevicePrimaryCtxSetFlags)(CUdevice dev, unsigned int flags) { FWD("u u", dev, flags); }
FN(tcuDevicePrimaryCtxGetState, cuDevicePrimaryCtxGetState)(CUdevice dev, unsigned int *flags, int *active) { FWD("u o4 o4", dev, flags, active); }
FN(tcuDevicePrimaryCtxReset, cuDevicePrimaryCtxReset)(CUdevice dev) { FWD("u", dev); }
/* The driver exports these under _v2 names as well (cuda.h maps the plain names to them) */
__declspec(dllexport) CUresult CUDAAPI cuDevicePrimaryCtxRelease_v2(CUdevice dev) { FWD("u", dev); }
__declspec(dllexport) CUresult CUDAAPI cuDevicePrimaryCtxSetFlags_v2(CUdevice dev, unsigned int flags) { FWD("u u", dev, flags); }
__declspec(dllexport) CUresult CUDAAPI cuDevicePrimaryCtxReset_v2(CUdevice dev) { FWD("u", dev); }

FN(tcuMemAlloc_v2, cuMemAlloc_v2)(CUdeviceptr *dptr, size_t size) { FWD("o8 v", dptr, size); }
FN(tcuMemAllocPitch_v2, cuMemAllocPitch_v2)(CUdeviceptr *dptr, size_t *pitch, size_t w, size_t h, unsigned int elem) { FWD("o8 o8 v v u", dptr, pitch, w, h, elem); }
FN(tcuMemFree_v2, cuMemFree_v2)(CUdeviceptr dptr) { FWD("v", dptr); }
FN(tcuMemAllocAsync, cuMemAllocAsync)(CUdeviceptr *dptr, size_t size, CUstream s) { FWD("o8 v v", dptr, size, s); }
FN(tcuMemFreeAsync, cuMemFreeAsync)(CUdeviceptr dptr, CUstream s) { FWD("v v", dptr, s); }
__declspec(dllexport) CUresult CUDAAPI cuMemGetInfo_v2(size_t *free_bytes, size_t *total) { FWD("o8 o8", free_bytes, total); }
FN(tcuMemsetD8Async, cuMemsetD8Async)(CUdeviceptr d, unsigned char v, size_t n, CUstream s) { FWD("v u v v", d, v, n, s); }
FN(tcuMemsetD16Async, cuMemsetD16Async)(CUdeviceptr d, unsigned short v, size_t n, CUstream s) { FWD("v u v v", d, v, n, s); }
FN(tcuMemsetD32Async, cuMemsetD32Async)(CUdeviceptr d, unsigned int v, size_t n, CUstream s) { FWD("v u v v", d, v, n, s); }
FN(tcuMemsetD2D8Async, cuMemsetD2D8Async)(CUdeviceptr d, size_t pitch, unsigned char v, size_t w, size_t h, CUstream s) { FWD("v v u v v v", d, pitch, v, w, h, s); }
FN(tcuMemsetD2D16Async, cuMemsetD2D16Async)(CUdeviceptr d, size_t pitch, unsigned short v, size_t w, size_t h, CUstream s) { FWD("v v u v v v", d, pitch, v, w, h, s); }
FN(tcuMemsetD2D32Async, cuMemsetD2D32Async)(CUdeviceptr d, size_t pitch, unsigned int v, size_t w, size_t h, CUstream s) { FWD("v v u v v v", d, pitch, v, w, h, s); }
FN(tcuMemcpy, cuMemcpy)(CUdeviceptr dst, CUdeviceptr src, size_t n) { FWD("v v v", dst, src, n); }
FN(tcuMemcpyAsync, cuMemcpyAsync)(CUdeviceptr dst, CUdeviceptr src, size_t n, CUstream s) { FWD("v v v v", dst, src, n, s); }
FN(tcuMemcpyHtoD_v2, cuMemcpyHtoD_v2)(CUdeviceptr dst, const void *src, size_t n) { FWD("v b2 v", dst, src, n); }
FN(tcuMemcpyHtoDAsync_v2, cuMemcpyHtoDAsync_v2)(CUdeviceptr dst, const void *src, size_t n, CUstream s) { FWD("v b2 v v", dst, src, n, s); }
FN(tcuMemcpyDtoH_v2, cuMemcpyDtoH_v2)(void *dst, CUdeviceptr src, size_t n) { FWD("r2 v v", dst, src, n); }
FN(tcuMemcpyDtoHAsync_v2, cuMemcpyDtoHAsync_v2)(void *dst, CUdeviceptr src, size_t n, CUstream s) { FWD("r2 v v v", dst, src, n, s); }
FN(tcuMemcpyDtoD_v2, cuMemcpyDtoD_v2)(CUdeviceptr dst, CUdeviceptr src, size_t n) { FWD("v v v", dst, src, n); }
FN(tcuMemcpyDtoDAsync_v2, cuMemcpyDtoDAsync_v2)(CUdeviceptr dst, CUdeviceptr src, size_t n, CUstream s) { FWD("v v v v", dst, src, n, s); }

/* Host rows are packed for the trip: pitch = width, x = y = 0 on the host side (the server's custom_memcpy2d) */
static CUresult memcpy2d(int *id, const char *name, const CUDA_MEMCPY2D *c0, int async, CUstream stream)
{
   if (!c0)
      return CUDA_ERROR_INVALID_VALUE;
   CUDA_MEMCPY2D c = *c0;
   size_t w = c.WidthInBytes, h = c.Height;
   int src_host = c.srcMemoryType == CU_MEMORYTYPE_HOST, dst_host = c.dstMemoryType == CU_MEMORYTYPE_HOST;
   if (src_host) {
      c.srcXInBytes = c.srcY = 0;
      c.srcPitch = w;
   }
   if (dst_host) {
      c.dstXInBytes = c.dstY = 0;
      c.dstPitch = w;
   }
   cf_buf req = {0};
   cf_put(&req, &c, sizeof(c));
   cf_put8(&req, (uint8_t)async);
   cf_put64(&req, (uintptr_t)stream);
   for (size_t y = 0; src_host && y < h; y++)
      cf_put(&req, (const char *)c0->srcHost + (c0->srcY + y) * c0->srcPitch + c0->srcXInBytes, w);
   int result;
   cf_rd rd;
   int r = cf_custom(id, name, &req, &result, &rd);
   free(req.data);
   if (r)
      return cu(r);
   for (size_t y = 0; dst_host && result == CUDA_SUCCESS && y < h; y++)
      cf_get(&rd, (char *)c0->dstHost + (c0->dstY + y) * c0->dstPitch + c0->dstXInBytes, w);
   return (CUresult)result;
}

FN(tcuMemcpy2D_v2, cuMemcpy2D_v2)(const CUDA_MEMCPY2D *c) { static int id; return memcpy2d(&id, __func__, c, 0, NULL); }
FN(tcuMemcpy2DAsync_v2, cuMemcpy2DAsync_v2)(const CUDA_MEMCPY2D *c, CUstream s) { static int id; return memcpy2d(&id, __func__, c, 1, s); }

/* Host memory stays in the guest: pinning is only about speed there, and copies with it are synchronous here.
 * Mapped (device-visible) host memory can't be had across the VM boundary. */
FN(tcuMemHostAlloc, cuMemHostAlloc)(void **pp, size_t size, unsigned int flags)
{
   if (flags & 0x02) /* CU_MEMHOSTALLOC_DEVICEMAP */
      return CUDA_ERROR_NOT_SUPPORTED;
   *pp = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
   return *pp ? CUDA_SUCCESS : CUDA_ERROR_OUT_OF_MEMORY;
}
__declspec(dllexport) CUresult CUDAAPI cuMemAllocHost_v2(void **pp, size_t size) { return cuMemHostAlloc(pp, size, 0); }
FN(tcuMemFreeHost, cuMemFreeHost)(void *p) { return VirtualFree(p, 0, MEM_RELEASE) ? CUDA_SUCCESS : CUDA_ERROR_INVALID_VALUE; }
__declspec(dllexport) CUresult CUDAAPI cuMemHostRegister_v2(void *p, size_t size, unsigned int flags)
{
   return flags & 0x02 ? CUDA_ERROR_NOT_SUPPORTED : CUDA_SUCCESS; /* CU_MEMHOSTREGISTER_DEVICEMAP */
}
__declspec(dllexport) CUresult CUDAAPI cuMemHostUnregister(void *p) { return CUDA_SUCCESS; }
FN(tcuMemAllocManaged, cuMemAllocManaged)(CUdeviceptr *dptr, size_t size, unsigned int flags) { return CUDA_ERROR_NOT_SUPPORTED; }

FN(tcuStreamCreate, cuStreamCreate)(CUstream *s, unsigned int flags) { FWD("o8 u", s, flags); }
FN(tcuStreamCreateWithPriority, cuStreamCreateWithPriority)(CUstream *s, unsigned int flags, int priority) { FWD("o8 u u", s, flags, priority); }
FN(tcuStreamQuery, cuStreamQuery)(CUstream s) { FWD("v", s); }
FN(tcuStreamSynchronize, cuStreamSynchronize)(CUstream s) { FWD("v", s); }
FN(tcuStreamDestroy_v2, cuStreamDestroy_v2)(CUstream s) { FWD("v", s); }
FN(tcuStreamWaitEvent, cuStreamWaitEvent)(CUstream s, CUevent e, unsigned int flags) { FWD("v v u", s, e, flags); }
/* ponytail: host callbacks run in the calling thread once the stream is done (in order, but the caller waits);
 * a callback channel from the server if a program depends on them being asynchronous */
FN(tcuStreamAddCallback, cuStreamAddCallback)(CUstream s, CUstreamCallback *callback, void *data, unsigned int flags)
{
   CUresult r = cuStreamSynchronize(s);
   callback(s, r, data);
   return CUDA_SUCCESS;
}
FN(tcuLaunchHostFunc, cuLaunchHostFunc)(CUstream s, CUhostFn *fn, void *data)
{
   CUresult r = cuStreamSynchronize(s);
   if (r == CUDA_SUCCESS)
      fn(data);
   return r;
}
FN(tcuEventCreate, cuEventCreate)(CUevent *e, unsigned int flags) { FWD("o8 u", e, flags); }
FN(tcuEventDestroy_v2, cuEventDestroy_v2)(CUevent e) { FWD("v", e); }
FN(tcuEventSynchronize, cuEventSynchronize)(CUevent e) { FWD("v", e); }
FN(tcuEventQuery, cuEventQuery)(CUevent e) { FWD("v", e); }
FN(tcuEventRecord, cuEventRecord)(CUevent e, CUstream s) { FWD("v v", e, s); }
__declspec(dllexport) CUresult CUDAAPI cuEventElapsedTime(float *ms, CUevent start, CUevent end) { FWD("o4 v v", ms, start, end); }

/* A module image's size: fatbin (header + payload), ELF cubin (up to its header tables), else PTX text */
static size_t image_size(const void *image)
{
   const uint8_t *p = image;
   uint32_t magic;
   memcpy(&magic, p, 4);
   if (magic == 0xBA55ED50u) {
      uint16_t header;
      uint64_t payload;
      memcpy(&header, p + 6, 2);
      memcpy(&payload, p + 8, 8);
      return header + payload;
   }
   if (!memcmp(p, "\x7f" "ELF", 4) && p[4] == 2) {
      uint64_t phoff, shoff;
      uint16_t phentsize, phnum, shentsize, shnum;
      memcpy(&phoff, p + 0x20, 8);
      memcpy(&shoff, p + 0x28, 8);
      memcpy(&phentsize, p + 0x36, 2);
      memcpy(&phnum, p + 0x38, 2);
      memcpy(&shentsize, p + 0x3a, 2);
      memcpy(&shnum, p + 0x3c, 2);
      uint64_t a = phoff + (uint64_t)phentsize * phnum, b = shoff + (uint64_t)shentsize * shnum;
      return a > b ? a : b;
   }
   return strlen(image) + 1;
}

/* The size rides along as a third argument the host's cuModuleLoadData ignores */
FN(tcuModuleLoadData, cuModuleLoadData)(CUmodule *module, const void *image)
{
   if (!image)
      return CUDA_ERROR_INVALID_VALUE;
   static int id;
   return cu(cf_generic(&id, __func__, "o8 b2 v", module, image, (uint64_t)image_size(image)));
}
FN(tcuModuleUnload, cuModuleUnload)(CUmodule m) { FWD("v", m); }
FN(tcuModuleGetFunction, cuModuleGetFunction)(CUfunction *f, CUmodule m, const char *name) { FWD("o8 v z", f, m, name); }
FN(tcuModuleGetGlobal, cuModuleGetGlobal_v2)(CUdeviceptr *dptr, size_t *bytes, CUmodule m, const char *name) { FWD("o8 o8 v z", dptr, bytes, m, name); }

/* Kernel arguments are packed by the function's parameter sizes, asked from the host once per function */
static struct { CUfunction f; uint32_t count, sizes[64]; } params_cache[512];
static SRWLOCK params_lock = SRWLOCK_INIT;

static int param_sizes(CUfunction f, uint32_t *count, uint32_t *sizes)
{
   size_t slot = ((uintptr_t)f >> 4) % 512;
   AcquireSRWLockShared(&params_lock);
   int hit = params_cache[slot].f == f;
   if (hit) {
      *count = params_cache[slot].count;
      memcpy(sizes, params_cache[slot].sizes, *count * 4);
   }
   ReleaseSRWLockShared(&params_lock);
   if (hit)
      return CUDA_SUCCESS;
   static int id;
   cf_buf req = {0};
   cf_put64(&req, (uintptr_t)f);
   int result;
   cf_rd rd;
   int r = cf_custom(&id, "cfFuncParams", &req, &result, &rd);
   free(req.data);
   if (r || result)
      return r ? cu(r) : result;
   *count = cf_get32(&rd);
   if (*count > 64)
      return CUDA_ERROR_NOT_SUPPORTED;
   for (uint32_t i = 0; i < *count; i++) {
      cf_get32(&rd); /* offset */
      sizes[i] = cf_get32(&rd);
   }
   AcquireSRWLockExclusive(&params_lock);
   params_cache[slot].f = f;
   params_cache[slot].count = *count;
   memcpy(params_cache[slot].sizes, sizes, *count * 4);
   ReleaseSRWLockExclusive(&params_lock);
   return CUDA_SUCCESS;
}

FN(tcuLaunchKernel, cuLaunchKernel)(CUfunction f, unsigned int gx, unsigned int gy, unsigned int gz, unsigned int bx,
                                    unsigned int by, unsigned int bz, unsigned int shared, CUstream s, void **params,
                                    void **extra)
{
   cf_buf req = {0};
   cf_put64(&req, (uintptr_t)f);
   unsigned dims[7] = {gx, gy, gz, bx, by, bz, shared};
   cf_put(&req, dims, sizeof(dims));
   cf_put64(&req, (uintptr_t)s);
   if (extra) {
      const void *buffer = NULL;
      size_t size = 0;
      for (int i = 0; extra[i]; i += 2) {
         if (extra[i] == (void *)1) /* CU_LAUNCH_PARAM_BUFFER_POINTER */
            buffer = extra[i + 1];
         else if (extra[i] == (void *)2) /* CU_LAUNCH_PARAM_BUFFER_SIZE */
            size = *(size_t *)extra[i + 1];
      }
      cf_put32(&req, 0xffffffffu);
      cf_put64(&req, buffer ? size : 0);
      cf_put(&req, buffer, buffer ? size : 0);
   } else {
      uint32_t count = 0, sizes[64];
      int r = params ? param_sizes(f, &count, sizes) : CUDA_SUCCESS;
      if (r) {
         free(req.data);
         return (CUresult)r;
      }
      cf_put32(&req, count);
      for (uint32_t i = 0; i < count; i++) {
         cf_put32(&req, sizes[i]);
         cf_put(&req, params[i], sizes[i]);
      }
   }
   static int id;
   int result;
   cf_rd rd;
   int r = cf_custom(&id, __func__, &req, &result, &rd);
   free(req.data);
   return r ? cu(r) : (CUresult)result;
}

/* Error names and descriptions, kept per code */
static CUresult error_string(int *id, const char *name, CUresult e, const char **out, char **cache)
{
   if (!out)
      return CUDA_ERROR_INVALID_VALUE;
   if ((unsigned)e < 1024 && cache[e]) {
      *out = cache[e];
      return CUDA_SUCCESS;
   }
   cf_buf req = {0};
   cf_put32(&req, (uint32_t)e);
   int result;
   cf_rd rd;
   int r = cf_custom(id, name, &req, &result, &rd);
   free(req.data);
   *out = NULL;
   if (r)
      return cu(r);
   uint32_t n = cf_get32(&rd);
   const char *s = cf_take(&rd, n);
   if (result || !s || !n || s[n - 1])
      return result ? (CUresult)result : CUDA_ERROR_INVALID_VALUE;
   char *copy = _strdup(s);
   if ((unsigned)e < 1024 && InterlockedCompareExchangePointer((void **)&cache[e], copy, NULL)) {
      free(copy);
      copy = cache[e];
   }
   *out = copy; /* ponytail: codes of 1024 and up leak their copy per call */
   return CUDA_SUCCESS;
}
static char *error_names[1024], *error_strings[1024];
FN(tcuGetErrorName, cuGetErrorName)(CUresult e, const char **s) { static int id; return error_string(&id, __func__, e, s, error_names); }
FN(tcuGetErrorString, cuGetErrorString)(CUresult e, const char **s) { static int id; return error_string(&id, __func__, e, s, error_strings); }

FN(tcuTexObjectCreate, cuTexObjectCreate)(CUtexObject *t, const CUDA_RESOURCE_DESC *res, const CUDA_TEXTURE_DESC *tex, const CUDA_RESOURCE_VIEW_DESC *view)
{
   FWD(CF_DESC("o8 i%zu i%zu i%zu", sizeof(*res), sizeof(*tex), sizeof(*view)), t, res, tex, view);
}
FN(tcuTexObjectDestroy, cuTexObjectDestroy)(CUtexObject t) { FWD("v", t); }
FN(tcuArrayCreate, cuArrayCreate_v2)(CUarray *a, const CUDA_ARRAY_DESCRIPTOR *d) { FWD(CF_DESC("o8 i%zu", sizeof(*d)), a, d); }
FN(tcuArray3DCreate, cuArray3DCreate_v2)(CUarray *a, const CUDA_ARRAY3D_DESCRIPTOR *d) { FWD(CF_DESC("o8 i%zu", sizeof(*d)), a, d); }
FN(tcuArrayDestroy, cuArrayDestroy)(CUarray a) { FWD("v", a); }
FN(tcuArray3DGetDescriptor, cuArray3DGetDescriptor_v2)(CUDA_ARRAY3D_DESCRIPTOR *d, CUarray a) { FWD(CF_DESC("o%zu v", sizeof(*d)), d, a); }
FN(tcuArrayGetPlane, cuArrayGetPlane)(CUarray *plane, CUarray a, unsigned int i) { FWD("o8 v u", plane, a, i); }

/* --- not across the VM boundary (yet): graphics interop, external memory, JIT linking, EGL --- */

#define NO(type, name, ...) FN(type, name)(__VA_ARGS__) { return CUDA_ERROR_NOT_SUPPORTED; }
NO(tcuLinkCreate, cuLinkCreate_v2, unsigned int n, CUjit_option *o, void **v, CUlinkState *s)
NO(tcuLinkAddData, cuLinkAddData_v2, CUlinkState s, CUjitInputType t, void *d, size_t n, const char *name, unsigned int no, CUjit_option *o, void **v)
NO(tcuLinkComplete, cuLinkComplete, CUlinkState s, void **cubin, size_t *size)
NO(tcuLinkDestroy, cuLinkDestroy, CUlinkState s)
NO(tcuGraphicsGLRegisterImage, cuGraphicsGLRegisterImage, CUgraphicsResource *r, GLuint image, GLenum target, unsigned int flags)
NO(tcuGraphicsUnregisterResource, cuGraphicsUnregisterResource, CUgraphicsResource r)
NO(tcuGraphicsMapResources, cuGraphicsMapResources, unsigned int n, CUgraphicsResource *r, CUstream s)
NO(tcuGraphicsUnmapResources, cuGraphicsUnmapResources, unsigned int n, CUgraphicsResource *r, CUstream s)
NO(tcuGraphicsSubResourceGetMappedArray, cuGraphicsSubResourceGetMappedArray, CUarray *a, CUgraphicsResource r, unsigned int i, unsigned int level)
NO(tcuGraphicsResourceGetMappedPointer, cuGraphicsResourceGetMappedPointer_v2, CUdeviceptr *p, size_t *size, CUgraphicsResource r)
NO(tcuGraphicsD3D11RegisterResource, cuGraphicsD3D11RegisterResource, CUgraphicsResource *r, void *resource, unsigned int flags)
NO(tcuImportExternalMemory, cuImportExternalMemory, CUexternalMemory *m, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC *d)
NO(tcuDestroyExternalMemory, cuDestroyExternalMemory, CUexternalMemory m)
NO(tcuExternalMemoryGetMappedBuffer, cuExternalMemoryGetMappedBuffer, CUdeviceptr *p, CUexternalMemory m, const CUDA_EXTERNAL_MEMORY_BUFFER_DESC *d)
NO(tcuExternalMemoryGetMappedMipmappedArray, cuExternalMemoryGetMappedMipmappedArray, CUmipmappedArray *a, CUexternalMemory m, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC *d)
NO(tcuMipmappedArrayGetLevel, cuMipmappedArrayGetLevel, CUarray *a, CUmipmappedArray m, unsigned int level)
NO(tcuMipmappedArrayDestroy, cuMipmappedArrayDestroy, CUmipmappedArray m)
NO(tcuImportExternalSemaphore, cuImportExternalSemaphore, CUexternalSemaphore *s, const CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC *d)
NO(tcuDestroyExternalSemaphore, cuDestroyExternalSemaphore, CUexternalSemaphore s)
NO(tcuSignalExternalSemaphoresAsync, cuSignalExternalSemaphoresAsync, const CUexternalSemaphore *s, const CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS *p, unsigned int n, CUstream st)
NO(tcuWaitExternalSemaphoresAsync, cuWaitExternalSemaphoresAsync, const CUexternalSemaphore *s, const CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS *p, unsigned int n, CUstream st)
NO(tcuEGLStreamProducerConnect, cuEGLStreamProducerConnect, CUeglStreamConnection *c, ffnv_EGLStreamKHR s, ffnv_EGLint w, ffnv_EGLint h)
NO(tcuEGLStreamProducerDisconnect, cuEGLStreamProducerDisconnect, CUeglStreamConnection *c)
NO(tcuEGLStreamConsumerDisconnect, cuEGLStreamConsumerDisconnect, CUeglStreamConnection *c)
NO(tcuEGLStreamProducerPresentFrame, cuEGLStreamProducerPresentFrame, CUeglStreamConnection *c, CUeglFrame f, CUstream *s)
NO(tcuEGLStreamProducerReturnFrame, cuEGLStreamProducerReturnFrame, CUeglStreamConnection *c, CUeglFrame *f, CUstream *s)

/* No CUDA device behind any Direct3D or OpenGL device of the guest */
FN(tcuD3D11GetDevice, cuD3D11GetDevice)(CUdevice *device, void *adapter) { return CUDA_ERROR_NO_DEVICE; }
FN(tcuD3D11GetDevices, cuD3D11GetDevices)(unsigned int *count, CUdevice *devices, unsigned int n, void *device, CUd3d11DeviceList list)
{
   if (count)
      *count = 0;
   return CUDA_ERROR_NO_DEVICE;
}
FN(tcuGLGetDevices_v2, cuGLGetDevices_v2)(unsigned int *count, CUdevice *devices, unsigned int n, CUGLDeviceList list)
{
   if (count)
      *count = 0;
   return CUDA_ERROR_NO_DEVICE;
}
