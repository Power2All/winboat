/* sinkwriter-test.exe <in.nv12> <w> <h> <frames> <out.mp4> [hevc]: Windows' own encoding route: the Media Foundation
 * Sink Writer with a D3D11 device manager and hardware transforms enabled, NV12 frames uploaded to D3D11 textures;
 * prints the encoder it picked */
#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                                                                       \
   do {                                                                                                                \
      HRESULT hr_ = (x);                                                                                               \
      if (FAILED(hr_)) {                                                                                               \
         printf("%s: 0x%08lx\n", #x, hr_);                                                                             \
         return 1;                                                                                                     \
      }                                                                                                                \
   } while (0)

int main(int argc, char **argv)
{
   if (argc < 6)
      return 2;
   UINT32 w = atoi(argv[2]), h = atoi(argv[3]), frames = atoi(argv[4]);
   bool hevc = argc > 6;
   FILE *f = fopen(argv[1], "rb");
   if (!f)
      return 3;
   size_t frameSize = (size_t)w * h * 3 / 2;
   unsigned char *frame = (unsigned char *)malloc(frameSize);
   CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
   CHECK(MFStartup(MF_VERSION, 0));
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0,
                           D3D11_SDK_VERSION, &dev, NULL, &ctx));
   ID3D10Multithread *mt;
   CHECK(ctx->QueryInterface(IID_ID3D10Multithread, (void **)&mt));
   mt->SetMultithreadProtected(TRUE);
   UINT token;
   IMFDXGIDeviceManager *mgr;
   CHECK(MFCreateDXGIDeviceManager(&token, &mgr));
   CHECK(mgr->ResetDevice(dev, token));
   IMFAttributes *a;
   CHECK(MFCreateAttributes(&a, 3));
   a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
   a->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, mgr);
   wchar_t out[MAX_PATH];
   MultiByteToWideChar(CP_ACP, 0, argv[5], -1, out, MAX_PATH);
   IMFSinkWriter *sw;
   CHECK(MFCreateSinkWriterFromURL(out, NULL, a, &sw));
   IMFMediaType *ot, *it;
   CHECK(MFCreateMediaType(&ot));
   ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
   ot->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
   ot->SetUINT32(MF_MT_AVG_BITRATE, 10000000);
   ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
   MFSetAttributeSize(ot, MF_MT_FRAME_SIZE, w, h);
   MFSetAttributeRatio(ot, MF_MT_FRAME_RATE, 30, 1);
   MFSetAttributeRatio(ot, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
   DWORD stream;
   CHECK(sw->AddStream(ot, &stream));
   CHECK(MFCreateMediaType(&it));
   it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
   it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
   it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
   MFSetAttributeSize(it, MF_MT_FRAME_SIZE, w, h);
   MFSetAttributeRatio(it, MF_MT_FRAME_RATE, 30, 1);
   MFSetAttributeRatio(it, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
   CHECK(sw->SetInputMediaType(stream, it, NULL));
   CHECK(sw->BeginWriting());

   /* the encoder the sink writer chose */
   IMFSinkWriterEx *swx;
   if (SUCCEEDED(sw->QueryInterface(IID_IMFSinkWriterEx, (void **)&swx))) {
      for (DWORD i = 0;; i++) {
         GUID cat;
         IMFTransform *t;
         if (FAILED(swx->GetTransformForStream(stream, i, &cat, &t)))
            break;
         IMFAttributes *ta = NULL;
         wchar_t *url = NULL;
         UINT32 n;
         if (SUCCEEDED(t->GetAttributes(&ta)) && ta)
            ta->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute, &url, &n);
         printf("transform %lu: %s, hardware URL %ls\n", i, cat == MFT_CATEGORY_VIDEO_ENCODER ? "encoder" : "other",
                url ? url : L"(none: software)");
         CoTaskMemFree(url);
         if (ta)
            ta->Release();
         t->Release();
      }
      swx->Release();
   }

   D3D11_TEXTURE2D_DESC d = {};
   d.Width = w, d.Height = h, d.MipLevels = d.ArraySize = 1, d.Format = DXGI_FORMAT_NV12, d.SampleDesc.Count = 1;
   d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   for (UINT32 i = 0; i < frames; i++) {
      if (fread(frame, 1, frameSize, f) != frameSize)
         break;
      ID3D11Texture2D *tex;
      D3D11_SUBRESOURCE_DATA init = {frame, w, 0};
      CHECK(dev->CreateTexture2D(&d, &init, &tex));
      IMFMediaBuffer *b;
      CHECK(MFCreateDXGISurfaceBuffer(IID_ID3D11Texture2D, tex, 0, FALSE, &b));
      b->SetCurrentLength((DWORD)frameSize);
      IMFSample *s;
      CHECK(MFCreateSample(&s));
      s->AddBuffer(b);
      s->SetSampleTime((LONGLONG)i * 333333);
      s->SetSampleDuration(333333);
      CHECK(sw->WriteSample(stream, s));
      s->Release();
      b->Release();
      tex->Release();
   }
   CHECK(sw->Finalize());
   printf("written\n");
   return 0;
}
