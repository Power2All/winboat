// Video, the full test ("video-full", its own row in the GPU check): the routes Windows programs decode and encode video
// on the GPU besides video.inc's Direct3D 11 decoding, each result checked:
// - the key frames video.inc decodes through Direct3D 11 also through DXVA2 (Direct3D 9: older players, Windows 7's
//   video renderer) and Direct3D 12 (ffmpeg's d3d12va), compared with libavcodec's pictures
// - Media Foundation's H.264 decoder on the GPU (DXVA through a D3D11 device manager: players, browsers, Films & TV) on
//   a stream with P and B frames from Microsoft's software encoder: every frame identical to the same decoder's on the
//   CPU (decoders are bit-exact)
// - the GPU's Media Foundation encoders (Windows' capture, ffmpeg's *_mf) and Direct3D 12 video encoding (ffmpeg's
//   *_d3d12va), H.264 and HEVC with P frames: the stream decoded again (Microsoft's decoders; HEVC's needs the HEVC
//   Video Extensions, without them its NAL units are checked) and compared with the source

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <d3d12video.h>

static const UINT VW = 320, VH = 192, VFRAMES = 30;

// Frame i of the source (NV12): a moving diagonal gradient, a bright square crossing it, chroma bands
static std::vector<uint8_t> sourceFrame(UINT i)
{
   std::vector<uint8_t> f(VW * VH * 3 / 2);
   for (UINT y = 0; y < VH; y++)
      for (UINT x = 0; x < VW; x++) {
         bool square = x >= 40 + i * 6 && x < 88 + i * 6 && y >= 60 && y < 108;
         f[y * VW + x] = square ? 220 : (uint8_t)(32 + (x + y + i * 4) % 160);
      }
   for (UINT y = 0; y < VH / 2; y++)
      for (UINT x = 0; x < VW / 2; x++) {
         f[VW * VH + y * VW + x * 2] = (uint8_t)(64 + (x * 2 + i) % 128);
         f[VW * VH + y * VW + x * 2 + 1] = (uint8_t)(64 + y * 3 % 128);
      }
   return f;
}

static double psnrY(const uint8_t *a, const uint8_t *b)
{
   double se = 0;
   for (UINT i = 0; i < VW * VH; i++) se += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
   return se == 0 ? 99 : 10 * log10(255.0 * 255.0 * VW * VH / se);
}

// The NAL unit types of an Annex B stream (H.264: the low 5 bits of the header byte, HEVC: bits 1-6)
static std::vector<int> nalTypes(const std::vector<uint8_t> &s, bool hevc)
{
   std::vector<int> types;
   for (size_t i = 0; i + 3 < s.size(); i++) {
      if (!s[i] && !s[i + 1] && s[i + 2] == 1) types.push_back(hevc ? (s[i + 3] >> 1) & 63 : s[i + 3] & 31);
   }
   return types;
}

// ---------------------------------------------------------------------------------------------------------------
// Media Foundation (looked up: Windows 7's mfplat has neither the DXGI device manager nor DXGI buffers)

struct MF {
   HRESULT(WINAPI *startup)(ULONG, DWORD) = nullptr;
   HRESULT(WINAPI *createMediaType)(IMFMediaType **) = nullptr;
   HRESULT(WINAPI *createSample)(IMFSample **) = nullptr;
   HRESULT(WINAPI *createMemoryBuffer)(DWORD, IMFMediaBuffer **) = nullptr;
   HRESULT(WINAPI *enumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFActivate ***,
                           UINT32 *) = nullptr;
   HRESULT(WINAPI *createDXGIDeviceManager)(UINT *, IMFDXGIDeviceManager **) = nullptr;
   HRESULT(WINAPI *createDXGISurfaceBuffer)(REFIID, IUnknown *, UINT, BOOL, IMFMediaBuffer **) = nullptr;

   bool load()
   {
      HMODULE lib = LoadLibraryA("mfplat.dll");
      if (!lib) return false;
#define MF_GET(field, name) field = (decltype(field))GetProcAddress(lib, name)
      MF_GET(startup, "MFStartup");
      MF_GET(createMediaType, "MFCreateMediaType");
      MF_GET(createSample, "MFCreateSample");
      MF_GET(createMemoryBuffer, "MFCreateMemoryBuffer");
      MF_GET(enumEx, "MFTEnumEx");
      MF_GET(createDXGIDeviceManager, "MFCreateDXGIDeviceManager");
      MF_GET(createDXGISurfaceBuffer, "MFCreateDXGISurfaceBuffer");
#undef MF_GET
      return startup && createMediaType && createSample && createMemoryBuffer && enumEx &&
             SUCCEEDED(startup(MF_VERSION, MFSTARTUP_NOSOCKET));
   }
   bool dxgi() const { return createDXGIDeviceManager && createDXGISurfaceBuffer; }
};

static IMFMediaType *videoType(MF &mf, const GUID &subtype, UINT32 bitrate = 0, UINT32 profile = 0)
{
   IMFMediaType *t = nullptr;
   if (FAILED(mf.createMediaType(&t))) return nullptr;
   t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
   t->SetGUID(MF_MT_SUBTYPE, subtype);
   t->SetUINT64(MF_MT_FRAME_SIZE, (UINT64)VW << 32 | VH);
   t->SetUINT64(MF_MT_FRAME_RATE, (UINT64)30 << 32 | 1);
   t->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1);
   t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
   if (bitrate) t->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
   if (profile) t->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
   return t;
}

static IMFSample *memorySample(MF &mf, const std::vector<uint8_t> &data, UINT index)
{
   IMFMediaBuffer *b = nullptr;
   IMFSample *s = nullptr;
   if (FAILED(mf.createMemoryBuffer((DWORD)data.size(), &b))) return nullptr;
   BYTE *p = nullptr;
   if (SUCCEEDED(b->Lock(&p, nullptr, nullptr))) {
      memcpy(p, data.data(), data.size());
      b->Unlock();
      b->SetCurrentLength((DWORD)data.size());
   }
   if (SUCCEEDED(mf.createSample(&s))) {
      s->AddBuffer(b);
      s->SetSampleTime(index * 333333LL);
      s->SetSampleDuration(333333);
   }
   b->Release();
   return s;
}

// A sample's bytes, contiguous (a picture: its planes without padding)
static bool sampleBytes(IMFSample *s, std::vector<uint8_t> &out)
{
   IMFMediaBuffer *b = nullptr;
   if (FAILED(s->ConvertToContiguousBuffer(&b))) return false;
   BYTE *p = nullptr;
   DWORD len = 0;
   bool ok = SUCCEEDED(b->Lock(&p, nullptr, &len));
   if (ok) {
      out.assign(p, p + len);
      b->Unlock();
   }
   b->Release();
   return ok;
}

static bool setOutputSubtype(IMFTransform *t, const GUID &subtype)
{
   for (DWORD i = 0;; i++) {
      IMFMediaType *mt = nullptr;
      if (FAILED(t->GetOutputAvailableType(0, i, &mt))) return false;
      GUID g = {};
      mt->GetGUID(MF_MT_SUBTYPE, &g);
      bool ok = g == subtype && SUCCEEDED(t->SetOutputType(0, mt, 0));
      mt->Release();
      if (ok) return true;
   }
}

// A synchronous transform (Microsoft's software encoder, the decoders): every input sample in, everything out,
// drained at the end; the output type set again when it changes (a decoder that has seen the stream). The outputs'
// bytes in out, and whether they were GPU (DXGI) buffers in gpu
static bool runSync(MF &mf, IMFTransform *t, const std::vector<std::vector<uint8_t>> &in, const GUID &outSubtype,
                    std::vector<std::vector<uint8_t>> &out, bool *gpu, std::string &e)
{
   t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
   t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
   if (gpu) *gpu = true;
   size_t next = 0;
   bool drained = false;
   for (int round = 0; round < 100000; round++) {
      for (;;) {
         MFT_OUTPUT_STREAM_INFO si = {};
         t->GetOutputStreamInfo(0, &si);
         MFT_OUTPUT_DATA_BUFFER ob = {};
         if (!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            IMFMediaBuffer *b = nullptr;
            mf.createMemoryBuffer(std::max<DWORD>(si.cbSize, VW * VH * 2), &b);
            mf.createSample(&ob.pSample);
            if (ob.pSample && b) ob.pSample->AddBuffer(b);
            release(b);
         }
         DWORD status = 0;
         HRESULT h = t->ProcessOutput(0, 1, &ob, &status);
         release(ob.pEvents);
         if (FAILED(h)) release(ob.pSample);
         if (h == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
         if (h == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!setOutputSubtype(t, outSubtype)) {
               e = "the decoder's new output type has no NV12";
               return false;
            }
            continue;
         }
         if (FAILED(h)) {
            e = "ProcessOutput: " + hrs(h);
            return false;
         }
         IMFMediaBuffer *b = nullptr;
         IMFDXGIBuffer *dxgi = nullptr;
         if (gpu && SUCCEEDED(ob.pSample->GetBufferByIndex(0, &b)) &&
             FAILED(b->QueryInterface(__uuidof(IMFDXGIBuffer), (void **)&dxgi)))
            *gpu = false;
         release(dxgi);
         release(b);
         out.emplace_back();
         sampleBytes(ob.pSample, out.back());
         ob.pSample->Release();
      }
      if (drained) return true;
      if (next < in.size()) {
         IMFSample *s = memorySample(mf, in[next], (UINT)next);
         HRESULT h = s ? t->ProcessInput(0, s, 0) : E_OUTOFMEMORY;
         release(s);
         if (h == MF_E_NOTACCEPTING) continue;
         if (FAILED(h)) {
            e = fmt("ProcessInput (sample %zu): %s", next, hrs(h).c_str());
            return false;
         }
         next++;
      } else {
         t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
         t->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
         drained = true;
      }
   }
   e = "the transform never finished";
   return false;
}

// A D3D11 device with video and a DXGI device manager for it (the GPU's decoders and encoders take it)
struct MFDevice {
   ID3D11Device *dev = nullptr;
   ID3D11DeviceContext *ctx = nullptr;
   IMFDXGIDeviceManager *manager = nullptr;
   bool create(MF &mf, std::string &e)
   {
      HRESULT h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                    D3D11_SDK_VERSION, &dev, nullptr, &ctx);
      ID3D10Multithread *mt = nullptr;
      if (SUCCEEDED(h) && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt))) {
         mt->SetMultithreadProtected(TRUE);
         mt->Release();
      }
      UINT token = 0;
      if (SUCCEEDED(h)) h = mf.createDXGIDeviceManager(&token, &manager);
      if (SUCCEEDED(h)) h = manager->ResetDevice(dev, token);
      if (FAILED(h)) e = "a D3D11 video device with a DXGI device manager: " + hrs(h);
      return SUCCEEDED(h);
   }
   void release()
   {
      ::release(manager);
      ::release(ctx);
      ::release(dev);
   }
};

// Decodes H.264 or HEVC access units with Microsoft's decoder, on the GPU (DXVA through the device manager) or the CPU
static bool mfDecode(MF &mf, bool hevc, IMFDXGIDeviceManager *manager, const std::vector<std::vector<uint8_t>> &units,
                     std::vector<std::vector<uint8_t>> &frames, std::string &e)
{
   // (HEVC's comes with a Store package: Media Foundation lists it, it isn't made by its CLSID)
   IMFTransform *t = nullptr;
   HRESULT h = E_NOINTERFACE;
   if (hevc) {
      MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_HEVC}, out = {MFMediaType_Video, MFVideoFormat_NV12};
      IMFActivate **acts = nullptr;
      UINT32 count = 0;
      mf.enumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                &in, &out, &acts, &count);
      if (count) h = acts[0]->ActivateObject(__uuidof(IMFTransform), (void **)&t);
      for (UINT32 i = 0; i < count; i++) acts[i]->Release();
      CoTaskMemFree(acts);
   } else {
      h = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform), (void **)&t);
   }
   if (FAILED(h)) {
      e = hevc ? "no HEVC decoder in Media Foundation (Microsoft's HEVC Video Extensions)" : "no H.264 decoder (" + hrs(h) + ")";
      return false;
   }
   bool gpu = manager != nullptr;
   if (manager) {
      IMFAttributes *a = nullptr;
      UINT32 aware = 0;
      if (SUCCEEDED(t->GetAttributes(&a))) a->GetUINT32(MF_SA_D3D11_AWARE, &aware);
      release(a);
      h = aware ? t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)manager) : E_NOINTERFACE;
      if (FAILED(h)) {
         e = "the decoder took no D3D11 device (" + hrs(h) + "): it has no DXVA profile on the GPU";
         t->Release();
         return false;
      }
   }
   IMFMediaType *in = videoType(mf, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
   h = in ? t->SetInputType(0, in, 0) : E_OUTOFMEMORY;
   release(in);
   if (FAILED(h) || !setOutputSubtype(t, MFVideoFormat_NV12)) {
      e = "the decoder's types: " + hrs(h);
      t->Release();
      return false;
   }
   bool onGpu = false;
   bool ok = runSync(mf, t, units, MFVideoFormat_NV12, frames, gpu ? &onGpu : nullptr, e);
   if (ok && gpu && !onGpu) {
      e = "the decoder's pictures were in system memory: it decoded on the CPU";
      ok = false;
   }
   t->Release();
   return ok;
}

// Decoded frames against the source: frame count, average and lowest luma PSNR
static bool compareWithSource(const std::vector<std::vector<uint8_t>> &frames, std::string &detail, double minAverage = 30)
{
   if (frames.size() != VFRAMES) {
      detail = fmt("%zu frames decoded, %u encoded", frames.size(), VFRAMES);
      return false;
   }
   double sum = 0, low = 99;
   for (UINT i = 0; i < VFRAMES; i++) {
      if (frames[i].size() < VW * VH) {
         detail = fmt("frame %u has %zu bytes", i, frames[i].size());
         return false;
      }
      double p = psnrY(frames[i].data(), sourceFrame(i).data());
      sum += p;
      low = std::min(low, p);
   }
   detail = fmt("%u frames, PSNR %.1f dB (lowest %.1f)", VFRAMES, sum / VFRAMES, low);
   return sum / VFRAMES >= minAverage && low >= minAverage - 8;
}

// An encoder's stream checked: decoded again on the CPU where Microsoft has a decoder for it (H.264; HEVC with the
// HEVC Video Extensions, which decode on the GPU), else its parameter sets and pictures counted
static void checkStream(MF &mf, const std::string &name, bool hevc, const std::vector<std::vector<uint8_t>> &units,
                        const std::string &note)
{
   // DXTEST_VIDEO_DUMP=1: the stream saved next to the report (<test name>.h264/.h265) to look into
   if (getenv("DXTEST_VIDEO_DUMP")) {
      std::string file = name;
      for (char &c : file) if (!isalnum((unsigned char)c)) c = '-';
      if (FILE *f = fopen((g_outDir + "\\" + file + (hevc ? ".h265" : ".h264")).c_str(), "wb")) {
         for (auto &u : units) fwrite(u.data(), 1, u.size(), f);
         fclose(f);
      }
   }
   std::vector<std::vector<uint8_t>> frames;
   std::string detail;
   MFDevice gpu;
   IMFDXGIDeviceManager *manager = nullptr;
   if (hevc && mf.dxgi() && gpu.create(mf, detail)) manager = gpu.manager;
   if (mfDecode(mf, hevc, manager, units, frames, detail)) {
      bool ok = compareWithSource(frames, detail);
      check(ok, name, detail + note);
   } else if (hevc && detail.find("Extensions") != std::string::npos) {
      // no decoder: the stream's NAL units (VPS, SPS, PPS, then an IRAP picture and the others)
      std::vector<uint8_t> all;
      for (auto &u : units) all.insert(all.end(), u.begin(), u.end());
      std::vector<int> types = nalTypes(all, true);
      int pictures = 0;
      bool sets = types.size() > 3 && types[0] == 32 && types[1] == 33 && types[2] == 34;
      for (int t : types) pictures += t < 32;
      bool ok = sets && pictures == (int)VFRAMES;
      check(ok, name, fmt("%u frames, %zu bytes; checked as a bitstream only (VPS/SPS/PPS %s, %d pictures): %s", VFRAMES,
                          all.size(), sets ? "first" : "missing", pictures, detail.c_str()) + note);
   } else {
      check(false, name, "decoding it again: " + detail);
   }
   gpu.release();
}

// The H.264 test stream: Microsoft's software encoder, Main profile, 2 B frames between P frames, a GOP of 15
static bool softwareH264(MF &mf, std::vector<std::vector<uint8_t>> &units, std::string &e)
{
   IMFTransform *t = nullptr;
   HRESULT h = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform), (void **)&t);
   if (FAILED(h)) {
      e = "Microsoft's H.264 encoder: " + hrs(h);
      return false;
   }
   ICodecAPI *api = nullptr;
   if (SUCCEEDED(t->QueryInterface(IID_ICodecAPI, (void **)&api))) {
      VARIANT v = {};
      v.vt = VT_UI4;
      v.ulVal = 2;
      api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
      v.ulVal = 15;
      api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
      api->Release();
   }
   IMFMediaType *out = videoType(mf, MFVideoFormat_H264, 2000000, eAVEncH264VProfile_Main);
   IMFMediaType *in = videoType(mf, MFVideoFormat_NV12);
   h = out && in ? t->SetOutputType(0, out, 0) : E_OUTOFMEMORY;
   if (SUCCEEDED(h)) h = t->SetInputType(0, in, 0);
   release(out);
   release(in);
   std::vector<std::vector<uint8_t>> frames;
   for (UINT i = 0; i < VFRAMES; i++) frames.push_back(sourceFrame(i));
   bool ok = SUCCEEDED(h) && runSync(mf, t, frames, MFVideoFormat_H264, units, nullptr, e);
   if (FAILED(h)) e = "the encoder's types: " + hrs(h);
   t->Release();
   return ok;
}

static void testMfDecode(MF &mf)
{
   const char *name = "Media Foundation H.264 decoding on the GPU (P and B frames)";
   if (!startTest(name)) return;
   if (!mf.dxgi()) {
      optional(false, name, "Windows 8 or later (Media Foundation's D3D11 decoding)");
      return;
   }
   std::vector<std::vector<uint8_t>> units, cpu, gpu;
   std::string e;
   MFDevice d;
   if (!softwareH264(mf, units, e) || !mfDecode(mf, false, nullptr, units, cpu, e) || !d.create(mf, e) ||
       !mfDecode(mf, false, d.manager, units, gpu, e)) {
      // (a GPU without H.264 decoding: Media Foundation decodes on the CPU, as it should)
      if (e.rfind("the decoder took no D3D11 device", 0) == 0) optional(false, name, e);
      else check(false, name, e);
      d.release();
      return;
   }
   d.release();
   std::vector<int> types;
   for (auto &u : units) {
      std::vector<int> t = nalTypes(u, false);
      types.insert(types.end(), t.begin(), t.end());
   }
   int differ = 0;
   for (size_t i = 0; i < std::min(cpu.size(), gpu.size()); i++) differ += cpu[i] != gpu[i];
   std::string detail;
   bool ok = compareWithSource(gpu, detail) && cpu.size() == gpu.size() && !differ;
   if (cpu.size() != gpu.size()) detail = fmt("%zu frames on the GPU, %zu on the CPU", gpu.size(), cpu.size());
   else if (differ) detail = fmt("%d of %zu frames differ from the CPU's; ", differ, gpu.size()) + detail;
   else detail += ", every frame identical to the CPU's";
   check(ok, name, detail);
}

// ---------------------------------------------------------------------------------------------------------------
// The GPU's Media Foundation encoders (asynchronous hardware transforms: events say when they take input and have
// output), fed D3D11 textures

static void testMfEncoder(MF &mf, bool hevc)
{
   const char *name = hevc ? "Media Foundation hardware encoder: HEVC" : "Media Foundation hardware encoder: H.264";
   if (!startTest(name)) return;
   if (!mf.dxgi()) {
      optional(false, name, "Windows 8 or later");
      return;
   }
   MFT_REGISTER_TYPE_INFO inInfo = {MFMediaType_Video, MFVideoFormat_NV12};
   MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Video, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
   IMFActivate **acts = nullptr;
   UINT32 count = 0;
   mf.enumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inInfo, &outInfo, &acts, &count);
   IMFTransform *t = nullptr;
   IMFActivate *act = count ? acts[0] : nullptr;
   std::string encoder = "?";
   if (act) {
      act->AddRef();
      WCHAR *friendly = nullptr;
      UINT32 len = 0;
      if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len))) {
         encoder = narrow(friendly);
         CoTaskMemFree(friendly);
      }
      acts[0]->ActivateObject(__uuidof(IMFTransform), (void **)&t);
   }
   for (UINT32 i = 0; i < count; i++) acts[i]->Release();
   CoTaskMemFree(acts);
   if (!t) {
      optional(false, name, "no hardware encoder in Media Foundation");
      release(act);
      return;
   }
   std::string e;
   MFDevice d;
   IMFAttributes *a = nullptr;
   IMFMediaEventGenerator *events = nullptr;
   std::vector<std::vector<uint8_t>> units;
   bool ok = d.create(mf, e);
   if (ok && SUCCEEDED(t->GetAttributes(&a))) a->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
   release(a);
   HRESULT h = ok ? t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)d.manager) : E_FAIL;
   IMFMediaType *out = videoType(mf, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, 2000000,
                                 hevc ? (UINT32)eAVEncH265VProfile_Main_420_8 : (UINT32)eAVEncH264VProfile_Main);
   IMFMediaType *in = videoType(mf, MFVideoFormat_NV12);
   if (SUCCEEDED(h)) h = t->SetOutputType(0, out, 0);
   if (SUCCEEDED(h)) h = t->SetInputType(0, in, 0);
   release(out);
   release(in);
   if (SUCCEEDED(h)) h = t->QueryInterface(__uuidof(IMFMediaEventGenerator), (void **)&events);
   if (ok && FAILED(h)) e = "setting the encoder up: " + hrs(h), ok = false;
   if (ok) {
      t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
      t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
      UINT next = 0;
      bool drainSent = false, done = false;
      DWORD deadline = GetTickCount() + 60000;
      while (ok && !done) {
         if ((int)(GetTickCount() - deadline) > 0) {
            e = fmt("no end after a minute (%u frames in, %zu out)", next, units.size());
            ok = false;
            break;
         }
         IMFMediaEvent *ev = nullptr;
         h = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
         if (h == MF_E_NO_EVENTS_AVAILABLE) {
            Sleep(1);
            continue;
         }
         if (FAILED(h)) {
            e = "GetEvent: " + hrs(h);
            ok = false;
            break;
         }
         MediaEventType type = MEUnknown;
         ev->GetType(&type);
         ev->Release();
         if (type == METransformNeedInput && next < VFRAMES) {
            std::vector<uint8_t> f = sourceFrame(next);
            D3D11_TEXTURE2D_DESC td = {VW, VH, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT,
                                       D3D11_BIND_SHADER_RESOURCE, 0, 0};
            D3D11_SUBRESOURCE_DATA init = {f.data(), VW, 0};
            ID3D11Texture2D *tex = nullptr;
            IMFMediaBuffer *b = nullptr;
            IMFSample *s = nullptr;
            h = d.dev->CreateTexture2D(&td, &init, &tex);
            if (SUCCEEDED(h)) h = mf.createDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, &b);
            if (SUCCEEDED(h)) h = mf.createSample(&s);
            if (SUCCEEDED(h)) {
               s->AddBuffer(b);
               s->SetSampleTime(next * 333333LL);
               s->SetSampleDuration(333333);
               h = t->ProcessInput(0, s, 0);
            }
            release(s);
            release(b);
            release(tex);
            if (FAILED(h)) e = fmt("frame %u in: %s", next, hrs(h).c_str()), ok = false;
            next++;
         } else if (type == METransformNeedInput && !drainSent) {
            t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            t->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            drainSent = true;
         } else if (type == METransformHaveOutput) {
            MFT_OUTPUT_DATA_BUFFER ob = {};
            DWORD status = 0;
            h = t->ProcessOutput(0, 1, &ob, &status);
            release(ob.pEvents);
            if (SUCCEEDED(h) && ob.pSample) {
               units.emplace_back();
               sampleBytes(ob.pSample, units.back());
            } else if (h != MF_E_TRANSFORM_STREAM_CHANGE) {
               e = "ProcessOutput: " + hrs(h), ok = false;
            }
            release(ob.pSample);
         } else if (type == METransformDrainComplete) {
            done = true;
         }
      }
      t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
   }
   release(events);
   t->Release();
   act->ShutdownObject();
   act->Release();
   d.release();
   if (!ok) check(false, name, encoder + ": " + e);
   else checkStream(mf, name, hevc, units, " (" + encoder + ")");
}

// ---------------------------------------------------------------------------------------------------------------
// DXVA2 (Direct3D 9) decoding of video.inc's key frames

static void testDxva2Decode(const VideoProfile &p)
{
   std::string name = std::string("DXVA2 (Direct3D 9) ") + p.name;
   if (!startTest(name)) return;
   const VideoSample &s = *p.sample;
   const UINT W = SAMPLE_SIZE, H = SAMPLE_SIZE;
   D9Ex d;
   std::string detail;
   IDirectXVideoDecoderService *vds = nullptr;
   IDirect3DSurface9 *surf = nullptr;
   IDirectXVideoDecoder *dec = nullptr;
   GUID *guids = nullptr;
   DXVA2_ConfigPictureDecode *cfgs = nullptr;
   UINT count = 0;
   bool offered = false, ok = false;
   HRESULT h = createEx(d, detail) ? DXVA2CreateVideoService(d.dev, __uuidof(IDirectXVideoDecoderService), (void **)&vds) : E_FAIL;
   if (SUCCEEDED(h)) h = vds->GetDecoderDeviceGuids(&count, &guids);
   for (UINT i = 0; SUCCEEDED(h) && i < count; i++) offered |= guids[i] == p.guid;
   const D3DFORMAT nv12 = (D3DFORMAT)MAKEFOURCC('N', 'V', '1', '2');
   DXVA2_VideoDesc desc = {};
   desc.SampleWidth = W;
   desc.SampleHeight = H;
   desc.SampleFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
   desc.Format = nv12;
   desc.InputSampleFreq = desc.OutputFrameFreq = {30, 1};
   DXVA2_ConfigPictureDecode cfg = {};
   if (SUCCEEDED(h) && offered) {
      h = vds->GetDecoderConfigurations(p.guid, &desc, nullptr, &count, &cfgs);
      for (UINT i = 0; SUCCEEDED(h) && i < count; i++) {
         cfg = cfgs[i];
         if (!s.qmatrix || cfg.ConfigBitstreamRaw == 2) break;
      }
      if (SUCCEEDED(h)) h = vds->CreateSurface(W, H, 0, nv12, D3DPOOL_DEFAULT, 0, DXVA2_VideoDecoderRenderTarget, &surf, nullptr);
      if (SUCCEEDED(h)) h = vds->CreateVideoDecoder(p.guid, &desc, &cfg, &surf, 1, &dec);
      if (SUCCEEDED(h)) h = dec->BeginFrame(surf, nullptr);
   }
   if (!d.dev || FAILED(h) || !offered) {
      // (no decoders at all: DXVA2's service or decoder list fails, which programs take for no DXVA2 decoding)
      if (d.dev && (SUCCEEDED(h) || !offered))
         optional(false, name, SUCCEEDED(h) ? "not offered by the GPU's DXVA2 decoder" : "no DXVA2 decoders (" + hrs(h) + ")");
      else check(false, name, detail.empty() ? "setting up the decoder: " + hrs(h) : detail);
   } else {
      UINT sliceCount = 0;
      std::vector<uint8_t> slices = sliceControl(s, sliceCount);
      struct Part { UINT type; const void *data; UINT size; } parts[] = {
         {DXVA2_PictureParametersBufferType, s.picture, s.pictureSize},
         {DXVA2_InverseQuantizationMatrixBufferType, s.qmatrix, s.qmatrixSize},
         {DXVA2_SliceControlBufferType, slices.data(), (UINT)slices.size()},
         {DXVA2_BitStreamDateBufferType, s.stream, s.streamSize},
      };
      std::vector<DXVA2_DecodeBufferDesc> descs;
      for (auto &part : parts) {
         if (!part.size) continue;
         void *ptr = nullptr;
         UINT size = 0;
         h = dec->GetBuffer(part.type, &ptr, &size);
         if (FAILED(h) || size < part.size) {
            detail = FAILED(h) ? "GetBuffer: " + hrs(h) : "decoder buffer too small";
            if (SUCCEEDED(h)) dec->ReleaseBuffer(part.type);
            break;
         }
         memcpy(ptr, part.data, part.size);
         dec->ReleaseBuffer(part.type);
         DXVA2_DecodeBufferDesc bd = {};
         bd.CompressedBufferType = part.type;
         bd.DataSize = part.size;
         if (part.type == DXVA2_SliceControlBufferType) bd.NumMBsInBuffer = sliceCount;
         descs.push_back(bd);
      }
      if (detail.empty()) {
         DXVA2_DecodeExecuteParams ep = {(UINT)descs.size(), descs.data(), nullptr};
         h = dec->Execute(&ep);
         if (FAILED(h)) detail = "Execute: " + hrs(h);
      }
      dec->EndFrame(nullptr);
      D3DLOCKED_RECT lr = {};
      if (detail.empty() && SUCCEEDED(h = surf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
         uint32_t hash = 0x811c9dc5u;
         const uint8_t *px = (const uint8_t *)lr.pBits;
         for (UINT y = 0; y < H + H / 2; y++) hash = fnv1a(hash, px + y * lr.Pitch, W);
         surf->UnlockRect();
         ok = hash == s.hash;
         detail = ok ? "a key frame decoded on the GPU, identical to the software decoder's"
                     : fmt("the decoded picture differs from the software decoder's (hash %08x, expected %08x)", hash, s.hash);
      } else if (detail.empty()) {
         detail = "LockRect: " + hrs(h);
      }
      check(ok, name, detail);
   }
   if (cfgs) CoTaskMemFree(cfgs);
   if (guids) CoTaskMemFree(guids);
   release(dec);
   release(surf);
   release(vds);
   d.release();
}

// ---------------------------------------------------------------------------------------------------------------
// Direct3D 12: a device on the GPU and its video queues

struct Video12 {
   D12 d;
   HMODULE lib = nullptr;
   bool create(std::string &e)
   {
      typedef HRESULT(WINAPI * PFN_Create)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
      lib = LoadLibraryA("d3d12.dll");
      auto create = lib ? (PFN_Create)GetProcAddress(lib, "D3D12CreateDevice") : nullptr;
      if (!create) {
         e = "no Direct3D 12 (Windows 10 or later)";
         return false;
      }
      IDXGIFactory4 *factory = nullptr;
      IDXGIAdapter1 *adapter = nullptr, *a = nullptr;
      CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&factory);
      for (UINT i = 0; factory && !adapter && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
         DXGI_ADAPTER_DESC1 desc;
         a->GetDesc1(&desc);
         if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) a->Release();
         else adapter = a;
      }
      HRESULT h = create(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&d.dev);
      ::release(adapter);
      ::release(factory);
      D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
      if (SUCCEEDED(h)) h = d.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&d.queue);
      if (SUCCEEDED(h)) h = d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&d.alloc);
      if (SUCCEEDED(h)) h = d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&d.list);
      if (SUCCEEDED(h)) h = d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&d.fence);
      d.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
      if (FAILED(h)) e = "a Direct3D 12 device on the GPU: " + hrs(h);
      return SUCCEEDED(h);
   }
   void release()
   {
      ::release(d.list);
      ::release(d.alloc);
      ::release(d.queue);
      ::release(d.fence);
      ::release(d.dev);
      if (d.event) CloseHandle(d.event);
   }
};

// A video queue with its allocator and command list (decode or encode)
template <class List> struct VideoQueue12 {
   ID3D12CommandQueue *queue = nullptr;
   ID3D12CommandAllocator *alloc = nullptr;
   List *list = nullptr;
   HRESULT create(D12 &d, D3D12_COMMAND_LIST_TYPE type)
   {
      D3D12_COMMAND_QUEUE_DESC qd = {type};
      HRESULT h = d.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&queue);
      if (SUCCEEDED(h)) h = d.dev->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator), (void **)&alloc);
      if (SUCCEEDED(h)) h = d.dev->CreateCommandList(0, type, alloc, nullptr, __uuidof(List), (void **)&list);
      return h;
   }
   bool submit(D12 &d)
   {
      if (FAILED(list->Close())) return false;
      ID3D12CommandList *lists[] = {list};
      queue->ExecuteCommandLists(1, lists);
      bool ok = waitIdle12(d, queue);
      alloc->Reset();
      list->Reset(alloc);
      return ok;
   }
   void release()
   {
      ::release(list);
      ::release(alloc);
      ::release(queue);
   }
};

static void videoBarrier(ID3D12VideoDecodeCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b = {};
   b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
   list->ResourceBarrier(1, &b);
}

static void videoBarrier(ID3D12VideoEncodeCommandList2 *list, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b = {};
   b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
   list->ResourceBarrier(1, &b);
}

// An NV12 or P010 texture's two planes read back through the direct queue: rows of w bytes (luma, then chroma)
static bool readPlanes12(D12 &d, ID3D12Resource *tex, UINT w, UINT h, std::vector<uint8_t> &out)
{
   D3D12_RESOURCE_DESC rd = tex->GetDesc();
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2];
   UINT rows[2];
   UINT64 rowBytes[2], total = 0;
   d.dev->GetCopyableFootprints(&rd, 0, 2, 0, fp, rows, rowBytes, &total);
   ID3D12Resource *rb = buffer12(d, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   if (!rb) return false;
   for (UINT p = 0; p < 2; p++) {
      D3D12_TEXTURE_COPY_LOCATION dst = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      dst.PlacedFootprint = fp[p];
      src.SubresourceIndex = p;
      d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
   }
   bool ok = submit12(d);
   uint8_t *m = nullptr;
   if (ok) ok = SUCCEEDED(rb->Map(0, nullptr, (void **)&m));
   if (ok) {
      out.clear();
      for (UINT y = 0; y < h; y++) out.insert(out.end(), m + fp[0].Offset + y * fp[0].Footprint.RowPitch, m + fp[0].Offset + y * fp[0].Footprint.RowPitch + w);
      for (UINT y = 0; y < h / 2; y++) out.insert(out.end(), m + fp[1].Offset + y * fp[1].Footprint.RowPitch, m + fp[1].Offset + y * fp[1].Footprint.RowPitch + w);
      rb->Unmap(0, nullptr);
   }
   rb->Release();
   return ok;
}

// An NV12 texture filled from a picture through the direct queue
static bool uploadNv12(D12 &d, ID3D12Resource *tex, const std::vector<uint8_t> &pic)
{
   D3D12_RESOURCE_DESC rd = tex->GetDesc();
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2];
   UINT rows[2];
   UINT64 rowBytes[2], total = 0;
   d.dev->GetCopyableFootprints(&rd, 0, 2, 0, fp, rows, rowBytes, &total);
   ID3D12Resource *up = buffer12(d, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
   uint8_t *m = nullptr;
   if (!up || FAILED(up->Map(0, nullptr, (void **)&m))) {
      release(up);
      return false;
   }
   for (UINT y = 0; y < VH; y++) memcpy(m + fp[0].Offset + y * fp[0].Footprint.RowPitch, &pic[y * VW], VW);
   for (UINT y = 0; y < VH / 2; y++) memcpy(m + fp[1].Offset + y * fp[1].Footprint.RowPitch, &pic[VW * VH + y * VW], VW);
   up->Unmap(0, nullptr);
   barrier12(d.list, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
   for (UINT p = 0; p < 2; p++) {
      D3D12_TEXTURE_COPY_LOCATION src = {up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, dst = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      src.PlacedFootprint = fp[p];
      dst.SubresourceIndex = p;
      d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
   }
   barrier12(d.list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
   bool ok = submit12(d);
   up->Release();
   return ok;
}

// A buffer's first bytes read back through the direct queue
static bool readBuffer12(D12 &d, ID3D12Resource *buf, UINT64 size, std::vector<uint8_t> &out)
{
   ID3D12Resource *rb = buffer12(d, size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   if (!rb) return false;
   d.list->CopyBufferRegion(rb, 0, buf, 0, size);
   bool ok = submit12(d);
   uint8_t *m = nullptr;
   if (ok) ok = SUCCEEDED(rb->Map(0, nullptr, (void **)&m));
   if (ok) {
      out.assign(m, m + size);
      rb->Unmap(0, nullptr);
   }
   rb->Release();
   return ok;
}

// Direct3D 12 decoding of a key frame (the same buffers as through D3D11)
static void testDecode12(Video12 &v, ID3D12VideoDevice *vdev, const VideoProfile &p)
{
   std::string name = std::string("Direct3D 12 ") + p.name;
   if (!startTest(name)) return;
   const VideoSample &s = *p.sample;
   const UINT W = SAMPLE_SIZE, H = SAMPLE_SIZE;
   D12 &d = v.d;
   D3D12_VIDEO_DECODE_CONFIGURATION config = {p.guid, D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE, D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE};
   D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support = {0, config, W, H, p.format, {30, 1}, 0};
   if (FAILED(vdev->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &support, sizeof support)) ||
       !(support.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED)) {
      optional(false, name, "not offered by the GPU's Direct3D 12 decoder");
      return;
   }
   VideoQueue12<ID3D12VideoDecodeCommandList> q;
   ID3D12VideoDecoder *decoder = nullptr;
   ID3D12VideoDecoderHeap *heap = nullptr;
   D3D12_VIDEO_DECODER_DESC dd = {0, config};
   D3D12_VIDEO_DECODER_HEAP_DESC hd = {0, config, W, H, p.format, {30, 1}, 0, 2};
   HRESULT h = q.create(d, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE);
   if (SUCCEEDED(h)) h = vdev->CreateVideoDecoder(&dd, __uuidof(ID3D12VideoDecoder), (void **)&decoder);
   if (SUCCEEDED(h)) h = vdev->CreateVideoDecoderHeap(&hd, __uuidof(ID3D12VideoDecoderHeap), (void **)&heap);
   ID3D12Resource *out = SUCCEEDED(h) ? texture12(d, W, H, p.format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON) : nullptr;
   ID3D12Resource *bits = SUCCEEDED(h) ? buffer12(d, s.streamSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ) : nullptr;
   void *m = nullptr;
   if (SUCCEEDED(h) && (!out || !bits || FAILED(bits->Map(0, nullptr, &m)))) h = E_OUTOFMEMORY;
   std::string detail;
   bool ok = false;
   if (FAILED(h)) {
      detail = "setting up the decoder: " + hrs(h);
   } else {
      memcpy(m, s.stream, s.streamSize);
      bits->Unmap(0, nullptr);
      UINT sliceCount = 0;
      std::vector<uint8_t> slices = sliceControl(s, sliceCount);
      D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in = {};
      in.FrameArguments[in.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, s.pictureSize, (void *)s.picture};
      if (s.qmatrix)
         in.FrameArguments[in.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, s.qmatrixSize, (void *)s.qmatrix};
      in.FrameArguments[in.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, (UINT)slices.size(), slices.data()};
      in.CompressedBitstream = {bits, 0, s.streamSize};
      in.pHeap = heap;
      D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS o = {out, 0};
      videoBarrier(q.list, out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE);
      q.list->DecodeFrame(decoder, &o, &in);
      videoBarrier(q.list, out, D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
      std::vector<uint8_t> pic;
      if (!q.submit(d)) detail = "the decode queue didn't finish";
      else if (!readPlanes12(d, out, W, H, pic)) detail = "reading the picture back failed";
      else {
         uint32_t hash = fnv1a(0x811c9dc5u, pic.data(), pic.size());
         ok = hash == s.hash;
         detail = ok ? "a key frame decoded on the GPU, identical to the software decoder's"
                     : fmt("the decoded picture differs from the software decoder's (hash %08x, expected %08x)", hash, s.hash);
      }
   }
   check(ok, name, detail);
   release(bits);
   release(out);
   release(heap);
   release(decoder);
   q.release();
}

// ---------------------------------------------------------------------------------------------------------------
// Direct3D 12 encoding: the parameter sets are the program's to write (the encoder writes the slices), with the
// values the D3D12 video encoding spec has encoders assume

struct BitWriter {
   std::vector<uint8_t> bytes;
   uint32_t cur = 0;
   int n = 0;
   void u(int bits, uint32_t v)
   {
      for (int i = bits - 1; i >= 0; i--) {
         cur = cur << 1 | ((v >> i) & 1);
         if (++n == 8) bytes.push_back((uint8_t)cur), cur = 0, n = 0;
      }
   }
   void ue(uint32_t v)
   {
      int len = 0;
      for (uint32_t t = v + 1; t > 1; t >>= 1) len++;
      u(len, 0);
      u(len + 1, v + 1);
   }
   void se(int v) { ue(v > 0 ? 2 * v - 1 : -2 * v); }
   // rbsp_trailing_bits, then the NAL unit with its start code and emulation prevention bytes
   std::vector<uint8_t> nal(std::initializer_list<uint8_t> header)
   {
      u(1, 1);
      while (n) u(1, 0);
      std::vector<uint8_t> out = {0, 0, 0, 1};
      out.insert(out.end(), header);
      int zeros = 0;
      for (uint8_t b : bytes) {
         if (zeros >= 2 && b <= 3) out.push_back(3), zeros = 0;
         out.push_back(b);
         zeros = b ? 0 : zeros + 1;
      }
      return out;
   }
};

static std::vector<uint8_t> h264Headers(bool cabac, UINT levelIdc)
{
   BitWriter sps;
   sps.u(8, 100);   // High
   sps.u(8, 0);     // constraint flags, reserved
   sps.u(8, levelIdc);
   sps.ue(0);       // seq_parameter_set_id
   sps.ue(1);       // chroma_format_idc 4:2:0
   sps.ue(0);       // bit_depth_luma_minus8
   sps.ue(0);       // bit_depth_chroma_minus8
   sps.u(1, 0);     // qpprime_y_zero_transform_bypass_flag
   sps.u(1, 0);     // seq_scaling_matrix_present_flag
   sps.ue(0);       // log2_max_frame_num_minus4
   sps.ue(2);       // pic_order_cnt_type
   sps.ue(1);       // max_num_ref_frames
   sps.u(1, 0);     // gaps_in_frame_num_value_allowed_flag
   sps.ue(VW / 16 - 1);
   sps.ue(VH / 16 - 1);
   sps.u(1, 1);     // frame_mbs_only_flag
   sps.u(1, 1);     // direct_8x8_inference_flag
   sps.u(1, 0);     // frame_cropping_flag
   sps.u(1, 0);     // vui_parameters_present_flag
   BitWriter pps;
   pps.ue(0);       // pic_parameter_set_id
   pps.ue(0);       // seq_parameter_set_id
   pps.u(1, cabac);
   pps.u(1, 0);     // bottom_field_pic_order_in_frame_present_flag
   pps.ue(0);       // num_slice_groups_minus1
   pps.ue(0);       // num_ref_idx_l0_default_active_minus1
   pps.ue(0);       // num_ref_idx_l1_default_active_minus1
   pps.u(1, 0);     // weighted_pred_flag
   pps.u(2, 0);     // weighted_bipred_idc
   pps.se(0);       // pic_init_qp_minus26
   pps.se(0);       // pic_init_qs_minus26
   pps.se(0);       // chroma_qp_index_offset
   pps.u(1, 1);     // deblocking_filter_control_present_flag
   pps.u(1, 0);     // constrained_intra_pred_flag
   pps.u(1, 0);     // redundant_pic_cnt_present_flag
   pps.u(1, 1);     // transform_8x8_mode_flag (High)
   pps.u(1, 0);     // pic_scaling_matrix_present_flag
   pps.se(0);       // second_chroma_qp_index_offset
   std::vector<uint8_t> out = sps.nal({0x67});
   std::vector<uint8_t> p = pps.nal({0x68});
   out.insert(out.end(), p.begin(), p.end());
   return out;
}

static void hevcProfileTierLevel(BitWriter &b, UINT levelIdc)
{
   b.u(2, 0);                    // general_profile_space
   b.u(1, 0);                    // general_tier_flag (Main)
   b.u(5, 1);                    // general_profile_idc (Main)
   b.u(32, 0x60000000);          // general_profile_compatibility_flag[1], [2]
   b.u(1, 1);                    // general_progressive_source_flag
   b.u(1, 0);                    // general_interlaced_source_flag
   b.u(1, 1);                    // general_non_packed_constraint_flag
   b.u(1, 1);                    // general_frame_only_constraint_flag
   b.u(32, 0);                   // 43 reserved bits and general_inbld_flag
   b.u(12, 0);
   b.u(8, levelIdc);
}

static std::vector<uint8_t> hevcHeaders(const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC &c, UINT levelIdc)
{
   auto log2 = [](UINT v) { UINT l = 0; while ((1u << l) < v) l++; return l; };
   UINT minCu = 8u << c.MinLumaCodingUnitSize, maxCu = 8u << c.MaxLumaCodingUnitSize;
   UINT minTu = 4u << c.MinLumaTransformUnitSize, maxTu = 4u << c.MaxLumaTransformUnitSize;
   BitWriter vps;
   vps.u(4, 0);                  // vps_video_parameter_set_id
   vps.u(1, 1);                  // vps_base_layer_internal_flag
   vps.u(1, 1);                  // vps_base_layer_available_flag
   vps.u(6, 0);                  // vps_max_layers_minus1
   vps.u(3, 0);                  // vps_max_sub_layers_minus1
   vps.u(1, 1);                  // vps_temporal_id_nesting_flag
   vps.u(16, 0xffff);
   hevcProfileTierLevel(vps, levelIdc);
   vps.u(1, 1);                  // vps_sub_layer_ordering_info_present_flag
   vps.ue(1);                    // vps_max_dec_pic_buffering_minus1
   vps.ue(0);                    // vps_max_num_reorder_pics
   vps.ue(0);                    // vps_max_latency_increase_plus1
   vps.u(6, 0);                  // vps_max_layer_id
   vps.ue(0);                    // vps_num_layer_sets_minus1
   vps.u(1, 0);                  // vps_timing_info_present_flag
   vps.u(1, 0);                  // vps_extension_flag
   BitWriter sps;
   sps.u(4, 0);                  // sps_video_parameter_set_id
   sps.u(3, 0);                  // sps_max_sub_layers_minus1
   sps.u(1, 1);                  // sps_temporal_id_nesting_flag
   hevcProfileTierLevel(sps, levelIdc);
   sps.ue(0);                    // sps_seq_parameter_set_id
   sps.ue(1);                    // chroma_format_idc
   sps.ue(VW);
   sps.ue(VH);
   sps.u(1, 0);                  // conformance_window_flag
   sps.ue(0);                    // bit_depth_luma_minus8
   sps.ue(0);                    // bit_depth_chroma_minus8
   sps.ue(4);                    // log2_max_pic_order_cnt_lsb_minus4
   sps.u(1, 1);                  // sps_sub_layer_ordering_info_present_flag
   sps.ue(1);                    // sps_max_dec_pic_buffering_minus1
   sps.ue(0);                    // sps_max_num_reorder_pics
   sps.ue(0);                    // sps_max_latency_increase_plus1
   sps.ue(log2(minCu) - 3);
   sps.ue(log2(maxCu) - log2(minCu));
   sps.ue(log2(minTu) - 2);
   sps.ue(log2(maxTu) - log2(minTu));
   sps.ue(c.max_transform_hierarchy_depth_inter);
   sps.ue(c.max_transform_hierarchy_depth_intra);
   sps.u(1, 0);                  // scaling_list_enabled_flag
   sps.u(1, !!(c.ConfigurationFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION));
   sps.u(1, !!(c.ConfigurationFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_SAO_FILTER));
   sps.u(1, 0);                  // pcm_enabled_flag
   sps.ue(0);                    // num_short_term_ref_pic_sets
   sps.u(1, 0);                  // long_term_ref_pics_present_flag
   sps.u(1, 0);                  // sps_temporal_mvp_enabled_flag
   sps.u(1, 0);                  // strong_intra_smoothing_enabled_flag
   sps.u(1, 0);                  // vui_parameters_present_flag
   sps.u(1, 0);                  // sps_extension_present_flag
   BitWriter pps;
   pps.ue(0);                    // pps_pic_parameter_set_id
   pps.ue(0);                    // pps_seq_parameter_set_id
   pps.u(1, 0);                  // dependent_slice_segments_enabled_flag
   pps.u(1, 0);                  // output_flag_present_flag
   pps.u(3, 0);                  // num_extra_slice_header_bits
   pps.u(1, 0);                  // sign_data_hiding_enabled_flag
   pps.u(1, 1);                  // cabac_init_present_flag
   pps.ue(0);                    // num_ref_idx_l0_default_active_minus1
   pps.ue(0);                    // num_ref_idx_l1_default_active_minus1
   pps.se(0);                    // init_qp_minus26
   pps.u(1, 0);                  // constrained_intra_pred_flag
   pps.u(1, 0);                  // transform_skip_enabled_flag
   pps.u(1, 1);                  // cu_qp_delta_enabled_flag
   pps.ue(0);                    // diff_cu_qp_delta_depth
   pps.se(0);                    // pps_cb_qp_offset
   pps.se(0);                    // pps_cr_qp_offset
   pps.u(1, 1);                  // pps_slice_chroma_qp_offsets_present_flag
   pps.u(1, 0);                  // weighted_pred_flag
   pps.u(1, 0);                  // weighted_bipred_flag
   pps.u(1, 0);                  // transquant_bypass_enabled_flag
   pps.u(1, 0);                  // tiles_enabled_flag
   pps.u(1, 0);                  // entropy_coding_sync_enabled_flag
   pps.u(1, 1);                  // pps_loop_filter_across_slices_enabled_flag
   pps.u(1, 1);                  // deblocking_filter_control_present_flag
   pps.u(1, 0);                  // deblocking_filter_override_enabled_flag
   pps.u(1, 0);                  // pps_deblocking_filter_disabled_flag
   pps.se(0);                    // pps_beta_offset_div2
   pps.se(0);                    // pps_tc_offset_div2
   pps.u(1, 0);                  // pps_scaling_list_data_present_flag
   pps.u(1, 0);                  // lists_modification_present_flag
   pps.ue(0);                    // log2_parallel_merge_level_minus2
   pps.u(1, 0);                  // slice_segment_header_extension_present_flag
   pps.u(1, 0);                  // pps_extension_present_flag
   std::vector<uint8_t> out = vps.nal({0x40, 0x01});
   for (auto &n : {sps.nal({0x42, 0x01}), pps.nal({0x44, 0x01})}) out.insert(out.end(), n.begin(), n.end());
   return out;
}

// (newer than MinGW's d3d12video.h)
struct D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC {
   UINT NodeIndex;
   D3D12_VIDEO_ENCODER_CODEC Codec;
   BOOL IsSupported;
};
static const D3D12_VIDEO_ENCODER_SUPPORT_FLAGS READABLE_RECONSTRUCTED = (D3D12_VIDEO_ENCODER_SUPPORT_FLAGS)0x2000;

// Encodes the source with Direct3D 12 video encoding: an IDR frame, then P frames each referencing the one before
static bool encode12(Video12 &v, ID3D12VideoDevice3 *vdev, bool hevc, std::vector<std::vector<uint8_t>> &units,
                     std::string &e, bool &supported)
{
   D12 &d = v.d;
   const D3D12_VIDEO_ENCODER_CODEC codec = hevc ? D3D12_VIDEO_ENCODER_CODEC_HEVC : D3D12_VIDEO_ENCODER_CODEC_H264;
   D3D12_VIDEO_ENCODER_PROFILE_H264 profile264 = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
   D3D12_VIDEO_ENCODER_PROFILE_HEVC profile265 = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
   D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
   if (hevc) profile.DataSize = sizeof profile265, profile.pHEVCProfile = &profile265;
   else profile.DataSize = sizeof profile264, profile.pH264Profile = &profile264;
   supported = false;
   D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC fc = {0, codec};
   if (FAILED(vdev->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC, &fc, sizeof fc)) || !fc.IsSupported) {
      e = "not offered by the GPU's Direct3D 12 encoder";
      return false;
   }
   supported = true;

   // level 4.1, Main tier
   D3D12_VIDEO_ENCODER_LEVELS_H264 level264 = D3D12_VIDEO_ENCODER_LEVELS_H264_41;
   D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC level265 = {D3D12_VIDEO_ENCODER_LEVELS_HEVC_41, D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN};
   D3D12_VIDEO_ENCODER_LEVEL_SETTING level = {};
   if (hevc) level.DataSize = sizeof level265, level.pHEVCLevelSetting = &level265;
   else level.DataSize = sizeof level264, level.pH264LevelSetting = &level264;

   // the codec configuration the encoder supports: CABAC; HEVC's block sizes (and AMP when it must be on)
   D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264 support264 = {};
   D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC support265 = {};
   D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT cs = {0, codec, profile};
   if (hevc) cs.CodecSupportLimits.DataSize = sizeof support265, cs.CodecSupportLimits.pHEVCSupport = &support265;
   else cs.CodecSupportLimits.DataSize = sizeof support264, cs.CodecSupportLimits.pH264Support = &support264;
   if (FAILED(vdev->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &cs, sizeof cs)) || !cs.IsSupported) {
      e = "the codec configuration query failed";
      return false;
   }
   D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 config264 = {};
   bool cabac = support264.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CABAC_ENCODING_SUPPORT;
   if (cabac) config264.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING;
   config264.DirectModeConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED;
   config264.DisableDeblockingFilterConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_0_ALL_LUMA_CHROMA_SLICE_BLOCK_EDGES_ALWAYS_FILTERED;
   D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC config265 = {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_NONE,
                                                             support265.MinLumaCodingUnitSize, support265.MaxLumaCodingUnitSize,
                                                             support265.MinLumaTransformUnitSize, support265.MaxLumaTransformUnitSize,
                                                             support265.max_transform_hierarchy_depth_inter,
                                                             support265.max_transform_hierarchy_depth_intra};
   if (support265.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_REQUIRED)
      config265.ConfigurationFlags |= D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION;
   // SAO where offered (as ffmpeg's hevc_d3d12va): some encoders can't turn it off and refuse setups without it
   if (support265.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_SAO_FILTER_SUPPORT)
      config265.ConfigurationFlags |= D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_SAO_FILTER;
   D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION config = {};
   if (hevc) config.DataSize = sizeof config265, config.pHEVCConfig = &config265;
   else config.DataSize = sizeof config264, config.pH264Config = &config264;

   D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 gop264 = {VFRAMES, 1, 2, 0, 0};
   D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC gop265 = {VFRAMES, 1, 4};
   D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop = {};
   if (hevc) gop.DataSize = sizeof gop265, gop.pHEVCGroupOfPictures = &gop265;
   else gop.DataSize = sizeof gop264, gop.pH264GroupOfPictures = &gop264;
   D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {24, 26, 28};
   D3D12_VIDEO_ENCODER_RATE_CONTROL rc = {D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP, D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE,
                                          {sizeof cqp}, {30, 1}};
   rc.ConfigParams.pConfiguration_CQP = &cqp;
   D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC res = {VW, VH};

   // the whole setup supported? (and what the encoder needs: texture arrays, reference-only pictures)
   D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS limits = {};
   D3D12_VIDEO_ENCODER_PROFILE_H264 sp264;
   D3D12_VIDEO_ENCODER_PROFILE_HEVC sp265;
   D3D12_VIDEO_ENCODER_LEVELS_H264 sl264;
   D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC sl265;
   D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT es = {0, codec, DXGI_FORMAT_NV12, config, gop, rc, D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE,
                                                  D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME, 1, &res, 1};
   es.SuggestedProfile = profile;
   if (hevc) es.SuggestedProfile.pHEVCProfile = &sp265, es.SuggestedLevel = {sizeof sl265}, es.SuggestedLevel.pHEVCLevelSetting = &sl265;
   else es.SuggestedProfile.pH264Profile = &sp264, es.SuggestedLevel = {sizeof sl264}, es.SuggestedLevel.pH264LevelSetting = &sl264;
   es.pResolutionDependentSupport = &limits;
   HRESULT h = vdev->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &es, sizeof es);
   if (FAILED(h) || !(es.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK)) {
      e = fmt("the setup isn't supported (%s, validation flags 0x%x)", hrs(h).c_str(), (unsigned)es.ValidationFlags);
      return false;
   }
   bool arrays = es.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RECONSTRUCTED_FRAMES_REQUIRE_TEXTURE_ARRAYS;
   D3D12_RESOURCE_FLAGS reconFlags = es.SupportFlags & READABLE_RECONSTRUCTED ? D3D12_RESOURCE_FLAG_NONE
                                     : D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
   D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS rr = {0, codec, profile, DXGI_FORMAT_NV12, res};
   if (FAILED(vdev->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &rr, sizeof rr)) || !rr.IsSupported) {
      e = "the resource requirements query failed";
      return false;
   }

   ID3D12VideoEncoder *encoder = nullptr;
   ID3D12VideoEncoderHeap *heap = nullptr;
   VideoQueue12<ID3D12VideoEncodeCommandList2> q;
   D3D12_VIDEO_ENCODER_DESC ed = {0, D3D12_VIDEO_ENCODER_FLAG_NONE, codec, profile, DXGI_FORMAT_NV12, config,
                                  D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM};
   D3D12_VIDEO_ENCODER_HEAP_DESC hd = {0, D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE, codec, profile, level, 1, &res};
   h = vdev->CreateVideoEncoder(&ed, __uuidof(ID3D12VideoEncoder), (void **)&encoder);
   if (SUCCEEDED(h)) h = vdev->CreateVideoEncoderHeap(&hd, __uuidof(ID3D12VideoEncoderHeap), (void **)&heap);
   if (SUCCEEDED(h)) h = q.create(d, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE);
   // the input, two reconstructed pictures (the current one and its reference, swapped every frame), the stream and
   // the metadata (as the encoder writes it and resolved)
   ID3D12Resource *input = texture12(d, VW, VH, DXGI_FORMAT_NV12, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
   ID3D12Resource *recon[2] = {};
   if (arrays) {
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, VW, VH, 2, 1, DXGI_FORMAT_NV12, {1, 0},
                                D3D12_TEXTURE_LAYOUT_UNKNOWN, reconFlags};
      d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     __uuidof(ID3D12Resource), (void **)&recon[0]);
      if (recon[0]) recon[0]->AddRef(), recon[1] = recon[0];
   } else {
      for (auto &r : recon) r = texture12(d, VW, VH, DXGI_FORMAT_NV12, reconFlags, D3D12_RESOURCE_STATE_COMMON);
   }
   const UINT64 streamSize = 4 << 20;
   UINT64 metaSize = std::max<UINT64>(rr.MaxEncoderOutputMetadataBufferSize, 4096);
   ID3D12Resource *stream = buffer12(d, streamSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
   ID3D12Resource *meta = buffer12(d, metaSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
   ID3D12Resource *resolved = buffer12(d, 4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
   if (SUCCEEDED(h) && (!input || !recon[0] || !recon[1] || !stream || !meta || !resolved)) h = E_OUTOFMEMORY;
   if (FAILED(h)) e = "creating the encoder and its resources: " + hrs(h);

   UINT levelIdc = hevc ? 123 : 41;   // 4.1
   for (UINT i = 0; SUCCEEDED(h) && i < VFRAMES; i++) {
      if (!uploadNv12(d, input, sourceFrame(i))) {
         e = fmt("frame %u: uploading the picture failed", i);
         h = E_FAIL;
         break;
      }
      UINT cur = i & 1, prev = cur ^ 1;
      UINT curSub = arrays ? cur : 0, prevSub = arrays ? prev : 0;
      UINT refIndex = 0;
      D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264 ref264 = {0, FALSE, 0, 2 * (i - 1), i - 1, 0};
      D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 pic264 = {};
      pic264.FrameType = i ? D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME : D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME;
      pic264.PictureOrderCountNumber = 2 * i;
      pic264.FrameDecodingOrderNumber = i;
      D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC ref265 = {0, TRUE, FALSE, i - 1, 0};
      D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC pic265 = {};
      pic265.FrameType = i ? D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_P_FRAME : D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_IDR_FRAME;
      pic265.PictureOrderCountNumber = i;
      if (i) {
         pic264.List0ReferenceFramesCount = pic265.List0ReferenceFramesCount = 1;
         pic264.pList0ReferenceFrames = pic265.pList0ReferenceFrames = &refIndex;
         pic264.ReferenceFramesReconPictureDescriptorsCount = pic265.ReferenceFramesReconPictureDescriptorsCount = 1;
         pic264.pReferenceFramesReconPictureDescriptors = &ref264;
         pic265.pReferenceFramesReconPictureDescriptors = &ref265;
      }
      ID3D12Resource *refTex = recon[prev];
      D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS in = {};
      in.SequenceControlDesc = {D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE, {D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE, 0},
                                rc, res, D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME, {}, gop};
      in.PictureControlDesc.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE;
      if (hevc) in.PictureControlDesc.PictureControlCodecData = {sizeof pic265}, in.PictureControlDesc.PictureControlCodecData.pHEVCPicData = &pic265;
      else in.PictureControlDesc.PictureControlCodecData = {sizeof pic264}, in.PictureControlDesc.PictureControlCodecData.pH264PicData = &pic264;
      if (i) in.PictureControlDesc.ReferenceFrames = {1, &refTex, &prevSub};
      in.pInputFrame = input;
      D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS out = {{stream, 0}, {recon[cur], curSub}, {meta, 0}};
      ID3D12VideoEncodeCommandList2 *list = q.list;
      // (with texture arrays the reference is the same resource as the picture written: one state for both)
      videoBarrier(list, input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
      videoBarrier(list, recon[cur], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
      if (i && !arrays) videoBarrier(list, refTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
      videoBarrier(list, stream, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
      videoBarrier(list, meta, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
      list->EncodeFrame(encoder, heap, &in, &out);
      videoBarrier(list, meta, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
      videoBarrier(list, resolved, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
      D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS ri = {codec, profile, DXGI_FORMAT_NV12, res, {meta, 0}};
      D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS ro = {{resolved, 0}};
      list->ResolveEncoderOutputMetadata(&ri, &ro);
      videoBarrier(list, input, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON);
      videoBarrier(list, recon[cur], D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
      if (i && !arrays) videoBarrier(list, refTex, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON);
      videoBarrier(list, stream, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
      videoBarrier(list, meta, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON);
      videoBarrier(list, resolved, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
      std::vector<uint8_t> md, bytes;
      if (!q.submit(d) || !readBuffer12(d, resolved, sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA), md)) {
         e = fmt("frame %u: the encode queue didn't finish", i);
         h = E_FAIL;
         break;
      }
      D3D12_VIDEO_ENCODER_OUTPUT_METADATA result;
      memcpy(&result, md.data(), sizeof result);
      if (result.EncodeErrorFlags || !result.EncodedBitstreamWrittenBytesCount || result.EncodedBitstreamWrittenBytesCount > streamSize) {
         e = fmt("frame %u: error flags 0x%llx, %llu bytes", i, (unsigned long long)result.EncodeErrorFlags,
                 (unsigned long long)result.EncodedBitstreamWrittenBytesCount);
         h = E_FAIL;
         break;
      }
      if (!readBuffer12(d, stream, result.EncodedBitstreamWrittenBytesCount, bytes)) {
         e = fmt("frame %u: reading the stream back failed", i);
         h = E_FAIL;
         break;
      }
      units.push_back(i ? std::vector<uint8_t>() : hevc ? hevcHeaders(config265, levelIdc) : h264Headers(cabac, levelIdc));
      units.back().insert(units.back().end(), bytes.begin(), bytes.end());
   }
   release(resolved);
   release(meta);
   release(stream);
   release(recon[1]);
   release(recon[0]);
   release(input);
   q.release();
   release(heap);
   release(encoder);
   return SUCCEEDED(h);
}

static void testEncode12(MF &mf, bool mfLoaded, Video12 &v, ID3D12VideoDevice3 *vdev, bool hevc)
{
   const char *name = hevc ? "Direct3D 12 video encoding: HEVC Main (P frames)" : "Direct3D 12 video encoding: H.264 High (P frames)";
   if (!startTest(name)) return;
   if (!vdev) {
      optional(false, name, "no Direct3D 12 video encoding (Windows 11's runtime)");
      return;
   }
   std::vector<std::vector<uint8_t>> units;
   std::string e;
   bool supported = false;
   if (!encode12(v, vdev, hevc, units, e, supported)) {
      if (supported) check(false, name, e);
      else optional(false, name, e);
   } else if (!mfLoaded) {
      check(false, name, "no Media Foundation to decode the stream again");
   } else {
      checkStream(mf, name, hevc, units, "");
   }
}

static void testAv1Encode12(ID3D12VideoDevice3 *vdev)
{
   const char *name = "Direct3D 12 video encoding: AV1";
   if (!startTest(name)) return;
   D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC fc = {0, D3D12_VIDEO_ENCODER_CODEC_AV1};
   bool has = vdev && SUCCEEDED(vdev->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC, &fc, sizeof fc)) && fc.IsSupported;
   // (offered: not encoded here yet)
   optional(has, name, has ? "offered (not encoded by this test)" : "not offered by the GPU's encoder (NVIDIA: RTX 40 and later)");
}

// ---------------------------------------------------------------------------------------------------------------

static void groupVideoFull()
{
   if (g_warp) {
      report(Status::Skip, "Video decoding and encoding", "no video on WARP");
      return;
   }
   CoInitializeEx(nullptr, COINIT_MULTITHREADED);
   MF mf;
   bool mfLoaded = mf.load();
   if (!mfLoaded) report(Status::Info, "Media Foundation", "not available (Windows N editions: the Media Feature Pack)");

   for (auto &p : VIDEO_PROFILES)
      if (p.sample) testDxva2Decode(p);

   Video12 v;
   std::string e;
   ID3D12VideoDevice *vdev = nullptr;
   ID3D12VideoDevice3 *vdev3 = nullptr;
   bool d3d12 = windows10OrLater() && v.create(e);
   if (d3d12) {
      v.d.dev->QueryInterface(__uuidof(ID3D12VideoDevice), (void **)&vdev);
      v.d.dev->QueryInterface(__uuidof(ID3D12VideoDevice3), (void **)&vdev3);
   }
   for (auto &p : VIDEO_PROFILES) {
      if (!p.sample) continue;
      if (vdev) testDecode12(v, vdev, p);
      else if (startTest(std::string("Direct3D 12 ") + p.name))
         optional(false, std::string("Direct3D 12 ") + p.name, d3d12 ? "no Direct3D 12 video device" : "no Direct3D 12 (Windows 10 or later)");
   }

   if (mfLoaded) testMfDecode(mf);
   if (mfLoaded) {
      testMfEncoder(mf, false);
      testMfEncoder(mf, true);
   }
   testEncode12(mf, mfLoaded, v, vdev3, false);
   testEncode12(mf, mfLoaded, v, vdev3, true);
   testAv1Encode12(vdev3);

   release(vdev3);
   release(vdev);
   if (d3d12) v.release();
}
