// Compute shader and UAV tests (included by sm5test.cpp): each test runs on fresh, zeroed UAVs
//   u0 RWStructuredBuffer (256 x 16 bytes), u1 RWByteAddressBuffer (4096 bytes), u2 RWTexture2D<float4> 16x16
//   (R32G32B32A32_FLOAT), u3 RWBuffer<uint> 64 (R32_UINT), u4 Append/ConsumeStructuredBuffer<uint> 256 with counter
// and inputs t0 StructuredBuffer<float4> 64, t1 ByteAddressBuffer 1024 bytes, t2 Buffer<float4> 64, t3 the
// 8x8 mipmapped RGBA8 texture, s0 linear sampler. Compared: the UAVs a test names, exactly (u2 with a tolerance),
// u4 as its count and sorted contents (append order is the hardware's choice).

static const char *CS_HEADER = R"(
RWStructuredBuffer<uint4> u0 : register(u0);
RWByteAddressBuffer u1 : register(u1);
RWTexture2D<float4> u2 : register(u2);
RWBuffer<uint> u3 : register(u3);
RWTexture2D<uint> u5 : register(u5);
RWTexture2DArray<unorm float4> u6 : register(u6);
RWTexture3D<float> u7 : register(u7);
StructuredBuffer<float4> t0 : register(t0);
ByteAddressBuffer t1 : register(t1);
Buffer<float4> t2 : register(t2);
Texture2D<float4> t3 : register(t3);
SamplerState s0 : register(s0);
cbuffer C : register(b0) { uint4 cu; float4 cf; };
)";

struct CsTest {
   const char *name;
   unsigned compare;     // bit i: compare u<i>
   const char *cs;       // after CS_HEADER; null: a pixel shader test (ps) or a clear test
   UINT groups[3];
   const char *ps = nullptr;   // pixel shader writing UAVs (u1, u2, u3 here, after one render target)
   int special = 0;            // 1: indirect dispatch, 2: UAV clears + CopyStructureCount
};

static const CsTest cs_tests[] = {
   {"cs_basic", 0xf, R"(
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
   u0[dtid.y * 16 + dtid.x] = uint4(dtid.xy, gid.x * 10 + gid.y, gi);
   u2[dtid.xy] = float4(gtid.x / 8.0, gtid.y / 8.0, gid.x + gid.y, 1);
   if (dtid.y == 0) u3[dtid.x] = dtid.x * 3 + 1;
   u1.Store(dtid.y * 64 + dtid.x * 4, gi ^ 0x5a5a);
})", {2, 2, 1}},
   {"cs_shared", 0x1, R"(
groupshared float sh[64];
groupshared uint shu[64];
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
   sh[gi] = t0[gi].x + gid.x;
   shu[gi] = gi * 7 + gid.x;
   GroupMemoryBarrierWithGroupSync();
   [unroll] for (uint s = 32; s > 0; s >>= 1) {
      if (gi < s) { sh[gi] += sh[gi + s]; shu[gi] = max(shu[gi], shu[gi + s]); }
      GroupMemoryBarrierWithGroupSync();
   }
   if (gi == 0) u0[gid.x] = uint4(asuint(sh[0]), shu[0], gid.x, 7);
})", {4, 1, 1}},
   {"cs_atomics", 0xb, R"(
groupshared uint gsum;
[numthreads(32, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID)
{
   if (gi == 0) gsum = 0;
   GroupMemoryBarrierWithGroupSync();
   InterlockedAdd(gsum, dtid.x + 1);
   GroupMemoryBarrierWithGroupSync();
   if (gi == 0) u0[gid.x] = uint4(gsum, 0, 0, 0);
   u1.InterlockedAdd(0, 1);
   u1.InterlockedMax(4, dtid.x * 13 % 97);
   u1.InterlockedMin(8, 1000 - dtid.x);
   u1.InterlockedOr(12, 1u << (dtid.x % 32));
   // every thread tries to swap 0 for 5: one per slot succeeds (which one is the hardware's choice), count them
   uint prev;
   u1.InterlockedCompareExchange(16 + (dtid.x % 8) * 4, 0, 5, prev);
   if (prev == 0) u1.InterlockedAdd(52, 1);
   InterlockedAdd(u3[dtid.x % 4], 2);
   InterlockedMax(u3[8], dtid.x);
   int iv = (int)dtid.x - 50;
   u1.InterlockedMin(48, asuint(iv));
})", {4, 1, 1}},
   {"cs_structured_raw", 0x3, R"(
struct S { float3 a; uint b; };
StructuredBuffer<S> ts : register(t0);
[numthreads(16, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
   uint i = dtid.x;
   S s = ts[i];
   uint4 r = t1.Load4(i * 16);
   uint2 r2 = t1.Load2(i * 8 + 4);
   u0[i] = uint4(asuint(s.a.y + s.a.z), s.b, r.y ^ r2.x, r2.y);
   u1.Store3(i * 12, uint3(r.x, r.w, asuint(t2[i].z)));
   uint n, stride;
   ts.GetDimensions(n, stride);
   uint bytes;
   t1.GetDimensions(bytes);
   if (i == 0) u1.Store2(1024, uint2(n * 1000 + stride, bytes));
   u0[100 + i].y = i * 5;
})", {2, 1, 1}},
   {"cs_append", 0x10, R"(
AppendStructuredBuffer<uint> app : register(u4);
[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
   if (dtid.x % 3 != 1) app.Append(dtid.x * 2 + 1);
})", {2, 1, 1}},
   {"cs_consume", 0x11, R"(
ConsumeStructuredBuffer<uint> con : register(u4);
[numthreads(16, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
   uint v = con.Consume();
   u0[v % 256].x = 1;
})", {1, 1, 1}},
   {"cs_texture", 0x4, R"(
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
   float2 uv = (dtid.xy + 0.5) / 16.0;
   float4 c = t3.SampleLevel(s0, uv, 0.5) + t3.Load(int3(dtid.xy / 2, 0)) * 0.25;
   uint w, h, levels;
   t3.GetDimensions(0, w, h, levels);
   uint uw, uh;
   u2.GetDimensions(uw, uh);
   u2[dtid.xy] = c + float4(0, 0, 0, w + h * 10 + levels * 100 + uw * 1000 + uh * 10000);
})", {2, 2, 1}},
   {"cs_indirect", 0x1, R"(
[numthreads(4, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
   u0[dtid.x + gid.y * 12] = uint4(dtid.x, gid.x, gid.y, gid.z);
})", {0, 0, 0}, nullptr, 1},
   {"ps_uav", 0xe, nullptr, {0, 0, 0}, R"(
RWByteAddressBuffer u1 : register(u1);
RWTexture2D<float4> u2 : register(u2);
RWBuffer<uint> u3 : register(u3);
float4 main(float4 p : SV_Position) : SV_Target
{
   uint2 xy = (uint2)p.xy;
   u1.InterlockedAdd(0, 1);
   u1.InterlockedMax(4, xy.x * 100 + xy.y);
   u2[xy] = float4(xy / 16.0, 0.5, 1);
   InterlockedAdd(u3[xy.y % 8], xy.x);
   return 0;
})"},
   {"uav_clear", 0x1f, nullptr, {0, 0, 0}, nullptr, 2},
   {"cs_uav_textures", 0xea, R"(
[numthreads(4, 4, 4)]
void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
   InterlockedAdd(u5[dtid.xy % 16], dtid.z + 1);
   InterlockedMax(u3[0], gi);
   if (dtid.z < 3 && all(dtid.xy < 8)) u6[uint3(dtid.xy, dtid.z)] = float4(dtid.x / 8.0, dtid.y / 8.0, dtid.z / 3.0, 1);
   if (all(dtid < 4)) u7[dtid] = dtid.x + dtid.y * 10 + dtid.z * 100;
   uint w, h, e, d3w, d3h, d3d;
   u6.GetDimensions(w, h, e);
   u7.GetDimensions(d3w, d3h, d3d);
   if (gi == 0 && all(dtid == 0)) u1.Store(0, w + h * 10 + e * 100 + d3w * 1000 + d3h * 10000 + d3d * 100000);
})", {4, 4, 1}},
   {"cs_typed_load", 0x80, R"(
[numthreads(4, 4, 4)]
void main(uint3 dtid : SV_DispatchThreadID)
{
   u7[dtid] = dtid.x + dtid.y * 4 + dtid.z * 16;
   DeviceMemoryBarrierWithGroupSync();
   float v = u7[uint3(3 - dtid.x, dtid.y, dtid.z)];
   DeviceMemoryBarrierWithGroupSync();
   u7[dtid] = v * 2 + cf.x;
})", {1, 1, 1}},
   {"cs_typed_load_2d", 0x20, R"(
[numthreads(4, 4, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
   u5[dtid.xy] = dtid.x * 10 + dtid.y;
   DeviceMemoryBarrierWithGroupSync();
   uint v = u5[uint2(3 - dtid.x, dtid.y)];
   DeviceMemoryBarrierWithGroupSync();
   u5[dtid.xy + uint2(4, 0)] = v + 100;
})", {1, 1, 1}},
   {"cs_typed_load_3d_only", 0x80, R"(
[numthreads(4, 4, 4)]
void main(uint3 dtid : SV_DispatchThreadID)
{
   float v = u7[dtid];
   u7[dtid] = v + 1;
})", {1, 1, 1}, nullptr, 5},
   {"tex3d_roundtrip", 0xa0, nullptr, {0, 0, 0}, nullptr, 5},
   {"cs_big_groups", 0x3, R"(
groupshared uint gs[1024];
[numthreads(32, 32, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID)
{
   gs[gi] = gi * cu.x;
   GroupMemoryBarrierWithGroupSync();
   uint sum = 0;
   for (uint k = 0; k < 8; k++) sum += gs[(gi + k * 128) % 1024];
   if (gi < 256 && all(gid == uint3(1, 0, 1))) u0[gi].x = sum + gid.z * 1000;
   u1.InterlockedAdd((gid.x + gid.y * 2 + gid.z * 4) * 4, 1);
})", {2, 2, 2}},
   {"cs_indirect_offset", 0x1, R"(
[numthreads(2, 2, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
   u0[dtid.x + dtid.y * 16] = uint4(dtid.xy, gid.xy);
})", {0, 0, 0}, nullptr, 3},
   {"ps_uav_structured", 0x21, nullptr, {0, 0, 0}, R"(
RWStructuredBuffer<uint4> u0 : register(u1);
RWTexture2D<uint> u5 : register(u2);
float4 main(float4 p : SV_Position) : SV_Target
{
   uint2 xy = (uint2)p.xy;
   u0[xy.y * 16 + xy.x] = uint4(xy, 7, 9);
   InterlockedOr(u5[xy / 2], 1u << ((xy.x & 1) + (xy.y & 1) * 2));
   return 0;
})"},
};

struct Uavs {
   ID3D11Buffer *b0, *b1, *b3, *b4, *args;
   ID3D11Texture2D *tex2, *tex5, *tex6;
   ID3D11Texture3D *tex7;
   ID3D11Buffer *cb;
   ID3D11UnorderedAccessView *u[8];
   ID3D11ShaderResourceView *t[4];
   ID3D11SamplerState *s0;
   ID3D11Buffer *count;   // CopyStructureCount target
   ID3D11Buffer *args2;   // indirect arguments at offset 8
};

static ID3D11Buffer *make_buffer(ID3D11Device *dev, UINT size, UINT bind, UINT misc, UINT stride, const void *data)
{
   D3D11_BUFFER_DESC bd = {};
   bd.ByteWidth = size;
   bd.BindFlags = bind;
   bd.MiscFlags = misc;
   bd.StructureByteStride = stride;
   D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
   ID3D11Buffer *b = nullptr;
   HRESULT hr = dev->CreateBuffer(&bd, data ? &init : nullptr, &b);
   if (FAILED(hr)) printf("  CreateBuffer failed 0x%08lx\n", hr);
   return b;
}

static bool cs_setup(Dev &d, Uavs &u)
{
   std::vector<uint32_t> zero(4096 / 4, 0);
   u.b0 = make_buffer(d.dev, 256 * 16, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, zero.data());
   u.b1 = make_buffer(d.dev, 4096, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, zero.data());
   u.b3 = make_buffer(d.dev, 64 * 4, D3D11_BIND_UNORDERED_ACCESS, 0, 0, zero.data());
   u.b4 = make_buffer(d.dev, 256 * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, zero.data());
   uint32_t args[3] = {3, 2, 1};
   u.args = make_buffer(d.dev, 12, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, args);
   u.count = make_buffer(d.dev, 16, 0, 0, 0, zero.data());
   if (!u.b0 || !u.b1 || !u.b3 || !u.b4 || !u.args || !u.count) return false;
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = td.Height = 16;
   td.MipLevels = td.ArraySize = 1;
   td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
   td.SampleDesc.Count = 1;
   td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
   // UAV textures start zeroed (D3D doesn't zero textures created without data; WARP happens to)
   static uint32_t zeros[16 * 16 * 4];
   D3D11_SUBRESOURCE_DATA zinit[3] = {{zeros, 16 * 16, 0}, {zeros, 16 * 16, 0}, {zeros, 16 * 16, 0}};
   CHECK(d.dev->CreateTexture2D(&td, zinit, &u.tex2));

   D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
   ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
   ud.Format = DXGI_FORMAT_UNKNOWN;
   ud.Buffer.NumElements = 256;
   CHECK(d.dev->CreateUnorderedAccessView(u.b0, &ud, &u.u[0]));
   ud.Format = DXGI_FORMAT_R32_TYPELESS;
   ud.Buffer.NumElements = 1024;
   ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
   CHECK(d.dev->CreateUnorderedAccessView(u.b1, &ud, &u.u[1]));
   ud.Format = DXGI_FORMAT_R32_UINT;
   ud.Buffer.NumElements = 64;
   ud.Buffer.Flags = 0;
   CHECK(d.dev->CreateUnorderedAccessView(u.b3, &ud, &u.u[3]));
   ud.Format = DXGI_FORMAT_UNKNOWN;
   ud.Buffer.NumElements = 256;
   ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
   CHECK(d.dev->CreateUnorderedAccessView(u.b4, &ud, &u.u[4]));
   CHECK(d.dev->CreateUnorderedAccessView(u.tex2, nullptr, &u.u[2]));
   td.Format = DXGI_FORMAT_R32_UINT;
   zinit[0].SysMemPitch = 16 * 4;
   CHECK(d.dev->CreateTexture2D(&td, zinit, &u.tex5));
   CHECK(d.dev->CreateUnorderedAccessView(u.tex5, nullptr, &u.u[5]));
   td.Width = td.Height = 8;
   td.ArraySize = 3;
   td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   for (auto &z : zinit) z.SysMemPitch = 8 * 4;
   CHECK(d.dev->CreateTexture2D(&td, zinit, &u.tex6));
   CHECK(d.dev->CreateUnorderedAccessView(u.tex6, nullptr, &u.u[6]));
   D3D11_TEXTURE3D_DESC t3d = {};
   t3d.Width = t3d.Height = t3d.Depth = 4;
   t3d.MipLevels = 1;
   t3d.Format = DXGI_FORMAT_R32_FLOAT;
   t3d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
   D3D11_SUBRESOURCE_DATA z3 = {zeros, 4 * 4, 4 * 4 * 4};
   CHECK(d.dev->CreateTexture3D(&t3d, &z3, &u.tex7));
   CHECK(d.dev->CreateUnorderedAccessView(u.tex7, nullptr, &u.u[7]));
   struct { uint32_t cu[4]; float cf[4]; } cbdata = {{3, 0, 0, 0}, {0.5f, 0, 0, 0}};
   u.cb = make_buffer(d.dev, sizeof cbdata, D3D11_BIND_CONSTANT_BUFFER, 0, 0, &cbdata);
   uint32_t args2[6] = {99, 99, 3, 2, 1, 99};
   u.args2 = make_buffer(d.dev, sizeof args2, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, args2);

   // inputs: t0 structured float4 (also read as struct { float3; uint; }), t1 raw, t2 typed float4
   std::vector<float> f(64 * 4);
   for (int i = 0; i < 64 * 4; i++) f[i] = (i % 4 == 3) ? 0 : i * 0.5f - 7;
   for (int i = 0; i < 64; i++) {
      uint32_t v = i * 31 + 5;
      memcpy(&f[i * 4 + 3], &v, 4);
   }
   ID3D11Buffer *tb0 = make_buffer(d.dev, 64 * 16, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, f.data());
   std::vector<uint32_t> raw(256);
   for (int i = 0; i < 256; i++) raw[i] = i * 2654435761u;
   ID3D11Buffer *tb1 = make_buffer(d.dev, 1024, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, raw.data());
   ID3D11Buffer *tb2 = make_buffer(d.dev, 64 * 16, D3D11_BIND_SHADER_RESOURCE, 0, 0, f.data());
   if (!tb0 || !tb1 || !tb2) return false;
   D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
   sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
   sd.Format = DXGI_FORMAT_UNKNOWN;
   sd.Buffer.NumElements = 64;
   CHECK(d.dev->CreateShaderResourceView(tb0, &sd, &u.t[0]));
   sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
   sd.Format = DXGI_FORMAT_R32_TYPELESS;
   sd.BufferEx.NumElements = 256;
   sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
   CHECK(d.dev->CreateShaderResourceView(tb1, &sd, &u.t[1]));
   sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
   sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
   sd.Buffer.FirstElement = 0;
   sd.Buffer.NumElements = 64;
   CHECK(d.dev->CreateShaderResourceView(tb2, &sd, &u.t[2]));
   u.t[3] = d.srv[0];
   u.s0 = d.samp[0];
   return true;
}

// The UAVs' contents: u0..u7 (u4: count, then the sorted elements; textures slice by slice, rows packed)
static bool read_resource(Dev &d, ID3D11Resource *src, std::vector<uint32_t> &v)
{
   D3D11_RESOURCE_DIMENSION dim;
   src->GetType(&dim);
   ID3D11Resource *staging = nullptr;
   unsigned slices = 1, rows = 1, row_bytes = 0, depth = 1;
   if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      D3D11_BUFFER_DESC bd;
      ((ID3D11Buffer *)src)->GetDesc(&bd);
      row_bytes = bd.ByteWidth;
      bd.BindFlags = 0; bd.MiscFlags = 0; bd.StructureByteStride = 0;
      bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(d.dev->CreateBuffer(&bd, nullptr, (ID3D11Buffer **)&staging));
   } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      D3D11_TEXTURE2D_DESC td;
      ((ID3D11Texture2D *)src)->GetDesc(&td);
      slices = td.ArraySize; rows = td.Height;
      row_bytes = td.Width * (td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16 : 4);
      td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(d.dev->CreateTexture2D(&td, nullptr, (ID3D11Texture2D **)&staging));
   } else {
      D3D11_TEXTURE3D_DESC td;
      ((ID3D11Texture3D *)src)->GetDesc(&td);
      rows = td.Height; depth = td.Depth; row_bytes = td.Width * 4;
      td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(d.dev->CreateTexture3D(&td, nullptr, (ID3D11Texture3D **)&staging));
   }
   d.ctx->CopyResource(staging, src);
   v.clear();
   for (unsigned sl = 0; sl < slices; sl++) {
      D3D11_MAPPED_SUBRESOURCE m;
      CHECK(d.ctx->Map(staging, sl, D3D11_MAP_READ, 0, &m));
      if (getenv("CS_DEBUG") && dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
         printf("  3D map row %u depth %u:", m.RowPitch, m.DepthPitch);
         for (unsigned z = 0; z < depth; z++) printf(" z%u=%08x", z, *(uint32_t *)((char *)m.pData + z * m.DepthPitch));
         printf("\n");
      }
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < rows; y++) {
            const uint32_t *row = (const uint32_t *)((const char *)m.pData + z * m.DepthPitch + y * m.RowPitch);
            v.insert(v.end(), row, row + row_bytes / 4);
         }
      d.ctx->Unmap(staging, sl);
   }
   staging->Release();
   return true;
}

static bool cs_read(Dev &d, Uavs &u, std::vector<uint32_t> out[8])
{
   ID3D11Resource *res[8] = {u.b0, u.b1, u.tex2, u.b3, u.b4, u.tex5, u.tex6, u.tex7};
   d.ctx->CopyStructureCount(u.count, 0, u.u[4]);
   for (int i = 0; i < 8; i++) {
      if (getenv("CS_ONLY7") && i != 7) continue;
      if (!read_resource(d, res[i], out[i])) return false;
   }
   std::vector<uint32_t> count;
   if (!read_resource(d, u.count, count)) return false;
   uint32_t n = std::min<uint32_t>(count[0], 256);
   std::vector<uint32_t> e(out[4].begin(), out[4].begin() + n);
   std::sort(e.begin(), e.end());
   out[4].clear();
   out[4].push_back(count[0]);
   out[4].insert(out[4].end(), e.begin(), e.end());
   return true;
}

static bool cs_run(Dev &d, const CsTest &t, std::vector<uint32_t> out[8])
{
   Uavs u = {};
   if (!cs_setup(d, u)) return false;
   if (t.special == 2) {
      if (getenv("CS_DEBUG")) {
         std::vector<uint32_t> mark(1024, 0xaaaaaaaa);
         d.ctx->UpdateSubresource(u.b0, 0, nullptr, mark.data(), 0, 0);
      }
      UINT vu[4] = {0x12345678, 7, 0xffffffff, 3};
      float vf[4] = {0.25f, -1.5f, 1e6f, 0};
      d.ctx->ClearUnorderedAccessViewUint(u.u[0], vu);
      d.ctx->ClearUnorderedAccessViewUint(u.u[1], vu);
      d.ctx->ClearUnorderedAccessViewFloat(u.u[2], vf);
      d.ctx->ClearUnorderedAccessViewUint(u.u[3], vu);
      if (getenv("CS_DEBUG")) {
         D3D11_BUFFER_DESC bd;
         u.b0->GetDesc(&bd);
         printf("  b0 desc: width %u bind %x misc %x stride %u usage %d\n", bd.ByteWidth, bd.BindFlags, bd.MiscFlags,
                bd.StructureByteStride, bd.Usage);
         bd.BindFlags = 0; bd.MiscFlags = 0; bd.StructureByteStride = 0;
         bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
         ID3D11Buffer *s; HRESULT hr = d.dev->CreateBuffer(&bd, nullptr, &s);
         d.ctx->CopyResource(s, u.b0);
         D3D11_MAPPED_SUBRESOURCE m; HRESULT hr2 = d.ctx->Map(s, 0, D3D11_MAP_READ, 0, &m);
         printf("  debug readback: create %08lx map %08lx value %08x\n", hr, hr2, SUCCEEDED(hr2) ? ((uint32_t *)m.pData)[1] : 0);
         if (SUCCEEDED(hr2)) d.ctx->Unmap(s, 0);
      }
      return cs_read(d, u, out);
   }
   if (t.special == 5) {
      // u7 filled with known values first
      std::vector<float> v(64);
      for (int i = 0; i < 64; i++) v[i] = i * 1.5f;
      d.ctx->UpdateSubresource(u.tex7, 0, nullptr, v.data(), 16, 64);
   }
   if (!t.cs && !t.ps) {
      return cs_read(d, u, out);   // just the uploads (tex3d_roundtrip)
   }
   if (t.cs) {
      ID3DBlob *cs = compile(std::string(CS_HEADER) + t.cs, "cs_5_0");
      if (!cs) return false;
      ID3D11ComputeShader *shader;
      CHECK(d.dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &shader));
      d.ctx->CSSetShader(shader, nullptr, 0);
      d.ctx->CSSetShaderResources(0, 4, u.t);
      d.ctx->CSSetSamplers(0, 1, &u.s0);
      d.ctx->CSSetConstantBuffers(0, 1, &u.cb);
      UINT counts[8] = {};
      if (!strcmp(t.name, "cs_consume")) {
         // a filled consume buffer: 40 entries 3, 6, ..., 120
         std::vector<uint32_t> vals(256, 0);
         for (int i = 0; i < 40; i++) vals[i] = (i + 1) * 3;
         d.ctx->UpdateSubresource(u.b4, 0, nullptr, vals.data(), 0, 0);
         counts[4] = 40;
      }
      d.ctx->CSSetUnorderedAccessViews(0, 8, u.u, counts);
      if (t.special == 1) {
         d.ctx->DispatchIndirect(u.args, 0);
      } else if (t.special == 3) {
         d.ctx->DispatchIndirect(u.args2, 8);
      } else {
         d.ctx->Dispatch(t.groups[0], t.groups[1], t.groups[2]);
      }
      ID3D11UnorderedAccessView *none[8] = {};
      d.ctx->CSSetUnorderedAccessViews(0, 8, none, nullptr);
      d.ctx->CSSetShader(nullptr, nullptr, 0);
      shader->Release();
   } else {
      // pixel shader UAVs: a 16x16 render target in slot 0, UAVs from slot 1
      ID3DBlob *ps = compile(std::string("struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 dir : TEXCOORD1; };\n") + t.ps, "ps_5_0");
      if (!ps) return false;
      ID3D11PixelShader *pshader;
      CHECK(d.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pshader));
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = 16;
      td.MipLevels = td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      ID3D11Texture2D *rt;
      CHECK(d.dev->CreateTexture2D(&td, nullptr, &rt));
      ID3D11RenderTargetView *rtv;
      CHECK(d.dev->CreateRenderTargetView(rt, nullptr, &rtv));
      if (!strcmp(t.name, "ps_uav_structured")) {
         ID3D11UnorderedAccessView *list[2] = {u.u[0], u.u[5]};
         d.ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 2, list, nullptr);
      } else {
         d.ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 3, &u.u[1], nullptr);
      }
      D3D11_VIEWPORT vp = {0, 0, 16, 16, 0, 1};
      d.ctx->RSSetViewports(1, &vp);
      d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      d.ctx->VSSetShader(d.vs, nullptr, 0);
      d.ctx->PSSetShader(pshader, nullptr, 0);
      d.ctx->Draw(3, 0);
      d.ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
      rtv->Release();
      rt->Release();
      pshader->Release();
   }
   return cs_read(d, u, out);
}

static int cs_compare_all(Dev &hw, bool hw_ok, Dev &warp, const char *filter, int &total)
{
   int failed = 0;
   for (const CsTest &t : cs_tests) {
      if (filter && !strstr(t.name, filter)) continue;
      if (!startTest(sm5_title(t.name))) continue;
      total++;
      std::vector<uint32_t> a[8], b[8];
      bool ok = hw_ok && cs_run(hw, t, a);
      if (!cs_run(warp, t, b)) {
         report(Status::Skip, sm5_title(t.name), "WARP failed");
         continue;
      }
      int bad = 0;
      for (int k = 0; ok && k < 8; k++) {
         if (!(t.compare & (1 << k))) continue;
         if (a[k].size() != b[k].size()) {
            printf("  %s u%d: %zu vs %zu values\n", t.name, k, a[k].size(), b[k].size());
            bad++;
            continue;
         }
         for (size_t i = 0; i < a[k].size(); i++) {
            bool same = a[k][i] == b[k][i];
            if (!same && k == 6) {
               // RGBA8 written from floats: rounding may differ by one step per channel
               same = true;
               for (int c = 0; c < 4; c++)
                  same &= abs((int)((a[k][i] >> (c * 8)) & 255) - (int)((b[k][i] >> (c * 8)) & 255)) <= 1;
            }
            if (!same && k == 2) {
               float fa, fb;
               memcpy(&fa, &a[k][i], 4);
               memcpy(&fb, &b[k][i], 4);
               same = fabsf(fa - fb) <= (1.0f / 64) * (1 + fabsf(fb));
            }
            if (!same && bad++ < 8) printf("  %s u%d[%zu]: hw 0x%08x warp 0x%08x\n", t.name, k, i, a[k][i], b[k][i]);
         }
      }
      bool pass = ok && !bad;
      failed += !pass;
      sm5_result(pass, t.name, !ok ? "the GPU run failed" : bad ? std::to_string(bad) + " values differ from WARP's" : "");
   }
   return failed;
}
