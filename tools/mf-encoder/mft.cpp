/*
 * Copyright 2026 WinBoat contributors
 * SPDX-License-Identifier: MIT
 */

/* winboat_mft64.dll: hardware H.264 and HEVC encoders for Media Foundation on WinBoat's virtual GPU. Windows' own
 * capture and recording (Game Bar, Snipping Tool, Clipchamp, Teams...) and ffmpeg's h264_mf/hevc_mf look for a
 * hardware encoder MFT; GPU vendors ship one with their driver. These are asynchronous MFTs (as hardware ones are)
 * on the AMF runtime's Vulkan Video encoder (../amf-runtime/vk_encoder.c: NVENC on NVIDIA hosts, through Venus).
 *
 * Input: NV12 or IYUV, in system memory or D3D11 textures (MF_SA_D3D11_AWARE: a texture is copied to a staging
 * texture and read; Media Foundation's video processor converts other formats ahead of an encoder). Output: Annex B
 * with the parameter sets ahead of each IDR picture, and on the output type (MF_MT_MPEG_SEQUENCE_HEADER). Settings
 * from the output type (size, rate, bit rate, profile, level) and ICodecAPI (rate control, QP, GOP, forced key
 * frames). The encoder works synchronously: ProcessInput encodes, so one picture is in flight. ponytail: no
 * B-pictures, no 10-bit input (P010), no ROI/LTR; add when an application asks for them.
 *
 * Registered as hardware async MFTs: the driver's INF writes the entries DllRegisterServer does (AddReg: InfVerif
 * refuses RegisterDlls), regsvr32 for a manual install. */

#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <new>

extern "C" {
#include "vk_encoder.h"
void amf_log(const char *format, ...);
}

// {6C0F8E3A-5B1D-4F7E-9C2A-1D8B3E4F5A60}, {6C0F8E3A-5B1D-4F7E-9C2A-1D8B3E4F5A61}
DEFINE_GUID(CLSID_WinBoatH264Encoder, 0x6c0f8e3a, 0x5b1d, 0x4f7e, 0x9c, 0x2a, 0x1d, 0x8b, 0x3e, 0x4f, 0x5a, 0x60);
DEFINE_GUID(CLSID_WinBoatHEVCEncoder, 0x6c0f8e3a, 0x5b1d, 0x4f7e, 0x9c, 0x2a, 0x1d, 0x8b, 0x3e, 0x4f, 0x5a, 0x61);

static const GUID kNoExtendedType = {};
/* ICodecAPI's interface ID (MinGW declares it without a definition) */
DEFINE_GUID(kIID_ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda);
static HMODULE g_module;
static LONG g_objects;

/* ---------------------------------------------------------------------------------------------------------------- */
/* Log: WINBOAT_MFT_LOG=<file>, and the debugger */

void
amf_log(const char *format, ...)
{
   static FILE *f;
   static int tried;
   if (!tried) {
      tried = 1;
      const char *path = getenv("WINBOAT_MFT_LOG");
      f = path ? fopen(path, "a") : NULL;
   }
   char line[512];
   va_list ap;
   va_start(ap, format);
   vsnprintf(line, sizeof(line), format, ap);
   va_end(ap);
   OutputDebugStringA("winboat mft: ");
   OutputDebugStringA(line);
   OutputDebugStringA("\n");
   if (f) {
      fprintf(f, "%s\n", line);
      fflush(f);
   }
}

template <class T> static void
release(T *&p)
{
   if (p) {
      p->Release();
      p = NULL;
   }
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* The encoder MFT */

struct Output {
   IMFSample *sample;
   Output *next;
};

class Encoder final : public IMFTransform, public IMFMediaEventGenerator, public IMFShutdown, public ICodecAPI {
 public:
   explicit Encoder(bool hevc) : hevc(hevc)
   {
      InterlockedIncrement(&g_objects);
      InitializeCriticalSection(&cs);
   }

   HRESULT
   init()
   {
      HRESULT hr = MFCreateAttributes(&attrs, 6);
      if (SUCCEEDED(hr))
         hr = MFCreateEventQueue(&events);
      if (FAILED(hr))
         return hr;
      attrs->SetUINT32(MF_TRANSFORM_ASYNC, TRUE);
      attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
      attrs->SetUINT32(MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE, TRUE);
      attrs->SetString(MFT_ENUM_HARDWARE_URL_Attribute, hevc ? L"WinBoat\\HEVC" : L"WinBoat\\H264");
      attrs->SetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, L"VEN_1AF4");
      return S_OK;
   }

   /* IUnknown */
   STDMETHODIMP
   QueryInterface(REFIID iid, void **out) override
   {
      if (iid == IID_IUnknown || iid == IID_IMFTransform)
         *out = static_cast<IMFTransform *>(this);
      else if (iid == IID_IMFMediaEventGenerator)
         *out = static_cast<IMFMediaEventGenerator *>(this);
      else if (iid == IID_IMFShutdown)
         *out = static_cast<IMFShutdown *>(this);
      else if (iid == kIID_ICodecAPI)
         *out = static_cast<ICodecAPI *>(this);
      else {
         *out = NULL;
         return E_NOINTERFACE;
      }
      AddRef();
      return S_OK;
   }
   STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
   STDMETHODIMP_(ULONG)
   Release() override
   {
      ULONG n = InterlockedDecrement(&refs);
      if (!n)
         delete this;
      return n;
   }

   /* IMFTransform: one input and one output stream */
   STDMETHODIMP
   GetStreamLimits(DWORD *inMin, DWORD *inMax, DWORD *outMin, DWORD *outMax) override
   {
      *inMin = *inMax = *outMin = *outMax = 1;
      return S_OK;
   }
   STDMETHODIMP
   GetStreamCount(DWORD *in, DWORD *out) override
   {
      *in = *out = 1;
      return S_OK;
   }
   STDMETHODIMP GetStreamIDs(DWORD, DWORD *, DWORD, DWORD *) override { return E_NOTIMPL; }

   STDMETHODIMP
   GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO *info) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      Lock l(this);
      memset(info, 0, sizeof(*info));
      info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER |
                      MFT_INPUT_STREAM_FIXED_SAMPLE_SIZE;
      info->cbSize = width * height * 3 / 2;
      return S_OK;
   }
   STDMETHODIMP
   GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO *info) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      memset(info, 0, sizeof(*info));
      info->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES | MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER |
                      MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
      return S_OK;
   }
   STDMETHODIMP
   GetAttributes(IMFAttributes **out) override
   {
      attrs->AddRef();
      *out = attrs;
      return S_OK;
   }
   STDMETHODIMP GetInputStreamAttributes(DWORD, IMFAttributes **) override { return E_NOTIMPL; }
   STDMETHODIMP GetOutputStreamAttributes(DWORD, IMFAttributes **) override { return E_NOTIMPL; }
   STDMETHODIMP DeleteInputStream(DWORD) override { return E_NOTIMPL; }
   STDMETHODIMP AddInputStreams(DWORD, DWORD *) override { return E_NOTIMPL; }

   /* Types: the output type first (codec, size, rate, bit rate, profile, level), then NV12 or IYUV of that size */
   STDMETHODIMP
   GetInputAvailableType(DWORD id, DWORD index, IMFMediaType **out) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      Lock l(this);
      if (!outType)
         return MF_E_TRANSFORM_TYPE_NOT_SET;
      if (index > 1)
         return MF_E_NO_MORE_TYPES;
      IMFMediaType *t;
      HRESULT hr = MFCreateMediaType(&t);
      if (FAILED(hr))
         return hr;
      t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      t->SetGUID(MF_MT_SUBTYPE, index ? MFVideoFormat_IYUV : MFVideoFormat_NV12);
      MFSetAttributeSize(t, MF_MT_FRAME_SIZE, width, height);
      MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fpsNum, fpsDen);
      MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
      t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
      *out = t;
      return S_OK;
   }
   STDMETHODIMP
   GetOutputAvailableType(DWORD id, DWORD index, IMFMediaType **out) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      if (index)
         return MF_E_NO_MORE_TYPES;
      IMFMediaType *t;
      HRESULT hr = MFCreateMediaType(&t);
      if (FAILED(hr))
         return hr;
      t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      t->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
      Lock l(this);
      if (outType) {
         MFSetAttributeSize(t, MF_MT_FRAME_SIZE, width, height);
         MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fpsNum, fpsDen);
      }
      *out = t;
      return S_OK;
   }

   STDMETHODIMP
   SetOutputType(DWORD id, IMFMediaType *type, DWORD flags) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      Lock l(this);
      if (!type) {
         if (!(flags & MFT_SET_TYPE_TEST_ONLY)) {
            release(outType);
            release(inType);
         }
         return S_OK;
      }
      GUID major, sub;
      UINT32 w = 0, h = 0, num = 0, den = 0;
      if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video ||
          FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != (hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264))
         return MF_E_INVALIDMEDIATYPE;
      if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || !w || !h || (w & 1) || (h & 1))
         return MF_E_INVALIDMEDIATYPE;
      UINT32 interlace = MFGetAttributeUINT32(type, MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      if (interlace != MFVideoInterlace_Progressive && interlace != MFVideoInterlace_MixedInterlaceOrProgressive)
         return MF_E_INVALIDMEDIATYPE;
      if (flags & MFT_SET_TYPE_TEST_ONLY)
         return S_OK;
      if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) || !num || !den)
         num = 30, den = 1;
      if (inType && (w != width || h != height))
         release(inType);
      width = w, height = h, fpsNum = num, fpsDen = den;
      bitrate = MFGetAttributeUINT32(type, MF_MT_AVG_BITRATE, bitrate);
      profile = MFGetAttributeUINT32(type, MF_MT_MPEG2_PROFILE, profile);
      level = MFGetAttributeUINT32(type, MF_MT_MPEG2_LEVEL, level);
      release(outType);
      HRESULT hr = MFCreateMediaType(&outType);
      if (FAILED(hr))
         return hr;
      type->CopyAllItems(outType);
      drop_encoder();
      /* set again after the input type (ffmpeg does): the parameter sets go on this type too */
      if (inType && !make_encoder())
         return MF_E_INVALIDMEDIATYPE;
      return S_OK;
   }

   STDMETHODIMP
   SetInputType(DWORD id, IMFMediaType *type, DWORD flags) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      Lock l(this);
      if (!type) {
         if (!(flags & MFT_SET_TYPE_TEST_ONLY))
            release(inType);
         return S_OK;
      }
      if (!outType)
         return MF_E_TRANSFORM_TYPE_NOT_SET;
      GUID major, sub;
      UINT32 w = 0, h = 0;
      if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video ||
          FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub)) || (sub != MFVideoFormat_NV12 && sub != MFVideoFormat_IYUV))
         return MF_E_INVALIDMEDIATYPE;
      if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || w != width || h != height)
         return MF_E_INVALIDMEDIATYPE;
      if (flags & MFT_SET_TYPE_TEST_ONLY)
         return S_OK;
      release(inType);
      HRESULT hr = MFCreateMediaType(&inType);
      if (FAILED(hr))
         return hr;
      type->CopyAllItems(inType);
      threePlanes = sub == MFVideoFormat_IYUV;
      /* the encoder now, for the parameter sets on the output type (containers want them up front) */
      drop_encoder();
      return make_encoder() ? S_OK : MF_E_UNSUPPORTED_D3D_TYPE;
   }

   STDMETHODIMP
   GetInputCurrentType(DWORD id, IMFMediaType **out) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      Lock l(this);
      return copy_type(inType, out);
   }
   STDMETHODIMP
   GetOutputCurrentType(DWORD id, IMFMediaType **out) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      Lock l(this);
      return copy_type(outType, out);
   }
   STDMETHODIMP
   GetInputStatus(DWORD id, DWORD *status) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      Lock l(this);
      *status = inputRequested ? MFT_INPUT_STATUS_ACCEPT_DATA : 0;
      return S_OK;
   }
   STDMETHODIMP
   GetOutputStatus(DWORD *status) override
   {
      Lock l(this);
      *status = outputs ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0;
      return S_OK;
   }
   STDMETHODIMP SetOutputBounds(LONGLONG, LONGLONG) override { return E_NOTIMPL; }
   STDMETHODIMP ProcessEvent(DWORD, IMFMediaEvent *) override { return E_NOTIMPL; }

   STDMETHODIMP
   ProcessMessage(MFT_MESSAGE_TYPE message, ULONG_PTR param) override
   {
      Lock l(this);
      if (shutdown)
         return MF_E_SHUTDOWN;
      switch (message) {
      case MFT_MESSAGE_SET_D3D_MANAGER:
         release(dxgiManager);
         if (param && FAILED(((IUnknown *)param)->QueryInterface(IID_IMFDXGIDeviceManager, (void **)&dxgiManager)))
            return E_NOTIMPL; /* Direct3D 9 managers: system memory then */
         return S_OK;
      case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
         return S_OK;
      case MFT_MESSAGE_NOTIFY_START_OF_STREAM:
         streaming = true;
         draining = false;
         request_input();
         return S_OK;
      case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
      case MFT_MESSAGE_NOTIFY_END_STREAMING:
         return S_OK;
      case MFT_MESSAGE_COMMAND_DRAIN:
         /* nothing is held back: drained once the outputs are taken */
         inputRequested = 0;
         if (outputs)
            draining = true;
         else
            drain_complete();
         return S_OK;
      case MFT_MESSAGE_COMMAND_FLUSH:
         drop_outputs();
         inputRequested = 0;
         if (streaming)
            request_input();
         return S_OK;
      case MFT_MESSAGE_COMMAND_MARKER:
         return queue_event(METransformMarker, param);
      default:
         return S_OK;
      }
   }

   STDMETHODIMP
   ProcessInput(DWORD id, IMFSample *sample, DWORD) override
   {
      if (id)
         return MF_E_INVALIDSTREAMNUMBER;
      Lock l(this);
      if (shutdown)
         return MF_E_SHUTDOWN;
      if (!inType || !outType)
         return MF_E_TRANSFORM_TYPE_NOT_SET;
      if (!inputRequested)
         return MF_E_NOTACCEPTING;
      inputRequested--;
      if (!enc && !make_encoder())
         return E_FAIL;
      IMFMediaBuffer *buf = NULL;
      HRESULT hr = sample->GetBufferByIndex(0, &buf);
      if (FAILED(hr))
         return hr;
      size_t size = 0;
      int idr = 0;
      const unsigned char *bits = NULL;
      IMFDXGIBuffer *dxgi = NULL;
      if (SUCCEEDED(buf->QueryInterface(IID_IMFDXGIBuffer, (void **)&dxgi))) {
         hr = encode_texture(dxgi, &bits, &size, &idr);
         dxgi->Release();
      } else {
         hr = encode_memory(buf, &bits, &size, &idr);
      }
      buf->Release();
      if (FAILED(hr))
         return hr;
      IMFSample *out = NULL;
      hr = make_sample(bits, size, idr, sample, &out);
      if (FAILED(hr))
         return hr;
      Output *o = new (std::nothrow) Output{out, NULL};
      if (!o) {
         out->Release();
         return E_OUTOFMEMORY;
      }
      Output **tail = &outputs;
      while (*tail)
         tail = &(*tail)->next;
      *tail = o;
      return queue_event(METransformHaveOutput, 0);
   }

   STDMETHODIMP
   ProcessOutput(DWORD flags, DWORD count, MFT_OUTPUT_DATA_BUFFER *data, DWORD *status) override
   {
      (void)flags;
      Lock l(this);
      *status = 0;
      if (shutdown)
         return MF_E_SHUTDOWN;
      if (count != 1 || data[0].dwStreamID)
         return E_INVALIDARG;
      if (!outputs)
         return MF_E_TRANSFORM_NEED_MORE_INPUT;
      Output *o = outputs;
      outputs = o->next;
      data[0].pSample = o->sample;
      data[0].dwStatus = 0;
      data[0].pEvents = NULL;
      delete o;
      if (!outputs) {
         if (draining) {
            draining = false;
            drain_complete();
         } else if (streaming) {
            request_input();
         }
      }
      return S_OK;
   }

   /* IMFMediaEventGenerator: the event queue's */
   STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent **e) override { return events->GetEvent(flags, e); }
   STDMETHODIMP BeginGetEvent(IMFAsyncCallback *cb, IUnknown *s) override { return events->BeginGetEvent(cb, s); }
   STDMETHODIMP EndGetEvent(IMFAsyncResult *r, IMFMediaEvent **e) override { return events->EndGetEvent(r, e); }
   STDMETHODIMP
   QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT *v) override
   {
      return events->QueueEventParamVar(type, ext, status, v);
   }

   /* IMFShutdown */
   STDMETHODIMP
   Shutdown() override
   {
      Lock l(this);
      shutdown = true;
      drop_outputs();
      drop_encoder();
      release(dxgiManager);
      return events->Shutdown();
   }
   STDMETHODIMP
   GetShutdownStatus(MFSHUTDOWN_STATUS *status) override
   {
      Lock l(this);
      if (!shutdown)
         return MF_E_INVALIDREQUEST;
      *status = MFSHUTDOWN_COMPLETED;
      return S_OK;
   }

   /* ICodecAPI: rate control, QP, GOP, bit rates, forced key frames; B-pictures stay 0 */
   STDMETHODIMP
   IsSupported(const GUID *api) override
   {
      return known(api) ? S_OK : E_NOTIMPL;
   }
   STDMETHODIMP IsModifiable(const GUID *api) override { return known(api) ? S_OK : E_NOTIMPL; }
   STDMETHODIMP GetParameterRange(const GUID *, VARIANT *, VARIANT *, VARIANT *) override { return E_NOTIMPL; }
   STDMETHODIMP GetParameterValues(const GUID *, VARIANT **, ULONG *) override { return E_NOTIMPL; }
   STDMETHODIMP GetDefaultValue(const GUID *, VARIANT *) override { return E_NOTIMPL; }
   STDMETHODIMP
   GetValue(const GUID *api, VARIANT *v) override
   {
      Lock l(this);
      VariantInit(v);
      v->vt = VT_UI4;
      if (*api == CODECAPI_AVEncCommonRateControlMode)
         v->ulVal = rcMode;
      else if (*api == CODECAPI_AVEncCommonMeanBitRate)
         v->ulVal = bitrate;
      else if (*api == CODECAPI_AVEncCommonMaxBitRate)
         v->ulVal = maxBitrate;
      else if (*api == CODECAPI_AVEncCommonBufferSize)
         v->ulVal = bufferSize;
      else if (*api == CODECAPI_AVEncCommonQuality)
         v->ulVal = quality;
      else if (*api == CODECAPI_AVEncMPVGOPSize)
         v->ulVal = gop;
      else if (*api == CODECAPI_AVEncMPVDefaultBPictureCount)
         v->ulVal = 0;
      else if (*api == CODECAPI_AVEncH264CABACEnable)
         v->vt = VT_BOOL, v->boolVal = cabac ? VARIANT_TRUE : VARIANT_FALSE;
      else if (*api == CODECAPI_AVEncCommonLowLatency || *api == CODECAPI_AVLowLatencyMode)
         v->vt = VT_BOOL, v->boolVal = VARIANT_TRUE;
      else if (*api == CODECAPI_AVEncVideoEncodeQP)
         v->vt = VT_UI8, v->ullVal = qp;
      else
         return E_NOTIMPL;
      return S_OK;
   }
   STDMETHODIMP
   SetValue(const GUID *api, VARIANT *v) override
   {
      Lock l(this);
      ULONGLONG n = v->vt == VT_UI8 || v->vt == VT_I8 ? v->ullVal : v->vt == VT_BOOL ? v->boolVal != 0 : v->ulVal;
      if (*api == CODECAPI_AVEncVideoForceKeyFrame) {
         forceKey = n != 0;
         return S_OK;
      }
      if (*api == CODECAPI_AVEncCommonRateControlMode)
         rcMode = (UINT32)n;
      else if (*api == CODECAPI_AVEncCommonMeanBitRate)
         bitrate = (UINT32)n;
      else if (*api == CODECAPI_AVEncCommonMaxBitRate)
         maxBitrate = (UINT32)n;
      else if (*api == CODECAPI_AVEncCommonBufferSize)
         bufferSize = (UINT32)n;
      else if (*api == CODECAPI_AVEncCommonQuality)
         quality = (UINT32)n;
      else if (*api == CODECAPI_AVEncMPVGOPSize)
         gop = (UINT32)n;
      else if (*api == CODECAPI_AVEncMPVDefaultBPictureCount)
         return n ? E_INVALIDARG : S_OK;
      else if (*api == CODECAPI_AVEncH264CABACEnable)
         cabac = n != 0;
      else if (*api == CODECAPI_AVEncCommonLowLatency || *api == CODECAPI_AVLowLatencyMode)
         return S_OK; /* every picture comes out as it goes in */
      else if (*api == CODECAPI_AVEncVideoEncodeQP) {
         /* QP 0-51 in the low 16 bits (the bits above: P and B QPs, which follow the first here) */
         if ((n & 0xffff) > 51)
            return E_INVALIDARG;
         qp = n & 0xffff;
      } else if (*api == CODECAPI_AVEncCommonQuality && n > 100)
         return E_INVALIDARG;
      else
         return E_NOTIMPL;
      settingsChanged = true;
      return S_OK;
   }
   STDMETHODIMP RegisterForEvent(const GUID *, LONG_PTR) override { return E_NOTIMPL; }
   STDMETHODIMP UnregisterForEvent(const GUID *) override { return E_NOTIMPL; }
   STDMETHODIMP SetAllDefaults() override { return E_NOTIMPL; }
   STDMETHODIMP SetValueWithNotify(const GUID *api, VARIANT *v, GUID **, ULONG *n) override
   {
      *n = 0;
      return SetValue(api, v);
   }
   STDMETHODIMP SetAllDefaultsWithNotify(GUID **, ULONG *) override { return E_NOTIMPL; }
   STDMETHODIMP GetAllSettings(IStream *) override { return E_NOTIMPL; }
   STDMETHODIMP SetAllSettings(IStream *) override { return E_NOTIMPL; }
   STDMETHODIMP SetAllSettingsWithNotify(IStream *, GUID **, ULONG *) override { return E_NOTIMPL; }

 private:
   struct Lock {
      Encoder *e;
      explicit Lock(Encoder *e) : e(e) { EnterCriticalSection(&e->cs); }
      ~Lock() { LeaveCriticalSection(&e->cs); }
   };

   ~Encoder()
   {
      drop_outputs();
      drop_encoder();
      release(inType);
      release(outType);
      release(dxgiManager);
      release(staging);
      release(stagingDevice);
      if (events) {
         events->Shutdown();
         events->Release();
      }
      release(attrs);
      DeleteCriticalSection(&cs);
      InterlockedDecrement(&g_objects);
   }

   static bool
   known(const GUID *api)
   {
      const GUID *apis[] = {&CODECAPI_AVEncCommonRateControlMode, &CODECAPI_AVEncCommonMeanBitRate,
                            &CODECAPI_AVEncCommonMaxBitRate, &CODECAPI_AVEncCommonBufferSize,
                            &CODECAPI_AVEncCommonQuality, &CODECAPI_AVEncMPVGOPSize,
                            &CODECAPI_AVEncMPVDefaultBPictureCount, &CODECAPI_AVEncH264CABACEnable,
                            &CODECAPI_AVEncCommonLowLatency, &CODECAPI_AVLowLatencyMode,
                            &CODECAPI_AVEncVideoEncodeQP, &CODECAPI_AVEncVideoForceKeyFrame};
      for (const GUID *a : apis)
         if (*a == *api)
            return true;
      return false;
   }

   static HRESULT
   copy_type(IMFMediaType *t, IMFMediaType **out)
   {
      if (!t)
         return MF_E_TRANSFORM_TYPE_NOT_SET;
      HRESULT hr = MFCreateMediaType(out);
      if (SUCCEEDED(hr))
         hr = t->CopyAllItems(*out);
      return hr;
   }

   HRESULT
   queue_event(MediaEventType type, ULONG_PTR marker)
   {
      IMFMediaEvent *e;
      HRESULT hr = MFCreateMediaEvent(type, kNoExtendedType, S_OK, NULL, &e);
      if (FAILED(hr))
         return hr;
      if (type == METransformMarker)
         e->SetUINT64(MF_EVENT_MFT_CONTEXT, marker);
      else
         e->SetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, 0);
      hr = events->QueueEvent(e);
      e->Release();
      return hr;
   }
   void
   request_input()
   {
      if (!inputRequested && SUCCEEDED(queue_event(METransformNeedInput, 0)))
         inputRequested = 1;
   }
   void
   drain_complete()
   {
      queue_event(METransformDrainComplete, 0);
   }

   void
   drop_outputs()
   {
      while (outputs) {
         Output *o = outputs;
         outputs = o->next;
         o->sample->Release();
         delete o;
      }
   }
   void
   drop_encoder()
   {
      if (enc)
         vk_encoder_destroy(enc);
      enc = NULL;
   }

   /* the encoder from the types and ICodecAPI; the parameter sets go on the output type */
   bool
   make_encoder()
   {
      vk_encoder_config c;
      memset(&c, 0, sizeof(c));
      c.hevc = hevc;
      c.width = (int)width;
      c.height = (int)height;
      c.fpsNum = (int)fpsNum;
      c.fpsDen = (int)fpsDen;
      /* MF's profile values are H.264's profile_idc (constrained baseline aside) and HEVC's general_profile_idc,
       * its levels H.264's 10x and HEVC's 30x level: what the encoder takes */
      if (hevc)
         c.profile = 1;
      else
         c.profile = profile == eAVEncH264VProfile_Base || profile == eAVEncH264VProfile_ConstrainedBase ? 66
                     : profile == eAVEncH264VProfile_Main                                                ? 77
                                                                                                         : 100;
      c.level = level == (UINT32)-1 ? 0 : (int)level;
      c.cabac = cabac && c.profile != 66;
      c.idrPeriod = gop ? (int)gop : (int)(2 * fpsNum / fpsDen);
      UINT32 fixedQp = (UINT32)(qp & 0xffff);
      if (rcMode == eAVEncCommonRateControlMode_Quality || (qp && rcMode != eAVEncCommonRateControlMode_CBR)) {
         c.rc = VK_ENCODER_RC_CQP;
         c.qpI = c.qpP = qp ? (int)fixedQp : 45 - (int)quality * 35 / 100;
      } else {
         c.rc = rcMode == eAVEncCommonRateControlMode_CBR ? VK_ENCODER_RC_CBR : VK_ENCODER_RC_VBR;
         c.qpI = c.qpP = 26;
      }
      c.bitrate = bitrate ? bitrate : 8000000;
      c.peak = maxBitrate ? maxBitrate : c.bitrate * 3 / 2;
      c.vbv = bufferSize;
      amf_log("%s %ux%u %u/%u fps, profile %d level %d, rc %d (QP %d, %lld bit/s), IDR every %d", hevc ? "HEVC" : "H.264",
              width, height, fpsNum, fpsDen, c.profile, c.level, c.rc, c.qpI, c.bitrate, c.idrPeriod);
      enc = vk_encoder_create(&c);
      if (!enc)
         return false;
      settingsChanged = false;
      size_t n = 0;
      const unsigned char *headers = vk_encoder_headers(enc, &n);
      if (outType && n)
         outType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, headers, (UINT32)n);
      return true;
   }

   HRESULT
   encode(const unsigned char *y, int pitch, int rows, const unsigned char **bits, size_t *size, int *idr)
   {
      if (settingsChanged) {
         drop_encoder();
         if (!make_encoder())
            return E_FAIL;
      }
      vk_encoder_picture pic;
      memset(&pic, 0, sizeof(pic));
      pic.width = (int)width;
      pic.height = (int)height;
      pic.forceIdr = forceKey;
      forceKey = false;
      pic.planes[0] = y;
      pic.pitches[0] = pitch;
      if (threePlanes) {
         pic.planeCount = 3;
         pic.planes[1] = y + (size_t)pitch * rows;
         pic.planes[2] = pic.planes[1] + (size_t)(pitch / 2) * (rows / 2);
         pic.pitches[1] = pic.pitches[2] = pitch / 2;
      } else {
         pic.planeCount = 2;
         pic.planes[1] = y + (size_t)pitch * rows;
         pic.pitches[1] = pitch;
      }
      *bits = vk_encoder_encode(enc, &pic, size, idr);
      return *bits ? S_OK : E_FAIL;
   }

   HRESULT
   encode_memory(IMFMediaBuffer *buf, const unsigned char **bits, size_t *size, int *idr)
   {
      IMF2DBuffer *b2 = NULL;
      BYTE *p = NULL;
      LONG pitch = 0;
      HRESULT hr;
      if (SUCCEEDED(buf->QueryInterface(IID_IMF2DBuffer, (void **)&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch)) &&
          pitch > 0) {
         hr = encode(p, (int)pitch, (int)height, bits, size, idr);
         b2->Unlock2D();
         b2->Release();
         return hr;
      }
      release(b2);
      DWORD len = 0;
      hr = buf->Lock(&p, NULL, &len);
      if (FAILED(hr))
         return hr;
      if (len < width * height * 3 / 2)
         hr = E_INVALIDARG;
      else
         hr = encode(p, (int)width, (int)height, bits, size, idr);
      buf->Unlock();
      return hr;
   }

   /* a D3D11 texture (an array slice of one, often): copied to a staging texture and read */
   HRESULT
   encode_texture(IMFDXGIBuffer *dxgi, const unsigned char **bits, size_t *size, int *idr)
   {
      ID3D11Texture2D *tex = NULL;
      UINT sub = 0;
      HRESULT hr = dxgi->GetResource(IID_ID3D11Texture2D, (void **)&tex);
      if (FAILED(hr))
         return hr;
      dxgi->GetSubresourceIndex(&sub);
      ID3D11Device *dev = NULL;
      tex->GetDevice(&dev);
      D3D11_TEXTURE2D_DESC desc;
      tex->GetDesc(&desc);
      if (!staging)
         amf_log("input: D3D11 texture %ux%u format %u, %u slices", desc.Width, desc.Height, desc.Format, desc.ArraySize);
      if (staging) {
         D3D11_TEXTURE2D_DESC have;
         staging->GetDesc(&have);
         if (stagingDevice != dev || have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format)
            release(staging), release(stagingDevice);
      }
      if (!staging) {
         D3D11_TEXTURE2D_DESC s = desc;
         s.MipLevels = s.ArraySize = 1;
         s.SampleDesc.Count = 1, s.SampleDesc.Quality = 0;
         s.Usage = D3D11_USAGE_STAGING;
         s.BindFlags = 0;
         s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
         s.MiscFlags = 0;
         hr = dev->CreateTexture2D(&s, NULL, &staging);
         if (FAILED(hr)) {
            dev->Release();
            tex->Release();
            return hr;
         }
         stagingDevice = dev;
         stagingDevice->AddRef();
      }
      ID3D11DeviceContext *ctx = NULL;
      dev->GetImmediateContext(&ctx);
      ID3D10Multithread *mt = NULL;
      if (SUCCEEDED(ctx->QueryInterface(IID_ID3D10Multithread, (void **)&mt)))
         mt->Enter();
      ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, sub, NULL);
      D3D11_MAPPED_SUBRESOURCE m;
      hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
      if (SUCCEEDED(hr)) {
         hr = encode((const unsigned char *)m.pData, (int)m.RowPitch, (int)desc.Height, bits, size, idr);
         ctx->Unmap(staging, 0);
      }
      if (mt) {
         mt->Leave();
         mt->Release();
      }
      ctx->Release();
      dev->Release();
      tex->Release();
      return hr;
   }

   HRESULT
   make_sample(const unsigned char *bits, size_t size, int idr, IMFSample *input, IMFSample **out)
   {
      IMFMediaBuffer *b = NULL;
      HRESULT hr = MFCreateMemoryBuffer((DWORD)size, &b);
      if (FAILED(hr))
         return hr;
      BYTE *p;
      if (SUCCEEDED(hr = b->Lock(&p, NULL, NULL))) {
         memcpy(p, bits, size);
         b->Unlock();
         b->SetCurrentLength((DWORD)size);
         hr = MFCreateSample(out);
      }
      if (SUCCEEDED(hr)) {
         (*out)->AddBuffer(b);
         LONGLONG t;
         if (SUCCEEDED(input->GetSampleTime(&t)))
            (*out)->SetSampleTime(t);
         if (SUCCEEDED(input->GetSampleDuration(&t)))
            (*out)->SetSampleDuration(t);
         (*out)->SetUINT32(MFSampleExtension_CleanPoint, idr ? TRUE : FALSE);
      }
      b->Release();
      return hr;
   }

   LONG refs = 1;
   bool hevc;
   CRITICAL_SECTION cs;
   IMFAttributes *attrs = NULL;
   IMFMediaEventQueue *events = NULL;
   IMFMediaType *inType = NULL, *outType = NULL;
   IMFDXGIDeviceManager *dxgiManager = NULL;
   ID3D11Texture2D *staging = NULL;
   ID3D11Device *stagingDevice = NULL;
   vk_encoder *enc = NULL;
   Output *outputs = NULL;
   int inputRequested = 0;
   bool streaming = false, draining = false, shutdown = false, forceKey = false, threePlanes = false;
   bool settingsChanged = false, cabac = true;
   UINT32 width = 0, height = 0, fpsNum = 30, fpsDen = 1;
   UINT32 profile = 0, level = (UINT32)-1;
   UINT32 rcMode = eAVEncCommonRateControlMode_PeakConstrainedVBR, bitrate = 0, maxBitrate = 0, bufferSize = 0;
   UINT32 quality = 70, gop = 0;
   ULONGLONG qp = 0;
};

/* ---------------------------------------------------------------------------------------------------------------- */
/* COM: the class factory and registration */

class Factory : public IClassFactory {
 public:
   explicit Factory(bool hevc) : hevc(hevc) {}
   STDMETHODIMP
   QueryInterface(REFIID iid, void **out) override
   {
      if (iid == IID_IUnknown || iid == IID_IClassFactory) {
         *out = this;
         AddRef();
         return S_OK;
      }
      *out = NULL;
      return E_NOINTERFACE;
   }
   STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&g_objects) ? 2 : 1; }
   STDMETHODIMP_(ULONG) Release() override { return InterlockedDecrement(&g_objects) ? 2 : 1; }
   STDMETHODIMP
   CreateInstance(IUnknown *outer, REFIID iid, void **out) override
   {
      *out = NULL;
      if (outer)
         return CLASS_E_NOAGGREGATION;
      /* No encoder on the host (no Venus, or no Vulkan video encoding there): not made, so that programs take the
       * next encoder Media Foundation lists, as with a GPU without one */
      if (!vk_encoder_supported(hevc))
         return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
      Encoder *e = new (std::nothrow) Encoder(hevc);
      if (!e)
         return E_OUTOFMEMORY;
      HRESULT hr = e->init();
      if (SUCCEEDED(hr))
         hr = e->QueryInterface(iid, out);
      e->Release();
      return hr;
   }
   STDMETHODIMP LockServer(BOOL) override { return S_OK; }

 private:
   bool hevc;
};

static Factory g_h264(false), g_hevc(true);

extern "C" __declspec(dllexport) HRESULT WINAPI
DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
   if (clsid == CLSID_WinBoatH264Encoder)
      return g_h264.QueryInterface(iid, out);
   if (clsid == CLSID_WinBoatHEVCEncoder)
      return g_hevc.QueryInterface(iid, out);
   *out = NULL;
   return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI
DllCanUnloadNow(void)
{
   return g_objects ? S_FALSE : S_OK;
}

static HRESULT
register_one(REFCLSID clsid, bool hevc, const wchar_t *path)
{
   wchar_t id[64], key[128];
   StringFromGUID2(clsid, id, 64);
   swprintf(key, 128, L"SOFTWARE\\Classes\\CLSID\\%ls\\InprocServer32", id);
   HKEY k;
   LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
   if (r)
      return HRESULT_FROM_WIN32(r);
   RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE *)path, (DWORD)(wcslen(path) + 1) * 2);
   RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Both", 10);
   RegCloseKey(k);

   MFT_REGISTER_TYPE_INFO in[] = {{MFMediaType_Video, MFVideoFormat_NV12}, {MFMediaType_Video, MFVideoFormat_IYUV}};
   MFT_REGISTER_TYPE_INFO out = {MFMediaType_Video, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
   IMFAttributes *a = NULL;
   HRESULT hr = MFCreateAttributes(&a, 2);
   if (FAILED(hr))
      return hr;
   a->SetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, L"VEN_1AF4");
   a->SetUINT32(MF_TRANSFORM_ASYNC, TRUE);
   hr = MFTRegister(clsid, MFT_CATEGORY_VIDEO_ENCODER,
                    (LPWSTR)(hevc ? L"WinBoat HEVC Encoder" : L"WinBoat H.264 Encoder"),
                    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT, 2, in, 1, &out, a);
   a->Release();
   return hr;
}

static void
unregister_one(REFCLSID clsid)
{
   MFTUnregister(clsid);
   wchar_t id[64], key[128];
   StringFromGUID2(clsid, id, 64);
   swprintf(key, 128, L"SOFTWARE\\Classes\\CLSID\\%ls", id);
   RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
}

extern "C" __declspec(dllexport) HRESULT WINAPI
DllRegisterServer(void)
{
   wchar_t path[MAX_PATH];
   if (!GetModuleFileNameW(g_module, path, MAX_PATH))
      return HRESULT_FROM_WIN32(GetLastError());
   HRESULT hr = register_one(CLSID_WinBoatH264Encoder, false, path);
   if (SUCCEEDED(hr))
      hr = register_one(CLSID_WinBoatHEVCEncoder, true, path);
   return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI
DllUnregisterServer(void)
{
   unregister_one(CLSID_WinBoatH264Encoder);
   unregister_one(CLSID_WinBoatHEVCEncoder);
   return S_OK;
}

BOOL WINAPI
DllMain(HINSTANCE module, DWORD reason, void *)
{
   if (reason == DLL_PROCESS_ATTACH) {
      g_module = module;
      DisableThreadLibraryCalls(module);
   }
   return TRUE;
}
