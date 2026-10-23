// Direct3D 10 and 10.1: devices through both runtimes (d3d10.dll, d3d10_1.dll), shader models 4.0 and 4.1, geometry
// shaders and stream output, texture arrays, MSAA, queries. 64x64 RGBA8 target, compared with the expected colors.

static const int N10 = 64;

typedef HRESULT(WINAPI *PFN_D3D10CreateDevice1)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT,
                                               D3D10_FEATURE_LEVEL1, UINT, ID3D10Device1 **);

struct D10 {
   ID3D10Device1 *dev = nullptr;
   ID3D10Texture2D *rt = nullptr, *staging = nullptr;
   ID3D10RenderTargetView *rtv = nullptr;
   ID3D10VertexShader *fullVs = nullptr;
   std::vector<uint32_t> pixels;
};

static const char *FULL_VS10 = R"(
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V main(uint id : SV_VertexID)
{
   V v;
   v.uv = float2((id << 1) & 2, id & 2);
   v.pos = float4(v.uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
   return v;
}
)";

template <typename T, typename F> static T *create10(const char *src, const char *profile, std::string &detail, F create)
{
   ID3DBlob *b = compileHlsl(src, profile, &detail);
   if (!b) return nullptr;
   T *s = nullptr;
   HRESULT h = create(b, &s);
   if (FAILED(h)) detail = fmt("shader creation: %s", hrs(h).c_str());
   b->Release();
   return s;
}

static ID3D10VertexShader *vs10(D10 &d, const char *src, std::string &detail, const char *profile = "vs_4_0")
{
   return create10<ID3D10VertexShader>(src, profile, detail, [&](ID3DBlob *b, ID3D10VertexShader **s) {
      return d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), s);
   });
}

static ID3D10PixelShader *ps10(D10 &d, const char *src, std::string &detail, const char *profile = "ps_4_0")
{
   return create10<ID3D10PixelShader>(src, profile, detail, [&](ID3DBlob *b, ID3D10PixelShader **s) {
      return d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), s);
   });
}

static void reset10(D10 &d, float r = 0, float g = 0, float b = 1)
{
   const float c[4] = {r, g, b, 1};
   d.dev->ClearState();
   d.dev->ClearRenderTargetView(d.rtv, c);
   d.dev->OMSetRenderTargets(1, &d.rtv, nullptr);
   D3D10_VIEWPORT vp = {0, 0, N10, N10, 0, 1};
   d.dev->RSSetViewports(1, &vp);
   d.dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   d.pixels.clear();
}

static bool read10(D10 &d, ID3D10Texture2D *tex, std::vector<uint32_t> &out)
{
   d.dev->CopyResource(d.staging, tex);
   D3D10_MAPPED_TEXTURE2D m;
   if (FAILED(d.staging->Map(0, D3D10_MAP_READ, 0, &m))) return false;
   out.resize(N10 * N10);
   for (int y = 0; y < N10; y++) memcpy(&out[y * N10], (char *)m.pData + y * m.RowPitch, N10 * 4);
   d.staging->Unmap(0);
   return true;
}

// RGBA8 pixel (R in the low byte) against r, g, b
static bool expectRgba(uint32_t p, int r, int g, int b, std::string &detail, int tolerance = 3, int x = -1, int y = -1)
{
   int pr = p & 255, pg = (p >> 8) & 255, pb = (p >> 16) & 255;
   bool ok = near8(pr, r, tolerance) && near8(pg, g, tolerance) && near8(pb, b, tolerance);
   if (!ok && detail.size() < 300) {
      detail += fmt("%spixel%s (%d,%d,%d), expected (%d,%d,%d)", detail.empty() ? "" : "; ",
                    x >= 0 ? fmt(" %d,%d", x, y).c_str() : "", pr, pg, pb, r, g, b);
   }
   return ok;
}

static bool pix10(D10 &d, int x, int y, int r, int g, int b, std::string &detail, int tolerance = 3)
{
   if (d.pixels.empty() && !read10(d, d.rt, d.pixels)) {
      detail = "read-back failed";
      return false;
   }
   return expectRgba(d.pixels[y * N10 + x], r, g, b, detail, tolerance, x, y);
}

static ID3D10Buffer *buffer10(D10 &d, UINT bind, const void *data, UINT size, D3D10_USAGE usage = D3D10_USAGE_DEFAULT)
{
   D3D10_BUFFER_DESC bd = {size, usage, bind, 0, 0};
   D3D10_SUBRESOURCE_DATA init = {data, 0, 0};
   ID3D10Buffer *b = nullptr;
   d.dev->CreateBuffer(&bd, data ? &init : nullptr, &b);
   return b;
}

static ID3D10Texture2D *texture10(D10 &d, UINT w, UINT h, UINT arraySize, DXGI_FORMAT format, const void *data,
                                  UINT pitch, UINT bind = D3D10_BIND_SHADER_RESOURCE, UINT samples = 1, UINT misc = 0)
{
   D3D10_TEXTURE2D_DESC td = {w, h, 1, arraySize, format, {samples, 0}, D3D10_USAGE_DEFAULT, bind, 0, misc};
   std::vector<D3D10_SUBRESOURCE_DATA> init(arraySize);
   for (UINT i = 0; i < arraySize; i++) init[i] = {(const char *)data + i * pitch * h, pitch, pitch * h};
   ID3D10Texture2D *t = nullptr;
   d.dev->CreateTexture2D(&td, data ? init.data() : nullptr, &t);
   return t;
}

static void groupD3D10()
{
   D3D10_DRIVER_TYPE type = g_warp ? D3D10_DRIVER_TYPE_WARP : D3D10_DRIVER_TYPE_HARDWARE;
   {
      ID3D10Device *d10 = nullptr;
      HRESULT h = D3D10CreateDevice(nullptr, type, nullptr, 0, D3D10_SDK_VERSION, &d10);
      if (g_warp && FAILED(h)) report(Status::Skip, "Create a Direct3D 10.0 device (d3d10.dll)", "no WARP for 10.0");
      else check(SUCCEEDED(h), "Create a Direct3D 10.0 device (d3d10.dll)", SUCCEEDED(h) ? "" : hrs(h));
      release(d10);
   }
   auto create1 = (PFN_D3D10CreateDevice1)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1");
   if (!create1) {
      check(false, "Direct3D 10.1 runtime (d3d10_1.dll)");
      return;
   }
   D10 d;
   for (D3D10_FEATURE_LEVEL1 level : {D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_10_1}) {
      ID3D10Device1 *dev = nullptr;
      HRESULT h = create1(nullptr, type, nullptr, 0, level, D3D10_1_SDK_VERSION, &dev);
      checkAtLevel((D3D_FEATURE_LEVEL)level, SUCCEEDED(h),
                   fmt("Create a Direct3D 10.1 device at feature level %s", level == D3D10_FEATURE_LEVEL_10_1 ? "10_1" : "10_0"),
                   SUCCEEDED(h) ? "" : hrs(h));
      if (dev) {
         release(d.dev);
         d.dev = dev;
      }
   }
   if (!d.dev) {
      report(Status::Skip, "The other Direct3D 10 tests", "no device");
      return;
   }
   bool is101 = d.dev->GetFeatureLevel() == D3D10_FEATURE_LEVEL_10_1;
   d.rt = texture10(d, N10, N10, 1, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0, D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE);
   D3D10_TEXTURE2D_DESC sd = {N10, N10, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_STAGING, 0, D3D10_CPU_ACCESS_READ, 0};
   d.dev->CreateTexture2D(&sd, nullptr, &d.staging);
   if (!d.rt || !d.staging || FAILED(d.dev->CreateRenderTargetView(d.rt, nullptr, &d.rtv))) {
      check(false, "Render target and read-back texture");
      return;
   }
   std::string detail;
   d.fullVs = vs10(d, FULL_VS10, detail);

   auto test = [&](const char *name, const std::function<bool()> &fn) {
      if (!startTest(name)) return;
      detail.clear();
      reset10(d);
      check(fn(), name, detail);
   };
   auto fullDraw = [&](ID3D10PixelShader *ps) {
      d.dev->VSSetShader(d.fullVs);
      d.dev->PSSetShader(ps);
      d.dev->Draw(3, 0);
   };

   test("Shaders (shader model 4.0) and constant buffers", [&] {
      ID3D10PixelShader *ps = ps10(d, "cbuffer C { float4 color; }; float4 main() : SV_Target { return color; }", detail);
      const float color[4] = {1, 0.5f, 0.25f, 1};
      ID3D10Buffer *cb = buffer10(d, D3D10_BIND_CONSTANT_BUFFER, color, 16);
      if (!ps || !cb || !d.fullVs) return false;
      d.dev->PSSetConstantBuffers(0, 1, &cb);
      fullDraw(ps);
      bool ok = pix10(d, 32, 32, 255, 128, 64, detail);
      ps->Release();
      cb->Release();
      return ok;
   });
   test("Vertex buffers and input layouts", [&] {
      const char *src = "struct V { float4 p : SV_Position; float4 c : COLOR; };"
                        "V main(float2 p : POSITION, float4 c : COLOR) { V v; v.p = float4(p, 0.5, 1); v.c = c; return v; }";
      ID3DBlob *b = compileHlsl(src, "vs_4_0", &detail);
      ID3D10PixelShader *ps = ps10(d, "float4 main(float4 p : SV_Position, float4 c : COLOR) : SV_Target { return c; }", detail);
      if (!b || !ps) return false;
      ID3D10VertexShader *vs = nullptr;
      d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), &vs);
      D3D10_INPUT_ELEMENT_DESC el[] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
                                       {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D10_INPUT_PER_VERTEX_DATA, 0}};
      ID3D10InputLayout *layout = nullptr;
      d.dev->CreateInputLayout(el, 2, b->GetBufferPointer(), b->GetBufferSize(), &layout);
      struct Vtx { float x, y; uint32_t c; } v[4] = {{-1, 1, 0xff00ff00}, {1, 1, 0xff00ff00}, {-1, -1, 0xff00ff00}, {1, -1, 0xff00ff00}};
      ID3D10Buffer *vb = buffer10(d, D3D10_BIND_VERTEX_BUFFER, v, sizeof v);
      if (!vs || !layout || !vb) return false;
      UINT stride = sizeof(Vtx), offset = 0;
      d.dev->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
      d.dev->IASetInputLayout(layout);
      d.dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      d.dev->VSSetShader(vs);
      d.dev->PSSetShader(ps);
      d.dev->Draw(4, 0);
      bool ok = pix10(d, 5, 5, 0, 255, 0, detail) && pix10(d, 60, 60, 0, 255, 0, detail);
      b->Release();
      vs->Release();
      ps->Release();
      layout->Release();
      vb->Release();
      return ok;
   });
   test("Texture sampling (Texture2D and sampler state)", [&] {
      const uint32_t texels[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};
      ID3D10Texture2D *t = texture10(d, 2, 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, texels, 8);
      ID3D10ShaderResourceView *srv = nullptr;
      if (t) d.dev->CreateShaderResourceView(t, nullptr, &srv);
      D3D10_SAMPLER_DESC smp = {D3D10_FILTER_MIN_MAG_MIP_POINT, D3D10_TEXTURE_ADDRESS_CLAMP, D3D10_TEXTURE_ADDRESS_CLAMP,
                                D3D10_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D10_COMPARISON_NEVER, {0, 0, 0, 0}, 0, 1000};
      ID3D10SamplerState *s = nullptr;
      d.dev->CreateSamplerState(&smp, &s);
      ID3D10PixelShader *ps = ps10(d, "Texture2D t; SamplerState s; float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) "
                                      ": SV_Target { return t.Sample(s, uv); }", detail);
      if (!srv || !s || !ps) return false;
      d.dev->PSSetShaderResources(0, 1, &srv);
      d.dev->PSSetSamplers(0, 1, &s);
      fullDraw(ps);
      bool ok = pix10(d, 16, 16, 255, 0, 0, detail) && pix10(d, 48, 16, 0, 255, 0, detail) &&
                pix10(d, 16, 48, 0, 0, 255, detail) && pix10(d, 48, 48, 255, 255, 255, detail);
      t->Release();
      srv->Release();
      s->Release();
      ps->Release();
      return ok;
   });
   test("Geometry shaders (a point expanded to a quad)", [&] {
      ID3D10VertexShader *vs = vs10(d, "float4 main(uint id : SV_VertexID) : POSITION { return float4(-0.5, 0, 0.5, 1); }", detail);
      ID3D10GeometryShader *gs = create10<ID3D10GeometryShader>(R"(
struct G { float4 p : SV_Position; };
[maxvertexcount(4)] void main(point float4 c[1] : POSITION, inout TriangleStream<G> s)
{
   G g;
   g.p = c[0] + float4(-0.5, 0.5, 0, 0); s.Append(g);
   g.p = c[0] + float4(0.5, 0.5, 0, 0); s.Append(g);
   g.p = c[0] + float4(-0.5, -0.5, 0, 0); s.Append(g);
   g.p = c[0] + float4(0.5, -0.5, 0, 0); s.Append(g);
})", "gs_4_0", detail, [&](ID3DBlob *b, ID3D10GeometryShader **s) { return d.dev->CreateGeometryShader(b->GetBufferPointer(), b->GetBufferSize(), s); });
      ID3D10PixelShader *ps = ps10(d, "float4 main() : SV_Target { return float4(1, 1, 0, 1); }", detail);
      if (!vs || !gs || !ps) return false;
      d.dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
      d.dev->VSSetShader(vs);
      d.dev->GSSetShader(gs);
      d.dev->PSSetShader(ps);
      d.dev->Draw(1, 0);
      bool ok = pix10(d, 16, 32, 255, 255, 0, detail) && pix10(d, 48, 32, 0, 0, 255, detail);
      vs->Release();
      gs->Release();
      ps->Release();
      return ok;
   });
   test("Stream output and DrawAuto", [&] {
      // pass 1: a full-screen triangle streamed out; pass 2: drawn from the stream-output buffer
      ID3DBlob *b = compileHlsl("float4 main(uint id : SV_VertexID) : SV_Position "
                                "{ float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1); }",
                                "vs_4_0", &detail);
      if (!b) return false;
      ID3D10VertexShader *vs = nullptr;
      d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), &vs);
      D3D10_SO_DECLARATION_ENTRY so = {"SV_Position", 0, 0, 4, 0};
      ID3D10GeometryShader *gs = nullptr;
      d.dev->CreateGeometryShaderWithStreamOutput(b->GetBufferPointer(), b->GetBufferSize(), &so, 1, 16, &gs);
      ID3D10Buffer *sob = buffer10(d, D3D10_BIND_STREAM_OUTPUT | D3D10_BIND_VERTEX_BUFFER, nullptr, 16 * 16);
      ID3DBlob *b2 = compileHlsl("float4 main(float4 p : POSITION) : SV_Position { return p; }", "vs_4_0", &detail);
      ID3D10VertexShader *vs2 = nullptr;
      ID3D10InputLayout *layout = nullptr;
      if (b2) {
         d.dev->CreateVertexShader(b2->GetBufferPointer(), b2->GetBufferSize(), &vs2);
         D3D10_INPUT_ELEMENT_DESC el = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0};
         d.dev->CreateInputLayout(&el, 1, b2->GetBufferPointer(), b2->GetBufferSize(), &layout);
      }
      ID3D10PixelShader *ps = ps10(d, "float4 main() : SV_Target { return float4(0, 1, 1, 1); }", detail);
      ID3D10Query *stats = nullptr;
      D3D10_QUERY_DESC qd = {D3D10_QUERY_SO_STATISTICS, 0};
      d.dev->CreateQuery(&qd, &stats);
      if (!vs || !gs || !sob || !vs2 || !layout || !ps || !stats) {
         if (detail.empty()) detail = "creating the stream-output pipeline failed";
         return false;
      }
      UINT offset = 0;
      d.dev->SOSetTargets(1, &sob, &offset);
      d.dev->VSSetShader(vs);
      d.dev->GSSetShader(gs);
      d.dev->PSSetShader(nullptr);
      stats->Begin();
      d.dev->Draw(3, 0);
      stats->End();
      ID3D10Buffer *none = nullptr;
      d.dev->SOSetTargets(1, &none, &offset);
      D3D10_QUERY_DATA_SO_STATISTICS st = {};
      while (stats->GetData(&st, sizeof st, 0) == S_FALSE) Sleep(1);
      UINT stride = 16;
      d.dev->IASetVertexBuffers(0, 1, &sob, &stride, &offset);
      d.dev->IASetInputLayout(layout);
      d.dev->VSSetShader(vs2);
      d.dev->GSSetShader(nullptr);
      d.dev->PSSetShader(ps);
      d.dev->DrawAuto();
      bool ok = st.NumPrimitivesWritten == 1 && pix10(d, 32, 32, 0, 255, 255, detail);
      if (st.NumPrimitivesWritten != 1) detail += fmt(" %llu primitives written, expected 1", (unsigned long long)st.NumPrimitivesWritten);
      b->Release();
      b2->Release();
      vs->Release();
      gs->Release();
      vs2->Release();
      layout->Release();
      ps->Release();
      sob->Release();
      stats->Release();
      return ok;
   });
   test("Texture arrays", [&] {
      uint32_t texels[3][4];
      const uint32_t colors[3] = {0xff0000ff, 0xff00ff00, 0xffff00ff};
      for (int s = 0; s < 3; s++)
         for (int i = 0; i < 4; i++) texels[s][i] = colors[s];
      ID3D10Texture2D *t = texture10(d, 2, 2, 3, DXGI_FORMAT_R8G8B8A8_UNORM, texels, 8);
      ID3D10ShaderResourceView *srv = nullptr;
      if (t) d.dev->CreateShaderResourceView(t, nullptr, &srv);
      ID3D10PixelShader *ps = ps10(d, "Texture2DArray<float4> t; float4 main() : SV_Target { return t.Load(int4(0, 0, 2, 0)); }", detail);
      if (!srv || !ps) return false;
      d.dev->PSSetShaderResources(0, 1, &srv);
      fullDraw(ps);
      bool ok = pix10(d, 32, 32, 255, 0, 255, detail);
      t->Release();
      srv->Release();
      ps->Release();
      return ok;
   });
   test("Render to texture", [&] {
      ID3D10Texture2D *t = texture10(d, N10, N10, 1, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0,
                                     D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE);
      ID3D10RenderTargetView *rtv = nullptr;
      ID3D10ShaderResourceView *srv = nullptr;
      if (t) {
         d.dev->CreateRenderTargetView(t, nullptr, &rtv);
         d.dev->CreateShaderResourceView(t, nullptr, &srv);
      }
      ID3D10PixelShader *ps = ps10(d, "Texture2D<float4> t; float4 main(float4 p : SV_Position) : SV_Target { return t.Load(int3(p.xy, 0)); }", detail);
      if (!rtv || !srv || !ps) return false;
      const float yellow[4] = {1, 1, 0, 1};
      d.dev->ClearRenderTargetView(rtv, yellow);
      d.dev->PSSetShaderResources(0, 1, &srv);
      fullDraw(ps);
      bool ok = pix10(d, 32, 32, 255, 255, 0, detail);
      t->Release();
      rtv->Release();
      srv->Release();
      ps->Release();
      return ok;
   });
   test("Depth and stencil buffers", [&] {
      ID3D10Texture2D *ds = texture10(d, N10, N10, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, nullptr, 0, D3D10_BIND_DEPTH_STENCIL);
      ID3D10DepthStencilView *dsv = nullptr;
      if (ds) d.dev->CreateDepthStencilView(ds, nullptr, &dsv);
      ID3D10PixelShader *ps = ps10(d, "cbuffer C { float4 color; float depth; }; float4 main(out float z : SV_Depth) : SV_Target { z = depth; return color; }", detail);
      ID3D10Buffer *cb = buffer10(d, D3D10_BIND_CONSTANT_BUFFER, nullptr, 32);
      D3D10_DEPTH_STENCIL_DESC dsd = {};
      dsd.DepthEnable = TRUE;
      dsd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
      dsd.DepthFunc = D3D10_COMPARISON_LESS;
      dsd.StencilEnable = TRUE;
      dsd.StencilReadMask = dsd.StencilWriteMask = 0xff;
      dsd.FrontFace = {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_INCR, D3D10_COMPARISON_ALWAYS};
      dsd.BackFace = dsd.FrontFace;
      ID3D10DepthStencilState *dss = nullptr, *eq = nullptr;
      d.dev->CreateDepthStencilState(&dsd, &dss);
      dsd.DepthEnable = FALSE;
      dsd.FrontFace = {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_EQUAL};
      dsd.BackFace = dsd.FrontFace;
      d.dev->CreateDepthStencilState(&dsd, &eq);
      if (!dsv || !ps || !cb || !dss || !eq) return false;
      d.dev->ClearDepthStencilView(dsv, D3D10_CLEAR_DEPTH | D3D10_CLEAR_STENCIL, 1.0f, 0);
      d.dev->OMSetRenderTargets(1, &d.rtv, dsv);
      d.dev->OMSetDepthStencilState(dss, 0);
      d.dev->PSSetConstantBuffers(0, 1, &cb);
      auto draw = [&](float r, float g, float b, float z) {
         const float data[8] = {r, g, b, 1, z, 0, 0, 0};
         d.dev->UpdateSubresource(cb, 0, nullptr, data, 0, 0);
         fullDraw(ps);
      };
      draw(1, 0, 0, 0.3f);   // red, stencil 1
      draw(0, 1, 0, 0.7f);   // behind: hidden, stencil stays 1
      d.dev->OMSetDepthStencilState(eq, 1);
      draw(1, 1, 1, 0.5f);   // stencil == 1: white
      bool ok = pix10(d, 32, 32, 255, 255, 255, detail);
      d.dev->OMSetDepthStencilState(nullptr, 0);
      ds->Release();
      dsv->Release();
      ps->Release();
      cb->Release();
      dss->Release();
      eq->Release();
      return ok;
   });
   test("Blending", [&] {
      D3D10_BLEND_DESC bd = {};
      bd.BlendEnable[0] = TRUE;
      bd.SrcBlend = D3D10_BLEND_SRC_ALPHA;
      bd.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
      bd.BlendOp = D3D10_BLEND_OP_ADD;
      bd.SrcBlendAlpha = D3D10_BLEND_ONE;
      bd.DestBlendAlpha = D3D10_BLEND_ZERO;
      bd.BlendOpAlpha = D3D10_BLEND_OP_ADD;
      bd.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
      ID3D10BlendState *bs = nullptr;
      d.dev->CreateBlendState(&bd, &bs);
      ID3D10PixelShader *ps = ps10(d, "float4 main() : SV_Target { return float4(1, 0, 0, 0.25); }", detail);
      if (!bs || !ps) return false;
      d.dev->OMSetBlendState(bs, nullptr, 0xffffffff);
      fullDraw(ps);
      bool ok = pix10(d, 32, 32, 64, 0, 191, detail);
      bs->Release();
      ps->Release();
      return ok;
   });
   test("Instancing (SV_InstanceID)", [&] {
      ID3D10VertexShader *vs = vs10(d, R"(
float4 main(uint id : SV_VertexID, uint inst : SV_InstanceID) : SV_Position
{
   float2 c = float2(id & 1, id >> 1);
   return float4(c.x - 1 + inst, 1 - 2 * c.y, 0.5, 1);
})", detail);
      ID3D10PixelShader *ps = ps10(d, "float4 main(float4 p : SV_Position) : SV_Target { return float4(p.x < 32 ? 1 : 0, p.x < 32 ? 0 : 1, 0, 1); }", detail);
      if (!vs || !ps) return false;
      d.dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      d.dev->VSSetShader(vs);
      d.dev->PSSetShader(ps);
      d.dev->DrawInstanced(4, 2, 0, 0);
      bool ok = pix10(d, 16, 32, 255, 0, 0, detail) && pix10(d, 48, 32, 0, 255, 0, detail);
      vs->Release();
      ps->Release();
      return ok;
   });
   test("Occlusion query", [&] {
      ID3D10Query *q = nullptr;
      D3D10_QUERY_DESC qd = {D3D10_QUERY_OCCLUSION, 0};
      d.dev->CreateQuery(&qd, &q);
      ID3D10PixelShader *ps = ps10(d, "float4 main(float4 p : SV_Position) : SV_Target { if (p.x >= 16) discard; return 1; }", detail);
      if (!q || !ps) return false;
      q->Begin();
      fullDraw(ps);
      q->End();
      UINT64 n = 0;
      while (q->GetData(&n, sizeof n, 0) == S_FALSE) Sleep(1);
      detail = fmt("%llu pixels, expected %d", (unsigned long long)n, 16 * N10);
      q->Release();
      ps->Release();
      return n == (UINT64)(16 * N10);
   });
   test("Multisampling 4x and ResolveSubresource", [&] {
      UINT q = 0;
      d.dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &q);
      if (!q) {
         detail = "4x not offered";
         return false;
      }
      ID3D10Texture2D *ms = texture10(d, N10, N10, 1, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0, D3D10_BIND_RENDER_TARGET, 4);
      ID3D10RenderTargetView *rtv = nullptr;
      if (ms) d.dev->CreateRenderTargetView(ms, nullptr, &rtv);
      ID3D10VertexShader *vs = vs10(d, "float4 main(uint id : SV_VertexID) : SV_Position "
                                       "{ return float4(id == 1 ? 1 : -1, id == 2 ? 1 : -1, 0.5, 1); }", detail);
      ID3D10PixelShader *ps = ps10(d, "float4 main() : SV_Target { return float4(1, 0, 0, 1); }", detail);
      D3D10_RASTERIZER_DESC rd = {D3D10_FILL_SOLID, D3D10_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, TRUE, FALSE};
      ID3D10RasterizerState *rs = nullptr;
      d.dev->CreateRasterizerState(&rd, &rs);
      if (!rtv || !vs || !ps || !rs) return false;
      const float black[4] = {0, 0, 0, 1};
      d.dev->ClearRenderTargetView(rtv, black);
      d.dev->OMSetRenderTargets(1, &rtv, nullptr);
      d.dev->RSSetState(rs);
      d.dev->VSSetShader(vs);
      d.dev->PSSetShader(ps);
      d.dev->Draw(3, 0);
      d.dev->ResolveSubresource(d.rt, 0, ms, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
      bool ok = pix10(d, 8, 56, 255, 0, 0, detail) && pix10(d, 56, 8, 0, 0, 0, detail);
      int partial = 0;
      for (uint32_t p : d.pixels) partial += (p & 255) > 30 && (p & 255) < 225;
      if (partial < 20) detail += fmt(" only %d anti-aliased edge pixels", partial);
      ms->Release();
      rtv->Release();
      vs->Release();
      ps->Release();
      rs->Release();
      return ok && partial >= 20;
   });
   if (!is101) {
      report(Status::Skip, "Direct3D 10.1 features", "no feature level 10_1 device");
      return;
   }
   test("Shader model 4.1: multisampled texture loads (Texture2DMS)", [&] {
      ID3D10Texture2D *ms = texture10(d, N10, N10, 1, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0,
                                      D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE, 4);
      ID3D10RenderTargetView *rtv = nullptr;
      ID3D10ShaderResourceView *srv = nullptr;
      if (ms) {
         d.dev->CreateRenderTargetView(ms, nullptr, &rtv);
         d.dev->CreateShaderResourceView(ms, nullptr, &srv);
      }
      ID3D10PixelShader *ps = ps10(d, "Texture2DMS<float4, 4> t; float4 main(float4 p : SV_Position) : SV_Target "
                                      "{ return t.Load(int2(p.xy), 3); }", detail, "ps_4_1");
      if (!rtv || !srv || !ps) return false;
      const float green[4] = {0, 1, 0.5f, 1};
      d.dev->ClearRenderTargetView(rtv, green);
      d.dev->PSSetShaderResources(0, 1, &srv);
      fullDraw(ps);
      bool ok = pix10(d, 32, 32, 0, 255, 128, detail);
      ms->Release();
      rtv->Release();
      srv->Release();
      ps->Release();
      return ok;
   });
   test("Shader model 4.1: Gather", [&] {
      const float texels[4] = {0.25f, 0.5f, 0.75f, 1.0f};
      ID3D10Texture2D *t = texture10(d, 2, 2, 1, DXGI_FORMAT_R32_FLOAT, texels, 8);
      ID3D10ShaderResourceView *srv = nullptr;
      if (t) d.dev->CreateShaderResourceView(t, nullptr, &srv);
      D3D10_SAMPLER_DESC smp = {D3D10_FILTER_MIN_MAG_MIP_POINT, D3D10_TEXTURE_ADDRESS_CLAMP, D3D10_TEXTURE_ADDRESS_CLAMP,
                                D3D10_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D10_COMPARISON_NEVER, {0, 0, 0, 0}, 0, 1000};
      ID3D10SamplerState *s = nullptr;
      d.dev->CreateSamplerState(&smp, &s);
      // gather at the texture's center: the four texels (in the order x: (0,1), y: (1,1), z: (1,0), w: (0,0))
      ID3D10PixelShader *ps = ps10(d, "Texture2D<float> t; SamplerState s; float4 main() : SV_Target "
                                      "{ return t.Gather(s, float2(0.5, 0.5)); }", detail, "ps_4_1");
      if (!srv || !s || !ps) return false;
      d.dev->PSSetShaderResources(0, 1, &srv);
      d.dev->PSSetSamplers(0, 1, &s);
      fullDraw(ps);
      bool ok = pix10(d, 32, 32, 191, 255, 128, detail);
      t->Release();
      srv->Release();
      s->Release();
      ps->Release();
      return ok;
   });
   test("Cube map arrays (10.1)", [&] {
      uint32_t texels[12][4];
      for (int f = 0; f < 12; f++)
         for (int i = 0; i < 4; i++) texels[f][i] = f == 6 ? 0xff00ffff : 0xff000000;   // cube 1, +X face: yellow
      ID3D10Texture2D *t = texture10(d, 2, 2, 12, DXGI_FORMAT_R8G8B8A8_UNORM, texels, 8, D3D10_BIND_SHADER_RESOURCE, 1,
                                     D3D10_RESOURCE_MISC_TEXTURECUBE);
      ID3D10ShaderResourceView1 *srv = nullptr;
      if (t) {
         D3D10_SHADER_RESOURCE_VIEW_DESC1 vd = {};
         vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
         vd.ViewDimension = D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY;
         vd.TextureCubeArray.MipLevels = 1;
         vd.TextureCubeArray.NumCubes = 2;
         d.dev->CreateShaderResourceView1(t, &vd, &srv);
      }
      D3D10_SAMPLER_DESC smp = {D3D10_FILTER_MIN_MAG_MIP_POINT, D3D10_TEXTURE_ADDRESS_CLAMP, D3D10_TEXTURE_ADDRESS_CLAMP,
                                D3D10_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D10_COMPARISON_NEVER, {0, 0, 0, 0}, 0, 1000};
      ID3D10SamplerState *s = nullptr;
      d.dev->CreateSamplerState(&smp, &s);
      ID3D10PixelShader *ps = ps10(d, "TextureCubeArray t; SamplerState s; float4 main() : SV_Target "
                                      "{ return t.Sample(s, float4(1, 0, 0, 1)); }", detail, "ps_4_1");
      if (!srv || !s || !ps) return false;
      ID3D10ShaderResourceView *view = srv;
      d.dev->PSSetShaderResources(0, 1, &view);
      d.dev->PSSetSamplers(0, 1, &s);
      fullDraw(ps);
      bool ok = pix10(d, 32, 32, 255, 255, 0, detail);
      t->Release();
      srv->Release();
      s->Release();
      ps->Release();
      return ok;
   });
   test("Independent blend states per render target (10.1)", [&] {
      ID3D10Texture2D *t2 = texture10(d, N10, N10, 1, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0, D3D10_BIND_RENDER_TARGET);
      ID3D10RenderTargetView *rtv2 = nullptr;
      if (t2) d.dev->CreateRenderTargetView(t2, nullptr, &rtv2);
      D3D10_BLEND_DESC1 bd = {};
      bd.IndependentBlendEnable = TRUE;
      bd.RenderTarget[0] = {TRUE, D3D10_BLEND_ONE, D3D10_BLEND_ONE, D3D10_BLEND_OP_ADD, D3D10_BLEND_ONE, D3D10_BLEND_ZERO,
                            D3D10_BLEND_OP_ADD, D3D10_COLOR_WRITE_ENABLE_ALL};   // add
      bd.RenderTarget[1] = {FALSE, D3D10_BLEND_ONE, D3D10_BLEND_ZERO, D3D10_BLEND_OP_ADD, D3D10_BLEND_ONE, D3D10_BLEND_ZERO,
                            D3D10_BLEND_OP_ADD, D3D10_COLOR_WRITE_ENABLE_ALL};   // replace
      ID3D10BlendState1 *bs = nullptr;
      d.dev->CreateBlendState1(&bd, &bs);
      ID3D10PixelShader *ps = ps10(d, "struct O { float4 a : SV_Target0; float4 b : SV_Target1; };"
                                      "O main() { O o; o.a = float4(1, 0, 0, 1); o.b = float4(1, 0, 0, 1); return o; }", detail);
      if (!rtv2 || !bs || !ps) return false;
      const float blue[4] = {0, 0, 1, 1};
      d.dev->ClearRenderTargetView(rtv2, blue);
      ID3D10RenderTargetView *rtvs[2] = {d.rtv, rtv2};
      d.dev->OMSetRenderTargets(2, rtvs, nullptr);
      d.dev->OMSetBlendState(bs, nullptr, 0xffffffff);
      fullDraw(ps);
      std::vector<uint32_t> second;
      bool ok = pix10(d, 32, 32, 255, 0, 255, detail) && read10(d, t2, second) &&
                expectRgba(second[32 * N10 + 32], 255, 0, 0, detail);
      t2->Release();
      rtv2->Release();
      bs->Release();
      ps->Release();
      return ok;
   });
   if (!interactiveDesktop()) {
      report(Status::Skip, "Present to a window (DXGI swap chain)", "no interactive desktop");
      return;
   }
   if (startTest("Present to a window (DXGI swap chain)")) {
      HWND w = createWindow("dxtest d3d10 present", N10, N10, true);
      IDXGIDevice *dxgiDev = nullptr;
      IDXGIAdapter *adapter = nullptr;
      IDXGIFactory *factory = nullptr;
      IDXGISwapChain *sc = nullptr;
      d.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDev);
      if (dxgiDev) dxgiDev->GetAdapter(&adapter);
      if (adapter) adapter->GetParent(__uuidof(IDXGIFactory), (void **)&factory);
      DXGI_SWAP_CHAIN_DESC scd = {};
      scd.BufferDesc = {N10, N10, {0, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
      scd.SampleDesc.Count = 1;
      scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      scd.BufferCount = 1;
      scd.OutputWindow = w;
      scd.Windowed = TRUE;
      if (factory) factory->CreateSwapChain(d.dev, &scd, &sc);
      COLORREF c = 0;
      bool ok = false;
      if (sc) {
         ID3D10Texture2D *bb = nullptr;
         ID3D10RenderTargetView *rtv = nullptr;
         sc->GetBuffer(0, __uuidof(ID3D10Texture2D), (void **)&bb);
         if (bb) d.dev->CreateRenderTargetView(bb, nullptr, &rtv);
         const float color[4] = {0.25f, 0, 0.75f, 1};
         for (int i = 0; i < 10 && rtv; i++) {
            d.dev->ClearRenderTargetView(rtv, color);
            sc->Present(0, 0);
            pumpMessages();
            Sleep(30);
         }
         Sleep(300);
         ok = windowPixel(w, N10 / 2, N10 / 2, c) && near8(GetRValue(c), 64, 8) && near8(GetGValue(c), 0, 8) &&
              near8(GetBValue(c), 191, 8);
         release(rtv);
         release(bb);
         sc->Release();
      }
      check(ok, "Present to a window (DXGI swap chain)",
            sc ? fmt("window shows (%d,%d,%d), expected (64,0,191)", GetRValue(c), GetGValue(c), GetBValue(c)) : "no swap chain");
      release(factory);
      release(adapter);
      release(dxgiDev);
      DestroyWindow(w);
   }
}
