// Direct3D 9: a HAL device rendering into a 64x64 render target, read back and compared with the expected colors
// (there is no WARP for Direct3D 9). Fixed-function features, shader models 2 and 3, formats, queries, Direct3D 9Ex.

static const int N9 = 64;

struct D9 {
   IDirect3D9 *d3d = nullptr;
   IDirect3DDevice9 *dev = nullptr;
   HWND hwnd = nullptr;
   IDirect3DSurface9 *rt = nullptr, *ds = nullptr, *sys = nullptr;
   D3DCAPS9 caps = {};
   bool inScene = false, readValid = false;
   std::vector<uint32_t> pixels;
   IDirect3DVertexShader9 *vs3 = nullptr;   // vs_3_0 pass-through (ps_3_0 needs a vs_3_0)
};

struct V9 {
   float x, y, z;
   DWORD c;
   float u, v;
};
static const DWORD FVF9 = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

static void scene9(D9 &d)
{
   if (!d.inScene) {
      d.dev->BeginScene();
      d.inScene = true;
   }
   d.readValid = false;
}

static void endScene9(D9 &d)
{
   if (d.inScene) {
      d.dev->EndScene();
      d.inScene = false;
   }
}

static float f2dw_f;
static DWORD f2dw(float f)
{
   DWORD dw;
   memcpy(&dw, &f, 4);
   return dw;
}

// Every test starts from the same state: the render target cleared to blue, no blending, no depth, identity
// transforms, stage 0 passing the vertex color through
static void reset9(D9 &d, D3DCOLOR clear = D3DCOLOR_ARGB(255, 0, 0, 255))
{
   IDirect3DDevice9 *dev = d.dev;
   endScene9(d);
   dev->SetRenderTarget(0, d.rt);
   for (DWORD i = 1; i < 4; i++) dev->SetRenderTarget(i, nullptr);
   dev->SetDepthStencilSurface(d.ds);
   D3DVIEWPORT9 vp = {0, 0, N9, N9, 0.0f, 1.0f};
   dev->SetViewport(&vp);
   dev->SetVertexShader(nullptr);
   dev->SetPixelShader(nullptr);
   dev->SetFVF(FVF9);
   const DWORD rs[][2] = {
      {D3DRS_LIGHTING, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, TRUE},
      {D3DRS_ZFUNC, D3DCMP_LESSEQUAL}, {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
      {D3DRS_FOGENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
      {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_POINTSPRITEENABLE, FALSE},
      {D3DRS_COLORWRITEENABLE, 0xf}, {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_SPECULARENABLE, FALSE},
      {D3DRS_SHADEMODE, D3DSHADE_GOURAUD}, {D3DRS_MULTISAMPLEANTIALIAS, TRUE},
   };
   for (auto &s : rs) dev->SetRenderState((D3DRENDERSTATETYPE)s[0], s[1]);
   D3DMATRIX identity = {};
   identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
   dev->SetTransform(D3DTS_WORLD, &identity);
   dev->SetTransform(D3DTS_VIEW, &identity);
   dev->SetTransform(D3DTS_PROJECTION, &identity);
   for (DWORD s = 0; s < 4; s++) {
      dev->SetTexture(s, nullptr);
      dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
      dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, s);
   }
   dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
   dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
   dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
   dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
   dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
   dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
   dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
   dev->SetStreamSourceFreq(0, 1);
   dev->SetStreamSourceFreq(1, 1);
   dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, clear, 1.0f, 0);
   d.readValid = false;
}

static bool readSurface9(D9 &d, IDirect3DSurface9 *surface, std::vector<uint32_t> &out)
{
   endScene9(d);
   if (FAILED(d.dev->GetRenderTargetData(surface, d.sys))) return false;
   D3DLOCKED_RECT lr;
   if (FAILED(d.sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
   out.resize(N9 * N9);
   for (int y = 0; y < N9; y++) memcpy(&out[y * N9], (char *)lr.pBits + y * lr.Pitch, N9 * 4);
   d.sys->UnlockRect();
   return true;
}

static uint32_t px9(D9 &d, int x, int y)
{
   if (!d.readValid) {
      d.readValid = readSurface9(d, d.rt, d.pixels);
      if (!d.readValid) return 0xdeadbeef;
   }
   return d.pixels[y * N9 + x];
}

// Checks a pixel (ARGB) against r, g, b; describes a mismatch in detail
static bool expect9(uint32_t argb, int r, int g, int b, std::string &detail, int tolerance = 6, int x = -1, int y = -1)
{
   int pr = (argb >> 16) & 255, pg = (argb >> 8) & 255, pb = argb & 255;
   bool ok = near8(pr, r, tolerance) && near8(pg, g, tolerance) && near8(pb, b, tolerance);
   if (!ok && detail.size() < 300) {
      detail += fmt("%spixel%s (%d,%d,%d), expected (%d,%d,%d)", detail.empty() ? "" : "; ",
                    x >= 0 ? fmt(" %d,%d", x, y).c_str() : "", pr, pg, pb, r, g, b);
   }
   return ok;
}

static bool pix9(D9 &d, int x, int y, int r, int g, int b, std::string &detail, int tolerance = 6)
{
   return expect9(px9(d, x, y), r, g, b, detail, tolerance, x, y);
}

// A quad in clip space (identity transforms), u/v 0..1 from its top left
static void quad9(D9 &d, float x0, float y0, float x1, float y1, D3DCOLOR c, float z = 0.5f, D3DCOLOR c2 = 0)
{
   if (!c2) c2 = c;
   V9 v[4] = {{x0, y1, z, c, 0, 0}, {x1, y1, z, c2, 1, 0}, {x0, y0, z, c, 0, 1}, {x1, y0, z, c2, 1, 1}};
   scene9(d);
   d.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V9));
}

static void fullQuad9(D9 &d, D3DCOLOR c, float z = 0.5f) { quad9(d, -1, -1, 1, 1, c, z); }

static IDirect3DVertexShader9 *vs9(D9 &d, const char *src, const char *profile, std::string &detail)
{
   ID3DBlob *b = compileHlsl(src, profile, &detail);
   if (!b) return nullptr;
   IDirect3DVertexShader9 *s = nullptr;
   HRESULT h = d.dev->CreateVertexShader((const DWORD *)b->GetBufferPointer(), &s);
   if (FAILED(h)) detail = "CreateVertexShader: " + hrs(h);
   b->Release();
   return s;
}

static IDirect3DPixelShader9 *ps9(D9 &d, const char *src, const char *profile, std::string &detail)
{
   ID3DBlob *b = compileHlsl(src, profile, &detail);
   if (!b) return nullptr;
   IDirect3DPixelShader9 *s = nullptr;
   HRESULT h = d.dev->CreatePixelShader((const DWORD *)b->GetBufferPointer(), &s);
   if (FAILED(h)) detail = "CreatePixelShader: " + hrs(h);
   b->Release();
   return s;
}

static const char *VS3_PASS = R"(
struct O { float4 p : POSITION; float4 c : COLOR0; float2 t : TEXCOORD0; };
O main(float4 p : POSITION, float4 c : COLOR0, float2 t : TEXCOORD0) { O o; o.p = p; o.c = c; o.t = t; return o; }
)";

// Draws a full-screen quad with a ps_3_0 shader (and the pass-through vs_3_0)
static bool shaderQuad9(D9 &d, const char *ps, std::string &detail)
{
   IDirect3DPixelShader9 *p = ps9(d, ps, "ps_3_0", detail);
   if (!p || !d.vs3) {
      if (!d.vs3 && detail.empty()) detail = "no vs_3_0 pass-through shader";
      return false;
   }
   d.dev->SetVertexShader(d.vs3);
   d.dev->SetPixelShader(p);
   fullQuad9(d, D3DCOLOR_ARGB(255, 255, 255, 255));
   d.dev->SetVertexShader(nullptr);
   d.dev->SetPixelShader(nullptr);
   p->Release();
   return true;
}

static bool formatOk9(D9 &d, DWORD usage, D3DRESOURCETYPE type, D3DFORMAT format)
{
   return SUCCEEDED(d.d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, usage, type, format));
}

// A texture filled with one value per texel (or one block for compressed formats)
static IDirect3DTexture9 *texture9(D9 &d, D3DFORMAT format, UINT size, const void *texel, UINT texelBytes,
                                   bool blocks, std::string &detail, UINT levels = 1)
{
   IDirect3DTexture9 *t = nullptr;
   HRESULT h = d.dev->CreateTexture(size, size, levels, 0, format, D3DPOOL_MANAGED, &t, nullptr);
   if (FAILED(h)) {
      detail = "CreateTexture: " + hrs(h);
      return nullptr;
   }
   for (UINT level = 0; level < levels; level++) {
      D3DLOCKED_RECT lr;
      if (FAILED(t->LockRect(level, &lr, nullptr, 0))) continue;
      UINT s = std::max(1u, size >> level);
      UINT rows = blocks ? (s + 3) / 4 : s, cols = blocks ? (s + 3) / 4 : s;
      for (UINT y = 0; y < rows; y++) {
         for (UINT x = 0; x < cols; x++) memcpy((char *)lr.pBits + y * lr.Pitch + x * texelBytes, texel, texelBytes);
      }
      t->UnlockRect(level);
   }
   return t;
}

static void groupD3D9()
{
   D9 d;
   d.d3d = Direct3DCreate9(D3D_SDK_VERSION);
   check(d.d3d != nullptr, "Direct3DCreate9");
   if (!d.d3d) return;
   if (g_warp) report(Status::Info, "Mode", "Direct3D 9 has no WARP: these tests use the GPU even with --warp");

   D3DADAPTER_IDENTIFIER9 id = {};
   d.d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
   report(Status::Info, "Adapter", fmt("%s (%s, version %u.%u.%u.%u)", id.Description, id.Driver,
                                       HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart),
                                       HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart)));
   check(SUCCEEDED(d.d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &d.caps)), "Hardware (HAL) device type",
         "GetDeviceCaps(HAL)");

   d.hwnd = createWindow("dxtest d3d9", N9, N9, false);
   D3DPRESENT_PARAMETERS pp = {};
   pp.BackBufferWidth = pp.BackBufferHeight = N9;
   pp.BackBufferFormat = D3DFMT_X8R8G8B8;
   pp.BackBufferCount = 1;
   pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
   pp.hDeviceWindow = d.hwnd;
   pp.Windowed = TRUE;
   pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
   HRESULT h = d.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d.hwnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &d.dev);
   check(SUCCEEDED(h), "Create a device with hardware vertex processing", SUCCEEDED(h) ? "" : hrs(h));
   if (FAILED(h)) {
      report(Status::Skip, "The other Direct3D 9 tests", "no device");
      return;
   }
   d.dev->CreateRenderTarget(N9, N9, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &d.rt, nullptr);
   d.dev->CreateDepthStencilSurface(N9, N9, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &d.ds, nullptr);
   d.dev->CreateOffscreenPlainSurface(N9, N9, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &d.sys, nullptr);
   if (!d.rt || !d.ds || !d.sys) {
      check(false, "Render target, depth buffer and read-back surface");
      return;
   }
   {
      std::string e;
      d.vs3 = vs9(d, VS3_PASS, "vs_3_0", e);
   }

   // Caps
   check(d.caps.VertexShaderVersion >= D3DVS_VERSION(3, 0), "Vertex shader model 3.0",
         fmt("vs %lu.%lu", D3DSHADER_VERSION_MAJOR(d.caps.VertexShaderVersion), D3DSHADER_VERSION_MINOR(d.caps.VertexShaderVersion)));
   check(d.caps.PixelShaderVersion >= D3DPS_VERSION(3, 0), "Pixel shader model 3.0",
         fmt("ps %lu.%lu", D3DSHADER_VERSION_MAJOR(d.caps.PixelShaderVersion), D3DSHADER_VERSION_MINOR(d.caps.PixelShaderVersion)));
   check(d.caps.MaxTextureWidth >= 4096 && d.caps.MaxTextureHeight >= 4096, "Textures up to 4096x4096 or larger",
         fmt("max %lux%lu", d.caps.MaxTextureWidth, d.caps.MaxTextureHeight));
   check(!(d.caps.TextureCaps & D3DPTEXTURECAPS_POW2) || (d.caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL),
         "Non-power-of-two textures");
   check(d.caps.NumSimultaneousRTs >= 4, "Multiple render targets (4)", fmt("%lu", d.caps.NumSimultaneousRTs));
   optional(d.caps.MaxAnisotropy >= 16, "Anisotropic filtering 16x", fmt("max %lux", d.caps.MaxAnisotropy));
   check(d.caps.MaxStreams >= 8, "Vertex streams (8+)", fmt("%lu", d.caps.MaxStreams));

   std::string detail;
   auto test = [&](const char *name, const std::function<bool()> &fn) {
      if (!startTest(name)) return;
      detail.clear();
      reset9(d);
      bool ok = fn();
      check(ok, name, detail);
   };

   test("Clear", [&] {
      reset9(d, D3DCOLOR_ARGB(255, 0x20, 0x40, 0x80));
      return pix9(d, 5, 5, 0x20, 0x40, 0x80, detail) && pix9(d, 60, 60, 0x20, 0x40, 0x80, detail);
   });
   test("Pre-transformed vertices (XYZRHW)", [&] {
      struct TL { float x, y, z, rhw; DWORD c; } v[4] = {
         {-0.5f, -0.5f, 0.5f, 1, 0xffff0000}, {63.5f, -0.5f, 0.5f, 1, 0xffff0000},
         {-0.5f, 63.5f, 0.5f, 1, 0xffff0000}, {63.5f, 63.5f, 0.5f, 1, 0xffff0000}};
      d.dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
      scene9(d);
      d.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(TL));
      return pix9(d, 1, 1, 255, 0, 0, detail) && pix9(d, 62, 62, 255, 0, 0, detail);
   });
   test("Vertex and index buffers (DrawIndexedPrimitive)", [&] {
      IDirect3DVertexBuffer9 *vb = nullptr;
      IDirect3DIndexBuffer9 *ib = nullptr;
      d.dev->CreateVertexBuffer(4 * sizeof(V9), D3DUSAGE_WRITEONLY, FVF9, D3DPOOL_MANAGED, &vb, nullptr);
      d.dev->CreateIndexBuffer(6 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
      if (!vb || !ib) {
         detail = "buffer creation failed";
         return false;
      }
      V9 *v;
      vb->Lock(0, 0, (void **)&v, 0);
      V9 verts[4] = {{-1, 1, 0.5f, 0xff00ff00, 0, 0}, {1, 1, 0.5f, 0xff00ff00, 1, 0}, {-1, -1, 0.5f, 0xff00ff00, 0, 1},
                     {1, -1, 0.5f, 0xff00ff00, 1, 1}};
      memcpy(v, verts, sizeof verts);
      vb->Unlock();
      uint16_t *idx;
      ib->Lock(0, 0, (void **)&idx, 0);
      const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
      memcpy(idx, indices, sizeof indices);
      ib->Unlock();
      d.dev->SetStreamSource(0, vb, 0, sizeof(V9));
      d.dev->SetIndices(ib);
      scene9(d);
      d.dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
      bool ok = pix9(d, 10, 10, 0, 255, 0, detail) && pix9(d, 50, 50, 0, 255, 0, detail);
      d.dev->SetStreamSource(0, nullptr, 0, 0);
      d.dev->SetIndices(nullptr);
      vb->Release();
      ib->Release();
      return ok;
   });
   test("Gouraud shading (vertex color interpolation)", [&] {
      quad9(d, -1, -1, 1, 1, 0xffff0000, 0.5f, 0xff00ff00);
      return pix9(d, 32, 32, 128, 128, 0, detail, 12) && pix9(d, 1, 32, 252, 3, 0, detail, 12);
   });
   test("Flat shading", [&] {
      d.dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);
      quad9(d, -1, -1, 1, 1, 0xffff0000, 0.5f, 0xff00ff00);
      // flat: each triangle takes its first vertex's color
      uint32_t p = px9(d, 32, 32);
      int r = (p >> 16) & 255, g = (p >> 8) & 255;
      bool ok = (r == 255 && g == 0) || (r == 0 && g == 255);
      if (!ok) detail = fmt("pixel (%d,%d,%d) is a blend", r, g, (int)(p & 255));
      return ok;
   });
   test("Fixed-function transforms (world, view, projection)", [&] {
      D3DMATRIX proj = {};
      proj._11 = proj._22 = 0.5f;
      proj._33 = proj._44 = 1.0f;
      d.dev->SetTransform(D3DTS_PROJECTION, &proj);
      D3DMATRIX world = {};
      world._11 = world._22 = world._33 = world._44 = 1.0f;
      world._41 = 1.0f;   // moved right by 1 (half the screen after the projection)
      d.dev->SetTransform(D3DTS_WORLD, &world);
      fullQuad9(d, 0xffffff00);
      return pix9(d, 48, 32, 255, 255, 0, detail) && pix9(d, 20, 32, 0, 0, 255, detail);
   });
   test("Fixed-function lighting (directional light, material)", [&] {
      struct LV { float x, y, z, nx, ny, nz; } v[4] = {
         {-1, 1, 0.5f, 0, 0, -1}, {1, 1, 0.5f, 0, 0, -1}, {-1, -1, 0.5f, 0, 0, -1}, {1, -1, 0.5f, 0, 0, -1}};
      D3DLIGHT9 light = {};
      light.Type = D3DLIGHT_DIRECTIONAL;
      light.Diffuse = {1, 1, 1, 1};
      light.Direction = {0, 0, 1};
      D3DMATERIAL9 mat = {};
      mat.Diffuse = {1.0f, 0.5f, 0.0f, 1.0f};
      d.dev->SetLight(0, &light);
      d.dev->LightEnable(0, TRUE);
      d.dev->SetMaterial(&mat);
      d.dev->SetRenderState(D3DRS_LIGHTING, TRUE);
      d.dev->SetRenderState(D3DRS_AMBIENT, 0);
      d.dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
      d.dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL);
      scene9(d);
      d.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(LV));
      bool ok = pix9(d, 32, 32, 255, 128, 0, detail, 8);
      d.dev->LightEnable(0, FALSE);
      return ok;
   });
   test("Texture mapping (modulate with the vertex color)", [&] {
      IDirect3DTexture9 *t = nullptr;
      d.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr);
      if (!t) return false;
      D3DLOCKED_RECT lr;
      t->LockRect(0, &lr, nullptr, 0);
      for (int y = 0; y < 4; y++)
         for (int x = 0; x < 4; x++) ((uint32_t *)((char *)lr.pBits + y * lr.Pitch))[x] = x < 2 ? 0xffff0000 : 0xff00ff00;
      t->UnlockRect(0);
      d.dev->SetTexture(0, t);
      d.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      d.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      d.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      fullQuad9(d, 0xff808080);
      bool ok = pix9(d, 16, 32, 128, 0, 0, detail) && pix9(d, 48, 32, 0, 128, 0, detail);
      t->Release();
      return ok;
   });
   test("Bilinear filtering", [&] {
      uint32_t texels[2] = {0xff000000, 0xffffffff};
      IDirect3DTexture9 *t = nullptr;
      d.dev->CreateTexture(2, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr);
      if (!t) return false;
      D3DLOCKED_RECT lr;
      t->LockRect(0, &lr, nullptr, 0);
      memcpy(lr.pBits, texels, 8);
      t->UnlockRect(0);
      d.dev->SetTexture(0, t);
      d.dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
      d.dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      d.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      d.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      fullQuad9(d, 0xffffffff);
      bool ok = pix9(d, 32, 32, 128, 128, 128, detail, 12) && pix9(d, 2, 32, 0, 0, 0, detail, 12);
      t->Release();
      return ok;
   });
   test("Multitexturing (two texture stages)", [&] {
      uint32_t red = 0xffff0000, green = 0xff00ff00;
      IDirect3DTexture9 *a = texture9(d, D3DFMT_A8R8G8B8, 4, &red, 4, false, detail);
      IDirect3DTexture9 *b = texture9(d, D3DFMT_A8R8G8B8, 4, &green, 4, false, detail);
      if (!a || !b) return false;
      d.dev->SetTexture(0, a);
      d.dev->SetTexture(1, b);
      d.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      d.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      d.dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
      d.dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
      d.dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
      d.dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
      fullQuad9(d, 0xffffffff);
      bool ok = pix9(d, 32, 32, 255, 255, 0, detail);
      a->Release();
      b->Release();
      return ok;
   });

   // Texture formats: one texel value, sampled and compared
   struct Fmt9 {
      const char *name;
      D3DFORMAT format;
      bool required;
      uint8_t texel[16];
      UINT bytes;
      bool block;
      int r, g, b;   // expected; r < 0: alpha only (-r)
   };
   const uint16_t h025 = toHalf(0.25f), h05 = toHalf(0.5f), h075 = toHalf(0.75f), h1 = toHalf(1.0f);
   Fmt9 formats[] = {
      {"A8R8G8B8", D3DFMT_A8R8G8B8, true, {0xe0, 0x80, 0x20, 0xff}, 4, false, 0x20, 0x80, 0xe0},
      {"X8R8G8B8", D3DFMT_X8R8G8B8, true, {0x40, 0xc0, 0x10, 0x00}, 4, false, 0x10, 0xc0, 0x40},
      {"R5G6B5", D3DFMT_R5G6B5, true, {0x1f, 0xf8}, 2, false, 255, 0, 255},
      {"A1R5G5B5", D3DFMT_A1R5G5B5, true, {0xe0, 0x83}, 2, false, 0, 255, 0},
      {"A4R4G4B4", D3DFMT_A4R4G4B4, true, {0x0f, 0xf0}, 2, false, 0, 0, 255},
      {"L8 (luminance)", D3DFMT_L8, true, {0x80}, 1, false, 128, 128, 128},
      {"A8L8", D3DFMT_A8L8, true, {0x80, 0xff}, 2, false, 128, 128, 128},
      {"A8 (alpha only)", D3DFMT_A8, true, {0x80}, 1, false, -128, 0, 0},
      {"DXT1 (BC1)", D3DFMT_DXT1, true, {0xe0, 0x07, 0x00, 0x00, 0, 0, 0, 0}, 8, true, 0, 255, 0},
      {"DXT3 (BC2)", D3DFMT_DXT3, true, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x00, 0x00, 0, 0, 0, 0}, 16, true, 0, 0, 255},
      {"DXT5 (BC3)", D3DFMT_DXT5, true, {0xff, 0xff, 0, 0, 0, 0, 0, 0, 0x00, 0xf8, 0x00, 0x00, 0, 0, 0, 0}, 16, true, 255, 0, 0},
      {"A16B16G16R16F (half float)", D3DFMT_A16B16G16R16F, true,
       {(uint8_t)h025, (uint8_t)(h025 >> 8), (uint8_t)h05, (uint8_t)(h05 >> 8), (uint8_t)h075, (uint8_t)(h075 >> 8), (uint8_t)h1, (uint8_t)(h1 >> 8)},
       8, false, 64, 128, 191},
      {"R16F", D3DFMT_R16F, true, {(uint8_t)h05, (uint8_t)(h05 >> 8)}, 2, false, 128, 255, 255},
      {"R32F", D3DFMT_R32F, true, {0, 0, 0, 0x3f}, 4, false, 128, 255, 255},
      {"A32B32G32R32F", D3DFMT_A32B32G32R32F, true, {0, 0, 0x80, 0x3e, 0, 0, 0x40, 0x3f, 0, 0, 0, 0x3f, 0, 0, 0x80, 0x3f}, 16, false, 64, 191, 128},
      {"G16R16", D3DFMT_G16R16, false, {0x00, 0x80, 0xff, 0xff}, 4, false, 128, 255, 255},
      {"A2R10G10B10", D3DFMT_A2R10G10B10, false, {0x00, 0x00, 0xf0, 0xff}, 4, false, 255, 0, 0},
      {"V8U8 (bump map)", D3DFMT_V8U8, false, {0, 0}, 2, false, -1, 0, 0},
   };
   for (auto &f : formats) {
      std::string name = fmt("Texture format %s", f.name);
      if (!startTest(name)) continue;
      if (!formatOk9(d, 0, D3DRTYPE_TEXTURE, f.format)) {
         if (f.required) check(false, name, "not offered");
         else optional(false, name);
         continue;
      }
      if (f.r == -1) {   // offered; sampling it needs bump-mapping states: offered is enough
         check(true, name, "offered");
         continue;
      }
      detail.clear();
      reset9(d, D3DCOLOR_ARGB(255, 0, 0, 255));
      IDirect3DTexture9 *t = texture9(d, f.format, 8, f.texel, f.bytes, f.block, detail);
      bool ok = false;
      if (t) {
         d.dev->SetTexture(0, t);
         d.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
         d.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
         d.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
         d.dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
         fullQuad9(d, 0xffffffff);
         uint32_t p = px9(d, 32, 32);
         if (f.r < 0) {
            int a = p >> 24;
            ok = near8(a, -f.r, 4);
            if (!ok) detail = fmt("alpha %d, expected %d", a, -f.r);
         } else {
            ok = expect9(p, f.r, f.g, f.b, detail, 4);
         }
         t->Release();
      }
      check(ok, name, detail);
   }

   test("Mipmaps (sampled at a given level, ps_3_0 tex2Dlod)", [&] {
      IDirect3DTexture9 *t = nullptr;
      d.dev->CreateTexture(4, 4, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr);
      if (!t) return false;
      for (UINT level = 0; level < 2; level++) {
         D3DLOCKED_RECT lr;
         t->LockRect(level, &lr, nullptr, 0);
         for (UINT y = 0; y < (4u >> level); y++)
            for (UINT x = 0; x < (4u >> level); x++) ((uint32_t *)((char *)lr.pBits + y * lr.Pitch))[x] = level ? 0xff00ff00 : 0xffff0000;
         t->UnlockRect(level);
      }
      d.dev->SetTexture(0, t);
      d.dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
      bool ok = shaderQuad9(d, "sampler2D s : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR "
                               "{ return tex2Dlod(s, float4(uv, 0, 1)); }", detail) &&
                pix9(d, 32, 32, 0, 255, 0, detail);
      t->Release();
      return ok;
   });
   test("Automatic mipmap generation", [&] {
      HRESULT ch = d.d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_AUTOGENMIPMAP,
                                            D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
      if (ch != D3D_OK) {
         detail = "not offered for A8R8G8B8";
         return false;
      }
      IDirect3DTexture9 *t = nullptr;
      d.dev->CreateTexture(16, 16, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr);
      if (!t) return false;
      D3DLOCKED_RECT lr;
      t->LockRect(0, &lr, nullptr, 0);
      for (int y = 0; y < 16; y++)
         for (int x = 0; x < 16; x++) ((uint32_t *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xff0080ff;
      t->UnlockRect(0);
      t->GenerateMipSubLevels();
      d.dev->SetTexture(0, t);
      d.dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
      bool ok = shaderQuad9(d, "sampler2D s : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR "
                               "{ return tex2Dlod(s, float4(uv, 0, 3)); }", detail) &&
                pix9(d, 32, 32, 0, 128, 255, detail);
      t->Release();
      return ok;
   });
   test("Cube textures", [&] {
      IDirect3DCubeTexture9 *t = nullptr;
      d.dev->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr);
      if (!t) return false;
      const uint32_t colors[6] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xff00ffff, 0xffff00ff};
      for (int f = 0; f < 6; f++) {
         D3DLOCKED_RECT lr;
         t->LockRect((D3DCUBEMAP_FACES)f, 0, &lr, nullptr, 0);
         for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) ((uint32_t *)((char *)lr.pBits + y * lr.Pitch))[x] = colors[f];
         t->UnlockRect((D3DCUBEMAP_FACES)f, 0);
      }
      d.dev->SetTexture(0, t);
      // left half +X (face 0, red), right half -Y (face 3, yellow)
      bool ok = shaderQuad9(d, "samplerCUBE s : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR "
                               "{ return texCUBE(s, uv.x < 0.5 ? float3(1, 0, 0) : float3(0, -1, 0)); }", detail) &&
                pix9(d, 16, 32, 255, 0, 0, detail) && pix9(d, 48, 32, 255, 255, 0, detail);
      t->Release();
      return ok;
   });
   test("Volume (3D) textures", [&] {
      if (!(d.caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP)) {
         detail = "the device reports no volume textures (D3DPTEXTURECAPS_VOLUMEMAP)";
         return false;
      }
      IDirect3DVolumeTexture9 *t = nullptr;
      HRESULT ch = d.dev->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr);
      if (!t) {
         detail = "CreateVolumeTexture: " + hrs(ch);
         return false;
      }
      D3DLOCKED_BOX lb;
      ch = t->LockBox(0, &lb, nullptr, 0);
      if (FAILED(ch)) {
         detail = "LockBox: " + hrs(ch);
         t->Release();
         return false;
      }
      const uint32_t colors[4] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
      for (int z = 0; z < 4; z++)
         for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) ((uint32_t *)((char *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch))[x] = colors[z];
      t->UnlockBox(0);
      d.dev->SetTexture(0, t);
      bool ok = shaderQuad9(d, "sampler3D s : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR "
                               "{ return tex3D(s, float3(uv, 0.625)); }", detail) &&
                pix9(d, 32, 32, 0, 0, 255, detail);
      t->Release();
      return ok;
   });
   test("Alpha blending", [&] {
      d.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      d.dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      d.dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
      fullQuad9(d, D3DCOLOR_ARGB(128, 255, 0, 0));
      return pix9(d, 32, 32, 128, 0, 127, detail, 4);
   });
   test("Alpha test", [&] {
      d.dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
      d.dev->SetRenderState(D3DRS_ALPHAREF, 0x80);
      d.dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
      quad9(d, -1, -1, 0, 1, D3DCOLOR_ARGB(0x40, 255, 0, 0));
      quad9(d, 0, -1, 1, 1, D3DCOLOR_ARGB(0xc0, 255, 0, 0));
      return pix9(d, 16, 32, 0, 0, 255, detail) && pix9(d, 48, 32, 255, 0, 0, detail);
   });
   test("Fog (vertex fog factor)", [&] {
      struct FV { float x, y, z, rhw; DWORD c, spec; } v[4] = {
         {-0.5f, -0.5f, 0.5f, 1, 0xffff0000, 0x80000000}, {63.5f, -0.5f, 0.5f, 1, 0xffff0000, 0x80000000},
         {-0.5f, 63.5f, 0.5f, 1, 0xffff0000, 0x80000000}, {63.5f, 63.5f, 0.5f, 1, 0xffff0000, 0x80000000}};
      d.dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
      d.dev->SetRenderState(D3DRS_FOGCOLOR, 0xff00ff00);
      d.dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
      d.dev->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
      d.dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR);
      scene9(d);
      d.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(FV));
      return pix9(d, 32, 32, 128, 127, 0, detail, 6);
   });
   test("Depth buffer (D24S8)", [&] {
      d.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
      d.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
      fullQuad9(d, 0xffff0000, 0.3f);
      fullQuad9(d, 0xff00ff00, 0.7f);   // behind: hidden
      quad9(d, 0, -1, 1, 1, 0xffffff00, 0.2f);   // in front, right half
      return pix9(d, 16, 32, 255, 0, 0, detail) && pix9(d, 48, 32, 255, 255, 0, detail);
   });
   test("Stencil buffer", [&] {
      d.dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
      d.dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
      d.dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
      d.dev->SetRenderState(D3DRS_STENCILREF, 1);
      d.dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
      quad9(d, -1, -1, 0, 1, 0xffffffff);
      d.dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
      d.dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
      d.dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
      fullQuad9(d, 0xff00ff00);
      return pix9(d, 16, 32, 0, 255, 0, detail) && pix9(d, 48, 32, 0, 0, 255, detail);
   });
   test("Scissor test", [&] {
      RECT r = {0, 0, 32, 64};
      d.dev->SetScissorRect(&r);
      d.dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
      fullQuad9(d, 0xffff00ff);
      return pix9(d, 16, 32, 255, 0, 255, detail) && pix9(d, 48, 32, 0, 0, 255, detail);
   });
   test("User clip planes", [&] {
      float plane[4] = {-1, 0, 0, 0};   // keeps x <= 0
      d.dev->SetClipPlane(0, plane);
      d.dev->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
      fullQuad9(d, 0xff00ffff);
      return pix9(d, 16, 32, 0, 255, 255, detail) && pix9(d, 48, 32, 0, 0, 255, detail);
   });
   test("Color write mask", [&] {
      d.dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
      fullQuad9(d, 0xffffffff);
      return pix9(d, 32, 32, 255, 0, 255, detail);
   });
   test("Wireframe fill mode", [&] {
      d.dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
      quad9(d, -0.5f, -0.5f, 0.5f, 0.5f, 0xffffffff);
      int lit = 0;
      for (int i = 0; i < N9 * N9; i++) lit += (px9(d, i % N9, i / N9) & 0xffffff) == 0xffffff;
      bool ok = pix9(d, 26, 30, 0, 0, 255, detail) && lit > 40 && lit < 400;
      if (!(lit > 40 && lit < 400)) detail += fmt(" %d white pixels", lit);
      return ok;
   });
   test("Shader model 2.0 (vs_2_0 and ps_2_0, constants)", [&] {
      IDirect3DVertexShader9 *v = vs9(d, "float4 k : register(c0); struct O { float4 p : POSITION; float4 c : COLOR0; };"
                                         "O main(float4 p : POSITION) { O o; o.p = p; o.c = k; return o; }", "vs_2_0", detail);
      IDirect3DPixelShader9 *p = ps9(d, "float4 k : register(c1); float4 main(float4 c : COLOR0) : COLOR { return c * k; }",
                                     "ps_2_0", detail);
      if (!v || !p) return false;
      const float vk[4] = {1, 1, 0.5f, 1}, pk[4] = {0.5f, 1, 1, 1};
      d.dev->SetVertexShader(v);
      d.dev->SetPixelShader(p);
      d.dev->SetVertexShaderConstantF(0, vk, 1);
      d.dev->SetPixelShaderConstantF(1, pk, 1);
      fullQuad9(d, 0xffffffff);
      bool ok = pix9(d, 32, 32, 128, 255, 128, detail);
      d.dev->SetVertexShader(nullptr);
      d.dev->SetPixelShader(nullptr);
      v->Release();
      p->Release();
      return ok;
   });
   test("Shader model 3.0 (loops, branches, integer constants)", [&] {
      IDirect3DPixelShader9 *p = ps9(d, R"(
int n : register(i0);
float4 main(float2 uv : TEXCOORD0, float4 vpos : VPOS) : COLOR
{
   float s = 0;
   for (int i = 0; i < n; i++) s += 0.0625;
   if (vpos.x < 32) return float4(s, 0, 0, 1);
   return float4(0, s * 0.5, 1, 1);
})", "ps_3_0", detail);
      if (!p || !d.vs3) return false;
      const int ik[4] = {8, 0, 1, 0};
      d.dev->SetPixelShaderConstantI(0, ik, 1);
      d.dev->SetVertexShader(d.vs3);
      d.dev->SetPixelShader(p);
      fullQuad9(d, 0xffffffff);
      bool ok = pix9(d, 16, 32, 128, 0, 0, detail) && pix9(d, 48, 32, 0, 64, 255, detail);
      d.dev->SetVertexShader(nullptr);
      d.dev->SetPixelShader(nullptr);
      p->Release();
      return ok;
   });
   {
      std::string name = "Vertex texture fetch (vs_3_0 tex2Dlod)";
      if (!formatOk9(d, D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F)) {
         optional(false, name, "no vertex texture format (A32B32G32R32F)");
      } else {
         test(name.c_str(), [&] {
            const float texel[4] = {0.25f, 0.75f, 0.5f, 1.0f};
            IDirect3DTexture9 *t = texture9(d, D3DFMT_A32B32G32R32F, 4, texel, 16, false, detail);
            IDirect3DVertexShader9 *v = vs9(d, "sampler2D s : register(s0); struct O { float4 p : POSITION; float4 c : COLOR0; };"
                                               "O main(float4 p : POSITION) { O o; o.p = p; o.c = tex2Dlod(s, float4(0.5, 0.5, 0, 0)); return o; }",
                                            "vs_3_0", detail);
            IDirect3DPixelShader9 *p = ps9(d, "float4 main(float4 c : COLOR0) : COLOR { return c; }", "ps_3_0", detail);
            if (!t || !v || !p) return false;
            d.dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, t);
            d.dev->SetVertexShader(v);
            d.dev->SetPixelShader(p);
            fullQuad9(d, 0xffffffff);
            bool ok = pix9(d, 32, 32, 64, 191, 128, detail, 4);
            d.dev->SetVertexShader(nullptr);
            d.dev->SetPixelShader(nullptr);
            d.dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
            t->Release();
            v->Release();
            p->Release();
            return ok;
         });
      }
   }
   test("Multiple render targets (ps_3_0 COLOR0/COLOR1)", [&] {
      IDirect3DSurface9 *rt2 = nullptr;
      d.dev->CreateRenderTarget(N9, N9, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt2, nullptr);
      if (!rt2) return false;
      d.dev->SetRenderTarget(1, rt2);
      d.dev->ColorFill(rt2, nullptr, 0xff000000);
      bool ok = shaderQuad9(d, "struct O { float4 a : COLOR0; float4 b : COLOR1; };"
                               "O main(float2 uv : TEXCOORD0) { O o; o.a = float4(1, 0, 0, 1); o.b = float4(0, 1, 0, 1); return o; }",
                            detail);
      std::vector<uint32_t> second;
      ok = ok && pix9(d, 32, 32, 255, 0, 0, detail) && readSurface9(d, rt2, second) &&
           expect9(second[32 * N9 + 32], 0, 255, 0, detail);
      d.dev->SetRenderTarget(1, nullptr);
      rt2->Release();
      return ok;
   });
   test("Render to texture", [&] {
      IDirect3DTexture9 *t = nullptr;
      d.dev->CreateTexture(N9, N9, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr);
      if (!t) return false;
      IDirect3DSurface9 *s;
      t->GetSurfaceLevel(0, &s);
      d.dev->SetRenderTarget(0, s);
      fullQuad9(d, 0xffffff00);
      d.dev->SetRenderTarget(0, d.rt);
      d.dev->SetTexture(0, t);
      d.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      d.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      fullQuad9(d, 0xff000000);
      bool ok = pix9(d, 32, 32, 255, 255, 0, detail);
      d.dev->SetTexture(0, nullptr);
      s->Release();
      t->Release();
      return ok;
   });
   test("StretchRect and ColorFill", [&] {
      IDirect3DSurface9 *small = nullptr;
      d.dev->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &small, nullptr);
      if (!small) return false;
      endScene9(d);
      d.dev->ColorFill(small, nullptr, 0xff00ffff);
      RECT left = {0, 0, 32, 64}, right = {32, 0, 64, 64};
      HRESULT a = d.dev->StretchRect(small, nullptr, d.rt, &left, D3DTEXF_LINEAR);
      HRESULT b = d.dev->ColorFill(d.rt, &right, 0xffff00ff);
      d.readValid = false;
      bool ok = SUCCEEDED(a) && SUCCEEDED(b) && pix9(d, 16, 32, 0, 255, 255, detail) && pix9(d, 48, 32, 255, 0, 255, detail);
      small->Release();
      return ok;
   });
   test("Multisampling 4x (render, resolve with StretchRect)", [&] {
      DWORD q = 0;
      if (FAILED(d.d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE,
                                                   D3DMULTISAMPLE_4_SAMPLES, &q))) {
         detail = "4x not offered";
         return false;
      }
      IDirect3DSurface9 *ms = nullptr;
      d.dev->CreateRenderTarget(N9, N9, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &ms, nullptr);
      if (!ms) return false;
      d.dev->SetRenderTarget(0, ms);
      d.dev->SetDepthStencilSurface(nullptr);
      d.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
      V9 v[3] = {{-1, -1, 0.5f, 0xffff0000, 0, 0}, {1, -1, 0.5f, 0xffff0000, 0, 0}, {-1, 1, 0.5f, 0xffff0000, 0, 0}};
      scene9(d);
      d.dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(V9));
      endScene9(d);
      HRESULT h = d.dev->StretchRect(ms, nullptr, d.rt, nullptr, D3DTEXF_NONE);
      d.readValid = false;
      int partial = 0;
      for (int i = 0; i < N9 * N9; i++) {
         int r = (px9(d, i % N9, i / N9) >> 16) & 255;
         partial += r > 30 && r < 225;
      }
      bool ok = SUCCEEDED(h) && pix9(d, 8, 56, 255, 0, 0, detail) && pix9(d, 56, 8, 0, 0, 0, detail) && partial >= 20;
      if (partial < 20) detail += fmt(" only %d anti-aliased edge pixels", partial);
      d.dev->SetRenderTarget(0, d.rt);
      d.dev->SetDepthStencilSurface(d.ds);
      ms->Release();
      return ok;
   });
   test("Hardware instancing (SetStreamSourceFreq)", [&] {
      D3DVERTEXELEMENT9 el[] = {
         {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
         {1, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
         {1, 8, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
         D3DDECL_END()};
      IDirect3DVertexDeclaration9 *decl = nullptr;
      d.dev->CreateVertexDeclaration(el, &decl);
      IDirect3DVertexShader9 *v = vs9(d, "struct O { float4 p : POSITION; float4 c : COLOR0; float2 t : TEXCOORD0; };"
                                         "O main(float2 p : POSITION, float2 off : TEXCOORD1, float4 c : COLOR0)"
                                         "{ O o; o.p = float4(p * 0.5 + off, 0.5, 1); o.c = c; o.t = 0; return o; }",
                                      "vs_3_0", detail);
      IDirect3DPixelShader9 *p = ps9(d, "float4 main(float4 c : COLOR0) : COLOR { return c; }", "ps_3_0", detail);
      IDirect3DVertexBuffer9 *vb0 = nullptr, *vb1 = nullptr;
      IDirect3DIndexBuffer9 *ib = nullptr;
      d.dev->CreateVertexBuffer(32, 0, 0, D3DPOOL_MANAGED, &vb0, nullptr);
      d.dev->CreateVertexBuffer(24, 0, 0, D3DPOOL_MANAGED, &vb1, nullptr);
      d.dev->CreateIndexBuffer(12, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
      if (!decl || !v || !p || !vb0 || !vb1 || !ib) return false;
      void *ptr;
      const float corners[8] = {-1, 1, 1, 1, -1, -1, 1, -1};
      vb0->Lock(0, 0, &ptr, 0);
      memcpy(ptr, corners, sizeof corners);
      vb0->Unlock();
      struct Inst { float x, y; DWORD c; } inst[2] = {{-0.5f, 0, 0xffff0000}, {0.5f, 0, 0xff00ff00}};
      vb1->Lock(0, 0, &ptr, 0);
      memcpy(ptr, inst, sizeof inst);
      vb1->Unlock();
      const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
      ib->Lock(0, 0, &ptr, 0);
      memcpy(ptr, idx, sizeof idx);
      ib->Unlock();
      d.dev->SetVertexDeclaration(decl);
      d.dev->SetVertexShader(v);
      d.dev->SetPixelShader(p);
      d.dev->SetStreamSource(0, vb0, 0, 8);
      d.dev->SetStreamSource(1, vb1, 0, 12);
      d.dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 2);
      d.dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
      d.dev->SetIndices(ib);
      scene9(d);
      d.dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
      bool ok = pix9(d, 16, 32, 255, 0, 0, detail) && pix9(d, 48, 32, 0, 255, 0, detail) && pix9(d, 16, 4, 0, 0, 255, detail);
      d.dev->SetStreamSourceFreq(0, 1);
      d.dev->SetStreamSourceFreq(1, 1);
      d.dev->SetStreamSource(0, nullptr, 0, 0);
      d.dev->SetStreamSource(1, nullptr, 0, 0);
      d.dev->SetIndices(nullptr);
      d.dev->SetVertexShader(nullptr);
      d.dev->SetPixelShader(nullptr);
      d.dev->SetVertexDeclaration(nullptr);
      decl->Release();
      v->Release();
      p->Release();
      vb0->Release();
      vb1->Release();
      ib->Release();
      return ok;
   });
   test("Dynamic vertex buffers (Lock DISCARD and NOOVERWRITE)", [&] {
      IDirect3DVertexBuffer9 *vb = nullptr;
      d.dev->CreateVertexBuffer(8 * sizeof(V9), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF9, D3DPOOL_DEFAULT, &vb, nullptr);
      if (!vb) return false;
      V9 left[4] = {{-1, 1, 0.5f, 0xffff0000, 0, 0}, {0, 1, 0.5f, 0xffff0000, 0, 0}, {-1, -1, 0.5f, 0xffff0000, 0, 0}, {0, -1, 0.5f, 0xffff0000, 0, 0}};
      V9 right[4] = {{0, 1, 0.5f, 0xff00ff00, 0, 0}, {1, 1, 0.5f, 0xff00ff00, 0, 0}, {0, -1, 0.5f, 0xff00ff00, 0, 0}, {1, -1, 0.5f, 0xff00ff00, 0, 0}};
      void *ptr;
      d.dev->SetStreamSource(0, vb, 0, sizeof(V9));
      vb->Lock(0, sizeof left, &ptr, D3DLOCK_DISCARD);
      memcpy(ptr, left, sizeof left);
      vb->Unlock();
      scene9(d);
      d.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
      vb->Lock(sizeof left, sizeof right, &ptr, D3DLOCK_NOOVERWRITE);
      memcpy(ptr, right, sizeof right);
      vb->Unlock();
      d.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 4, 2);
      bool ok = pix9(d, 16, 32, 255, 0, 0, detail) && pix9(d, 48, 32, 0, 255, 0, detail);
      d.dev->SetStreamSource(0, nullptr, 0, 0);
      vb->Release();
      return ok;
   });
   test("Occlusion query (pixel count)", [&] {
      IDirect3DQuery9 *q = nullptr;
      if (FAILED(d.dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q))) {
         detail = "not offered";
         return false;
      }
      scene9(d);
      q->Issue(D3DISSUE_BEGIN);
      quad9(d, -1, -1, 0, 1, 0xffffffff);
      q->Issue(D3DISSUE_END);
      endScene9(d);
      DWORD count = 0;
      HRESULT h = S_FALSE;
      for (int i = 0; i < 2000 && (h = q->GetData(&count, sizeof count, D3DGETDATA_FLUSH)) == S_FALSE; i++) Sleep(1);
      q->Release();
      detail = h == S_OK ? fmt("%lu pixels, expected %d", count, N9 * N9 / 2) : h == S_FALSE ? "no result in 2 s" : "GetData: " + hrs(h);
      return h == S_OK && count == (DWORD)(N9 * N9 / 2);
   });
   test("Event query", [&] {
      IDirect3DQuery9 *q = nullptr;
      if (FAILED(d.dev->CreateQuery(D3DQUERYTYPE_EVENT, &q))) return false;
      fullQuad9(d, 0xffffffff);
      endScene9(d);
      q->Issue(D3DISSUE_END);
      BOOL done = FALSE;
      HRESULT h = S_FALSE;
      for (int i = 0; i < 2000 && (h = q->GetData(&done, sizeof done, D3DGETDATA_FLUSH)) == S_FALSE; i++) Sleep(1);
      q->Release();
      if (h != S_OK) detail = h == S_FALSE ? "never signaled (2 s)" : "GetData: " + hrs(h);
      return h == S_OK;
   });
   {
      IDirect3DQuery9 *q = nullptr, *freq = nullptr;
      bool offered = startTest("Timestamp queries") && SUCCEEDED(d.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q)) &&
                     SUCCEEDED(d.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freq));
      if (!offered) {
         optional(false, "Timestamp queries");
      } else {
         reset9(d);
         q->Issue(D3DISSUE_END);
         freq->Issue(D3DISSUE_END);
         UINT64 t = 0, f = 0;
         HRESULT a = S_FALSE, b = S_FALSE;
         for (int i = 0; i < 2000 && (a = q->GetData(&t, sizeof t, D3DGETDATA_FLUSH)) == S_FALSE; i++) Sleep(1);
         for (int i = 0; i < 2000 && (b = freq->GetData(&f, sizeof f, D3DGETDATA_FLUSH)) == S_FALSE; i++) Sleep(1);
         check(a == S_OK && b == S_OK && f > 0, "Timestamp queries", fmt("frequency %llu Hz", (unsigned long long)f));
      }
      release(q);
      release(freq);
   }
   if (!formatOk9(d, D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)) {
      check(false, "sRGB render target writes", "not offered");
   } else {
      test("sRGB render target writes", [&] {
         d.dev->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE);
         fullQuad9(d, 0xff808080);
         return pix9(d, 32, 32, 188, 188, 188, detail, 3);
      });
   }
   if (!formatOk9(d, D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)) {
      check(false, "sRGB texture reads", "not offered");
   } else {
      test("sRGB texture reads", [&] {
         uint32_t texel = 0xffbcbcbc;
         IDirect3DTexture9 *t = texture9(d, D3DFMT_A8R8G8B8, 4, &texel, 4, false, detail);
         if (!t) return false;
         d.dev->SetTexture(0, t);
         d.dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE);
         d.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
         d.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
         fullQuad9(d, 0xffffffff);
         bool ok = pix9(d, 32, 32, 128, 128, 128, detail, 3);
         t->Release();
         return ok;
      });
   }
   test("Point sprites", [&] {
      if (d.caps.MaxPointSize < 16.0f) {
         detail = fmt("max point size %.0f", d.caps.MaxPointSize);
         return false;
      }
      struct PV { float x, y, z; DWORD c; } v = {0, 0, 0.5f, 0xffffff00};
      d.dev->SetRenderState(D3DRS_POINTSPRITEENABLE, TRUE);
      d.dev->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);
      d.dev->SetRenderState(D3DRS_POINTSIZE, f2dw(16.0f));
      d.dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
      scene9(d);
      d.dev->DrawPrimitiveUP(D3DPT_POINTLIST, 1, &v, sizeof v);
      return pix9(d, 32, 32, 255, 255, 0, detail) && pix9(d, 38, 32, 255, 255, 0, detail) &&
             pix9(d, 46, 32, 0, 0, 255, detail);
   });
   test("State blocks", [&] {
      IDirect3DStateBlock9 *sb = nullptr;
      d.dev->BeginStateBlock();
      d.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff00ff00);
      d.dev->EndStateBlock(&sb);
      if (!sb) return false;
      d.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xffff0000);
      sb->Apply();
      d.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      d.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
      fullQuad9(d, 0xffffffff);
      sb->Release();
      return pix9(d, 32, 32, 0, 255, 0, detail);
   });
   {
      // Formats games ask for: optional, reported as warnings when missing
      optional(formatOk9(d, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z')),
               "Depth textures (INTZ, read depth in shaders)");
      optional(formatOk9(d, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D24S8), "Hardware shadow maps (D24S8 depth textures)");
      optional(formatOk9(d, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, (D3DFORMAT)MAKEFOURCC('N', 'U', 'L', 'L')),
               "NULL render targets (depth-only passes)");
      optional(formatOk9(d, 0, D3DRTYPE_TEXTURE, (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2')), "ATI2/3Dc normal map compression");
      optional(formatOk9(d, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F) &&
               formatOk9(d, D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING | D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F),
               "HDR render targets with blending (A16B16G16R16F)");
   }

   // Present to a window, then the Reset of a device
   if (!interactiveDesktop()) {
      report(Status::Skip, "Present to a window", "no interactive desktop");
   } else if (startTest("Present to a window")) {
      HWND w = createWindow("dxtest d3d9 present", N9, N9, true);
      D3DPRESENT_PARAMETERS wp = pp;
      wp.hDeviceWindow = w;
      IDirect3DDevice9 *pd = nullptr;
      HRESULT ph = d.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &wp, &pd);
      COLORREF c = 0;
      bool ok = false;
      if (pd) {
         for (int i = 0; i < 10; i++) {
            pd->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff00c040, 1.0f, 0);
            pd->Present(nullptr, nullptr, nullptr, nullptr);
            pumpMessages();
            Sleep(30);
         }
         Sleep(300);
         ok = windowPixel(w, N9 / 2, N9 / 2, c) && near8(GetRValue(c), 0, 8) && near8(GetGValue(c), 0xc0, 8) &&
              near8(GetBValue(c), 0x40, 8);
         pd->Release();
      }
      check(ok, "Present to a window", pd ? fmt("window shows (%d,%d,%d), expected (0,192,64)", GetRValue(c), GetGValue(c), GetBValue(c))
                                          : "CreateDevice: " + hrs(ph));
      DestroyWindow(w);
   }
   if (startTest("Direct3D 9Ex device (Vista and later games, DXVK-style presenters)")) {
      typedef HRESULT(WINAPI * PFN_Create9Ex)(UINT, IDirect3D9Ex **);
      auto create9Ex = (PFN_Create9Ex)GetProcAddress(GetModuleHandleA("d3d9.dll"), "Direct3DCreate9Ex");
      IDirect3D9Ex *ex = nullptr;
      IDirect3DDevice9Ex *devEx = nullptr;
      HRESULT eh = create9Ex ? create9Ex(D3D_SDK_VERSION, &ex) : E_NOINTERFACE;
      if (ex) eh = ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d.hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, nullptr, &devEx);
      bool ok = false;
      std::string det = SUCCEEDED(eh) ? "" : hrs(eh);
      if (devEx) {
         IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
         devEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
         devEx->CreateOffscreenPlainSurface(N9, N9, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
         devEx->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff8040c0, 1.0f, 0);
         if (bb && sys && SUCCEEDED(devEx->GetRenderTargetData(bb, sys))) {
            D3DLOCKED_RECT lr;
            sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            ok = expect9(((uint32_t *)lr.pBits)[0], 0x80, 0x40, 0xc0, det);
            sys->UnlockRect();
         }
         release(bb);
         release(sys);
         devEx->Release();
      }
      check(ok, "Direct3D 9Ex device (Vista and later games, DXVK-style presenters)", det);
      release(ex);
   }
   test("Device reset", [&] {
      endScene9(d);
      release(d.rt);
      release(d.ds);
      release(d.sys);
      release(d.vs3);
      D3DPRESENT_PARAMETERS rp = pp;
      rp.BackBufferWidth = rp.BackBufferHeight = 128;
      HRESULT h = d.dev->Reset(&rp);
      detail = SUCCEEDED(h) ? "" : hrs(h);
      // keep the rest of the group from touching released surfaces
      d.dev->CreateRenderTarget(N9, N9, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &d.rt, nullptr);
      d.dev->CreateDepthStencilSurface(N9, N9, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &d.ds, nullptr);
      d.dev->CreateOffscreenPlainSurface(N9, N9, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &d.sys, nullptr);
      return SUCCEEDED(h);
   });
}
