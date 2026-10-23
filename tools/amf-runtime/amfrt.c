/*
 * Copyright 2026 WinBoat contributors
 * SPDX-License-Identifier: MIT
 */

/* amfrt64.dll: an AMF runtime (AMD's Advanced Media Framework, the public API of its SDK: amf/, MIT) for WinBoat's
 * virtual GPU. Applications that encode through AMF (ffmpeg's h264_amf/hevc_amf, and the programs built on it,
 * Topaz Video AI among them) get the host GPU's encoder: the encoder component records Vulkan Video encoding through
 * the guest's Vulkan driver (Venus: NVENC on NVIDIA hosts).
 *
 * What is there: the factory, a context (host-memory surfaces and buffers; DirectX/Vulkan initialization accepted,
 * nothing shared with them), trace (no output), the H.264 and HEVC encoder components (NV12, YUV420P, P010 and
 * packed RGB input; 8- and 10-bit, I and P pictures, constant QP, CBR and VBR). The encoder works synchronously:
 * SubmitInput encodes, QueryOutput hands the result over. ponytail: no B-pictures (GetCaps says so), no D3D11/D3D12
 * surfaces (applications with GPU frames get AMF_NOT_SUPPORTED and download them), no AV1 (NVIDIA's RTX 30 has no AV1
 * encoder). */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"
#include "core/Factory.h"
#include "core/Context.h"
#include "core/Trace.h"
#include "components/Component.h"
#include "components/VideoEncoderVCE.h"
#include "components/VideoEncoderHEVC.h"

/* ---------------------------------------------------------------------------------------------------------------- */
/* Log: WINBOAT_AMF_LOG=<file> */

void amf_log(const char *format, ...);

void
amf_log(const char *format, ...)
{
   static FILE *f;
   static int tried;
   if (!tried) {
      tried = 1;
      const char *path = getenv("WINBOAT_AMF_LOG");
      f = path ? fopen(path, "a") : NULL;
   }
   char line[512];
   va_list ap;
   va_start(ap, format);
   vsnprintf(line, sizeof(line), format, ap);
   va_end(ap);
   OutputDebugStringA("winboat amf: ");
   OutputDebugStringA(line);
   OutputDebugStringA("\n");
   if (f) {
      fprintf(f, "%s\n", line);
      fflush(f);
   }
}

static int
guid_eq(const AMFGuid *a, AMFGuid b)
{
   return !memcmp(a, &b, sizeof(b));
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Properties: what every AMF object keeps */

typedef struct {
   wchar_t name[96];
   AMFVariantStruct value;
} prop;

typedef struct {
   prop *items;
   size_t count, size;
} props;

static prop *
props_find(props *p, const wchar_t *name)
{
   for (size_t i = 0; i < p->count; i++)
      if (!wcscmp(p->items[i].name, name))
         return &p->items[i];
   return NULL;
}

static AMF_RESULT
props_set(props *p, const wchar_t *name, const AMFVariantStruct *value)
{
   if (!name)
      return AMF_INVALID_ARG;
   prop *e = props_find(p, name);
   if (!e) {
      if (p->count == p->size) {
         size_t size = p->size ? p->size * 2 : 16;
         prop *items = realloc(p->items, size * sizeof(*items));
         if (!items)
            return AMF_OUT_OF_MEMORY;
         p->items = items;
         p->size = size;
      }
      e = &p->items[p->count++];
      memset(e, 0, sizeof(*e));
      wcsncpy(e->name, name, 95);
      AMFVariantInit(&e->value);
   }
   return AMFVariantCopy(&e->value, value);
}

static AMF_RESULT
props_get(props *p, const wchar_t *name, AMFVariantStruct *value)
{
   prop *e = name ? props_find(p, name) : NULL;
   if (!e || !value)
      return AMF_NOT_FOUND;
   AMFVariantInit(value);
   return AMFVariantCopy(value, &e->value);
}

static void
props_clear(props *p)
{
   for (size_t i = 0; i < p->count; i++)
      AMFVariantClear(&p->items[i].value);
   p->count = 0;
}

static void
props_free(props *p)
{
   props_clear(p);
   free(p->items);
   p->items = NULL;
   p->size = 0;
}

static amf_int64
props_int(props *p, const wchar_t *name, amf_int64 def)
{
   prop *e = props_find(p, name);
   if (!e)
      return def;
   switch (e->value.type) {
   case AMF_VARIANT_INT64: return e->value.int64Value;
   case AMF_VARIANT_BOOL: return e->value.boolValue;
   case AMF_VARIANT_DOUBLE: return (amf_int64)e->value.doubleValue;
   case AMF_VARIANT_FLOAT: return (amf_int64)e->value.floatValue;
   default: return def;
   }
}

/* The property storage part of each vtable, for an object type T whose props member is named props */
#define PROPS_METHODS(T, Iface)                                                                                       \
   static AMF_RESULT AMF_STD_CALL T##_SetProperty(Iface *pThis, const wchar_t *name, AMFVariantStruct value)       \
   {                                                                                                                   \
      return props_set(&((T *)pThis)->props, name, &value);                                                            \
   }                                                                                                                   \
   static AMF_RESULT AMF_STD_CALL T##_GetProperty(Iface *pThis, const wchar_t *name, AMFVariantStruct *pValue)     \
   {                                                                                                                   \
      return props_get(&((T *)pThis)->props, name, pValue);                                                            \
   }                                                                                                                   \
   static amf_bool AMF_STD_CALL T##_HasProperty(Iface *pThis, const wchar_t *name)                                   \
   {                                                                                                                   \
      return props_find(&((T *)pThis)->props, name) != NULL;                                                           \
   }                                                                                                                   \
   static amf_size AMF_STD_CALL T##_GetPropertyCount(Iface *pThis) { return ((T *)pThis)->props.count; }             \
   static AMF_RESULT AMF_STD_CALL T##_GetPropertyAt(Iface *pThis, amf_size index, wchar_t *name, amf_size nameSize, \
                                                    AMFVariantStruct *pValue)                                         \
   {                                                                                                                   \
      props *p = &((T *)pThis)->props;                                                                                 \
      if (index >= p->count)                                                                                           \
         return AMF_INVALID_ARG;                                                                                       \
      if (name && nameSize)                                                                                            \
         wcsncpy(name, p->items[index].name, nameSize);                                                                \
      return pValue ? (AMFVariantInit(pValue), AMFVariantCopy(pValue, &p->items[index].value)) : AMF_OK;            \
   }                                                                                                                   \
   static AMF_RESULT AMF_STD_CALL T##_Clear(Iface *pThis)                                                             \
   {                                                                                                                   \
      props_clear(&((T *)pThis)->props);                                                                               \
      return AMF_OK;                                                                                                   \
   }                                                                                                                   \
   static AMF_RESULT AMF_STD_CALL T##_AddTo(Iface *pThis, AMFPropertyStorage *pDest, amf_bool overwrite,             \
                                            amf_bool deep)                                                            \
   {                                                                                                                   \
      props *p = &((T *)pThis)->props;                                                                                 \
      (void)deep;                                                                                                      \
      for (size_t i = 0; i < p->count; i++)                                                                            \
         if (overwrite || !pDest->pVtbl->HasProperty(pDest, p->items[i].name))                                        \
            pDest->pVtbl->SetProperty(pDest, p->items[i].name, p->items[i].value);                                     \
      return AMF_OK;                                                                                                   \
   }                                                                                                                   \
   static AMF_RESULT AMF_STD_CALL T##_CopyTo(Iface *pThis, AMFPropertyStorage *pDest, amf_bool deep)                 \
   {                                                                                                                   \
      pDest->pVtbl->Clear(pDest);                                                                                      \
      return T##_AddTo(pThis, pDest, 1, deep);                                                                         \
   }                                                                                                                   \
   static void AMF_STD_CALL T##_AddObserver(Iface *pThis, AMFPropertyStorageObserver *o) { (void)pThis; (void)o; }   \
   static void AMF_STD_CALL T##_RemoveObserver(Iface *pThis, AMFPropertyStorageObserver *o) { (void)pThis; (void)o; }

#define PROPS_VTBL(T)                                                                                                 \
   T##_SetProperty, T##_GetProperty, T##_HasProperty, T##_GetPropertyCount, T##_GetPropertyAt, T##_Clear, T##_AddTo, \
      T##_CopyTo, T##_AddObserver, T##_RemoveObserver

/* Reference counting, for an object type T with a refs member and a T##_free function */
#define REFS_METHODS(T, Iface)                                                                                        \
   static void T##_free(T *o);                                                                                         \
   static amf_long AMF_STD_CALL T##_Acquire(Iface *pThis) { return InterlockedIncrement(&((T *)pThis)->refs); }     \
   static amf_long AMF_STD_CALL T##_Release(Iface *pThis)                                                             \
   {                                                                                                                   \
      LONG n = InterlockedDecrement(&((T *)pThis)->refs);                                                             \
      if (!n)                                                                                                          \
         T##_free((T *)pThis);                                                                                         \
      return n;                                                                                                        \
   }

/* ---------------------------------------------------------------------------------------------------------------- */
/* Buffers (host memory) */

typedef struct {
   const AMFBufferVtbl *vtbl;
   LONG refs;
   props props;
   unsigned char *data;
   amf_size size;
   amf_pts pts, duration;
} buffer;

REFS_METHODS(buffer, AMFBuffer)
PROPS_METHODS(buffer, AMFBuffer)

static void
buffer_free(buffer *b)
{
   props_free(&b->props);
   free(b->data);
   free(b);
}

static AMF_RESULT AMF_STD_CALL
buffer_QueryInterface(AMFBuffer *pThis, const AMFGuid *id, void **out)
{
   if (guid_eq(id, IID_AMFBuffer()) || guid_eq(id, IID_AMFData()) || guid_eq(id, IID_AMFPropertyStorage()) ||
       guid_eq(id, IID_AMFInterface())) {
      buffer_Acquire(pThis);
      *out = pThis;
      return AMF_OK;
   }
   *out = NULL;
   return AMF_NO_INTERFACE;
}

static AMF_MEMORY_TYPE AMF_STD_CALL buffer_GetMemoryType(AMFBuffer *pThis) { (void)pThis; return AMF_MEMORY_HOST; }
static AMF_RESULT AMF_STD_CALL buffer_Duplicate(AMFBuffer *pThis, AMF_MEMORY_TYPE type, AMFData **out)
{
   (void)pThis; (void)type; *out = NULL;
   return AMF_NOT_IMPLEMENTED;
}
static AMF_RESULT AMF_STD_CALL buffer_Convert(AMFBuffer *pThis, AMF_MEMORY_TYPE type)
{
   (void)pThis;
   return type == AMF_MEMORY_HOST || type == AMF_MEMORY_UNKNOWN ? AMF_OK : AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL buffer_Interop(AMFBuffer *pThis, AMF_MEMORY_TYPE type) { return buffer_Convert(pThis, type); }
static AMF_DATA_TYPE AMF_STD_CALL buffer_GetDataType(AMFBuffer *pThis) { (void)pThis; return AMF_DATA_BUFFER; }
static amf_bool AMF_STD_CALL buffer_IsReusable(AMFBuffer *pThis) { (void)pThis; return 1; }
static void AMF_STD_CALL buffer_SetPts(AMFBuffer *pThis, amf_pts pts) { ((buffer *)pThis)->pts = pts; }
static amf_pts AMF_STD_CALL buffer_GetPts(AMFBuffer *pThis) { return ((buffer *)pThis)->pts; }
static void AMF_STD_CALL buffer_SetDuration(AMFBuffer *pThis, amf_pts d) { ((buffer *)pThis)->duration = d; }
static amf_pts AMF_STD_CALL buffer_GetDuration(AMFBuffer *pThis) { return ((buffer *)pThis)->duration; }
static AMF_RESULT AMF_STD_CALL buffer_SetSize(AMFBuffer *pThis, amf_size size)
{
   buffer *b = (buffer *)pThis;
   if (size > b->size) {
      unsigned char *data = realloc(b->data, size);
      if (!data)
         return AMF_OUT_OF_MEMORY;
      b->data = data;
   }
   b->size = size;
   return AMF_OK;
}
static amf_size AMF_STD_CALL buffer_GetSize(AMFBuffer *pThis) { return ((buffer *)pThis)->size; }
static void *AMF_STD_CALL buffer_GetNative(AMFBuffer *pThis) { return ((buffer *)pThis)->data; }
static void AMF_STD_CALL buffer_AddObserver_Buffer(AMFBuffer *pThis, AMFBufferObserver *o) { (void)pThis; (void)o; }
static void AMF_STD_CALL buffer_RemoveObserver_Buffer(AMFBuffer *pThis, AMFBufferObserver *o) { (void)pThis; (void)o; }

static const AMFBufferVtbl buffer_vtbl = {
   buffer_Acquire, buffer_Release, buffer_QueryInterface, PROPS_VTBL(buffer),
   buffer_GetMemoryType, buffer_Duplicate, buffer_Convert, buffer_Interop, buffer_GetDataType, buffer_IsReusable,
   buffer_SetPts, buffer_GetPts, buffer_SetDuration, buffer_GetDuration,
   buffer_SetSize, buffer_GetSize, buffer_GetNative, buffer_AddObserver_Buffer, buffer_RemoveObserver_Buffer,
};

static buffer *
buffer_new(amf_size size)
{
   buffer *b = calloc(1, sizeof(*b));
   if (!b)
      return NULL;
   b->vtbl = &buffer_vtbl;
   b->refs = 1;
   b->data = malloc(size ? size : 1);
   if (!b->data) {
      free(b);
      return NULL;
   }
   b->size = size;
   return b;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Surfaces and planes (host memory) */

typedef struct surface surface;

typedef struct {
   const AMFPlaneVtbl *vtbl;
   surface *owner;
   AMF_PLANE_TYPE type;
   unsigned char *data;
   amf_int32 width, height, pitch, pixelSize;
} plane;

struct surface {
   const AMFSurfaceVtbl *vtbl;
   LONG refs;
   props props;
   AMF_SURFACE_FORMAT format;
   amf_int32 width, height;
   amf_int32 cropX, cropY, cropW, cropH;
   unsigned char *data;
   int ownsData;
   plane planes[3];
   amf_size planeCount;
   amf_pts pts, duration;
   AMF_FRAME_TYPE frameType;
};

REFS_METHODS(surface, AMFSurface)
PROPS_METHODS(surface, AMFSurface)

static void
surface_free(surface *s)
{
   props_free(&s->props);
   if (s->ownsData)
      free(s->data);
   free(s);
}

/* the planes as an encoder input wants them: NV12, YUV420P, P010 and packed RGB; the pitch aligned to 256 bytes */
static int
surface_layout(surface *s, amf_int32 pitch)
{
   amf_int32 w = s->width, h = s->height;
   int bytes = s->format == AMF_SURFACE_P010 ? 2 : 1;
   if (!pitch)
      pitch = (w * bytes + 255) & ~255;
   switch (s->format) {
   case AMF_SURFACE_NV12:
   case AMF_SURFACE_P010:
      s->planeCount = 2;
      s->planes[0] = (plane){NULL, s, AMF_PLANE_Y, NULL, w, h, pitch, bytes};
      s->planes[1] = (plane){NULL, s, AMF_PLANE_UV, NULL, (w + 1) / 2, (h + 1) / 2, pitch, 2 * bytes};
      s->planes[1].data = (unsigned char *)(size_t)(pitch * h);
      return pitch * h + pitch * ((h + 1) / 2);
   case AMF_SURFACE_YUV420P:
      s->planeCount = 3;
      s->planes[0] = (plane){NULL, s, AMF_PLANE_Y, NULL, w, h, pitch, 1};
      s->planes[1] = (plane){NULL, s, AMF_PLANE_U, NULL, (w + 1) / 2, (h + 1) / 2, pitch / 2, 1};
      s->planes[2] = (plane){NULL, s, AMF_PLANE_V, NULL, (w + 1) / 2, (h + 1) / 2, pitch / 2, 1};
      s->planes[1].data = (unsigned char *)(size_t)(pitch * h);
      s->planes[2].data = (unsigned char *)(size_t)(pitch * h + (pitch / 2) * ((h + 1) / 2));
      return pitch * h + 2 * (pitch / 2) * ((h + 1) / 2);
   case AMF_SURFACE_BGRA:
   case AMF_SURFACE_ARGB:
   case AMF_SURFACE_RGBA:
   case AMF_SURFACE_R10G10B10A2:
      if (pitch == ((w * bytes + 255) & ~255))
         pitch = (w * 4 + 255) & ~255;
      s->planeCount = 1;
      s->planes[0] = (plane){NULL, s, AMF_PLANE_PACKED, NULL, w, h, pitch, 4};
      return pitch * h;
   default:
      return 0;
   }
}

static AMF_RESULT AMF_STD_CALL
surface_QueryInterface(AMFSurface *pThis, const AMFGuid *id, void **out)
{
   if (guid_eq(id, IID_AMFSurface()) || guid_eq(id, IID_AMFData()) || guid_eq(id, IID_AMFPropertyStorage()) ||
       guid_eq(id, IID_AMFInterface())) {
      surface_Acquire(pThis);
      *out = pThis;
      return AMF_OK;
   }
   *out = NULL;
   return AMF_NO_INTERFACE;
}

static AMF_MEMORY_TYPE AMF_STD_CALL surface_GetMemoryType(AMFSurface *pThis) { (void)pThis; return AMF_MEMORY_HOST; }
static AMF_RESULT AMF_STD_CALL surface_Duplicate(AMFSurface *pThis, AMF_MEMORY_TYPE type, AMFData **out)
{
   (void)pThis; (void)type; *out = NULL;
   return AMF_NOT_IMPLEMENTED;
}
static AMF_RESULT AMF_STD_CALL surface_Convert(AMFSurface *pThis, AMF_MEMORY_TYPE type)
{
   (void)pThis;
   return type == AMF_MEMORY_HOST || type == AMF_MEMORY_UNKNOWN ? AMF_OK : AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL surface_Interop(AMFSurface *pThis, AMF_MEMORY_TYPE t) { return surface_Convert(pThis, t); }
static AMF_DATA_TYPE AMF_STD_CALL surface_GetDataType(AMFSurface *pThis) { (void)pThis; return AMF_DATA_SURFACE; }
static amf_bool AMF_STD_CALL surface_IsReusable(AMFSurface *pThis) { (void)pThis; return 1; }
static void AMF_STD_CALL surface_SetPts(AMFSurface *pThis, amf_pts pts) { ((surface *)pThis)->pts = pts; }
static amf_pts AMF_STD_CALL surface_GetPts(AMFSurface *pThis) { return ((surface *)pThis)->pts; }
static void AMF_STD_CALL surface_SetDuration(AMFSurface *pThis, amf_pts d) { ((surface *)pThis)->duration = d; }
static amf_pts AMF_STD_CALL surface_GetDuration(AMFSurface *pThis) { return ((surface *)pThis)->duration; }
static AMF_SURFACE_FORMAT AMF_STD_CALL surface_GetFormat(AMFSurface *pThis) { return ((surface *)pThis)->format; }
static amf_size AMF_STD_CALL surface_GetPlanesCount(AMFSurface *pThis) { return ((surface *)pThis)->planeCount; }
static AMFPlane *AMF_STD_CALL surface_GetPlaneAt(AMFSurface *pThis, amf_size index)
{
   surface *s = (surface *)pThis;
   return index < s->planeCount ? (AMFPlane *)&s->planes[index] : NULL;
}
static AMFPlane *AMF_STD_CALL surface_GetPlane(AMFSurface *pThis, AMF_PLANE_TYPE type)
{
   surface *s = (surface *)pThis;
   for (amf_size i = 0; i < s->planeCount; i++)
      if (s->planes[i].type == type || (type == AMF_PLANE_PACKED && i == 0))
         return (AMFPlane *)&s->planes[i];
   return NULL;
}
static AMF_FRAME_TYPE AMF_STD_CALL surface_GetFrameType(AMFSurface *pThis) { return ((surface *)pThis)->frameType; }
static void AMF_STD_CALL surface_SetFrameType(AMFSurface *pThis, AMF_FRAME_TYPE t) { ((surface *)pThis)->frameType = t; }
static AMF_RESULT AMF_STD_CALL surface_SetCrop(AMFSurface *pThis, amf_int32 x, amf_int32 y, amf_int32 w, amf_int32 h)
{
   surface *s = (surface *)pThis;
   if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > s->width || y + h > s->height)
      return AMF_INVALID_ARG;
   s->cropX = x;
   s->cropY = y;
   s->cropW = w;
   s->cropH = h;
   return AMF_OK;
}
static AMF_RESULT AMF_STD_CALL surface_CopySurfaceRegion(AMFSurface *pThis, AMFSurface *pDest, amf_int32 dstX,
                                                         amf_int32 dstY, amf_int32 srcX, amf_int32 srcY,
                                                         amf_int32 width, amf_int32 height)
{
   (void)pThis; (void)pDest; (void)dstX; (void)dstY; (void)srcX; (void)srcY; (void)width; (void)height;
   return AMF_NOT_IMPLEMENTED;
}
static void AMF_STD_CALL surface_AddObserver_Surface(AMFSurface *pThis, AMFSurfaceObserver *o) { (void)pThis; (void)o; }
static void AMF_STD_CALL surface_RemoveObserver_Surface(AMFSurface *pThis, AMFSurfaceObserver *o) { (void)pThis; (void)o; }

static const AMFSurfaceVtbl surface_vtbl = {
   surface_Acquire, surface_Release, surface_QueryInterface, PROPS_VTBL(surface),
   surface_GetMemoryType, surface_Duplicate, surface_Convert, surface_Interop, surface_GetDataType,
   surface_IsReusable, surface_SetPts, surface_GetPts, surface_SetDuration, surface_GetDuration,
   surface_GetFormat, surface_GetPlanesCount, surface_GetPlaneAt, surface_GetPlane, surface_GetFrameType,
   surface_SetFrameType, surface_SetCrop, surface_CopySurfaceRegion, surface_AddObserver_Surface,
   surface_RemoveObserver_Surface,
};

/* Planes live in their surface and share its reference count */
static amf_long AMF_STD_CALL plane_Acquire(AMFPlane *pThis) { return surface_Acquire((AMFSurface *)((plane *)pThis)->owner); }
static amf_long AMF_STD_CALL plane_Release(AMFPlane *pThis) { return surface_Release((AMFSurface *)((plane *)pThis)->owner); }
static AMF_RESULT AMF_STD_CALL plane_QueryInterface(AMFPlane *pThis, const AMFGuid *id, void **out)
{
   if (guid_eq(id, IID_AMFPlane()) || guid_eq(id, IID_AMFInterface())) {
      plane_Acquire(pThis);
      *out = pThis;
      return AMF_OK;
   }
   *out = NULL;
   return AMF_NO_INTERFACE;
}
static AMF_PLANE_TYPE AMF_STD_CALL plane_GetType(AMFPlane *pThis) { return ((plane *)pThis)->type; }
static void *AMF_STD_CALL plane_GetNative(AMFPlane *pThis) { return ((plane *)pThis)->data; }
static amf_int32 AMF_STD_CALL plane_GetPixelSizeInBytes(AMFPlane *pThis) { return ((plane *)pThis)->pixelSize; }
static amf_int32 AMF_STD_CALL plane_GetOffsetX(AMFPlane *pThis)
{
   plane *p = (plane *)pThis;
   return p->type == AMF_PLANE_Y ? p->owner->cropX : p->owner->cropX / 2;
}
static amf_int32 AMF_STD_CALL plane_GetOffsetY(AMFPlane *pThis)
{
   plane *p = (plane *)pThis;
   return p->type == AMF_PLANE_Y ? p->owner->cropY : p->owner->cropY / 2;
}
static amf_int32 AMF_STD_CALL plane_GetWidth(AMFPlane *pThis)
{
   plane *p = (plane *)pThis;
   return p->type == AMF_PLANE_Y ? p->owner->cropW : (p->owner->cropW + 1) / 2;
}
static amf_int32 AMF_STD_CALL plane_GetHeight(AMFPlane *pThis)
{
   plane *p = (plane *)pThis;
   return p->type == AMF_PLANE_Y ? p->owner->cropH : (p->owner->cropH + 1) / 2;
}
static amf_int32 AMF_STD_CALL plane_GetHPitch(AMFPlane *pThis) { return ((plane *)pThis)->pitch; }
static amf_int32 AMF_STD_CALL plane_GetVPitch(AMFPlane *pThis) { return ((plane *)pThis)->height; }
static amf_bool AMF_STD_CALL plane_IsTiled(AMFPlane *pThis) { (void)pThis; return 0; }

static const AMFPlaneVtbl plane_vtbl = {
   plane_Acquire, plane_Release, plane_QueryInterface, plane_GetType, plane_GetNative, plane_GetPixelSizeInBytes,
   plane_GetOffsetX, plane_GetOffsetY, plane_GetWidth, plane_GetHeight, plane_GetHPitch, plane_GetVPitch,
   plane_IsTiled,
};

static surface *
surface_new(AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, amf_int32 pitch, void *data)
{
   if (width <= 0 || height <= 0)
      return NULL;
   surface *s = calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   s->vtbl = &surface_vtbl;
   s->refs = 1;
   s->format = format;
   s->width = s->cropW = width;
   s->height = s->cropH = height;
   s->frameType = AMF_FRAME_PROGRESSIVE;
   int size = surface_layout(s, pitch);
   if (!size) {
      free(s);
      return NULL;
   }
   s->data = data ? data : malloc(size);
   s->ownsData = !data;
   if (!s->data) {
      free(s);
      return NULL;
   }
   for (amf_size i = 0; i < s->planeCount; i++) {
      s->planes[i].vtbl = &plane_vtbl;
      s->planes[i].data = s->data + (size_t)s->planes[i].data;
   }
   return s;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Trace: accepted, nothing written */

static AMFTrace the_trace;
static void AMF_CDECL_CALL trace_TraceW(AMFTrace *t, const wchar_t *p, amf_int32 l, amf_int32 lv, const wchar_t *s,
                                        amf_int32 n, const wchar_t *f, ...)
{
   (void)t; (void)p; (void)l; (void)lv; (void)s; (void)n; (void)f;
}
static void AMF_STD_CALL trace_Trace(AMFTrace *t, const wchar_t *p, amf_int32 l, amf_int32 lv, const wchar_t *s,
                                     const wchar_t *m, va_list *a)
{
   (void)t; (void)p; (void)l; (void)lv; (void)s; (void)m; (void)a;
}
static amf_int32 trace_level = AMF_TRACE_WARNING;
static amf_int32 AMF_STD_CALL trace_SetGlobalLevel(AMFTrace *t, amf_int32 level)
{
   (void)t;
   amf_int32 old = trace_level;
   trace_level = level;
   return old;
}
static amf_int32 AMF_STD_CALL trace_GetGlobalLevel(AMFTrace *t) { (void)t; return trace_level; }
static amf_bool AMF_STD_CALL trace_EnableWriter(AMFTrace *t, const wchar_t *id, amf_bool e) { (void)t; (void)id; (void)e; return 0; }
static amf_bool AMF_STD_CALL trace_WriterEnabled(AMFTrace *t, const wchar_t *id) { (void)t; (void)id; return 0; }
static AMF_RESULT AMF_STD_CALL trace_TraceEnableAsync(AMFTrace *t, amf_bool e) { (void)t; (void)e; return AMF_OK; }
static AMF_RESULT AMF_STD_CALL trace_TraceFlush(AMFTrace *t) { (void)t; return AMF_OK; }
static AMF_RESULT AMF_STD_CALL trace_SetPath(AMFTrace *t, const wchar_t *p) { (void)t; (void)p; return AMF_OK; }
static AMF_RESULT AMF_STD_CALL trace_GetPath(AMFTrace *t, wchar_t *p, amf_size *s)
{
   (void)t;
   if (p && s && *s)
      p[0] = 0;
   return AMF_OK;
}
static amf_int32 AMF_STD_CALL trace_SetWriterLevel(AMFTrace *t, const wchar_t *id, amf_int32 l) { (void)t; (void)id; (void)l; return trace_level; }
static amf_int32 AMF_STD_CALL trace_GetWriterLevel(AMFTrace *t, const wchar_t *id) { (void)t; (void)id; return trace_level; }
static amf_int32 AMF_STD_CALL trace_SetWriterLevelForScope(AMFTrace *t, const wchar_t *id, const wchar_t *s, amf_int32 l)
{
   (void)t; (void)id; (void)s; (void)l;
   return trace_level;
}
static amf_int32 AMF_STD_CALL trace_GetWriterLevelForScope(AMFTrace *t, const wchar_t *id, const wchar_t *s)
{
   (void)t; (void)id; (void)s;
   return trace_level;
}
static amf_int32 AMF_STD_CALL trace_GetIndentation(AMFTrace *t) { (void)t; return 0; }
static void AMF_STD_CALL trace_Indent(AMFTrace *t, amf_int32 n) { (void)t; (void)n; }
static void AMF_STD_CALL trace_RegisterWriter(AMFTrace *t, const wchar_t *id, AMFTraceWriter *w, amf_bool e)
{
   (void)t; (void)id; (void)w; (void)e;
}
static void AMF_STD_CALL trace_UnregisterWriter(AMFTrace *t, const wchar_t *id) { (void)t; (void)id; }
static const wchar_t *AMF_STD_CALL trace_GetResultText(AMFTrace *t, AMF_RESULT r) { (void)t; (void)r; return L"AMF_RESULT"; }
static const wchar_t *AMF_STD_CALL trace_SurfaceGetFormatName(AMFTrace *t, const AMF_SURFACE_FORMAT f)
{
   (void)t;
   return f == AMF_SURFACE_NV12 ? L"NV12" : f == AMF_SURFACE_P010 ? L"P010" : f == AMF_SURFACE_YUV420P ? L"YUV420P"
          : f == AMF_SURFACE_BGRA ? L"BGRA" : f == AMF_SURFACE_ARGB ? L"ARGB" : f == AMF_SURFACE_RGBA ? L"RGBA"
          : f == AMF_SURFACE_R10G10B10A2 ? L"R10G10B10A2" : L"?";
}
static AMF_SURFACE_FORMAT AMF_STD_CALL trace_SurfaceGetFormatByName(AMFTrace *t, const wchar_t *n) { (void)t; (void)n; return AMF_SURFACE_UNKNOWN; }
static const wchar_t *AMF_STD_CALL trace_GetMemoryTypeName(AMFTrace *t, const AMF_MEMORY_TYPE m) { (void)t; (void)m; return L"Host"; }
static AMF_MEMORY_TYPE AMF_STD_CALL trace_GetMemoryTypeByName(AMFTrace *t, const wchar_t *n) { (void)t; (void)n; return AMF_MEMORY_HOST; }
static const wchar_t *AMF_STD_CALL trace_GetSampleFormatName(AMFTrace *t, const AMF_AUDIO_FORMAT f) { (void)t; (void)f; return L"?"; }
static AMF_AUDIO_FORMAT AMF_STD_CALL trace_GetSampleFormatByName(AMFTrace *t, const wchar_t *n) { (void)t; (void)n; return AMFAF_UNKNOWN; }

static const AMFTraceVtbl trace_vtbl = {
   trace_TraceW, trace_Trace, trace_SetGlobalLevel, trace_GetGlobalLevel, trace_EnableWriter, trace_WriterEnabled,
   trace_TraceEnableAsync, trace_TraceFlush, trace_SetPath, trace_GetPath, trace_SetWriterLevel,
   trace_GetWriterLevel, trace_SetWriterLevelForScope, trace_GetWriterLevelForScope, trace_GetIndentation,
   trace_Indent, trace_RegisterWriter, trace_UnregisterWriter, trace_GetResultText, trace_SurfaceGetFormatName,
   trace_SurfaceGetFormatByName, trace_GetMemoryTypeName, trace_GetMemoryTypeByName, trace_GetSampleFormatName,
   trace_GetSampleFormatByName,
};

/* ---------------------------------------------------------------------------------------------------------------- */
/* The context (Context, Context1 and Context2 share the layout: one object answers to all three) */

typedef struct {
   const AMFContext2Vtbl *vtbl;
   LONG refs;
   props props;
   void *dx11, *dx12, *vulkan;
} context;

REFS_METHODS(context, AMFContext2)
PROPS_METHODS(context, AMFContext2)

static void
context_free(context *c)
{
   props_free(&c->props);
   free(c);
}

static AMF_RESULT AMF_STD_CALL
context_QueryInterface(AMFContext2 *pThis, const AMFGuid *id, void **out)
{
   if (guid_eq(id, IID_AMFContext()) || guid_eq(id, IID_AMFContext1()) || guid_eq(id, IID_AMFContext2()) ||
       guid_eq(id, IID_AMFPropertyStorage()) || guid_eq(id, IID_AMFInterface())) {
      context_Acquire(pThis);
      *out = pThis;
      return AMF_OK;
   }
   *out = NULL;
   return AMF_NO_INTERFACE;
}

static AMF_RESULT AMF_STD_CALL ctx_ok(AMFContext2 *pThis) { (void)pThis; return AMF_OK; }
static AMF_RESULT AMF_STD_CALL ctx_no(AMFContext2 *pThis) { (void)pThis; return AMF_NOT_SUPPORTED; }
static void *AMF_STD_CALL ctx_null(AMFContext2 *pThis) { (void)pThis; return NULL; }
/* DirectX and Vulkan: accepted (applications initialize one before anything else); their surfaces aren't */
static AMF_RESULT AMF_STD_CALL ctx_InitDX9(AMFContext2 *pThis, void *d) { (void)pThis; (void)d; return AMF_OK; }
static void *AMF_STD_CALL ctx_GetDX9Device(AMFContext2 *pThis, AMF_DX_VERSION v) { (void)pThis; (void)v; return NULL; }
static AMF_RESULT AMF_STD_CALL ctx_InitDX11(AMFContext2 *pThis, void *d, AMF_DX_VERSION v) { (void)v; ((context *)pThis)->dx11 = d; return AMF_OK; }
static void *AMF_STD_CALL ctx_GetDX11Device(AMFContext2 *pThis, AMF_DX_VERSION v) { (void)v; return ((context *)pThis)->dx11; }
static AMF_RESULT AMF_STD_CALL ctx_InitOpenCL(AMFContext2 *pThis, void *q) { (void)pThis; (void)q; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL ctx_GetOpenCLComputeFactory(AMFContext2 *pThis, AMFComputeFactory **f) { (void)pThis; *f = NULL; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL ctx_InitOpenCLEx(AMFContext2 *pThis, AMFComputeDevice *d) { (void)pThis; (void)d; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL ctx_InitOpenGL(AMFContext2 *pThis, amf_handle a, amf_handle b, amf_handle c) { (void)pThis; (void)a; (void)b; (void)c; return AMF_NOT_SUPPORTED; }
static amf_handle AMF_STD_CALL ctx_hnull(AMFContext2 *pThis) { (void)pThis; return NULL; }
static AMF_RESULT AMF_STD_CALL ctx_InitXV(AMFContext2 *pThis, void *d) { (void)pThis; (void)d; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL ctx_InitGralloc(AMFContext2 *pThis, void *d) { (void)pThis; (void)d; return AMF_NOT_SUPPORTED; }

static AMF_RESULT AMF_STD_CALL
ctx_AllocBuffer(AMFContext2 *pThis, AMF_MEMORY_TYPE type, amf_size size, AMFBuffer **out)
{
   (void)pThis;
   if (type != AMF_MEMORY_HOST && type != AMF_MEMORY_UNKNOWN) {
      *out = NULL;
      return AMF_NOT_SUPPORTED;
   }
   *out = (AMFBuffer *)buffer_new(size);
   return *out ? AMF_OK : AMF_OUT_OF_MEMORY;
}

static AMF_RESULT AMF_STD_CALL
ctx_AllocSurface(AMFContext2 *pThis, AMF_MEMORY_TYPE type, AMF_SURFACE_FORMAT format, amf_int32 w, amf_int32 h,
                 AMFSurface **out)
{
   (void)pThis;
   *out = NULL;
   if (type != AMF_MEMORY_HOST && type != AMF_MEMORY_UNKNOWN) {
      amf_log("AllocSurface: memory type %d not supported (host memory only)", (int)type);
      return AMF_NOT_SUPPORTED;
   }
   *out = (AMFSurface *)surface_new(format, w, h, 0, NULL);
   return *out ? AMF_OK : AMF_NOT_SUPPORTED;
}

static AMF_RESULT AMF_STD_CALL
ctx_AllocAudioBuffer(AMFContext2 *pThis, AMF_MEMORY_TYPE t, AMF_AUDIO_FORMAT f, amf_int32 s, amf_int32 r, amf_int32 c,
                     AMFAudioBuffer **out)
{
   (void)pThis; (void)t; (void)f; (void)s; (void)r; (void)c;
   *out = NULL;
   return AMF_NOT_SUPPORTED;
}

static AMF_RESULT AMF_STD_CALL
ctx_CreateBufferFromHostNative(AMFContext2 *pThis, void *data, amf_size size, AMFBuffer **out, AMFBufferObserver *o)
{
   (void)pThis; (void)o;
   buffer *b = buffer_new(size);
   if (b)
      memcpy(b->data, data, size);
   *out = (AMFBuffer *)b;
   return b ? AMF_OK : AMF_OUT_OF_MEMORY;
}

static AMF_RESULT AMF_STD_CALL
ctx_CreateSurfaceFromHostNative(AMFContext2 *pThis, AMF_SURFACE_FORMAT format, amf_int32 w, amf_int32 h,
                                amf_int32 hPitch, amf_int32 vPitch, void *data, AMFSurface **out, AMFSurfaceObserver *o)
{
   (void)pThis; (void)vPitch; (void)o;
   *out = (AMFSurface *)surface_new(format, w, h, hPitch, data);
   return *out ? AMF_OK : AMF_NOT_SUPPORTED;
}

static AMF_RESULT AMF_STD_CALL
ctx_CreateSurfaceFromNative(AMFContext2 *pThis, void *native, AMFSurface **out, AMFSurfaceObserver *o)
{
   (void)pThis; (void)native; (void)o;
   amf_log("a DirectX/Vulkan surface: not supported (host memory only)");
   *out = NULL;
   return AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL
ctx_CreateSurfaceFromOpenGLNative(AMFContext2 *pThis, AMF_SURFACE_FORMAT f, amf_handle h, AMFSurface **out, AMFSurfaceObserver *o)
{
   (void)pThis; (void)f; (void)h; (void)o; *out = NULL;
   return AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL
ctx_CreateSurfaceFromGrallocNative(AMFContext2 *pThis, amf_handle h, AMFSurface **out, AMFSurfaceObserver *o)
{
   (void)pThis; (void)h; (void)o; *out = NULL;
   return AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL
ctx_CreateSurfaceFromOpenCLNative(AMFContext2 *pThis, AMF_SURFACE_FORMAT f, amf_int32 w, amf_int32 h, void **p,
                                  AMFSurface **out, AMFSurfaceObserver *o)
{
   (void)pThis; (void)f; (void)w; (void)h; (void)p; (void)o; *out = NULL;
   return AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL
ctx_CreateBufferFromOpenCLNative(AMFContext2 *pThis, void *b, amf_size s, AMFBuffer **out)
{
   (void)pThis; (void)b; (void)s; *out = NULL;
   return AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL ctx_GetCompute(AMFContext2 *pThis, AMF_MEMORY_TYPE t, AMFCompute **out)
{
   (void)pThis; (void)t; *out = NULL;
   return AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL ctx_CreateBufferFromNative(AMFContext2 *pThis, void *b, AMFBuffer **out, AMFBufferObserver *o)
{
   (void)pThis; (void)b; (void)o; *out = NULL;
   return AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL
ctx_AllocBufferEx(AMFContext2 *pThis, AMF_MEMORY_TYPE type, amf_size size, AMF_BUFFER_USAGE u, AMF_MEMORY_CPU_ACCESS a,
                  AMFBuffer **out)
{
   (void)u; (void)a;
   return ctx_AllocBuffer(pThis, type, size, out);
}
static AMF_RESULT AMF_STD_CALL
ctx_AllocSurfaceEx(AMFContext2 *pThis, AMF_MEMORY_TYPE type, AMF_SURFACE_FORMAT format, amf_int32 w, amf_int32 h,
                   AMF_SURFACE_USAGE u, AMF_MEMORY_CPU_ACCESS a, AMFSurface **out)
{
   (void)u; (void)a;
   return ctx_AllocSurface(pThis, type, format, w, h, out);
}
static AMF_RESULT AMF_STD_CALL ctx_InitVulkan(AMFContext2 *pThis, void *d) { ((context *)pThis)->vulkan = d; return AMF_OK; }
static void *AMF_STD_CALL ctx_GetVulkanDevice(AMFContext2 *pThis) { return ((context *)pThis)->vulkan; }
static AMF_RESULT AMF_STD_CALL ctx_GetVulkanDeviceExtensions(AMFContext2 *pThis, amf_size *count, const char **exts)
{
   (void)pThis; (void)exts;
   if (count)
      *count = 0;
   return AMF_OK;
}
static AMF_RESULT AMF_STD_CALL ctx_InitDX12(AMFContext2 *pThis, void *d, AMF_DX_VERSION v) { (void)v; ((context *)pThis)->dx12 = d; return AMF_OK; }
static void *AMF_STD_CALL ctx_GetDX12Device(AMFContext2 *pThis, AMF_DX_VERSION v) { (void)v; return ((context *)pThis)->dx12; }

static const AMFContext2Vtbl context_vtbl = {
   context_Acquire, context_Release, context_QueryInterface, PROPS_VTBL(context),
   ctx_ok /* Terminate */, ctx_InitDX9, ctx_GetDX9Device, ctx_ok, ctx_ok, ctx_InitDX11, ctx_GetDX11Device, ctx_ok,
   ctx_ok, ctx_InitOpenCL, ctx_null, ctx_null, ctx_null, ctx_GetOpenCLComputeFactory, ctx_InitOpenCLEx, ctx_no,
   ctx_no, ctx_InitOpenGL, ctx_hnull, ctx_hnull, ctx_no, ctx_no, ctx_InitXV, ctx_null, ctx_no, ctx_no,
   ctx_InitGralloc, ctx_null, ctx_no, ctx_no, ctx_AllocBuffer, ctx_AllocSurface, ctx_AllocAudioBuffer,
   ctx_CreateBufferFromHostNative, ctx_CreateSurfaceFromHostNative, ctx_CreateSurfaceFromNative,
   ctx_CreateSurfaceFromNative, ctx_CreateSurfaceFromOpenGLNative, ctx_CreateSurfaceFromGrallocNative,
   ctx_CreateSurfaceFromOpenCLNative, ctx_CreateBufferFromOpenCLNative, ctx_GetCompute,
   /* Context1 */
   ctx_CreateBufferFromNative, ctx_AllocBufferEx, ctx_AllocSurfaceEx, ctx_InitVulkan, ctx_GetVulkanDevice, ctx_ok,
   ctx_ok, ctx_CreateSurfaceFromNative, ctx_CreateBufferFromNative, ctx_GetVulkanDeviceExtensions,
   /* Context2 */
   ctx_InitDX12, ctx_GetDX12Device, ctx_ok, ctx_ok, ctx_CreateSurfaceFromNative, ctx_CreateBufferFromNative,
};

/* ---------------------------------------------------------------------------------------------------------------- */
/* The encoder (vk_encoder.c: Vulkan Video) */

#include "vk_encoder.h"

typedef struct {
   const AMFComponentVtbl *vtbl;
   LONG refs;
   props props;
   context *ctx;
   int hevc;
   vk_encoder *enc;
   AMF_SURFACE_FORMAT format;
   /* encoded pictures waiting for QueryOutput; draining: EOF once they're out */
   buffer **out;
   size_t outCount, outSize;
   int draining;
   amf_int64 frames;
   /* RGB input: the 4:2:0 picture converted from it (rgb_convert) */
   unsigned char *conv;
   size_t convSize;
   int coef[3][3], yOff, cOff, convTenBit;
} encoder;

REFS_METHODS(encoder, AMFComponent)
PROPS_METHODS(encoder, AMFComponent)

static void
encoder_drop_output(encoder *e)
{
   for (size_t i = 0; i < e->outCount; i++)
      buffer_Release((AMFBuffer *)e->out[i]);
   e->outCount = 0;
}

static void
encoder_free(encoder *e)
{
   encoder_drop_output(e);
   free(e->out);
   free(e->conv);
   if (e->enc)
      vk_encoder_destroy(e->enc);
   if (e->ctx)
      context_Release((AMFContext2 *)e->ctx);
   props_free(&e->props);
   free(e);
}

static AMF_RESULT AMF_STD_CALL
encoder_QueryInterface(AMFComponent *pThis, const AMFGuid *id, void **out)
{
   if (guid_eq(id, IID_AMFComponent()) || guid_eq(id, IID_AMFPropertyStorageEx()) ||
       guid_eq(id, IID_AMFPropertyStorage()) || guid_eq(id, IID_AMFInterface())) {
      encoder_Acquire(pThis);
      *out = pThis;
      return AMF_OK;
   }
   *out = NULL;
   return AMF_NO_INTERFACE;
}

static amf_size AMF_STD_CALL encoder_GetPropertiesInfoCount(AMFComponent *pThis) { (void)pThis; return 0; }
static AMF_RESULT AMF_STD_CALL encoder_GetPropertyInfoAt(AMFComponent *pThis, amf_size i, const AMFPropertyInfo **info)
{
   (void)pThis; (void)i; *info = NULL;
   return AMF_NOT_FOUND;
}
static AMF_RESULT AMF_STD_CALL encoder_GetPropertyInfo(AMFComponent *pThis, const wchar_t *n, const AMFPropertyInfo **info)
{
   (void)pThis; (void)n; *info = NULL;
   return AMF_NOT_FOUND;
}
static AMF_RESULT AMF_STD_CALL encoder_ValidateProperty(AMFComponent *pThis, const wchar_t *n, AMFVariantStruct v,
                                                        AMFVariantStruct *out)
{
   (void)pThis; (void)n;
   if (out) {
      AMFVariantInit(out);
      AMFVariantCopy(out, &v);
   }
   return AMF_OK;
}

/* The settings, from the properties (H.264's and HEVC's names) */
static void
encoder_config(encoder *e, vk_encoder_config *c)
{
   props *p = &e->props;
   memset(c, 0, sizeof(*c));
   c->hevc = e->hevc;
   c->tenBit = e->format == AMF_SURFACE_P010;
   AMFVariantStruct v;
   prop *rate = props_find(p, e->hevc ? AMF_VIDEO_ENCODER_HEVC_FRAMERATE : AMF_VIDEO_ENCODER_FRAMERATE);
   c->fpsNum = rate && rate->value.type == AMF_VARIANT_RATE && rate->value.rateValue.num ? rate->value.rateValue.num : 30;
   c->fpsDen = rate && rate->value.type == AMF_VARIANT_RATE && rate->value.rateValue.den ? rate->value.rateValue.den : 1;
   (void)v;
   if (e->hevc) {
      c->profile = (int)props_int(p, AMF_VIDEO_ENCODER_HEVC_PROFILE, AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN);
      c->tenBit |= c->profile == AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10 ||
                   props_int(p, AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_8) == AMF_COLOR_BIT_DEPTH_10;
      c->highTier = props_int(p, AMF_VIDEO_ENCODER_HEVC_TIER, AMF_VIDEO_ENCODER_HEVC_TIER_MAIN) == AMF_VIDEO_ENCODER_HEVC_TIER_HIGH;
      c->level = (int)props_int(p, AMF_VIDEO_ENCODER_HEVC_PROFILE_LEVEL, 0);
      amf_int64 gop = props_int(p, AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, 60);
      amf_int64 gops = props_int(p, AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, 1);
      c->idrPeriod = (int)(gop * (gops > 0 ? gops : 1));
      amf_int64 rc = props_int(p, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR);
      c->rc = rc == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP ? VK_ENCODER_RC_CQP
              : rc == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR      ? VK_ENCODER_RC_CBR
                                                                          : VK_ENCODER_RC_VBR;
      c->qpI = (int)props_int(p, AMF_VIDEO_ENCODER_HEVC_QP_I, 26);
      c->qpP = (int)props_int(p, AMF_VIDEO_ENCODER_HEVC_QP_P, 26);
      c->bitrate = props_int(p, AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, 20000000);
      c->peak = props_int(p, AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, c->bitrate * 3 / 2);
      c->vbv = props_int(p, AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, 0);
   } else {
      c->profile = (int)props_int(p, AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_MAIN);
      c->level = (int)props_int(p, AMF_VIDEO_ENCODER_PROFILE_LEVEL, 0);
      c->idrPeriod = (int)props_int(p, AMF_VIDEO_ENCODER_IDR_PERIOD, 30);
      c->cabac = props_int(p, AMF_VIDEO_ENCODER_CABAC_ENABLE, AMF_VIDEO_ENCODER_UNDEFINED) != AMF_VIDEO_ENCODER_CALV &&
                 c->profile != AMF_VIDEO_ENCODER_PROFILE_BASELINE &&
                 c->profile != AMF_VIDEO_ENCODER_PROFILE_CONSTRAINED_BASELINE;
      amf_int64 rc = props_int(p, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR);
      c->rc = rc == AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP ? VK_ENCODER_RC_CQP
              : rc == AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR      ? VK_ENCODER_RC_CBR
                                                                     : VK_ENCODER_RC_VBR;
      c->qpI = (int)props_int(p, AMF_VIDEO_ENCODER_QP_I, 22);
      c->qpP = (int)props_int(p, AMF_VIDEO_ENCODER_QP_P, 22);
      c->bitrate = props_int(p, AMF_VIDEO_ENCODER_TARGET_BITRATE, 20000000);
      c->peak = props_int(p, AMF_VIDEO_ENCODER_PEAK_BITRATE, c->bitrate * 3 / 2);
      c->vbv = props_int(p, AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, 0);
   }
   if (c->idrPeriod <= 0)
      c->idrPeriod = 1 << 30;
}

/* RGB input, converted to the encoder's 4:2:0 on the CPU (AMD's encoder converts in its own hardware). The matrix:
 * the output color profile (ffmpeg's from the colorspace and range), unknown: BT.601 below 720 lines, else BT.709 (AMF's
 * "auto by size"); studio range unless the profile says full (the range properties describe YUV input: ffmpeg sets
 * them full for any RGB input). ponytail: no RGBA_F16 (scRGB/HDR
 * input needs a transfer function too), a CPU pass per picture (~ms at 1080p), a shader if 4K60 RGB ever matters */
static int
is_rgb(AMF_SURFACE_FORMAT f)
{
   return f == AMF_SURFACE_BGRA || f == AMF_SURFACE_ARGB || f == AMF_SURFACE_RGBA || f == AMF_SURFACE_R10G10B10A2;
}

static void
rgb_matrix(encoder *e, int height, int tenBit, int tenBitIn)
{
   props *p = &e->props;
   amf_int64 profile = props_int(p, e->hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE : AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE,
                                 AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN);
   if (profile == AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN)
      profile = props_int(p, e->hevc ? AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PROFILE : AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE,
                          AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN);
   int full = profile == AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601 || profile == AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709 ||
              profile == AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020;
   double kr = 0.2126, kb = 0.0722;
   if (profile == AMF_VIDEO_CONVERTER_COLOR_PROFILE_601 || profile == AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601 ||
       (profile == AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN && height < 720))
      kr = 0.299, kb = 0.114;
   else if (profile == AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020 || profile == AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020)
      kr = 0.2627, kb = 0.0593;
   double kg = 1 - kr - kb, in = tenBitIn ? 1023 : 255, k = tenBit ? 4 : 1;
   double ys = (full ? (tenBit ? 1023 : 255) : 219 * k) / in, cs = (full ? (tenBit ? 1023 : 255) : 224 * k) / in;
   double m[3][3] = {
      {kr * ys, kg * ys, kb * ys},
      {-kr / (2 * (1 - kb)) * cs, -kg / (2 * (1 - kb)) * cs, 0.5 * cs},
      {0.5 * cs, -kg / (2 * (1 - kr)) * cs, -kb / (2 * (1 - kr)) * cs},
   };
   for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
         e->coef[i][j] = (int)(m[i][j] * 65536 + (m[i][j] < 0 ? -0.5 : 0.5));
   e->yOff = full ? 0 : 16 * (int)k;
   e->cOff = 128 * (int)k;
   e->convTenBit = tenBit;
   amf_log("Init: RGB input, %s range, Kr %.4f Kb %.4f", full ? "full" : "studio", kr, kb);
}

static inline void
rgb_pixel(AMF_SURFACE_FORMAT f, const unsigned char *p, int *r, int *g, int *b)
{
   switch (f) {
   case AMF_SURFACE_BGRA: *r = p[2], *g = p[1], *b = p[0]; break;
   case AMF_SURFACE_ARGB: *r = p[1], *g = p[2], *b = p[3]; break;
   case AMF_SURFACE_RGBA: *r = p[0], *g = p[1], *b = p[2]; break;
   default: {
      unsigned v = (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
      *r = v & 1023, *g = v >> 10 & 1023, *b = v >> 20 & 1023;
   }
   }
}

static inline int
clampi(int v, int max)
{
   return v < 0 ? 0 : v > max ? max : v;
}

/* the picture's RGB (pic: one packed plane) into e->conv as NV12 / P010, pic pointed at it; chroma from each 2x2 block's
 * mean */
static int
rgb_convert(encoder *e, AMF_SURFACE_FORMAT f, vk_encoder_picture *pic)
{
   int tenBit = e->convTenBit, b = tenBit ? 2 : 1, max = tenBit ? 1023 : 255;
   int w = pic->width, h = pic->height, w2 = (w + 1) / 2, h2 = (h + 1) / 2;
   size_t size = (size_t)w * h * b + (size_t)w2 * h2 * 2 * b;
   if (size > e->convSize) {
      unsigned char *conv = realloc(e->conv, size);
      if (!conv)
         return 0;
      e->conv = conv;
      e->convSize = size;
   }
   const unsigned char *src = pic->planes[0];
   int pitch = pic->pitches[0];
   unsigned char *y = e->conv, *uv = e->conv + (size_t)w * h * b;
   const int (*c)[3] = e->coef;
   for (int by = 0; by < h2; by++) {
      for (int bx = 0; bx < w2; bx++) {
         int sr = 0, sg = 0, sb = 0;
         for (int i = 0; i < 4; i++) {
            int px = clampi(2 * bx + (i & 1), w - 1), py = clampi(2 * by + (i >> 1), h - 1), r, g, bl;
            rgb_pixel(f, src + (size_t)py * pitch + (size_t)px * 4, &r, &g, &bl);
            sr += r, sg += g, sb += bl;
            if (2 * bx + (i & 1) >= w || 2 * by + (i >> 1) >= h)
               continue;
            int v = clampi(e->yOff + ((c[0][0] * r + c[0][1] * g + c[0][2] * bl + 32768) >> 16), max);
            size_t o = (size_t)py * w + px;
            if (tenBit)
               ((uint16_t *)y)[o] = (uint16_t)(v << 6);
            else
               y[o] = (unsigned char)v;
         }
         int u = clampi(e->cOff + ((c[1][0] * sr + c[1][1] * sg + c[1][2] * sb + (1 << 17)) >> 18), max);
         int v = clampi(e->cOff + ((c[2][0] * sr + c[2][1] * sg + c[2][2] * sb + (1 << 17)) >> 18), max);
         size_t o = ((size_t)by * w2 + bx) * 2;
         if (tenBit) {
            ((uint16_t *)uv)[o] = (uint16_t)(u << 6);
            ((uint16_t *)uv)[o + 1] = (uint16_t)(v << 6);
         } else {
            uv[o] = (unsigned char)u;
            uv[o + 1] = (unsigned char)v;
         }
      }
   }
   pic->planes[0] = y;
   pic->planes[1] = uv;
   pic->pitches[0] = w * b;
   pic->pitches[1] = w2 * 2 * b;
   pic->planeCount = 2;
   return 1;
}

static AMF_RESULT AMF_STD_CALL
encoder_Init(AMFComponent *pThis, AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height)
{
   encoder *e = (encoder *)pThis;
   if (e->enc)
      return AMF_ALREADY_INITIALIZED;
   if (format != AMF_SURFACE_NV12 && format != AMF_SURFACE_YUV420P && format != AMF_SURFACE_P010 && !is_rgb(format)) {
      amf_log("Init: surface format %d not supported (NV12, YUV420P, P010, BGRA, ARGB, RGBA, R10G10B10A2)", (int)format);
      return AMF_NOT_SUPPORTED;
   }
   e->format = format;
   vk_encoder_config c;
   encoder_config(e, &c);
   c.width = width;
   c.height = height;
   amf_log("Init: %s %dx%d profile %d level %d, %s, IDR every %d, rc %d (QP %d/%d, %lld bit/s)", e->hevc ? "HEVC" : "H.264",
           width, height, c.profile, c.level, c.tenBit ? "10-bit" : "8-bit", c.idrPeriod, c.rc, c.qpI, c.qpP,
           (long long)c.bitrate);
   if (is_rgb(format))
      rgb_matrix(e, height, c.tenBit, format == AMF_SURFACE_R10G10B10A2);
   e->enc = vk_encoder_create(&c);
   if (!e->enc)
      return AMF_FAIL;
   /* the parameter sets, for applications that want them apart (ffmpeg: extradata) */
   size_t size = 0;
   const unsigned char *headers = vk_encoder_headers(e->enc, &size);
   buffer *b = buffer_new(size);
   if (b) {
      memcpy(b->data, headers, size);
      AMFVariantStruct v;
      AMFVariantInit(&v);
      v.type = AMF_VARIANT_INTERFACE;
      v.pInterface = (AMFInterface *)b;
      props_set(&e->props, e->hevc ? AMF_VIDEO_ENCODER_HEVC_EXTRADATA : AMF_VIDEO_ENCODER_EXTRADATA, &v);
      buffer_Release((AMFBuffer *)b);
   }
   e->frames = 0;
   return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL encoder_ReInit(AMFComponent *pThis, amf_int32 w, amf_int32 h)
{
   encoder *e = (encoder *)pThis;
   if (e->enc) {
      vk_encoder_destroy(e->enc);
      e->enc = NULL;
   }
   return encoder_Init(pThis, e->format, w, h);
}

static AMF_RESULT AMF_STD_CALL encoder_Terminate(AMFComponent *pThis)
{
   encoder *e = (encoder *)pThis;
   encoder_drop_output(e);
   if (e->enc) {
      vk_encoder_destroy(e->enc);
      e->enc = NULL;
   }
   e->draining = 0;
   return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL encoder_Drain(AMFComponent *pThis)
{
   ((encoder *)pThis)->draining = 1;
   return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL encoder_Flush(AMFComponent *pThis)
{
   encoder *e = (encoder *)pThis;
   encoder_drop_output(e);
   e->draining = 0;
   return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL
encoder_SubmitInput(AMFComponent *pThis, AMFData *data)
{
   encoder *e = (encoder *)pThis;
   if (!e->enc)
      return AMF_NOT_INITIALIZED;
   if (e->draining)
      return AMF_EOF;
   if (!data)
      return AMF_INVALID_ARG;
   AMFSurface *in = NULL;
   AMFGuid id = IID_AMFSurface();
   if (data->pVtbl->QueryInterface(data, &id, (void **)&in) != AMF_OK || !in)
      return AMF_INVALID_DATA_TYPE;
   if (in->pVtbl != &surface_vtbl) {
      in->pVtbl->Release(in);
      amf_log("SubmitInput: not one of this runtime's surfaces");
      return AMF_NOT_SUPPORTED;
   }
   surface *s = (surface *)in;

   /* IDR: the first picture, the IDR period, or the application's ask */
   const wchar_t *forceName = e->hevc ? AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE : AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE;
   amf_int64 force = props_int(&s->props, forceName, 0);
   int idr = e->hevc ? (force == AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR || force == AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_I)
                     : (force == AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR || force == AMF_VIDEO_ENCODER_PICTURE_TYPE_I);

   vk_encoder_picture pic;
   memset(&pic, 0, sizeof(pic));
   pic.width = s->cropW;
   pic.height = s->cropH;
   pic.forceIdr = idr;
   for (amf_size i = 0; i < s->planeCount; i++) {
      plane *pl = &s->planes[i];
      int sub = pl->type == AMF_PLANE_Y || pl->type == AMF_PLANE_PACKED ? 1 : 2;
      pic.planes[i] = pl->data + (size_t)(s->cropY / sub) * pl->pitch + (size_t)(s->cropX / sub) * pl->pixelSize;
      pic.pitches[i] = pl->pitch;
   }
   pic.planeCount = (int)s->planeCount;
   if (is_rgb(s->format)) {
      if (s->format != e->format) {
         in->pVtbl->Release(in);
         amf_log("SubmitInput: surface format %d, the encoder's %d", (int)s->format, (int)e->format);
         return AMF_INVALID_FORMAT;
      }
      if (!rgb_convert(e, s->format, &pic)) {
         in->pVtbl->Release(in);
         return AMF_OUT_OF_MEMORY;
      }
   }

   size_t size = 0;
   int wasIdr = 0;
   const unsigned char *bits = vk_encoder_encode(e->enc, &pic, &size, &wasIdr);
   if (!bits) {
      in->pVtbl->Release(in);
      return AMF_FAIL;
   }
   buffer *b = buffer_new(size);
   if (!b) {
      in->pVtbl->Release(in);
      return AMF_OUT_OF_MEMORY;
   }
   memcpy(b->data, bits, size);
   /* the input's properties go along (applications pass their timestamps and frame references that way) */
   for (size_t i = 0; i < s->props.count; i++)
      props_set(&b->props, s->props.items[i].name, &s->props.items[i].value);
   AMFVariantStruct v;
   AMFVariantInit(&v);
   v.type = AMF_VARIANT_INT64;
   v.int64Value = e->hevc ? (wasIdr ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR : AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_P)
                          : (wasIdr ? AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_P);
   props_set(&b->props, e->hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &v);
   b->pts = s->pts;
   b->duration = s->duration;
   in->pVtbl->Release(in);

   if (e->outCount == e->outSize) {
      size_t n = e->outSize ? e->outSize * 2 : 8;
      buffer **out = realloc(e->out, n * sizeof(*out));
      if (!out) {
         buffer_Release((AMFBuffer *)b);
         return AMF_OUT_OF_MEMORY;
      }
      e->out = out;
      e->outSize = n;
   }
   e->out[e->outCount++] = b;
   e->frames++;
   return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL
encoder_QueryOutput(AMFComponent *pThis, AMFData **out)
{
   encoder *e = (encoder *)pThis;
   *out = NULL;
   if (!e->outCount)
      return e->draining ? AMF_EOF : AMF_REPEAT;
   *out = (AMFData *)e->out[0];
   memmove(e->out, e->out + 1, (e->outCount - 1) * sizeof(*e->out));
   e->outCount--;
   return AMF_OK;
}

static AMFContext *AMF_STD_CALL encoder_GetContext(AMFComponent *pThis) { return (AMFContext *)((encoder *)pThis)->ctx; }
static AMF_RESULT AMF_STD_CALL encoder_SetOutputDataAllocatorCB(AMFComponent *pThis, AMFDataAllocatorCB *cb)
{
   (void)pThis; (void)cb;
   return AMF_OK;
}
/* no capability object: applications then assume none of the optional features (B-pictures) */
static AMF_RESULT AMF_STD_CALL encoder_GetCaps(AMFComponent *pThis, AMFCaps **caps)
{
   (void)pThis;
   *caps = NULL;
   return AMF_NOT_SUPPORTED;
}
static AMF_RESULT AMF_STD_CALL encoder_Optimize(AMFComponent *pThis, AMFComponentOptimizationCallback *cb)
{
   (void)pThis; (void)cb;
   return AMF_OK;
}

static const AMFComponentVtbl encoder_vtbl = {
   encoder_Acquire, encoder_Release, encoder_QueryInterface, PROPS_VTBL(encoder),
   encoder_GetPropertiesInfoCount, encoder_GetPropertyInfoAt, encoder_GetPropertyInfo, encoder_ValidateProperty,
   encoder_Init, encoder_ReInit, encoder_Terminate, encoder_Drain, encoder_Flush, encoder_SubmitInput,
   encoder_QueryOutput, encoder_GetContext, encoder_SetOutputDataAllocatorCB, encoder_GetCaps, encoder_Optimize,
};

/* ---------------------------------------------------------------------------------------------------------------- */
/* The factory and the entry points */

static AMF_RESULT AMF_STD_CALL
factory_CreateContext(AMFFactory *pThis, AMFContext **out)
{
   (void)pThis;
   context *c = calloc(1, sizeof(*c));
   if (!c) {
      *out = NULL;
      return AMF_OUT_OF_MEMORY;
   }
   c->vtbl = &context_vtbl;
   c->refs = 1;
   *out = (AMFContext *)c;
   return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL
factory_CreateComponent(AMFFactory *pThis, AMFContext *ctx, const wchar_t *id, AMFComponent **out)
{
   (void)pThis;
   *out = NULL;
   int hevc;
   if (id && !wcscmp(id, AMFVideoEncoderVCE_AVC))
      hevc = 0;
   else if (id && !wcscmp(id, AMFVideoEncoder_HEVC))
      hevc = 1;
   else {
      amf_log("CreateComponent %ls: not available (H.264 and HEVC encoders only)", id ? id : L"(null)");
      return AMF_CODEC_NOT_SUPPORTED;
   }
   if (!vk_encoder_supported(hevc)) {
      amf_log("CreateComponent %ls: the GPU has no such encoder", id);
      return AMF_CODEC_NOT_SUPPORTED;
   }
   encoder *e = calloc(1, sizeof(*e));
   if (!e)
      return AMF_OUT_OF_MEMORY;
   e->vtbl = &encoder_vtbl;
   e->refs = 1;
   e->hevc = hevc;
   if (ctx && ctx->pVtbl == (const AMFContextVtbl *)&context_vtbl) {
      e->ctx = (context *)ctx;
      context_Acquire((AMFContext2 *)ctx);
   }
   *out = (AMFComponent *)e;
   return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL factory_SetCacheFolder(AMFFactory *pThis, const wchar_t *p) { (void)pThis; (void)p; return AMF_OK; }
static const wchar_t *AMF_STD_CALL factory_GetCacheFolder(AMFFactory *pThis) { (void)pThis; return L""; }
static AMF_RESULT AMF_STD_CALL factory_GetDebug(AMFFactory *pThis, AMFDebug **out) { (void)pThis; *out = NULL; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL factory_GetTrace(AMFFactory *pThis, AMFTrace **out)
{
   (void)pThis;
   the_trace.pVtbl = &trace_vtbl;
   *out = &the_trace;
   return AMF_OK;
}
static AMF_RESULT AMF_STD_CALL factory_GetPrograms(AMFFactory *pThis, AMFPrograms **out) { (void)pThis; *out = NULL; return AMF_NOT_SUPPORTED; }

static const AMFFactoryVtbl factory_vtbl = {
   factory_CreateContext, factory_CreateComponent, factory_SetCacheFolder, factory_GetCacheFolder,
   factory_GetDebug, factory_GetTrace, factory_GetPrograms,
};
static AMFFactory the_factory = {&factory_vtbl};

__declspec(dllexport) AMF_RESULT AMF_CDECL_CALL
AMFQueryVersion(amf_uint64 *version)
{
   if (!version)
      return AMF_INVALID_ARG;
   *version = AMF_FULL_VERSION;
   return AMF_OK;
}

__declspec(dllexport) AMF_RESULT AMF_CDECL_CALL
AMFInit(amf_uint64 version, AMFFactory **factory)
{
   (void)version;
   if (!factory)
      return AMF_INVALID_ARG;
   *factory = &the_factory;
   return AMF_OK;
}
