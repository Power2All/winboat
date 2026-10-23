// Tessellation tests (included by sm5test.cpp): one patch covering the 16x16 target (so the tessellation's edges
// don't decide coverage), its domain coordinates and patch constants output as colors, compared with WARP within a
// tolerance. The vertex shader makes the control points from SV_VertexID: CP_COUNT 4 a quad over the target, 3 a
// triangle larger than it; PATCHES 2: two quads side by side. Isolines and points: drawn on pixel centers.

static const char *TESS_VS = R"(
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
CP main(uint id : SV_VertexID)
{
   CP c;
   float2 quad[4] = { float2(-1, 1), float2(1, 1), float2(1, -1), float2(-1, -1) };
   float2 tri[3] = { float2(-1, 1), float2(3, 1), float2(-1, -3) };
   float2 p = CP_COUNT == 4 ? quad[id % 4] : tri[id % 3];
   if (PATCHES == 2) p.x = p.x * 0.5 - 0.5 + id / CP_COUNT;
   c.pos = float4(p, 0.5, 1);
   c.uv = p * 0.5 + 0.5;
   return c;
}
)";

static const char *TESS_PS = R"(
float4 main(float4 pos : SV_Position, float4 c : COLOR) : SV_Target
{
   return c;
}
)";

struct TessTest {
   const char *name;
   int cps;              // control points per patch
   const char *hs;
   const char *ds;
   int patches = 1;
   // instead of WARP (whose isolines are wrong, and differ from run to run): the expected map of drawn pixels and
   // x's step value (tess_map: '.' not drawn, '0'/'1', '+' between), rows in order
   const char *expect = nullptr;
};

static std::string tess_map(const std::vector<uint32_t> &v, int y)
{
   std::string row;
   for (int x = 0; x < W; x++) {
      float w, r;
      memcpy(&w, &v[(y * W + x) * 4 + 3], 4);
      memcpy(&r, &v[(y * W + x) * 4], 4);
      row += w == 0 ? '.' : r < 0.01f ? '0' : r > 0.99f ? '1' : '+';
   }
   return row;
}

static const TessTest tess_tests[] = {
   {"tess_quad", 4, R"(
cbuffer C : register(b0) { float4 tf; };
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct HSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float w : WEIGHT; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; float3 extra : EXTRA; };
PC pcf(InputPatch<CP, 4> ip, OutputPatch<HSOut, 4> op, uint pid : SV_PrimitiveID)
{
   PC o;
   [unroll] for (uint i = 0; i < 4; i++) o.edges[i] = 3 + i + op[i].w;
   o.inside[0] = 4; o.inside[1] = 5;
   o.extra = float3(ip[1].uv, 0.25 + pid);
   return o;
}
[domain("quad")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(4)]
[patchconstantfunc("pcf")] [maxtessfactor(32.0)]
HSOut main(InputPatch<CP, 4> ip, uint i : SV_OutputControlPointID)
{
   HSOut o;
   o.pos = ip[i].pos;
   o.uv = ip[(i + 1) % 4].uv;
   o.w = i * 0.5;
   return o;
})", R"(
struct HSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float w : WEIGHT; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; float3 extra : EXTRA; };
struct DSOut { float4 pos : SV_Position; float4 c : COLOR; };
[domain("quad")]
DSOut main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<HSOut, 4> p)
{
   DSOut o;
   float4 a = lerp(p[0].pos, p[1].pos, uv.x), b = lerp(p[3].pos, p[2].pos, uv.x);
   o.pos = lerp(a, b, uv.y);
   float2 t = lerp(lerp(p[0].uv, p[1].uv, uv.x), lerp(p[3].uv, p[2].uv, uv.x), uv.y);
   o.c = float4(t, pc.extra.z + pc.edges[1] * 0.01, pc.extra.x + p[2].w);
   return o;
})"},
   {"tess_tri_fractional", 3, R"(
cbuffer C : register(b0) { float4 tf; };
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct PC { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
PC pcf(InputPatch<CP, 3> ip)
{
   PC o;
   o.edges[0] = 3.5; o.edges[1] = 2.25; o.edges[2] = 5.75;
   o.inside = 4.5;
   return o;
}
[domain("tri")] [partitioning("fractional_odd")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)]
[patchconstantfunc("pcf")]
CP main(InputPatch<CP, 3> ip, uint i : SV_OutputControlPointID)
{
   return ip[i];
})", R"(
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct PC { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
struct DSOut { float4 pos : SV_Position; float4 c : COLOR; };
[domain("tri")]
DSOut main(PC pc, float3 b : SV_DomainLocation, const OutputPatch<CP, 3> p)
{
   DSOut o;
   o.pos = p[0].pos * b.x + p[1].pos * b.y + p[2].pos * b.z;
   float2 uv = p[0].uv * b.x + p[1].uv * b.y + p[2].uv * b.z;
   o.c = float4(uv, pc.inside * 0.1, pc.edges[2] * 0.1);
   return o;
})"},
   {"tess_passthrough", 4, R"(
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };
PC pcf(InputPatch<CP, 4> ip)
{
   PC o;
   o.edges[0] = o.edges[1] = o.edges[2] = o.edges[3] = 2;
   o.inside[0] = o.inside[1] = 3;
   return o;
}
[domain("quad")] [partitioning("integer")] [outputtopology("triangle_ccw")] [outputcontrolpoints(4)]
[patchconstantfunc("pcf")]
void main(InputPatch<CP, 4> ip)
{
})", R"(
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };
struct DSOut { float4 pos : SV_Position; float4 c : COLOR; };
[domain("quad")]
DSOut main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<CP, 4> p)
{
   DSOut o;
   o.pos = lerp(lerp(p[0].pos, p[1].pos, uv.x), lerp(p[3].pos, p[2].pos, uv.x), uv.y);
   o.c = float4(lerp(lerp(p[0].uv, p[1].uv, uv.x), lerp(p[3].uv, p[2].uv, uv.x), uv.y), 1, 0.5);
   return o;
})"},
   {"tess_isoline", 4, R"(
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct PC { float f[2] : SV_TessFactor; };
PC pcf(InputPatch<CP, 4> ip)
{
   PC o;
   o.f[0] = 3; o.f[1] = 4;
   return o;
}
[domain("isoline")] [partitioning("integer")] [outputtopology("line")] [outputcontrolpoints(4)]
[patchconstantfunc("pcf")]
CP main(InputPatch<CP, 4> ip, uint i : SV_OutputControlPointID)
{
   return ip[i];
})", R"(
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct PC { float f[2] : SV_TessFactor; };
struct DSOut { float4 pos : SV_Position; float4 c : COLOR; };
[domain("isoline")]
DSOut main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<CP, 4> p)
{
   // lines on pixel rows 0, 4, 8, ... (v = 0, 1/n, ...), across the target; a step half way (segments show)
   DSOut o;
   o.pos = float4(-1.25 + 2.5 * uv.x, 1 - (uv.y * 16 + 0.5) / 8, 0.5, 1);
   o.c = float4(uv.x < 0.5 ? 0 : 1, uv.y, pc.f[0] * 0.1, 1);
   return o;
})", 1,
   // SV_TessFactor[0] is the line density (3 lines: v = 0, 1/3, 2/3), [1] the detail (4 segments: the step ramps
   // from u = 1/4 to 1/2)
   "000+++++11111111"
   "................"
   "................"
   "................"
   "................"
   "000+++++11111111"
   "................"
   "................"
   "................"
   "................"
   "................"
   "000+++++11111111"
   "................"
   "................"
   "................"
   "................"},
   {"tess_points", 4, R"(
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };
PC pcf(InputPatch<CP, 4> ip)
{
   PC o;
   o.edges[0] = o.edges[1] = o.edges[2] = o.edges[3] = 3;
   o.inside[0] = o.inside[1] = 3;
   return o;
}
[domain("quad")] [partitioning("integer")] [outputtopology("point")] [outputcontrolpoints(4)]
[patchconstantfunc("pcf")]
CP main(InputPatch<CP, 4> ip, uint i : SV_OutputControlPointID)
{
   return ip[i];
})", R"(
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };
struct DSOut { float4 pos : SV_Position; float4 c : COLOR; };
[domain("quad")]
DSOut main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<CP, 4> p)
{
   // a 4x4 grid on pixel centers (columns and rows 1, 5, 9, 13)
   DSOut o;
   o.pos = float4(-1 + (2 * (1 + 12 * uv.x) + 1) / 16, 1 - (2 * (1 + 12 * uv.y) + 1) / 16, 0.5, 1);
   o.c = float4(uv, 0.5, 1);
   return o;
})"},
   {"tess_two_patches", 4, R"(
struct CP { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct HSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; uint pid : PRIM; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; float tag : TAG; };
PC pcf(InputPatch<CP, 4> ip, uint pid : SV_PrimitiveID)
{
   PC o;
   o.edges[0] = o.edges[1] = o.edges[2] = o.edges[3] = 2 + pid;
   o.inside[0] = o.inside[1] = 2;
   o.tag = 0.25 + 0.5 * pid;
   return o;
}
[domain("quad")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(4)]
[patchconstantfunc("pcf")]
HSOut main(InputPatch<CP, 4> ip, uint i : SV_OutputControlPointID, uint pid : SV_PrimitiveID)
{
   HSOut o;
   o.pos = ip[i].pos;
   o.uv = ip[i].uv;
   o.pid = pid * 10 + i;
   return o;
})", R"(
struct HSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; uint pid : PRIM; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; float tag : TAG; };
struct DSOut { float4 pos : SV_Position; float4 c : COLOR; };
[domain("quad")]
DSOut main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<HSOut, 4> p, uint pid : SV_PrimitiveID)
{
   DSOut o;
   o.pos = lerp(lerp(p[0].pos, p[1].pos, uv.x), lerp(p[3].pos, p[2].pos, uv.x), uv.y);
   o.c = float4(lerp(lerp(p[0].uv, p[1].uv, uv.x), lerp(p[3].uv, p[2].uv, uv.x), uv.y),
                pc.tag + pc.edges[2] * 0.01, pid + p[3].pid * 0.01);
   return o;
})", 2},
};

// The winding tests: tess_quad and tess_passthrough with back faces culled (whether anything is drawn shows the
// winding)
static bool tess_run(Dev &d, const TessTest &t, D3D11_CULL_MODE cull, std::vector<uint32_t> &out)
{
   std::string defines = "#define CP_COUNT " + std::to_string(t.cps) + "\n#define PATCHES " + std::to_string(t.patches) + "\n";
   ID3DBlob *vs = compile(defines + TESS_VS, "vs_5_0"), *hs = compile(t.hs, "hs_5_0"), *ds = compile(t.ds, "ds_5_0"),
            *ps = compile(TESS_PS, "ps_5_0");
   if (!vs || !hs || !ds || !ps) return false;
   ID3D11VertexShader *vshader;
   ID3D11HullShader *hshader;
   ID3D11DomainShader *dshader;
   ID3D11PixelShader *pshader;
   CHECK(d.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vshader));
   CHECK(d.dev->CreateHullShader(hs->GetBufferPointer(), hs->GetBufferSize(), nullptr, &hshader));
   CHECK(d.dev->CreateDomainShader(ds->GetBufferPointer(), ds->GetBufferSize(), nullptr, &dshader));
   CHECK(d.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pshader));
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = W;
   td.Height = H;
   td.MipLevels = td.ArraySize = 1;
   td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
   td.SampleDesc.Count = 1;
   td.BindFlags = D3D11_BIND_RENDER_TARGET;
   ID3D11Texture2D *rt, *staging;
   CHECK(d.dev->CreateTexture2D(&td, nullptr, &rt));
   td.BindFlags = 0;
   td.Usage = D3D11_USAGE_STAGING;
   td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   CHECK(d.dev->CreateTexture2D(&td, nullptr, &staging));
   ID3D11RenderTargetView *rtv;
   CHECK(d.dev->CreateRenderTargetView(rt, nullptr, &rtv));
   D3D11_RASTERIZER_DESC rd = {};
   rd.FillMode = D3D11_FILL_SOLID;
   rd.CullMode = cull;
   rd.DepthClipEnable = TRUE;
   ID3D11RasterizerState *rs;
   CHECK(d.dev->CreateRasterizerState(&rd, &rs));
   float clear[4] = {0, 0, 0, 0};
   d.ctx->ClearRenderTargetView(rtv, clear);
   d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
   D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
   d.ctx->RSSetViewports(1, &vp);
   d.ctx->RSSetState(rs);
   d.ctx->IASetPrimitiveTopology((D3D11_PRIMITIVE_TOPOLOGY)(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + t.cps - 1));
   d.ctx->VSSetShader(vshader, nullptr, 0);
   d.ctx->HSSetShader(hshader, nullptr, 0);
   d.ctx->DSSetShader(dshader, nullptr, 0);
   d.ctx->PSSetShader(pshader, nullptr, 0);
   d.ctx->HSSetConstantBuffers(0, 1, &d.cb);
   d.ctx->Draw(t.cps * t.patches, 0);
   d.ctx->HSSetShader(nullptr, nullptr, 0);
   d.ctx->DSSetShader(nullptr, nullptr, 0);
   d.ctx->RSSetState(nullptr);
   d.ctx->CopyResource(staging, rt);
   D3D11_MAPPED_SUBRESOURCE m;
   CHECK(d.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
   out.resize(W * H * 4);
   for (int y = 0; y < H; y++)
      memcpy(&out[y * W * 4], (char *)m.pData + y * m.RowPitch, W * 16);
   d.ctx->Unmap(staging, 0);
   d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
   return true;
}

static int tess_compare_all(Dev &hw, bool hw_ok, Dev &warp, const char *filter, int &total)
{
   int failed = 0;
   for (const TessTest &t : tess_tests) {
      for (int c = 0; c < 2; c++) {
         D3D11_CULL_MODE cull = c ? D3D11_CULL_BACK : D3D11_CULL_NONE;
         std::string name = std::string(t.name) + (c ? "_cull_back" : "");
         if (filter && !strstr(name.c_str(), filter)) continue;
         if (!startTest(sm5_title(name))) continue;
         total++;
         std::vector<uint32_t> a, b;
         bool ok = hw_ok && tess_run(hw, t, cull, a);
         if (t.expect) {
            std::string got;
            for (int y = 0; y < H; y++) got += tess_map(a, y);
            bool pass = ok && got == t.expect;
            failed += !pass;
            for (int y = 0; ok && !pass && y < H; y++)
               printf("  %s | %.*s\n", got.substr(y * W, W).c_str(), W, t.expect + y * W);
            sm5_result(pass, name, !ok ? "the GPU run failed" : "compared with the expected image (WARP's isolines are wrong)");
            continue;
         }
         if (!tess_run(warp, t, cull, b)) {
            report(Status::Skip, sm5_title(name), "WARP failed");
            continue;
         }
         int bad = 0, drawn_a = 0, drawn_b = 0;
         for (int i = 0; ok && i < W * H * 4; i++) {
            float fa, fb;
            memcpy(&fa, &a[i], 4);
            memcpy(&fb, &b[i], 4);
            drawn_a += i % 4 == 3 && fa != 0;
            drawn_b += i % 4 == 3 && fb != 0;
            if (fabsf(fa - fb) > 0.02f * (1 + fabsf(fb)) && bad++ < 6)
               printf("  %s (%d,%d).%c: hw %g warp %g\n", name.c_str(), i / 4 % W, i / 4 / W, "xyzw"[i % 4], fa, fb);
         }
         bool pass = ok && !bad;
         if (ok && bad && getenv("TESS_MAP")) {
            // which pixels are drawn (alpha set), hardware | WARP, with x's step value (0/1, '+' between)
            for (int y = 0; y < H; y++)
               printf("  %s | %s\n", tess_map(a, y).c_str(), tess_map(b, y).c_str());
         }
         failed += !pass;
         sm5_result(pass, name, !ok ? "the GPU run failed" : fmt("drawn: GPU %d, WARP %d pixels", drawn_a, drawn_b));
      }
   }
   return failed;
}
