// Shader model 4/5 pixel and vertex shader tests (from the D3D10/11 driver's development tests): each shader is
// compiled at run time, rendered on the GPU and on WARP, and the pixels compared.

static const int W = 16, H = 16;

static const char *VS = R"(
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 dir : TEXCOORD1; };
V main(uint id : SV_VertexID)
{
   V v;
   float2 t = float2((id << 1) & 2, id & 2);
   v.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
   v.uv = t;
   // off the diagonals: which cube face wins a tie (|x| = |y|) is up to the implementation
   v.dir = float3((t.x * 2 - 1) * 1.07 + 0.013, t.y * 2 - 1, 0.75);
   return v;
}
)";

// Shared declarations for the pixel shaders
static const char *PS_HEADER = R"(
Texture2D<float4> albedo : register(t0);
Texture2DArray<float4> layers : register(t1);
Texture2D<uint> ids : register(t2);
Texture2D<float> depth : register(t3);
TextureCube<float4> env : register(t4);
SamplerState lin : register(s0);
SamplerComparisonState cmp : register(s1);
SamplerState pnt : register(s2);
cbuffer C : register(b0) { uint4 u; int4 s; float4 f; uint4 n; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 dir : TEXCOORD1; };
)";

// Typed buffers: t5 R32_UINT, t6 R32_SINT, t7 R32G32B32A32_FLOAT (for vertex shader tests too)
static const char *BUF_HEADER = R"(
Buffer<uint> bu : register(t5);
Buffer<int> bi : register(t6);
Buffer<float4> bf : register(t7);
)";

struct Test {
   const char *name;
   bool uint_out;      // R32G32B32A32_UINT target, else _FLOAT
   float tolerance;    // float targets: allowed |hw - warp| / (1 + |warp|); uint targets: allowed |hw - warp|
   const char *profile;
   const char *ps;
   const char *vs = nullptr;   // vs_5_0 with the buffer declarations (BUF_HEADER), else the plain VS
};

static const Test tests[] = {
   // indexed immediate constant buffers (virglrenderer ignores indirect immediates) and indexable temps written at
   // two run-time indices in one shader (it indexes temporary destinations with ADDR[0] only)
   {"idx_tables", true, 0, "ps_5_0", R"(
static const uint table[8] = { 3, 141, 59, 26, 535, 89, 79, 323 };
static const uint4 table4[5] = { uint4(1, 2, 3, 4), uint4(5, 6, 7, 8), uint4(9, 10, 11, 12), uint4(13, 14, 15, 16),
                                 uint4(17, 18, 19, 20) };
uint4 main(V i) : SV_Target
{
   uint px = (uint)i.pos.x, py = (uint)i.pos.y;
   uint a[8], b[8];
   [loop] for (uint k = 0; k < 8; k++) { a[k] = k * 7 + u.x; b[k] = k + py; }
   a[px & 7] = table[(px + py) & 7];
   b[(py * 3) & 7] = a[(px + 1) & 7] + table4[(px + 2 * py) % 5][py & 3];
   a[(px >> 1) & 7] += b[px & 7];
   return uint4(a[px & 7], b[(py * 3) & 7], a[(px + 5) & 7] ^ b[(py + 2) & 7], table4[(py + 1) % 5].w + table[7 - (px & 7)]);
})"},
   // subroutines ([call] switch: a call per case, nested), inlined (virglrenderer has no TGSI CAL/BGNSUB)
   {"cf_calls", false, 1e-5f, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   uint px = (uint)i.pos.x, py = (uint)i.pos.y;
   float4 acc = float4(0.1, 0.2, 0.3, 0.4);
   [loop] for (uint k = 0; k < 3; k++) {
      [call] switch ((px + py + k) % 4) {
      case 0:
         acc.x += 1;
         [loop] for (uint j = 0; j < px % 7; j++) {
            if (j == 2) continue;
            acc.y += 0.125;
            if (acc.y > 1.5) break;
         }
         break;
      case 1:
         [call] switch (py % 3) {
         case 0: acc.z += 0.5; break;
         case 1: acc.w -= 0.25; break;
         default: acc.xy *= 0.5; break;
         }
         break;
      case 2:
         [forcecase] switch (px % 3) {
         case 0: acc.w += 1; break;
         default: acc.z -= 0.125; break;
         }
         acc.x += 0.03125;
         break;
      default:
         acc = acc.yzwx;
         break;
      }
   }
   return acc;
})"},
   {"cf_loop_switch", false, 1e-5f, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   uint px = (uint)i.pos.x, py = (uint)i.pos.y;
   float4 acc = 0;
   [loop] for (uint k = 0; k < px + 3; k++) {
      if ((k & 1) == 0) continue;
      [forcecase] switch ((k + py) % 5) {
      case 0: acc.x += 1; break;
      case 1:
      case 2: acc.y += 1; acc.w += 0.5; break;
      default: acc.z += 1; break;
      case 4: acc.x += 0.25; break;
      }
      if (acc.x > 3) break;
   }
   return acc;
})"},
   {"cf_nested", false, 1e-5f, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   uint px = (uint)i.pos.x, py = (uint)i.pos.y;
   float4 acc = 0;
   [loop] for (uint a = 0; a < 4; a++) {
      [loop] for (uint b = 0; b < 6; b++) {
         if (b == py % 6) continue;
         if (a * b > px) break;
         [forcecase] switch (b % 3) {
         case 0:
            [forcecase] switch (a) { case 1: acc.x += 2; break; default: acc.x += 1; break; }
            break;
         case 1: acc.y += b; break;
         default: acc.z += a; break;
         }
      }
      if (acc.y > 9 + py) continue;
      acc.w += 1;
   }
   return acc;
})"},
   {"cf_sm4", false, 1e-5f, "ps_4_0", R"(
float4 main(V i) : SV_Target
{
   uint px = (uint)i.pos.x, py = (uint)i.pos.y;
   float4 acc = 0;
   [loop] for (uint k = 0; k < 8; k++) {
      if (k == px % 4) continue;
      [forcecase] switch (k + py) { case 3: acc.x += 1; break; case 7: acc.y += 2; break; default: acc.z += 0.5; break; }
   }
   return acc;
})"},
   {"tex_sample", false, 1.0f / 64, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   float4 r = albedo.Sample(lin, i.uv);
   r.z += albedo.Sample(lin, i.uv, int2(1, -1)).x;
   r.w += albedo.SampleLevel(lin, i.uv * 1.3, 1.5).y;
   r.x += albedo.SampleBias(lin, i.uv, 0.75).w;
   r.y += albedo.SampleGrad(lin, i.uv, float2(0.1, 0), float2(0, 0.2)).z;
   r.zw += albedo.Sample(pnt, i.uv, int2(-2, 3)).yx;
   return r;
})"},
   {"tex_pairs", false, 1.0f / 64, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   float4 r = albedo.Sample(pnt, i.uv * 1.7);
   r += layers.Sample(lin, float3(i.uv, 1)) * 0.5;
   r.yx += layers.SampleLevel(pnt, float3(i.uv.yx, 2), 0).xw;
   r += albedo.Sample(lin, i.uv * 0.6);
   return r;
})"},
   {"tex_int_dims", true, 0, "ps_5_0", R"(
uint4 main(V i) : SV_Target
{
   uint4 r = 0;
   r.x = ids.Load(int3(i.pos.xy / 4, 0));
   r.y = ids.Load(int3(i.pos.xy / 8, 0), int2(1, 0));
   uint w, h, levels;
   albedo.GetDimensions(1, w, h, levels);
   r.z = w * 100 + h * 10 + levels;
   float fw, fh, fl;
   albedo.GetDimensions(0, fw, fh, fl);
   uint aw, ah, ae, al;
   layers.GetDimensions(0, aw, ah, ae, al);
   r.w = (uint)(1.0 / (1.0 / fw) * 1000 + fh * 10) + aw * 100000 + ae * 1000000 + al * 10000000;
   return r;
})"},
   {"tex_cmp", false, 1.0f / 64, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   float4 r;
   float ref = i.uv.y;
   r.x = depth.SampleCmp(cmp, i.uv, ref);
   r.y = depth.SampleCmpLevelZero(cmp, i.uv, ref * 0.9);
   r.z = depth.SampleCmpLevelZero(cmp, i.uv, ref, int2(2, 0));
   r.w = depth.Sample(pnt, i.uv);
   return r;
})"},
   {"tex_gather", false, 1e-5f, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   float4 r = albedo.GatherRed(pnt, i.uv);
   r += albedo.GatherGreen(pnt, i.uv) * 2;
   r += albedo.GatherAlpha(pnt, i.uv, int2(1, 1)) * 4;
   r += depth.GatherCmpRed(cmp, i.uv, 0.5) * 8;
   int2 o = int2((uint)i.pos.x % 3, -((int)i.pos.y % 2));
   r += albedo.GatherBlue(pnt, i.uv, o) * 16;
   r += albedo.Gather(pnt, i.uv) * 32;
   return r;
})"},
   {"tex_cube", false, 1.0f / 64, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   float4 r = env.Sample(lin, i.dir);
   r += env.SampleLevel(pnt, i.dir.zxy, 0) * 0.5;
   r.wz += env.SampleBias(lin, -i.dir, 0.5).xy;
   return r;
})"},
   {"tex_lod", false, 0.1f, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   float2 uv = i.uv * (1 + i.uv.x * 3);
   return float4(albedo.CalculateLevelOfDetail(lin, uv), albedo.CalculateLevelOfDetailUnclamped(lin, uv * 0.1),
                 albedo.CalculateLevelOfDetail(lin, uv * 9), 0);
})"},
   {"alu_bits", true, 0, "ps_5_0", R"(
uint4 main(V i) : SV_Target
{
   uint px = (uint)i.pos.x, py = (uint)i.pos.y;
   uint v = u.x * (px + 1) ^ (u.y << py) ^ (px * 0x01010101u);
   int sv = s.x * (int)(px + 1) - (int)py * 77777;
   uint4 r;
   r.x = countbits(v) + firstbitlow(v) * 100 + firstbithigh(v) * 10000 + (uint)firstbithigh(sv) * 1000000 +
         (uint)firstbithigh(-sv) * 100000000;
   r.y = reversebits(v) ^ firstbitlow(0u) ^ (firstbithigh(0u) << 8);
   uint off = px * 3 % 32, wid = (py * 5 + 1) % 33;
   uint mask = wid >= 32 ? 0xffffffffu : ((1u << wid) - 1);
   r.z = (v >> off) & mask;
   r.z ^= (uint)((sv << (py % 16)) >> (px + 8));
   r.w = (v & ~(mask << off)) | ((u.z << off) & (mask << off));
   return r;
})"},
   // one unit apart allowed: f32tof16 of values that become half denormals (1e-7 and its multiples) rounds to nearest
   // in Windows 10's WARP and truncates in Windows 11's (and on the GPU)
   {"alu_half", true, 1, "ps_5_0", R"(
uint4 main(V i) : SV_Target
{
   uint px = (uint)i.pos.x, py = (uint)i.pos.y;
   uint idx = py * 16 + px;
   float vals[8] = { 0.0, -0.0, 1.0, 65504.0, 65520.0, 1e-7, -3.14159, 6.1e-5 };
   float x = vals[idx % 8] * (1 + (idx / 8) * 0.37);
   if (idx == 200) x = asfloat(0x7f800000);
   if (idx == 201) x = asfloat(0x7fc00000);
   if (idx == 202) x = asfloat(0xff800000);
   if (idx == 203) x = 5.96e-8;
   if (idx == 204) x = 1e6;
   if (idx == 205) x = 65536.0;
   if (idx == 206) x = -65535.0;
   if (idx == 207) x = asfloat(0x00000001);
   uint4 r;
   r.x = f32tof16(x);
   r.y = f32tof16(f.x * idx);
   r.z = asuint(f16tof32(idx * 257u + (idx << 9)));
   r.w = asuint(f16tof32(0x7c00 | idx)) ^ asuint(f16tof32(idx & 0x3ff)) ^ asuint(f16tof32(0x8000 | idx * 129));
   return r;
})"},
   {"buf_ps", true, 0, "ps_5_0", R"(
Buffer<uint> bu : register(t5);
Buffer<int> bi : register(t6);
Buffer<float4> bf : register(t7);
uint4 main(V i) : SV_Target
{
   uint idx = ((uint)i.pos.x + (uint)i.pos.y * 16) % 64;
   uint w, n;
   bu.GetDimensions(w);
   bf.GetDimensions(n);
   return uint4(bu.Load(idx), asuint(bi.Load(idx)), asuint(bf.Load(idx % 16).y), w * 1000 + n);
})"},
   {"buf_ps_mismatch", true, 0, "ps_5_0", R"(
Buffer<int> bim : register(t5);
uint4 main(V i) : SV_Target
{
   uint idx = ((uint)i.pos.x + (uint)i.pos.y * 16) % 64;
   return uint4(asuint(bim.Load(idx)), 0, 0, 1);
})"},
   {"buf_vs", false, 1e-5f, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   return float4(i.dir, 1);
})", R"(
Buffer<uint> bu : register(t5);
Buffer<int> bim : register(t6);
Buffer<float4> bf : register(t7);
V main(uint id : SV_VertexID)
{
   V v;
   float2 t = float2((id << 1) & 2, id & 2);
   v.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
   v.uv = t;
   v.dir = float3(bu.Load(5), bim.Load(7), bf.Load(3).z);
   return v;
})"},
   {"sb_vs", false, 1e-5f, "ps_5_0", R"(
StructuredBuffer<float4> sb : register(t8);
ByteAddressBuffer rb : register(t9);
float4 main(V i) : SV_Target
{
   uint n, stride, bytes;
   sb.GetDimensions(n, stride);
   rb.GetDimensions(bytes);
   return float4(i.dir, sb[(uint)i.pos.x % 14].w + asfloat(rb.Load((uint)i.pos.y * 4)) + n * 100 + bytes * 0.001);
})", R"(
StructuredBuffer<float4> sb : register(t8);
ByteAddressBuffer rb : register(t9);
V main(uint id : SV_VertexID)
{
   V v;
   float2 t = float2((id << 1) & 2, id & 2);
   v.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
   v.uv = t;
   v.dir = float3(sb[3].y, asfloat(rb.Load(8)), asfloat(rb.Load3(4).z));
   return v;
})"},
   {"alu_misc", false, 1e-4f, "ps_5_0", R"(
float4 main(V i) : SV_Target
{
   float4 r;
   float t = i.uv.x * i.uv.x * 7 + i.uv.y;
   r.x = rcp(1 + i.uv.x * 3);
   r.y = ddx_fine(t) + ddy_fine(t) * 3;
   // coarse derivatives may come from the quad's first row/column or the pixel's own: a linear function, where
   // both are the same
   float l = i.uv.x * 7 + i.uv.y * 3;
   r.z = ddx_coarse(l) + ddy_coarse(l) * 5;
   r.w = mad(i.uv.y, 3, 1);
   return r;
})"},
};

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("  %s failed: 0x%08lx\n", #x, hr_); return false; } } while (0)

struct Dev {
   ID3D11Device *dev = nullptr;
   ID3D11DeviceContext *ctx = nullptr;
   ID3D11VertexShader *vs = nullptr;
   ID3D11ShaderResourceView *srv[10] = {};
   ID3D11SamplerState *samp[3] = {};
   ID3D11Buffer *cb = nullptr;
   bool warp = false;   // the reference device (WARP)
};

static ID3DBlob *compile(const std::string &src, const char *profile)
{
   ID3DBlob *code = nullptr, *errors = nullptr;
   HRESULT hr = compileD3D(src.data(), src.size(), "test", "main", profile, 0, &code, &errors);
   if (errors) {
      printf("  compiler: %.*s\n", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
      errors->Release();
   }
   return SUCCEEDED(hr) ? code : nullptr;
}

static uint32_t rgba(int r, int g, int b, int a) { return r | g << 8 | b << 16 | (uint32_t)a << 24; }

// The depth texture: an 8x8 R32_TYPELESS depth buffer, drawn with depth = (x + 2y) / 24 by a shader writing SV_Depth
static bool make_depth(Dev &d)
{
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = td.Height = 8;
   td.MipLevels = td.ArraySize = 1;
   td.Format = DXGI_FORMAT_R32_TYPELESS;
   td.SampleDesc.Count = 1;
   td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
   ID3D11Texture2D *tex;
   CHECK(d.dev->CreateTexture2D(&td, nullptr, &tex));
   D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
   dd.Format = DXGI_FORMAT_D32_FLOAT;
   dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
   ID3D11DepthStencilView *dsv;
   CHECK(d.dev->CreateDepthStencilView(tex, &dd, &dsv));
   D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
   sd.Format = DXGI_FORMAT_R32_FLOAT;
   sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
   sd.Texture2D.MipLevels = 1;
   CHECK(d.dev->CreateShaderResourceView(tex, &sd, &d.srv[3]));

   ID3DBlob *ps = compile("float main(float4 p : SV_Position) : SV_Depth { return (floor(p.x) + 2 * floor(p.y)) / 24; }",
                          "ps_4_0");
   if (!ps) return false;
   ID3D11PixelShader *pshader;
   CHECK(d.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pshader));
   D3D11_DEPTH_STENCIL_DESC dsd = {};
   dsd.DepthEnable = TRUE;
   dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
   dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
   ID3D11DepthStencilState *dss;
   CHECK(d.dev->CreateDepthStencilState(&dsd, &dss));
   d.ctx->OMSetRenderTargets(0, nullptr, dsv);
   d.ctx->OMSetDepthStencilState(dss, 0);
   D3D11_VIEWPORT vp = {0, 0, 8, 8, 0, 1};
   d.ctx->RSSetViewports(1, &vp);
   d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   d.ctx->VSSetShader(d.vs, nullptr, 0);
   d.ctx->PSSetShader(pshader, nullptr, 0);
   d.ctx->Draw(3, 0);
   d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
   d.ctx->OMSetDepthStencilState(nullptr, 0);
   return true;
}

static bool setup(Dev &d, bool warp)
{
   // 11_1 where offered (its tests check), else 11_0
   const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
   D3D_FEATURE_LEVEL got;
   d.warp = warp;
   CHECK(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2,
                           D3D11_SDK_VERSION, &d.dev, &got, &d.ctx));
   ID3DBlob *vs = compile(VS, "vs_4_0");
   if (!vs) return false;
   CHECK(d.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &d.vs));

   // t0: 8x8 RGBA8, 4 mips
   {
      std::vector<uint32_t> data;
      D3D11_SUBRESOURCE_DATA init[4];
      size_t offsets[4];
      for (int m = 0, size = 8; m < 4; m++, size /= 2) {
         offsets[m] = data.size();
         for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
               data.push_back(rgba(x * 255 / 7, y * 255 / 7, (x * 37 + y * 11 + m * 64) & 255, 255 - m * 60));
      }
      for (int m = 0, size = 8; m < 4; m++, size /= 2) {
         init[m].pSysMem = data.data() + offsets[m];
         init[m].SysMemPitch = size * 4;
      }
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = 8;
      td.MipLevels = 4;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      ID3D11Texture2D *tex;
      CHECK(d.dev->CreateTexture2D(&td, init, &tex));
      CHECK(d.dev->CreateShaderResourceView(tex, nullptr, &d.srv[0]));
   }
   // t1: 4x4x3 RGBA8 array
   {
      std::vector<uint32_t> data;
      D3D11_SUBRESOURCE_DATA init[3];
      for (int l = 0; l < 3; l++)
         for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
               data.push_back(rgba(l * 120, x * 80, y * 80, 255 - x * 40));
      for (int l = 0; l < 3; l++) {
         init[l].pSysMem = data.data() + l * 16;
         init[l].SysMemPitch = 16;
      }
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = 4;
      td.MipLevels = 1;
      td.ArraySize = 3;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      ID3D11Texture2D *tex;
      CHECK(d.dev->CreateTexture2D(&td, init, &tex));
      CHECK(d.dev->CreateShaderResourceView(tex, nullptr, &d.srv[1]));
   }
   // t2: 4x4 R32_UINT
   {
      uint32_t data[16];
      for (int i = 0; i < 16; i++) data[i] = 1000 + i * 7919u;
      D3D11_SUBRESOURCE_DATA init = {data, 16, 0};
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = 4;
      td.MipLevels = td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R32_UINT;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      ID3D11Texture2D *tex;
      CHECK(d.dev->CreateTexture2D(&td, &init, &tex));
      CHECK(d.dev->CreateShaderResourceView(tex, nullptr, &d.srv[2]));
   }
   // t4: 4x4 cube, a color per face with a gradient
   {
      std::vector<uint32_t> data;
      D3D11_SUBRESOURCE_DATA init[6];
      for (int f = 0; f < 6; f++)
         for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
               data.push_back(rgba(f * 40 + x * 10, 255 - f * 40, y * 60, 128 + f * 20));
      for (int f = 0; f < 6; f++) {
         init[f].pSysMem = data.data() + f * 16;
         init[f].SysMemPitch = 16;
      }
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = 4;
      td.MipLevels = 1;
      td.ArraySize = 6;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
      ID3D11Texture2D *tex;
      CHECK(d.dev->CreateTexture2D(&td, init, &tex));
      CHECK(d.dev->CreateShaderResourceView(tex, nullptr, &d.srv[4]));
   }
   if (!make_depth(d)) return false;
   // t5/t6: 64 R32_UINT / R32_SINT, t7: 16 R32G32B32A32_FLOAT
   for (int b = 0; b < 3; b++) {
      uint32_t data[64];
      for (int i = 0; i < 64; i++) {
         if (b == 2) {
            float f = i * 0.25f - 3;
            memcpy(&data[i], &f, 4);
         } else {
            data[i] = b == 0 ? 0x80000000u + i * 2654435761u % 100000 : (uint32_t)(i * 37 - 1000);
         }
      }
      D3D11_BUFFER_DESC bd = {};
      bd.ByteWidth = sizeof data;
      bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
      ID3D11Buffer *buf;
      CHECK(d.dev->CreateBuffer(&bd, &init, &buf));
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = b == 0 ? DXGI_FORMAT_R32_UINT : b == 1 ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      sd.Buffer.NumElements = b == 2 ? 16 : 64;
      CHECK(d.dev->CreateShaderResourceView(buf, &sd, &d.srv[5 + b]));
   }
   // t8: StructuredBuffer<float4> (16 elements), t9: ByteAddressBuffer (256 bytes)
   {
      float f[64];
      for (int i = 0; i < 64; i++) f[i] = i * 0.75f - 4;
      D3D11_BUFFER_DESC bd = {};
      bd.ByteWidth = sizeof f;
      bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      bd.StructureByteStride = 16;
      D3D11_SUBRESOURCE_DATA init = {f, 0, 0};
      ID3D11Buffer *sb, *rb;
      CHECK(d.dev->CreateBuffer(&bd, &init, &sb));
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
      bd.StructureByteStride = 0;
      CHECK(d.dev->CreateBuffer(&bd, &init, &rb));
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      sd.Format = DXGI_FORMAT_UNKNOWN;
      sd.Buffer.FirstElement = 2;
      sd.Buffer.NumElements = 14;
      CHECK(d.dev->CreateShaderResourceView(sb, &sd, &d.srv[8]));
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
      sd.Format = DXGI_FORMAT_R32_TYPELESS;
      sd.BufferEx.FirstElement = 4;
      sd.BufferEx.NumElements = 60;
      sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      CHECK(d.dev->CreateShaderResourceView(rb, &sd, &d.srv[9]));
   }

   D3D11_SAMPLER_DESC sd = {};
   sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
   sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
   sd.MaxLOD = D3D11_FLOAT32_MAX;
   CHECK(d.dev->CreateSamplerState(&sd, &d.samp[0]));
   sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
   sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
   sd.ComparisonFunc = D3D11_COMPARISON_LESS;
   CHECK(d.dev->CreateSamplerState(&sd, &d.samp[1]));
   sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
   sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
   CHECK(d.dev->CreateSamplerState(&sd, &d.samp[2]));

   struct { uint32_t u[4]; int32_t s[4]; float f[4]; uint32_t n[4]; } cbdata = {
      {0x9e3779b9u, 0x00f0f00fu, 0xdeadbeefu, 7}, {-123457, 98765, -1, 0}, {0.37f, -2.5f, 1e-6f, 1000.0f}, {5, 3, 0, 0}};
   D3D11_BUFFER_DESC bd = {};
   bd.ByteWidth = sizeof cbdata;
   bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   D3D11_SUBRESOURCE_DATA cbinit = {&cbdata, 0, 0};
   CHECK(d.dev->CreateBuffer(&bd, &cbinit, &d.cb));
   return true;
}

// Renders a test into a 16x16 RGBA32 target and reads it back
static bool run(Dev &d, const Test &t, std::vector<uint32_t> &out)
{
   ID3DBlob *ps = compile(std::string(PS_HEADER) + t.ps, t.profile);
   if (!ps) return false;
   ID3D11VertexShader *vshader = d.vs;
   if (t.vs) {
      ID3DBlob *vsb = compile(std::string("struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 dir : TEXCOORD1; };\n") + t.vs, "vs_5_0");
      if (!vsb) return false;
      CHECK(d.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vshader));
   }
   ID3D11PixelShader *pshader;
   CHECK(d.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pshader));
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = W;
   td.Height = H;
   td.MipLevels = td.ArraySize = 1;
   td.Format = t.uint_out ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
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
   float clear[4] = {0, 0, 0, 0};
   d.ctx->ClearRenderTargetView(rtv, clear);
   d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
   D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
   d.ctx->RSSetViewports(1, &vp);
   d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   d.ctx->VSSetShader(vshader, nullptr, 0);
   d.ctx->PSSetShader(pshader, nullptr, 0);
   d.ctx->PSSetShaderResources(0, 10, d.srv);
   d.ctx->VSSetShaderResources(0, 10, d.srv);
   d.ctx->PSSetSamplers(0, 3, d.samp);
   d.ctx->PSSetConstantBuffers(0, 1, &d.cb);
   d.ctx->Draw(3, 0);
   d.ctx->CopyResource(staging, rt);
   D3D11_MAPPED_SUBRESOURCE m;
   CHECK(d.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
   out.resize(W * H * 4);
   for (int y = 0; y < H; y++)
      memcpy(&out[y * W * 4], (char *)m.pData + y * m.RowPitch, W * 16);
   d.ctx->Unmap(staging, 0);
   d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
   rtv->Release();
   rt->Release();
   staging->Release();
   pshader->Release();
   ps->Release();
   return true;
}


