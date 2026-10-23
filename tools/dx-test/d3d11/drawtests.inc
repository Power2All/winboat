// Stage 4: indirect draws, MSAA (render, resolve, per-sample loads, sample masks, coverage output, depth),
// quality levels, the feature level without the development switch, and multisampled swap chains (DXGI Blt).
// Draws are compared with WARP; where the host's sample positions decide the result (edges), within one sample.

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

static const int DW = 32, DH = 32;

struct Target {
   ID3D11Texture2D *tex = nullptr, *resolved = nullptr, *staging = nullptr;
   ID3D11RenderTargetView *rtv = nullptr;
   ID3D11Texture2D *ds = nullptr;
   ID3D11DepthStencilView *dsv = nullptr;
   void release()
   {
      if (rtv) rtv->Release();
      if (tex) tex->Release();
      if (resolved) resolved->Release();
      if (staging) staging->Release();
      if (dsv) dsv->Release();
      if (ds) ds->Release();
      *this = Target();
   }
};

static bool make_target(Dev &d, Target &t, UINT samples, bool depth, bool srv = false)
{
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = DW;
   td.Height = DH;
   td.MipLevels = td.ArraySize = 1;
   td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   td.SampleDesc.Count = samples;
   td.BindFlags = D3D11_BIND_RENDER_TARGET | (srv ? D3D11_BIND_SHADER_RESOURCE : 0);
   CHECK(d.dev->CreateTexture2D(&td, nullptr, &t.tex));
   CHECK(d.dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv));
   td.SampleDesc.Count = 1;
   td.BindFlags = 0;
   if (samples > 1) {
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      CHECK(d.dev->CreateTexture2D(&td, nullptr, &t.resolved));
   }
   td.BindFlags = 0;
   td.Usage = D3D11_USAGE_STAGING;
   td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   CHECK(d.dev->CreateTexture2D(&td, nullptr, &t.staging));
   if (depth) {
      D3D11_TEXTURE2D_DESC dd = {};
      dd.Width = DW;
      dd.Height = DH;
      dd.MipLevels = dd.ArraySize = 1;
      dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
      dd.SampleDesc.Count = samples;
      dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
      CHECK(d.dev->CreateTexture2D(&dd, nullptr, &t.ds));
      CHECK(d.dev->CreateDepthStencilView(t.ds, nullptr, &t.dsv));
   }
   float clear[4] = {0, 0, 0, 0};
   d.ctx->ClearRenderTargetView(t.rtv, clear);
   if (t.dsv) d.ctx->ClearDepthStencilView(t.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
   d.ctx->OMSetRenderTargets(1, &t.rtv, t.dsv);
   D3D11_VIEWPORT vp = {0, 0, (float)DW, (float)DH, 0, 1};
   d.ctx->RSSetViewports(1, &vp);
   return true;
}

static bool read_target(Dev &d, Target &t, std::vector<uint32_t> &out)
{
   d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
   if (t.resolved) {
      d.ctx->ResolveSubresource(t.resolved, 0, t.tex, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
      d.ctx->CopyResource(t.staging, t.resolved);
   } else {
      d.ctx->CopyResource(t.staging, t.tex);
   }
   D3D11_MAPPED_SUBRESOURCE m;
   CHECK(d.ctx->Map(t.staging, 0, D3D11_MAP_READ, 0, &m));
   out.resize(DW * DH);
   for (int y = 0; y < DH; y++)
      memcpy(&out[y * DW], (char *)m.pData + y * m.RowPitch, DW * 4);
   d.ctx->Unmap(t.staging, 0);
   return true;
}

static ID3D11Buffer *make_buffer(Dev &d, UINT bind, UINT misc, const void *data, UINT size)
{
   D3D11_BUFFER_DESC bd = {};
   bd.ByteWidth = size;
   bd.BindFlags = bind;
   bd.MiscFlags = misc;
   D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
   ID3D11Buffer *b = nullptr;
   if (FAILED(d.dev->CreateBuffer(&bd, data ? &init : nullptr, &b))) {
      printf("  CreateBuffer(bind 0x%x misc 0x%x) failed\n", bind, misc);
   }
   return b;
}

// Instanced quads: per-vertex corner, per-instance offset and color (start instance shows in which data is read)
static const char *DRAW_VS = R"(
struct O { float4 pos : SV_Position; float4 color : COLOR; };
O main(float2 corner : CORNER, float2 offset : OFFSET, float4 color : COLOR, uint inst : SV_InstanceID)
{
   O o;
   o.pos = float4(corner * 0.4 + offset, 0.5, 1);
   o.color = color + float4(0, 0, inst * 0.25, 0);
   return o;
}
)";

static const char *DRAW_PS = R"(
float4 main(float4 pos : SV_Position, float4 color : COLOR) : SV_Target { return color; }
)";

struct DrawPipe {
   ID3D11VertexShader *vs = nullptr;
   ID3D11PixelShader *ps = nullptr;
   ID3D11InputLayout *layout = nullptr;
   ID3D11Buffer *corners = nullptr, *instances = nullptr;
   void release()
   {
      if (vs) vs->Release();
      if (ps) ps->Release();
      if (layout) layout->Release();
      if (corners) corners->Release();
      if (instances) instances->Release();
   }
};

static bool make_draw_pipe(Dev &d, DrawPipe &p)
{
   ID3DBlob *vsb = compile(DRAW_VS, "vs_5_0"), *psb = compile(DRAW_PS, "ps_5_0");
   if (!vsb || !psb) return false;
   CHECK(d.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &p.vs));
   CHECK(d.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &p.ps));
   D3D11_INPUT_ELEMENT_DESC el[] = {
      {"CORNER", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1},
   };
   CHECK(d.dev->CreateInputLayout(el, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &p.layout));
   vsb->Release();
   psb->Release();
   // 8 corners: two quads' worth for index/base-vertex tests (the second set shifted)
   const float corners[] = {-1, -1, 1, -1, -1, 1, 1, 1, -0.5f, -1, 1, -0.5f, -1, 0.5f, 0.5f, 1};
   p.corners = make_buffer(d, D3D11_BIND_VERTEX_BUFFER, 0, corners, sizeof corners);
   const float inst[] = {
      -0.5f, -0.5f, 1, 0, 0, 1,
      0.5f, -0.5f, 0, 1, 0, 1,
      -0.5f, 0.5f, 1, 1, 0, 1,
      0.5f, 0.5f, 0, 0.5f, 1, 1,
      0.0f, 0.0f, 1, 0, 1, 1,
   };
   p.instances = make_buffer(d, D3D11_BIND_VERTEX_BUFFER, 0, inst, sizeof inst);
   return p.corners && p.instances;
}

static void bind_draw_pipe(Dev &d, DrawPipe &p, D3D11_PRIMITIVE_TOPOLOGY topo)
{
   ID3D11Buffer *vbs[2] = {p.corners, p.instances};
   UINT strides[2] = {8, 24}, offsets[2] = {0, 0};
   d.ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
   d.ctx->IASetInputLayout(p.layout);
   d.ctx->IASetPrimitiveTopology(topo);
   d.ctx->VSSetShader(p.vs, nullptr, 0);
   d.ctx->PSSetShader(p.ps, nullptr, 0);
   d.ctx->GSSetShader(nullptr, nullptr, 0);
   d.ctx->HSSetShader(nullptr, nullptr, 0);
   d.ctx->DSSetShader(nullptr, nullptr, 0);
   d.ctx->RSSetState(nullptr);
   d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
   d.ctx->OMSetDepthStencilState(nullptr, 0);
}

// The kinds of indirect draws
enum { IND_PLAIN, IND_INDEXED, IND_INDEXED_IB_OFFSET, IND_FROM_CS };

static bool run_indirect(Dev &d, int kind, std::vector<uint32_t> &out)
{
   Target t;
   DrawPipe p;
   if (!make_target(d, t, 1, false) || !make_draw_pipe(d, p)) return false;
   ID3D11Buffer *ib = nullptr, *args = nullptr;
   ID3D11UnorderedAccessView *uav = nullptr;
   if (kind == IND_PLAIN) {
      // junk before the arguments (offset 16): 4 vertices (strip), 3 instances from instance 1
      const UINT a[8] = {99, 99, 99, 99, 4, 3, 0, 1};
      args = make_buffer(d, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, a, sizeof a);
      bind_draw_pipe(d, p, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      d.ctx->DrawInstancedIndirect(args, 16);
   } else if (kind == IND_FROM_CS) {
      // a compute shader writes the arguments: 4 vertices, 2 instances from instance 2, at offset 8
      args = make_buffer(d, D3D11_BIND_UNORDERED_ACCESS,
                         D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, nullptr,
                         64);
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Format = DXGI_FORMAT_R32_TYPELESS;
      ud.Buffer.NumElements = 16;
      ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      CHECK(d.dev->CreateUnorderedAccessView(args, &ud, &uav));
      ID3DBlob *csb = compile(R"(
RWByteAddressBuffer a : register(u0);
[numthreads(1, 1, 1)] void main() { a.Store4(8, uint4(4, 2, 0, 2)); a.Store2(0, uint2(7, 7)); }
)", "cs_5_0");
      if (!csb) return false;
      ID3D11ComputeShader *cs;
      CHECK(d.dev->CreateComputeShader(csb->GetBufferPointer(), csb->GetBufferSize(), nullptr, &cs));
      d.ctx->CSSetShader(cs, nullptr, 0);
      d.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
      d.ctx->Dispatch(1, 1, 1);
      ID3D11UnorderedAccessView *none = nullptr;
      d.ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
      cs->Release();
      csb->Release();
      bind_draw_pipe(d, p, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      d.ctx->DrawInstancedIndirect(args, 8);
   } else {
      // indices of two strips (0-3 and 4-7 through base vertex 4), restart between; with the offset variant the
      // index buffer is bound 6 bytes in (3 junk indices)
      const uint16_t idx[] = {9, 9, 9, 9, 9, 0, 1, 2, 3, 0xffff, 0, 1, 2, 3};
      ib = make_buffer(d, D3D11_BIND_INDEX_BUFFER, 0, idx, sizeof idx);
      bool off = kind == IND_INDEXED_IB_OFFSET;
      // 4 indices from index 5 (2 with the offset), base vertex 4, 2 instances from instance 3
      const UINT a[6] = {5, off ? 2u : 5u, 4, 2, 4, 3};
      args = make_buffer(d, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, a, sizeof a);
      bind_draw_pipe(d, p, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      d.ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, off ? 6 : 0);
      d.ctx->DrawIndexedInstancedIndirect(args, 4);
      // and a second one from the other strip: 4 indices from 10 (7), base 0, 1 instance from 0
      const UINT b[5] = {4, 1, off ? 7u : 10u, 0, 0};
      ID3D11Buffer *args2 = make_buffer(d, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, b, sizeof b);
      d.ctx->DrawIndexedInstancedIndirect(args2, 0);
      args2->Release();
   }
   bool ok = read_target(d, t, out);
   if (uav) uav->Release();
   if (args) args->Release();
   if (ib) ib->Release();
   p.release();
   t.release();
   return ok;
}

// MSAA scenes on a 4x target, resolved
enum { MS_EDGES, MS_DEPTH, MS_SAMPLE_MASK, MS_COVERAGE_OUT, MS_PER_SAMPLE, MS_SAMPLE_INFO, DEPTH_1X, DEPTH_SRV,
       SCISSOR_1X, SCISSOR_4X };

static const char *MS_VS = R"(
struct O { float4 pos : SV_Position; float4 color : COLOR; };
O main(uint id : SV_VertexID)
{
   // two triangles with slanted edges, the second one crossing the first in depth
   static const float3 p[6] = { float3(-0.9, -0.8, 0.5), float3(0.7, -0.95, 0.5), float3(-0.2, 0.9, 0.5),
                                float3(-0.95, 0.3, 0.1), float3(0.95, 0.6, 0.9), float3(0.3, -0.7, 0.9) };
   static const float4 c[2] = { float4(1, 0.5, 0, 1), float4(0, 0.25, 1, 1) };
   O o;
   o.pos = float4(p[id], 1);
   o.color = c[id / 3];
   return o;
}
)";

static bool run_depth_srv(Dev &d, std::vector<uint32_t> &out);

static bool run_msaa(Dev &d, int kind, std::vector<uint32_t> &out)
{
   if (kind == DEPTH_SRV) return run_depth_srv(d, out);
   Target t;
   bool depth = kind == MS_DEPTH || kind == DEPTH_1X;
   if (!make_target(d, t, kind == DEPTH_1X || kind == SCISSOR_1X ? 1 : 4, depth, kind == MS_PER_SAMPLE)) return false;
   const char *ps_src;
   switch (kind) {
   case MS_COVERAGE_OUT:
      ps_src = "float4 main(float4 pos : SV_Position, float4 c : COLOR, out uint cov : SV_Coverage) : SV_Target"
               "{ cov = ((uint)pos.x & 1) ? 0x3 : 0xd; return c; }";
      break;
   case MS_PER_SAMPLE:
      // full screen (every sample covered, wherever the host puts them)
      ps_src = "float4 main(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target"
               "{ return float4(s * 0.2 + 0.13, frac(floor(pos.x) / 7), 0.3, 1); }";
      break;
   case MS_SAMPLE_INFO:
      ps_src = "Texture2DMS<float4> t : register(t0);"
               "float4 main(float4 pos : SV_Position, float4 c : COLOR, uint cov : SV_Coverage) : SV_Target"
               "{ uint w, h, n; t.GetDimensions(w, h, n); return float4(n / 10.0, w / 100.0, cov / 16.0, 1); }";
      break;
   default:
      ps_src = "float4 main(float4 pos : SV_Position, float4 c : COLOR) : SV_Target { return c; }";
      break;
   }
   ID3DBlob *vsb = compile(kind == MS_PER_SAMPLE ? VS : MS_VS, "vs_5_0"), *psb = compile(ps_src, "ps_5_0");
   if (!vsb || !psb) return false;
   ID3D11VertexShader *vs;
   ID3D11PixelShader *ps;
   CHECK(d.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs));
   CHECK(d.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps));
   ID3D11DepthStencilState *dss = nullptr;
   if (depth) {
      D3D11_DEPTH_STENCIL_DESC dd = {};
      dd.DepthEnable = TRUE;
      dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
      dd.DepthFunc = D3D11_COMPARISON_LESS;
      CHECK(d.dev->CreateDepthStencilState(&dd, &dss));
   }
   D3D11_RASTERIZER_DESC rd = {};
   rd.FillMode = D3D11_FILL_SOLID;
   rd.CullMode = D3D11_CULL_NONE;
   rd.DepthClipEnable = TRUE;
   rd.MultisampleEnable = TRUE;
   rd.ScissorEnable = kind == SCISSOR_1X || kind == SCISSOR_4X;
   D3D11_RECT sc = {5, 3, 21, 26};
   d.ctx->RSSetScissorRects(1, &sc);
   ID3D11RasterizerState *rs;
   CHECK(d.dev->CreateRasterizerState(&rd, &rs));
   ID3D11ShaderResourceView *srv = nullptr;
   Target other;
   if (kind == MS_SAMPLE_INFO) {
      // the shader reads another 4x texture's dimensions
      if (!make_target(d, other, 4, false, true)) return false;
      CHECK(d.dev->CreateShaderResourceView(other.tex, nullptr, &srv));
      d.ctx->OMSetRenderTargets(1, &t.rtv, nullptr);
      d.ctx->PSSetShaderResources(0, 1, &srv);
   }
   d.ctx->IASetInputLayout(nullptr);
   d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   d.ctx->VSSetShader(vs, nullptr, 0);
   d.ctx->PSSetShader(ps, nullptr, 0);
   d.ctx->RSSetState(rs);
   d.ctx->OMSetDepthStencilState(dss, 0);
   d.ctx->OMSetBlendState(nullptr, nullptr, kind == MS_SAMPLE_MASK ? 0x6 : 0xffffffff);
   d.ctx->Draw(kind == MS_PER_SAMPLE ? 3 : 6, 0);

   bool ok = true;
   if (kind == MS_PER_SAMPLE) {
      // each sample read back with Load: sample s of pixel (x, y) to channel s of the output
      Target r;
      if (!make_target(d, r, 1, false)) return false;
      ID3D11ShaderResourceView *ms;
      CHECK(d.dev->CreateShaderResourceView(t.tex, nullptr, &ms));
      ID3DBlob *lb = compile("Texture2DMS<float4, 4> t : register(t0);"
                             "float4 main(float4 pos : SV_Position) : SV_Target"
                             "{ int2 p = (int2)pos.xy; return float4(t.Load(p, 0).r, t.Load(p, 1).r,"
                             " t.Load(p, 2).r, t.Load(p, 3).g); }", "ps_5_0");
      ID3DBlob *fb = compile(VS, "vs_4_0");
      if (!lb || !fb) return false;
      ID3D11PixelShader *lps;
      ID3D11VertexShader *fvs;
      CHECK(d.dev->CreatePixelShader(lb->GetBufferPointer(), lb->GetBufferSize(), nullptr, &lps));
      CHECK(d.dev->CreateVertexShader(fb->GetBufferPointer(), fb->GetBufferSize(), nullptr, &fvs));
      d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
      d.ctx->VSSetShader(fvs, nullptr, 0);
      d.ctx->PSSetShader(lps, nullptr, 0);
      d.ctx->PSSetShaderResources(0, 1, &ms);
      d.ctx->Draw(3, 0);
      ID3D11ShaderResourceView *none = nullptr;
      d.ctx->PSSetShaderResources(0, 1, &none);
      ok = read_target(d, r, out);
      lps->Release();
      fvs->Release();
      lb->Release();
      fb->Release();
      ms->Release();
      r.release();
   } else {
      ok = read_target(d, t, out);
   }
   if (srv) {
      ID3D11ShaderResourceView *none = nullptr;
      d.ctx->PSSetShaderResources(0, 1, &none);
      srv->Release();
   }
   other.release();
   d.ctx->RSSetState(nullptr);
   d.ctx->OMSetDepthStencilState(nullptr, 0);
   d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
   rs->Release();
   if (dss) dss->Release();
   vs->Release();
   ps->Release();
   vsb->Release();
   psb->Release();
   t.release();
   return ok;
}

// D24S8 depth written by a pixel shader (SV_Depth), then read through an R24_UNORM_X8_TYPELESS view
static bool run_depth_srv(Dev &d, std::vector<uint32_t> &out)
{
   Target t;
   if (!make_target(d, t, 1, false)) return false;
   D3D11_TEXTURE2D_DESC dd = {};
   dd.Width = DW;
   dd.Height = DH;
   dd.MipLevels = dd.ArraySize = 1;
   dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
   dd.SampleDesc.Count = 1;
   dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
   ID3D11Texture2D *ds;
   CHECK(d.dev->CreateTexture2D(&dd, nullptr, &ds));
   D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
   dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
   dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
   ID3D11DepthStencilView *dsv;
   CHECK(d.dev->CreateDepthStencilView(ds, &dvd, &dsv));
   D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
   sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
   sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
   sd.Texture2D.MipLevels = 1;
   ID3D11ShaderResourceView *srv;
   CHECK(d.dev->CreateShaderResourceView(ds, &sd, &srv));
   ID3DBlob *wb = compile("float main(float4 p : SV_Position) : SV_Depth"
                          "{ return (floor(p.x) * 32 + floor(p.y) + 0.5) / 1024; }", "ps_4_0");
   ID3DBlob *rb = compile("Texture2D<float> t : register(t0);"
                          "float4 main(float4 p : SV_Position) : SV_Target"
                          "{ float z = t.Load(int3(p.xy, 0)); return float4(frac(z * 32), z, 0.25, 1); }", "ps_4_0");
   if (!wb || !rb) return false;
   ID3D11PixelShader *wps, *rps;
   CHECK(d.dev->CreatePixelShader(wb->GetBufferPointer(), wb->GetBufferSize(), nullptr, &wps));
   CHECK(d.dev->CreatePixelShader(rb->GetBufferPointer(), rb->GetBufferSize(), nullptr, &rps));
   D3D11_DEPTH_STENCIL_DESC dsd = {};
   dsd.DepthEnable = TRUE;
   dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
   dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
   ID3D11DepthStencilState *dss;
   CHECK(d.dev->CreateDepthStencilState(&dsd, &dss));
   d.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
   d.ctx->OMSetRenderTargets(1, &t.rtv, dsv);
   d.ctx->OMSetDepthStencilState(dss, 0);
   d.ctx->IASetInputLayout(nullptr);
   d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   d.ctx->VSSetShader(d.vs, nullptr, 0);
   d.ctx->PSSetShader(wps, nullptr, 0);
   d.ctx->RSSetState(nullptr);
   d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
   d.ctx->Draw(3, 0);
   d.ctx->OMSetRenderTargets(1, &t.rtv, nullptr);
   d.ctx->OMSetDepthStencilState(nullptr, 0);
   d.ctx->PSSetShader(rps, nullptr, 0);
   d.ctx->PSSetShaderResources(0, 1, &srv);
   d.ctx->Draw(3, 0);
   ID3D11ShaderResourceView *none = nullptr;
   d.ctx->PSSetShaderResources(0, 1, &none);
   bool ok = read_target(d, t, out);
   dss->Release();
   wps->Release();
   rps->Release();
   wb->Release();
   rb->Release();
   srv->Release();
   dsv->Release();
   ds->Release();
   t.release();
   return ok;
}

struct DrawTest {
   const char *name;
   bool msaa;
   int kind;
   int edge_tolerance;   // per channel, for pixels the host's sample positions decide (0: exact)
};

static const DrawTest draw_tests[] = {
   {"indirect_draw", false, IND_PLAIN, 0},
   {"indirect_indexed", false, IND_INDEXED, 0},
   {"indirect_indexed_ib_offset", false, IND_INDEXED_IB_OFFSET, 0},
   {"indirect_args_from_cs", false, IND_FROM_CS, 0},
   {"depth_d24s8", true, DEPTH_1X, 1},
   {"scissor", true, SCISSOR_1X, 1},
   {"scissor_msaa", true, SCISSOR_4X, 66},
   {"depth_d24s8_srv", true, DEPTH_SRV, 0},
   {"msaa_edges", true, MS_EDGES, 66},
   {"msaa_depth", true, MS_DEPTH, 66},
   {"msaa_sample_mask", true, MS_SAMPLE_MASK, 66},
   {"msaa_coverage_out", true, MS_COVERAGE_OUT, 66},
   {"msaa_per_sample", true, MS_PER_SAMPLE, 1},
   {"msaa_sample_info", true, MS_SAMPLE_INFO, 66},
};

static int channel_diff(uint32_t a, uint32_t b)
{
   int m = 0;
   for (int c = 0; c < 32; c += 8) m = std::max(m, abs((int)((a >> c) & 255) - (int)((b >> c) & 255)));
   return m;
}

// Interior pixels (all four neighbours equal in WARP's result) must match exactly, edge pixels within the tolerance
static bool compare_draw(const DrawTest &t, const std::vector<uint32_t> &a, const std::vector<uint32_t> &b)
{
   int bad = 0, edge = 0, aa = 0;
   for (int y = 0; y < DH; y++)
      for (int x = 0; x < DW; x++) {
         int i = y * DW + x;
         uint32_t v = b[i];
         bool interior = x > 0 && y > 0 && x < DW - 1 && y < DH - 1 && b[i - 1] == v && b[i + 1] == v &&
                         b[i - DW] == v && b[i + DW] == v;
         int diff = channel_diff(a[i], b[i]);
         // resolves of multisampled targets: rounding of the average may differ by one
         int allowed = interior ? (t.msaa ? 1 : 0) : t.edge_tolerance;
         if (diff && !interior) edge++;
         if (!interior && (a[i] & 0xff) && (a[i] & 0xff) != 0xff) aa++;
         if (diff > allowed && bad++ < 6)
            printf("  %s (%d,%d): hw %08x warp %08x%s\n", t.name, x, y, a[i], b[i], interior ? "" : " (edge)");
      }
   if (getenv("TESS_MAP")) printf("  %s: %d edge pixels differ, %d partly covered (hw)\n", t.name, edge, aa);
   return !bad;
}

static bool check_quality_levels(Dev &d)
{
   static const struct { DXGI_FORMAT f; const char *name; } formats[] = {
      {DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8"}, {DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA8"},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, "RGBA16F"}, {DXGI_FORMAT_R32_FLOAT, "R32F"},
      {DXGI_FORMAT_D24_UNORM_S8_UINT, "D24S8"}, {DXGI_FORMAT_D32_FLOAT, "D32F"},
      {DXGI_FORMAT_R24G8_TYPELESS, "R24G8_TYPELESS"},
   };
   bool ok = true;
   for (auto &f : formats) {
      printf("  quality levels %-15s", f.name);
      for (UINT n = 1; n <= 16; n *= 2) {
         UINT q = 0;
         HRESULT hr = d.dev->CheckMultisampleQualityLevels(f.f, n, &q);
         printf(" %ux:%u%s", n, q, FAILED(hr) ? "!" : "");
         if (n <= 4 && f.f != DXGI_FORMAT_R24G8_TYPELESS && !q) ok = false;
      }
      UINT support = 0;
      d.dev->CheckFormatSupport(f.f, &support);
      printf("  support 0x%08x\n", support);
   }
   return ok;
}

// A 4x multisampled swap chain (windowed, the bitblt model: DXGI resolves through the driver's Blt), read back
// from the window with PrintWindow at two points; compared with WARP's
static bool msaa_swapchain_colors(Dev &d, COLORREF colors[2])
{
   WNDCLASSA wc = {};
   wc.lpfnWndProc = DefWindowProcA;
   wc.hInstance = GetModuleHandleA(nullptr);
   wc.lpszClassName = "sm5test_msaa";
   RegisterClassA(&wc);
   RECT r = {0, 0, 64, 64};
   AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
   HWND hwnd = CreateWindowA("sm5test_msaa", "msaa", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
   if (!hwnd) return false;
   IDXGIDevice *dxgidev;
   IDXGIAdapter *adapter;
   IDXGIFactory *factory;
   CHECK(d.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgidev));
   CHECK(dxgidev->GetAdapter(&adapter));
   CHECK(adapter->GetParent(__uuidof(IDXGIFactory), (void **)&factory));
   DXGI_SWAP_CHAIN_DESC sd = {};
   sd.BufferDesc.Width = 64;
   sd.BufferDesc.Height = 64;
   sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   sd.SampleDesc.Count = 4;
   sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   sd.BufferCount = 1;
   sd.OutputWindow = hwnd;
   sd.Windowed = TRUE;
   sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
   IDXGISwapChain *sc;
   CHECK(factory->CreateSwapChain(d.dev, &sd, &sc));
   ID3D11Texture2D *bb;
   CHECK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb));
   ID3D11RenderTargetView *rtv;
   CHECK(d.dev->CreateRenderTargetView(bb, nullptr, &rtv));
   bool ok = false;
   for (int frame = 0; frame < 20; frame++) {
      float red[4] = {1, 0, 0, 1};
      d.ctx->ClearRenderTargetView(rtv, red);
      D3D11_RECT left = {0, 0, 32, 64};
      // left half green through a scissored clear-by-draw: a full-screen triangle with the scissor
      D3D11_RASTERIZER_DESC rd = {};
      rd.FillMode = D3D11_FILL_SOLID;
      rd.CullMode = D3D11_CULL_NONE;
      rd.ScissorEnable = TRUE;
      rd.DepthClipEnable = TRUE;
      ID3D11RasterizerState *rs;
      CHECK(d.dev->CreateRasterizerState(&rd, &rs));
      ID3DBlob *psb = compile("float4 main(float4 p : SV_Position) : SV_Target { return float4(0, 1, 0, 1); }",
                              "ps_4_0");
      ID3D11PixelShader *ps;
      CHECK(d.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps));
      D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
      d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
      d.ctx->RSSetViewports(1, &vp);
      d.ctx->RSSetScissorRects(1, &left);
      d.ctx->RSSetState(rs);
      d.ctx->IASetInputLayout(nullptr);
      d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      d.ctx->VSSetShader(d.vs, nullptr, 0);
      d.ctx->PSSetShader(ps, nullptr, 0);
      d.ctx->Draw(3, 0);
      d.ctx->RSSetState(nullptr);
      rs->Release();
      ps->Release();
      psb->Release();
      sc->Present(0, 0);
      MSG msg;
      while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
      Sleep(30);
   }
   Sleep(300);
   HDC wdc = GetDC(hwnd), mdc = CreateCompatibleDC(wdc);
   HBITMAP bmp = CreateCompatibleBitmap(wdc, 64, 64);
   SelectObject(mdc, bmp);
   RECT wr, cr;
   GetWindowRect(hwnd, &wr);
   GetClientRect(hwnd, &cr);
   POINT origin = {0, 0};
   ClientToScreen(hwnd, &origin);
   // PrintWindow draws the whole window: the client area is offset by the frame
   HBITMAP full = CreateCompatibleBitmap(wdc, wr.right - wr.left, wr.bottom - wr.top);
   HDC fdc = CreateCompatibleDC(wdc);
   SelectObject(fdc, full);
   if (PrintWindow(hwnd, fdc, PW_RENDERFULLCONTENT)) {
      COLORREF l = GetPixel(fdc, origin.x - wr.left + 10, origin.y - wr.top + 32);
      COLORREF rt = GetPixel(fdc, origin.x - wr.left + 54, origin.y - wr.top + 32);
      printf("  msaa swap chain: left %06lx right %06lx\n", (unsigned long)l, (unsigned long)rt);
      colors[0] = l;
      colors[1] = rt;
      ok = true;
   } else {
      printf("  PrintWindow failed\n");
   }
   DeleteDC(fdc);
   DeleteObject(full);
   DeleteDC(mdc);
   DeleteObject(bmp);
   ReleaseDC(hwnd, wdc);
   d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
   rtv->Release();
   bb->Release();
   sc->Release();
   factory->Release();
   adapter->Release();
   dxgidev->Release();
   DestroyWindow(hwnd);
   return ok;
}

static int draw_compare_all(Dev &hw, bool hw_ok, Dev &warp, const char *filter, int &total)
{
   int failed = 0;
   for (const DrawTest &t : draw_tests) {
      if (filter && !strstr(t.name, filter)) continue;
      if (!startTest(sm5_title(t.name))) continue;
      total++;
      std::vector<uint32_t> a, b;
      bool ok = hw_ok && (t.msaa ? run_msaa(hw, t.kind, a) : run_indirect(hw, t.kind, a));
      if (!(t.msaa ? run_msaa(warp, t.kind, b) : run_indirect(warp, t.kind, b))) {
         report(Status::Skip, sm5_title(t.name), "WARP failed");
         continue;
      }
      bool pass = ok && compare_draw(t, a, b);
      failed += !pass;
      sm5_result(pass, t.name, !ok ? "the GPU run failed" : "");
   }
   total++;
   bool pass;
   if (startTest(sm5_title("msaa_quality_levels"))) {
      pass = hw_ok && check_quality_levels(hw);
      failed += !pass;
      sm5_result(pass, "msaa_quality_levels", "");
   }
   if (!startTest(sm5_title("msaa_swapchain"))) return failed;
   total++;
   COLORREF a[2] = {}, b[2] = {};
   pass = hw_ok && msaa_swapchain_colors(hw, a) && msaa_swapchain_colors(warp, b) && a[0] && a[1] && a[0] == b[0] &&
          a[1] == b[1];
   failed += !pass;
   sm5_result(pass, "msaa_swapchain", fmt("window shows %06lx/%06lx, WARP's %06lx/%06lx", (unsigned long)a[0],
                                          (unsigned long)a[1], (unsigned long)b[0], (unsigned long)b[1]));
   return failed;
}
