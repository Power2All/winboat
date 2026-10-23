// Direct3D 9Ex and the parts of Direct3D 9 around it that applications use without drawing much themselves:
// surfaces shared between devices (and with Direct3D 11), residency and frame latency, GDI drawing on D3D9
// surfaces, the vertex cache query, DXVA2 video processing (what Windows' video renderer EVR uses) and decoders,
// exclusive full screen.

typedef HRESULT(WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex **);

struct D9Ex {
   IDirect3D9Ex *d3d = nullptr;
   IDirect3DDevice9Ex *dev = nullptr;
   HWND hwnd = nullptr;
   void release()
   {
      ::release(dev);
      ::release(d3d);
      if (hwnd) DestroyWindow(hwnd);
      hwnd = nullptr;
   }
};

static bool createEx(D9Ex &d, std::string &detail, bool fullscreen = false)
{
   auto create = (PFN_Direct3DCreate9Ex)GetProcAddress(GetModuleHandleA("d3d9.dll"), "Direct3DCreate9Ex");
   HRESULT h = create ? create(D3D_SDK_VERSION, &d.d3d) : E_NOINTERFACE;
   if (FAILED(h)) {
      detail = "Direct3DCreate9Ex: " + hrs(h);
      return false;
   }
   D3DDISPLAYMODEEX mode = {sizeof mode};
   d.d3d->GetAdapterDisplayModeEx(D3DADAPTER_DEFAULT, &mode, nullptr);
   d.hwnd = createWindow("dxtest d3d9ex", fullscreen ? mode.Width : N9, fullscreen ? mode.Height : N9, true);
   D3DPRESENT_PARAMETERS pp = {};
   pp.BackBufferWidth = fullscreen ? mode.Width : N9;
   pp.BackBufferHeight = fullscreen ? mode.Height : N9;
   pp.BackBufferFormat = D3DFMT_X8R8G8B8;
   pp.BackBufferCount = 1;
   pp.SwapEffect = fullscreen ? D3DSWAPEFFECT_FLIP : D3DSWAPEFFECT_FLIPEX;
   if (!fullscreen) pp.BackBufferCount = 2;
   pp.hDeviceWindow = d.hwnd;
   pp.Windowed = !fullscreen;
   pp.FullScreen_RefreshRateInHz = fullscreen ? mode.RefreshRate : 0;
   pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
   h = d.d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d.hwnd,
                             D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED,
                             &pp, fullscreen ? &mode : nullptr, &d.dev);
   if (FAILED(h)) {
      detail = "CreateDeviceEx: " + hrs(h);
      return false;
   }
   return true;
}

// The top-left pixel of a render target or offscreen surface (through GetRenderTargetData or a lock)
static bool readPixel9(IDirect3DDevice9 *dev, IDirect3DSurface9 *s, int x, int y, uint32_t &argb, std::string &detail)
{
   D3DSURFACE_DESC sd;
   s->GetDesc(&sd);
   IDirect3DSurface9 *sys = nullptr;
   dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
   if (!sys) {
      detail = "no system memory surface";
      return false;
   }
   HRESULT h = dev->GetRenderTargetData(s, sys);
   bool ok = SUCCEEDED(h);
   if (ok) {
      D3DLOCKED_RECT lr;
      ok = SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
      if (ok) {
         argb = ((uint32_t *)((char *)lr.pBits + y * lr.Pitch))[x];
         sys->UnlockRect();
      }
   } else {
      detail = "GetRenderTargetData: " + hrs(h);
   }
   sys->Release();
   return ok;
}

// Fills a YUV surface with one color: Y 120, U 100, V 160 (BT.601 studio range: about RGB 172, 106, 65)
static bool fillYuv(IDirect3DSurface9 *s, D3DFORMAT f, int w, int h)
{
   D3DLOCKED_RECT lr;
   if (FAILED(s->LockRect(&lr, nullptr, 0))) return false;
   uint8_t *p = (uint8_t *)lr.pBits;
   if (f == (D3DFORMAT)MAKEFOURCC('N', 'V', '1', '2')) {
      for (int y = 0; y < h; y++) memset(p + y * lr.Pitch, 120, w);
      for (int y = 0; y < h / 2; y++)
         for (int x = 0; x < w; x += 2) {
            p[(h + y) * lr.Pitch + x] = 100;
            p[(h + y) * lr.Pitch + x + 1] = 160;
         }
   } else {
      bool uyvy = f == (D3DFORMAT)MAKEFOURCC('U', 'Y', 'V', 'Y');
      uint32_t v = uyvy ? (100u | 120u << 8 | 160u << 16 | 120u << 24) : (120u | 100u << 8 | 120u << 16 | 160u << 24);
      for (int y = 0; y < h; y++)
         for (int x = 0; x < w / 2; x++) ((uint32_t *)(p + y * lr.Pitch))[x] = v;
   }
   s->UnlockRect();
   return true;
}

static void groupD3D9Ex()
{
   D9Ex a, b;
   std::string detail;
   bool ok = createEx(a, detail);
   check(ok, "Direct3D 9Ex device (flip model, multithreaded)", detail);
   if (!ok) {
      report(Status::Skip, "The other tests of this group", "no Direct3D 9Ex device");
      a.release();
      return;
   }
   createEx(b, detail);

   if (startTest("9Ex: render target shared between two devices")) {
      detail.clear();
      HANDLE share = nullptr;
      IDirect3DSurface9 *rt = nullptr, *opened = nullptr;
      IDirect3DTexture9 *tex = nullptr;
      HRESULT h = a.dev->CreateTexture(N9, N9, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, &share);
      bool pass = false;
      if (SUCCEEDED(h) && share && b.dev) {
         tex->GetSurfaceLevel(0, &rt);
         D3DRECT r = {0, 0, N9 / 2, N9};
         a.dev->ColorFill(rt, nullptr, D3DCOLOR_ARGB(255, 0x30, 0xa0, 0x50));
         a.dev->ColorFill(rt, (RECT *)&r, D3DCOLOR_ARGB(255, 0xe0, 0x20, 0x40));
         // the 9Ex way to share: GPU work is done when the other device reads (flush, then query an event)
         IDirect3DQuery9 *q = nullptr;
         a.dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
         if (q) {
            q->Issue(D3DISSUE_END);
            while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) Sleep(1);
            q->Release();
         }
         IDirect3DTexture9 *t2 = nullptr;
         h = b.dev->CreateTexture(N9, N9, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t2, &share);
         if (t2) t2->GetSurfaceLevel(0, &opened);
         uint32_t left = 0, right = 0;
         if (opened && readPixel9(b.dev, opened, 4, 4, left, detail) && readPixel9(b.dev, opened, N9 - 4, 4, right, detail)) {
            pass = expect9(left, 0xe0, 0x20, 0x40, detail) && expect9(right, 0x30, 0xa0, 0x50, detail);
         } else if (detail.empty()) {
            detail = "opening on the second device: " + hrs(h);
         }
         release(t2);
      } else {
         detail = SUCCEEDED(h) ? "no shared handle" : "CreateTexture (shared): " + hrs(h);
      }
      release(opened);
      release(rt);
      release(tex);
      check(pass, "9Ex: render target shared between two devices", detail);
   }

   if (startTest("9Ex: surface shared with Direct3D 11 (OpenSharedResource)")) {
      detail.clear();
      HANDLE share = nullptr;
      IDirect3DTexture9 *tex = nullptr;
      HRESULT h = a.dev->CreateTexture(N9, N9, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, &share);
      bool pass = false;
      ID3D11Device *dev11 = nullptr;
      ID3D11DeviceContext *ctx11 = nullptr;
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev11, nullptr, &ctx11);
      if (SUCCEEDED(h) && share && dev11) {
         IDirect3DSurface9 *rt = nullptr;
         tex->GetSurfaceLevel(0, &rt);
         a.dev->ColorFill(rt, nullptr, D3DCOLOR_ARGB(255, 0x11, 0x77, 0xcc));
         IDirect3DQuery9 *q = nullptr;
         a.dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
         if (q) {
            q->Issue(D3DISSUE_END);
            while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) Sleep(1);
            q->Release();
         }
         ID3D11Texture2D *t11 = nullptr;
         h = dev11->OpenSharedResource(share, __uuidof(ID3D11Texture2D), (void **)&t11);
         if (t11) {
            D3D11_TEXTURE2D_DESC td;
            t11->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.MiscFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D *st = nullptr;
            dev11->CreateTexture2D(&td, nullptr, &st);
            if (st) {
               ctx11->CopyResource(st, t11);
               D3D11_MAPPED_SUBRESOURCE m;
               if (SUCCEEDED(ctx11->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                  pass = expect9(((uint32_t *)m.pData)[0], 0x11, 0x77, 0xcc, detail);
                  ctx11->Unmap(st, 0);
               }
               st->Release();
            }
            t11->Release();
         } else {
            detail = "OpenSharedResource: " + hrs(h);
         }
         release(rt);
      } else if (detail.empty()) {
         detail = !dev11 ? "no Direct3D 11 device" : "CreateTexture (shared): " + hrs(h);
      }
      release(ctx11);
      release(dev11);
      release(tex);
      check(pass, "9Ex: surface shared with Direct3D 11 (OpenSharedResource)", detail);
   }

   if (startTest("9Ex: residency, frame latency, vblank, PresentEx")) {
      detail.clear();
      IDirect3DTexture9 *tex = nullptr;
      a.dev->CreateTexture(N9, N9, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr);
      IDirect3DResource9 *res[1] = {tex};
      HRESULT res_h = tex ? a.dev->CheckResourceResidency(res, 1) : E_FAIL;
      HRESULT lat = a.dev->SetMaximumFrameLatency(2);
      UINT got = 0;
      a.dev->GetMaximumFrameLatency(&got);
      HRESULT vb = a.dev->WaitForVBlank(0);
      a.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff336699, 1, 0);
      HRESULT pr = a.dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
      HRESULT pr2 = a.dev->PresentEx(nullptr, nullptr, nullptr, nullptr, D3DPRESENT_DONOTWAIT);
      HRESULT st = a.dev->CheckDeviceState(a.hwnd);
      release(tex);
      bool pass = res_h == S_OK && lat == S_OK && got == 2 && SUCCEEDED(vb) && SUCCEEDED(pr) &&
                  (SUCCEEDED(pr2) || pr2 == D3DERR_WASSTILLDRAWING) && SUCCEEDED(st);
      check(pass, "9Ex: residency, frame latency, vblank, PresentEx",
            fmt("residency %s, latency %s (%u), vblank %s, present %s / %s, device state %s", hrs(res_h).c_str(),
                hrs(lat).c_str(), got, hrs(vb).c_str(), hrs(pr).c_str(), hrs(pr2).c_str(), hrs(st).c_str()));
   }

   if (startTest("GDI drawing on a Direct3D 9 surface (GetDC)")) {
      detail.clear();
      IDirect3DSurface9 *s = nullptr;
      a.dev->CreateRenderTarget(N9, N9, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE, &s, nullptr);
      bool pass = false;
      if (s) {
         a.dev->ColorFill(s, nullptr, D3DCOLOR_ARGB(255, 0x20, 0x20, 0xa0));
         HDC dc = nullptr;
         HRESULT h = s->GetDC(&dc);
         if (SUCCEEDED(h)) {
            HBRUSH br = CreateSolidBrush(RGB(0xf0, 0xc0, 0x10));
            RECT r = {N9 / 2, 0, N9, N9};
            FillRect(dc, &r, br);
            DeleteObject(br);
            s->ReleaseDC(dc);
            uint32_t left = 0, right = 0;
            pass = readPixel9(a.dev, s, 4, 4, left, detail) && readPixel9(a.dev, s, N9 - 4, 4, right, detail) &&
                   expect9(left, 0x20, 0x20, 0xa0, detail) && expect9(right, 0xf0, 0xc0, 0x10, detail);
         } else {
            detail = "GetDC: " + hrs(h);
         }
         s->Release();
      }
      check(pass, "GDI drawing on a Direct3D 9 surface (GetDC)", detail);
   }

   if (startTest("Vertex cache query (D3DQUERYTYPE_VCACHE)")) {
      IDirect3DQuery9 *q = nullptr;
      HRESULT h = a.dev->CreateQuery(D3DQUERYTYPE_VCACHE, &q);
      if (FAILED(h)) {
         optional(false, "Vertex cache query (D3DQUERYTYPE_VCACHE)", "not offered (optional)");
      } else {
         q->Issue(D3DISSUE_END);
         D3DDEVINFO_VCACHE vc = {};
         HRESULT g = S_FALSE;
         for (int i = 0; i < 1000 && (g = q->GetData(&vc, sizeof vc, D3DGETDATA_FLUSH)) == S_FALSE; i++) Sleep(1);
         check(g == S_OK && vc.Pattern == MAKEFOURCC('C', 'A', 'C', 'H'), "Vertex cache query (D3DQUERYTYPE_VCACHE)",
               fmt("GetData %s, pattern 0x%08lx, size %lu", hrs(g).c_str(), vc.Pattern, vc.CacheSize));
         q->Release();
      }
   }

   // DXVA2 video processing: a YUV surface (one color) scaled into an RGB render target, for each YUV format
   const D3DFORMAT yuvFormats[] = {(D3DFORMAT)MAKEFOURCC('Y', 'U', 'Y', '2'), (D3DFORMAT)MAKEFOURCC('U', 'Y', 'V', 'Y'),
                                   (D3DFORMAT)MAKEFOURCC('N', 'V', '1', '2')};
   const char *yuvNames[] = {"YUY2", "UYVY", "NV12"};
   for (int fi = 0; fi < 3; fi++) {
      D3DFORMAT yf = yuvFormats[fi];
      std::string name = fmt("DXVA2 video processing (%s to RGB, scaled)", yuvNames[fi]);
      if (!startTest(name.c_str())) continue;
      detail.clear();
      IDirectXVideoProcessorService *vps = nullptr;
      HRESULT h = DXVA2CreateVideoService(a.dev, __uuidof(IDirectXVideoProcessorService), (void **)&vps);
      bool pass = false;
      if (SUCCEEDED(h)) {
         DXVA2_VideoDesc vd = {};
         vd.SampleWidth = 32;
         vd.SampleHeight = 32;
         vd.Format = yf;
         vd.SampleFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
         vd.SampleFormat.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_MPEG2;
         vd.SampleFormat.NominalRange = DXVA2_NominalRange_16_235;
         vd.SampleFormat.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT601;
         vd.SampleFormat.VideoLighting = DXVA2_VideoLighting_dim;
         vd.SampleFormat.VideoPrimaries = DXVA2_VideoPrimaries_BT709;
         vd.SampleFormat.VideoTransferFunction = DXVA2_VideoTransFunc_709;
         vd.InputSampleFreq = {30, 1};
         vd.OutputFrameFreq = {30, 1};
         UINT count = 0;
         GUID *guids = nullptr;
         h = vps->GetVideoProcessorDeviceGuids(&vd, &count, &guids);
         IDirectXVideoProcessor *vp = nullptr;
         if (SUCCEEDED(h) && count) {
            h = vps->CreateVideoProcessor(guids[0], &vd, D3DFMT_X8R8G8B8, 0, &vp);
            CoTaskMemFree(guids);
         } else if (SUCCEEDED(h)) {
            h = E_FAIL;
            detail = fmt("no video processor for %s", yuvNames[fi]);
         }
         IDirect3DSurface9 *src = nullptr, *dst = nullptr;
         if (vp) {
            h = vps->CreateSurface(32, 32, 0, yf, D3DPOOL_DEFAULT, 0, DXVA2_VideoProcessorRenderTarget, &src, nullptr);
            a.dev->CreateRenderTarget(N9, N9, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &dst, nullptr);
         }
         if (src && dst && fillYuv(src, yf, 32, 32)) {
            DXVA2_VideoProcessBltParams bp = {};
            bp.TargetFrame = 0;
            bp.TargetRect = {0, 0, N9, N9};
            bp.BackgroundColor = {0x8000, 0x8000, 0x8000, 0xffff};
            bp.DestFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
            bp.DestFormat.NominalRange = DXVA2_NominalRange_0_255;
            bp.Alpha = DXVA2_Fixed32OpaqueAlpha();
            DXVA2_VideoSample sample = {};
            sample.Start = 0;
            sample.End = 333333;
            sample.SampleFormat = vd.SampleFormat;
            sample.SrcSurface = src;
            sample.SrcRect = {0, 0, 32, 32};
            sample.DstRect = {0, 0, N9, N9};
            sample.PlanarAlpha = DXVA2_Fixed32OpaqueAlpha();
            h = vp->VideoProcessBlt(dst, &bp, &sample, 1, nullptr);
            uint32_t px = 0;
            if (SUCCEEDED(h) && readPixel9(a.dev, dst, N9 / 2, N9 / 2, px, detail)) {
               pass = expect9(px, 172, 106, 65, detail, 24);
            } else if (detail.empty()) {
               detail = "VideoProcessBlt: " + hrs(h);
            }
         } else if (detail.empty()) {
            detail = "creating the video processor or its surfaces: " + hrs(h);
         }
         release(src);
         release(dst);
         release(vp);
         vps->Release();
      } else {
         detail = "DXVA2CreateVideoService (processor): " + hrs(h);
      }
      check(pass, name.c_str(), detail);
   }

   // StretchRect from a YUV offscreen surface to an RGB render target (players without DXVA2 do this)
   for (int fi = 0; fi < 3; fi += 2) {
      D3DFORMAT yf = yuvFormats[fi];
      std::string name = fmt("StretchRect %s to RGB (format conversion)", yuvNames[fi]);
      if (!startTest(name.c_str())) continue;
      detail.clear();
      bool pass = false;
      HRESULT h = a.d3d->CheckDeviceFormatConversion(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, yf, D3DFMT_X8R8G8B8);
      IDirect3DSurface9 *src = nullptr, *dst = nullptr;
      if (SUCCEEDED(h)) h = a.dev->CreateOffscreenPlainSurface(32, 32, yf, D3DPOOL_DEFAULT, &src, nullptr);
      if (src) a.dev->CreateRenderTarget(N9, N9, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &dst, nullptr);
      if (src && dst && fillYuv(src, yf, 32, 32)) {
         h = a.dev->StretchRect(src, nullptr, dst, nullptr, D3DTEXF_LINEAR);
         uint32_t px = 0;
         if (SUCCEEDED(h) && readPixel9(a.dev, dst, N9 / 2, N9 / 2, px, detail))
            pass = expect9(px, 172, 106, 65, detail, 24);
         else if (detail.empty())
            detail = "StretchRect: " + hrs(h);
      } else if (detail.empty()) {
         detail = "conversion check or surface: " + hrs(h);
      }
      release(src);
      release(dst);
      check(pass, name.c_str(), detail);
   }

   if (startTest("DXVA2 video decoding (decoder profiles offered)")) {
      IDirectXVideoDecoderService *vds = nullptr;
      HRESULT h = DXVA2CreateVideoService(a.dev, __uuidof(IDirectXVideoDecoderService), (void **)&vds);
      UINT count = 0;
      GUID *guids = nullptr;
      if (SUCCEEDED(h)) h = vds->GetDecoderDeviceGuids(&count, &guids);
      optional(SUCCEEDED(h) && count > 0, "DXVA2 video decoding (decoder profiles offered)",
               SUCCEEDED(h) ? fmt("%u profiles", count) : "no decoder service (optional): " + hrs(h));
      if (guids) CoTaskMemFree(guids);
      release(vds);
   }

   b.release();
   a.release();

   // Exclusive full screen at the current display mode (no mode change): what most Direct3D 9 games use
   if (startTest("Exclusive full screen (current display mode)")) {
      D9Ex f;
      detail.clear();
      bool created = createEx(f, detail, true);
      if (!created) {
         // Remote sessions have no exclusive full screen
         if (GetSystemMetrics(SM_REMOTESESSION)) optional(false, "Exclusive full screen (current display mode)", "remote session: " + detail);
         else check(false, "Exclusive full screen (current display mode)", detail);
      } else {
         bool pass = true;
         for (int i = 0; i < 10 && pass; i++) {
            f.dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0x10, 0xd0, 0x30), 1, 0);
            HRESULT h = f.dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
            if (FAILED(h)) {
               detail = fmt("frame %d: PresentEx %s", i, hrs(h).c_str());
               pass = false;
            }
         }
         IDirect3DSurface9 *bb = nullptr;
         f.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
         uint32_t px = 0;
         if (pass && bb) {
            f.dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0x10, 0xd0, 0x30), 1, 0);
            pass = readPixel9(f.dev, bb, 8, 8, px, detail) && expect9(px, 0x10, 0xd0, 0x30, detail);
         }
         release(bb);
         check(pass, "Exclusive full screen (current display mode)", detail);
      }
      f.release();
   }
}
