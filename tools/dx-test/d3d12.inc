// Direct3D 12: a device on the GPU's adapter, its feature tiers, and rendering, compute, copy queues, bundles,
// indirect execution and texture sampling, each read back and compared with the expected values

struct D12 {
   ID3D12Device *dev = nullptr;
   ID3D12CommandQueue *queue = nullptr;
   ID3D12CommandAllocator *alloc = nullptr;
   ID3D12GraphicsCommandList *list = nullptr;
   ID3D12Fence *fence = nullptr;
   UINT64 fenceValue = 0;
   HANDLE event = nullptr;
};

static void barrier12(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b = {};
   b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition.pResource = r;
   b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   b.Transition.StateBefore = from;
   b.Transition.StateAfter = to;
   list->ResourceBarrier(1, &b);
}

// The debug layer's messages since the last call (DXTEST_D3D12_DEBUG), for a failing test's detail
static std::string debugMessages12(D12 &d)
{
   ID3D12InfoQueue *q = nullptr;
   std::string all;
   if (FAILED(d.dev->QueryInterface(__uuidof(ID3D12InfoQueue), (void **)&q))) return all;
   for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; i++) {
      SIZE_T size = 0;
      q->GetMessage(i, nullptr, &size);
      std::vector<BYTE> m(size);
      auto *msg = (D3D12_MESSAGE *)m.data();
      if (SUCCEEDED(q->GetMessage(i, msg, &size)) && msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
         all += std::string(" | ") + msg->pDescription;
   }
   q->ClearStoredMessages();
   q->Release();
   return all;
}

static bool waitIdle12(D12 &d, ID3D12CommandQueue *queue)
{
   d.fenceValue++;
   if (FAILED(queue->Signal(d.fence, d.fenceValue))) return false;
   if (d.fence->GetCompletedValue() < d.fenceValue) {
      d.fence->SetEventOnCompletion(d.fenceValue, d.event);
      if (WaitForSingleObject(d.event, 10000) != WAIT_OBJECT_0) return false;
   }
   return true;
}

// Closes, executes and waits for the list, then resets it for the next test
static bool submit12(D12 &d)
{
   if (FAILED(d.list->Close())) return false;
   ID3D12CommandList *lists[] = {d.list};
   d.queue->ExecuteCommandLists(1, lists);
   bool ok = waitIdle12(d, d.queue);
   d.alloc->Reset();
   d.list->Reset(d.alloc, nullptr);
   return ok;
}

static ID3D12Resource *buffer12(D12 &d, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
   D3D12_HEAP_PROPERTIES hp = {heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
   D3D12_RESOURCE_DESC rd = {};
   rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   rd.Width = size;
   rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
   rd.SampleDesc.Count = 1;
   rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   rd.Flags = flags;
   ID3D12Resource *r = nullptr;
   d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, __uuidof(ID3D12Resource), (void **)&r);
   return r;
}

static ID3D12Resource *texture12(D12 &d, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                 D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear = nullptr)
{
   D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
   D3D12_RESOURCE_DESC rd = {};
   rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   rd.Width = w;
   rd.Height = h;
   rd.DepthOrArraySize = rd.MipLevels = 1;
   rd.Format = format;
   rd.SampleDesc.Count = 1;
   rd.Flags = flags;
   ID3D12Resource *r = nullptr;
   d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, clear, __uuidof(ID3D12Resource), (void **)&r);
   return r;
}

static ID3D12RootSignature *rootSignature12(D12 &d, const D3D12_ROOT_SIGNATURE_DESC &desc, std::string &detail)
{
   ID3DBlob *blob = nullptr, *error = nullptr;
   // looked up, like D3D12CreateDevice: d3d12.dll isn't imported, so the test also starts where there's none (Windows 8.1)
   static auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(LoadLibraryA("d3d12.dll"),
                                                                             "D3D12SerializeRootSignature");
   HRESULT h = serialize ? serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error) : E_NOTIMPL;
   if (FAILED(h)) {
      detail = "D3D12SerializeRootSignature: " + hrs(h);
      release(error);
      return nullptr;
   }
   ID3D12RootSignature *rs = nullptr;
   h = d.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)&rs);
   if (FAILED(h)) detail = "CreateRootSignature: " + hrs(h);
   blob->Release();
   return rs;
}

static const char *VS12 = R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V main(uint id : SV_VertexID)
{ V v; v.uv = float2((id << 1) & 2, id & 2); v.p = float4(v.uv * float2(2, -2) + float2(-1, 1), 0.5, 1); return v; }
)";

static ID3D12PipelineState *graphicsPso12(D12 &d, ID3D12RootSignature *rs, const char *ps, std::string &detail)
{
   ID3DBlob *vsb = compileHlsl(VS12, "vs_5_0", &detail), *psb = compileHlsl(ps, "ps_5_0", &detail);
   if (!vsb || !psb) {
      release(vsb);
      release(psb);
      return nullptr;
   }
   D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
   pd.pRootSignature = rs;
   pd.VS = {vsb->GetBufferPointer(), vsb->GetBufferSize()};
   pd.PS = {psb->GetBufferPointer(), psb->GetBufferSize()};
   pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
   pd.SampleMask = 0xffffffff;
   pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
   pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
   pd.RasterizerState.DepthClipEnable = TRUE;
   pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
   pd.NumRenderTargets = 1;
   pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
   pd.SampleDesc.Count = 1;
   ID3D12PipelineState *pso = nullptr;
   HRESULT h = d.dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso);
   if (FAILED(h)) detail = "CreateGraphicsPipelineState: " + hrs(h);
   vsb->Release();
   psb->Release();
   return pso;
}

// A 32x32 RGBA8 render target, its RTV, and a read-back buffer
struct Target12 {
   ID3D12Resource *tex = nullptr, *readback = nullptr;
   ID3D12DescriptorHeap *rtvHeap = nullptr;
   D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
   static const UINT size = 32, pitch = 256;   // rows aligned to 256 bytes
   void release()
   {
      ::release(tex);
      ::release(readback);
      ::release(rtvHeap);
   }
};

static bool target12(D12 &d, Target12 &t)
{
   D3D12_CLEAR_VALUE cv = {DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 1, 1}};
   t.tex = texture12(d, Target12::size, Target12::size, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
   t.readback = buffer12(d, Target12::pitch * Target12::size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
   d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&t.rtvHeap);
   if (!t.tex || !t.readback || !t.rtvHeap) return false;
   t.rtv = t.rtvHeap->GetCPUDescriptorHandleForHeapStart();
   d.dev->CreateRenderTargetView(t.tex, nullptr, t.rtv);
   const float blue[4] = {0, 0, 1, 1};
   d.list->ClearRenderTargetView(t.rtv, blue, 0, nullptr);
   d.list->OMSetRenderTargets(1, &t.rtv, FALSE, nullptr);
   D3D12_VIEWPORT vp = {0, 0, (float)Target12::size, (float)Target12::size, 0, 1};
   D3D12_RECT sc = {0, 0, (LONG)Target12::size, (LONG)Target12::size};
   d.list->RSSetViewports(1, &vp);
   d.list->RSSetScissorRects(1, &sc);
   return true;
}

// Copies the target to its read-back buffer, runs the list, and reads pixel (x, y) as RGBA8 (R in the low byte)
static bool readTarget12(D12 &d, Target12 &t, std::vector<uint32_t> &pixels)
{
   barrier12(d.list, t.tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
   dst.pResource = t.readback;
   dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, Target12::size, Target12::size, 1, Target12::pitch};
   src.pResource = t.tex;
   src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
   if (!submit12(d)) return false;
   void *p = nullptr;
   D3D12_RANGE range = {0, Target12::pitch * Target12::size};
   if (FAILED(t.readback->Map(0, &range, &p))) return false;
   pixels.resize(Target12::size * Target12::size);
   for (UINT y = 0; y < Target12::size; y++) memcpy(&pixels[y * Target12::size], (char *)p + y * Target12::pitch, Target12::size * 4);
   D3D12_RANGE none = {0, 0};
   t.readback->Unmap(0, &none);
   return true;
}

// Variable rate shading: a pattern that changes every pixel, drawn at a coarse rate, comes out the same in each
// coarse block. Tier 1: RSSetShadingRate (2x2 everywhere); tier 2: a shading rate image (left tiles 1x1, right 4x4)
static const char *VRS_PS12 = "float4 main(float4 p : SV_Position) : SV_Target"
                              "{ return float4(frac(p.x * 0.37), frac(p.y * 0.41), 0, 1); }";

static bool sameBlocks(const std::vector<uint32_t> &px, UINT x0, UINT x1, UINT block)
{
   for (UINT y = 0; y < Target12::size; y++)
      for (UINT x = x0; x < x1; x++)
         if (px[y * Target12::size + x] != px[(y / block * block) * Target12::size + x / block * block]) return false;
   return true;
}

static void testVrs12(D12 &d, ID3D12RootSignature *rs, const D3D12_FEATURE_DATA_D3D12_OPTIONS6 &o6)
{
   const char *name1 = "Variable rate shading (2x2 for a draw)", *name2 = "Variable rate shading (shading rate image)";
   ID3D12GraphicsCommandList5 *list5 = nullptr;
   if (o6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED ||
       FAILED(d.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList5), (void **)&list5))) {
      report(Status::Skip, name1, "no variable rate shading");
      report(Status::Skip, name2, "no variable rate shading");
      return;
   }
   std::string detail;
   ID3D12PipelineState *pso = graphicsPso12(d, rs, VRS_PS12, detail);
   for (int image = 0; image < 2 && pso; image++) {
      const char *name = image ? name2 : name1;
      if (!startTest(name)) continue;
      if (image && (o6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2 || !o6.ShadingRateImageTileSize)) {
         report(Status::Skip, name, "tier 1 only");
         continue;
      }
      Target12 t;
      ID3D12Resource *rateImage = nullptr, *upload = nullptr;
      bool ok = target12(d, t);
      UINT tiles = (Target12::size + o6.ShadingRateImageTileSize - 1) / (image ? o6.ShadingRateImageTileSize : 1);
      if (ok && image) {
         // one rate per tile: the left half's tiles 1x1, the right half's 4x4
         rateImage = texture12(d, tiles, tiles, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
         upload = buffer12(d, 256 * tiles, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
         uint8_t *p = nullptr;
         ok = rateImage && upload && SUCCEEDED(upload->Map(0, nullptr, (void **)&p));
         if (ok) {
            for (UINT y = 0; y < tiles; y++)
               for (UINT x = 0; x < tiles; x++) p[y * 256 + x] = x < tiles / 2 ? D3D12_SHADING_RATE_1X1 : D3D12_SHADING_RATE_4X4;
            upload->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = rateImage;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UINT, tiles, tiles, 1, 256};
            d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier12(d.list, rateImage, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
         }
      }
      std::vector<uint32_t> px;
      if (ok) {
         d.list->SetGraphicsRootSignature(rs);
         d.list->SetPipelineState(pso);
         d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
         if (image) {
            D3D12_SHADING_RATE_COMBINER combiners[2] = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                                        D3D12_SHADING_RATE_COMBINER_OVERRIDE};
            list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
            list5->RSSetShadingRateImage(rateImage);
         } else {
            list5->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
         }
         d.list->DrawInstanced(3, 1, 0, 0);
         list5->RSSetShadingRateImage(nullptr);
         list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
         ok = readTarget12(d, t, px);
      }
      if (!ok) {
         check(false, name, "rendering or reading back failed");
      } else if (!image) {
         bool coarse = sameBlocks(px, 0, Target12::size, 2), varies = px[0] != px[2];
         check(coarse && varies, name, !varies ? "the pattern came out flat"
                                       : coarse ? "each 2x2 block one color, the pattern changing between blocks"
                                                : "pixels within 2x2 blocks differ");
      } else {
         UINT half = tiles / 2 * o6.ShadingRateImageTileSize;
         bool fine = px[0] != px[1] && px[Target12::size] != px[0], coarse = sameBlocks(px, half, Target12::size, 4);
         check(fine && coarse, name, fmt("tile size %u: 1x1 tiles %s, 4x4 tiles %s", o6.ShadingRateImageTileSize,
                                         fine ? "per pixel" : "NOT per pixel", coarse ? "in 4x4 blocks" : "NOT in 4x4 blocks"));
      }
      release(upload);
      release(rateImage);
      t.release();
   }
   release(pso);
   release(list5);
}

// Mesh shaders (shader model 6.5 DXIL, compiled by dxil/compile.sh): a triangle over the target from a mesh shader
// alone, and from one fed by an amplification shader's payload, each in its own color
#include "dxil/dxil_mesh_solo.h"
#include "dxil/dxil_mesh_as.h"
#include "dxil/dxil_mesh_payload.h"
#include "dxil/dxil_mesh_ps.h"

template <typename T>
static void streamAdd12(std::vector<BYTE> &stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T &value)
{
   struct alignas(void *) Sub {
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
      T value;
   } sub = {type, value};
   size_t at = (stream.size() + alignof(void *) - 1) & ~(alignof(void *) - 1);
   stream.resize(at + sizeof(sub));
   memcpy(&stream[at], &sub, sizeof(sub));
}

static void testMesh12(D12 &d, ID3D12RootSignature *rs, const D3D12_FEATURE_DATA_D3D12_OPTIONS7 &o7)
{
   const char *names[2] = {"Mesh shaders (a triangle)", "Mesh shaders (amplification shader payload)"};
   ID3D12Device2 *dev2 = nullptr;
   ID3D12GraphicsCommandList6 *list6 = nullptr;
   if (o7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED ||
       FAILED(d.dev->QueryInterface(__uuidof(ID3D12Device2), (void **)&dev2)) ||
       FAILED(d.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList6), (void **)&list6))) {
      for (auto n : names) report(Status::Skip, n, "no mesh shaders");
      release(dev2);
      return;
   }
   for (int amplified = 0; amplified < 2; amplified++) {
      if (!startTest(names[amplified])) continue;
      std::vector<BYTE> stream;
      streamAdd12(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, rs);
      if (amplified) streamAdd12(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE{dxil_mesh_as, sizeof(dxil_mesh_as)});
      streamAdd12(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,
                  amplified ? D3D12_SHADER_BYTECODE{dxil_mesh_payload, sizeof(dxil_mesh_payload)}
                            : D3D12_SHADER_BYTECODE{dxil_mesh_solo, sizeof(dxil_mesh_solo)});
      streamAdd12(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE{dxil_mesh_ps, sizeof(dxil_mesh_ps)});
      D3D12_BLEND_DESC blend = {};
      blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      streamAdd12(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend);
      streamAdd12(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, (UINT)0xffffffff);
      D3D12_RASTERIZER_DESC rast = {};
      rast.FillMode = D3D12_FILL_MODE_SOLID;
      rast.CullMode = D3D12_CULL_MODE_NONE;
      rast.DepthClipEnable = TRUE;
      streamAdd12(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, rast);
      D3D12_RT_FORMAT_ARRAY rtv = {};
      rtv.NumRenderTargets = 1;
      rtv.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      streamAdd12(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, rtv);
      streamAdd12(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC{1, 0});
      D3D12_PIPELINE_STATE_STREAM_DESC sd = {stream.size(), stream.data()};
      ID3D12PipelineState *pso = nullptr;
      HRESULT h = dev2->CreatePipelineState(&sd, __uuidof(ID3D12PipelineState), (void **)&pso);
      Target12 t;
      std::vector<uint32_t> px;
      bool ok = SUCCEEDED(h) && target12(d, t);
      if (ok) {
         d.list->SetGraphicsRootSignature(rs);
         d.list->SetPipelineState(pso);
         list6->DispatchMesh(1, 1, 1);
         ok = readTarget12(d, t, px);
      }
      // (0.25, 0.5, 0.75) from the mesh shader; (0.75, 0.5, 0.25) through the payload
      uint32_t c = ok ? px[16 * Target12::size + 16] : 0;
      int r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff, er = amplified ? 191 : 64, eb = amplified ? 64 : 191;
      check(ok && near8(r, er, 3) && near8(g, 128, 3) && near8(b, eb, 3), names[amplified],
            FAILED(h) ? "CreatePipelineState: " + hrs(h)
                      : !ok ? std::string("rendering or reading back failed")
                            : fmt("pixel (%d,%d,%d), expected (%d,128,%d)", r, g, b, er, eb));
      t.release();
      release(pso);
   }
   release(list6);
   release(dev2);
}

#include "dxil/dxil_rt_lib.h"

// DXR: a triangle at z = 1 in a bottom-level structure, one instance of it in a top-level one; ray 0 hits it (closest
// hit: barycentrics (0.25, 0.5), t = 1, alpha 0.5 from the hit group's local root constant), ray 1 misses
static void testRt12(D12 &d, const D3D12_FEATURE_DATA_D3D12_OPTIONS5 &o5)
{
   const char *name = "DirectX Raytracing (a ray hits a triangle, one misses)";
   ID3D12Device5 *dev5 = nullptr;
   ID3D12GraphicsCommandList4 *list4 = nullptr;
   if (o5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED ||
       FAILED(d.dev->QueryInterface(__uuidof(ID3D12Device5), (void **)&dev5)) ||
       FAILED(d.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), (void **)&list4))) {
      report(Status::Skip, name, "no ray tracing");
      release(dev5);
      return;
   }
   if (!startTest(name)) {
      release(list4);
      release(dev5);
      return;
   }
   std::string detail;
   ID3D12Resource *vb = nullptr, *inst = nullptr, *blas = nullptr, *tlas = nullptr, *scratch = nullptr, *table = nullptr,
                  *out = nullptr, *readback = nullptr;
   ID3D12RootSignature *global = nullptr, *local = nullptr;
   ID3D12StateObject *so = nullptr;
   ID3D12StateObjectProperties *props = nullptr;
   ID3D12DescriptorHeap *heap = nullptr;
   bool ok = false;
   do {
      // the geometry, and the instance (identity transform)
      const float tri[9] = {-1, -1, 1, 1, -1, 1, 0, 1, 1};
      vb = buffer12(d, sizeof tri, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      inst = buffer12(d, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      if (!vb || !inst) { detail = "upload buffers"; break; }
      void *p;
      vb->Map(0, nullptr, &p);
      memcpy(p, tri, sizeof tri);
      vb->Unmap(0, nullptr);

      D3D12_RAYTRACING_GEOMETRY_DESC geo = {};
      geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
      geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
      geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
      geo.Triangles.VertexCount = 3;
      geo.Triangles.VertexBuffer = {vb->GetGPUVirtualAddress(), 12};
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom = {};
      bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
      bottom.NumDescs = 1;
      bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
      bottom.pGeometryDescs = &geo;
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top = {};
      top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
      top.NumDescs = 1;
      top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
      top.InstanceDescs = inst->GetGPUVirtualAddress();
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pb = {}, pt = {};
      dev5->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &pb);
      dev5->GetRaytracingAccelerationStructurePrebuildInfo(&top, &pt);
      if (!pb.ResultDataMaxSizeInBytes || !pt.ResultDataMaxSizeInBytes) { detail = "no prebuild info"; break; }
      blas = buffer12(d, pb.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      tlas = buffer12(d, pt.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      scratch = buffer12(d, std::max(pb.ScratchDataSizeInBytes, pt.ScratchDataSizeInBytes), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      out = buffer12(d, 32, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      readback = buffer12(d, 32, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!blas || !tlas || !scratch || !out || !readback) { detail = "buffers"; break; }
      D3D12_RAYTRACING_INSTANCE_DESC id = {};
      id.Transform[0][0] = id.Transform[1][1] = id.Transform[2][2] = 1;
      id.InstanceMask = 0xff;
      id.AccelerationStructure = blas->GetGPUVirtualAddress();
      inst->Map(0, nullptr, &p);
      memcpy(p, &id, sizeof id);
      inst->Unmap(0, nullptr);

      // root signatures: global (the scene in a table, the output as a root UAV), the hit group's local (a constant)
      D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
      D3D12_ROOT_PARAMETER gp[2] = {};
      gp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      gp[0].DescriptorTable = {1, &range};
      gp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      D3D12_ROOT_SIGNATURE_DESC gd = {2, gp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
      global = rootSignature12(d, gd, detail);
      D3D12_ROOT_PARAMETER lp = {};
      lp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      lp.Constants = {0, 0, 1};
      D3D12_ROOT_SIGNATURE_DESC ld = {1, &lp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};
      if (global) local = rootSignature12(d, ld, detail);
      if (!local) break;

      // the pipeline
      D3D12_DXIL_LIBRARY_DESC lib = {{dxil_rt_lib, sizeof(dxil_rt_lib)}, 0, nullptr};
      D3D12_HIT_GROUP_DESC hg = {L"HitGroup", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"Hit", nullptr};
      D3D12_RAYTRACING_SHADER_CONFIG sc = {16, 8};
      D3D12_RAYTRACING_PIPELINE_CONFIG pc = {1};
      D3D12_GLOBAL_ROOT_SIGNATURE gs = {global};
      D3D12_LOCAL_ROOT_SIGNATURE ls = {local};
      D3D12_STATE_SUBOBJECT sub[7] = {
         {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
         {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg},
         {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc},
         {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc},
         {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gs},
         {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &ls},
      };
      LPCWSTR hitExports[] = {L"HitGroup"};
      D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc = {&sub[5], 1, hitExports};
      sub[6] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &assoc};
      D3D12_STATE_OBJECT_DESC sd = {D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 7, sub};
      HRESULT h = dev5->CreateStateObject(&sd, __uuidof(ID3D12StateObject), (void **)&so);
      if (FAILED(h)) { detail = "CreateStateObject: " + hrs(h); break; }
      so->QueryInterface(__uuidof(ID3D12StateObjectProperties), (void **)&props);
      void *ids[3] = {props->GetShaderIdentifier(L"RayGen"), props->GetShaderIdentifier(L"Miss"),
                      props->GetShaderIdentifier(L"HitGroup")};
      if (!ids[0] || !ids[1] || !ids[2]) { detail = "no shader identifiers"; break; }

      // the shader tables: one record each, 64 bytes apart (the hit group's with its constant, 0.5)
      table = buffer12(d, 192, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      if (!table) { detail = "shader table"; break; }
      table->Map(0, nullptr, &p);
      for (int i = 0; i < 3; i++) memcpy((BYTE *)p + 64 * i, ids[i], D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      const float alpha = 0.5f;
      memcpy((BYTE *)p + 128 + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, &alpha, 4);
      table->Unmap(0, nullptr);

      D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
      if (FAILED(d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&heap))) { detail = "descriptor heap"; break; }
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.RaytracingAccelerationStructure.Location = tlas->GetGPUVirtualAddress();
      d.dev->CreateShaderResourceView(nullptr, &srv, heap->GetCPUDescriptorHandleForHeapStart());

      // build both, trace, read back
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {blas->GetGPUVirtualAddress(), bottom, 0,
                                                                scratch->GetGPUVirtualAddress()};
      list4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
      D3D12_RESOURCE_BARRIER uav = {};
      uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      uav.UAV.pResource = blas;
      d.list->ResourceBarrier(1, &uav);
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {tlas->GetGPUVirtualAddress(), top, 0,
                                                                scratch->GetGPUVirtualAddress()};
      list4->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
      uav.UAV.pResource = tlas;
      d.list->ResourceBarrier(1, &uav);
      d.list->SetDescriptorHeaps(1, &heap);
      d.list->SetComputeRootSignature(global);
      d.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
      d.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
      list4->SetPipelineState1(so);
      D3D12_GPU_VIRTUAL_ADDRESS t = table->GetGPUVirtualAddress();
      D3D12_DISPATCH_RAYS_DESC rays = {};
      rays.RayGenerationShaderRecord = {t, 64};
      rays.MissShaderTable = {t + 64, 64, 64};
      rays.HitGroupTable = {t + 128, 64, 64};
      rays.Width = 2;
      rays.Height = rays.Depth = 1;
      list4->DispatchRays(&rays);
      barrier12(d.list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      d.list->CopyResource(readback, out);
      if (!submit12(d)) { detail = "submitting or waiting failed"; break; }
      float v[8];
      readback->Map(0, nullptr, &p);
      memcpy(v, p, sizeof v);
      readback->Unmap(0, nullptr);
      const float want[8] = {0.25f, 0.5f, 1, 0.5f, 0.75f, 0, 0.25f, 1};
      ok = true;
      for (int i = 0; i < 8; i++) ok = ok && fabsf(v[i] - want[i]) < 0.01f;
      detail = fmt("hit (%.2f,%.2f,%.2f,%.2f), miss (%.2f,%.2f,%.2f,%.2f)", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
      if (!ok) detail += ", expected hit (0.25,0.50,1.00,0.50), miss (0.75,0.00,0.25,1.00)";
   } while (false);
   check(ok, name, detail);
   release(heap);
   release(props);
   release(so);
   release(local);
   release(global);
   for (ID3D12Resource *r : {vb, inst, blas, tlas, scratch, table, out, readback}) release(r);
   release(list4);
   release(dev5);
}

#include "dxil/dxil_feedback_cs.h"

// Sampler feedback: a compute shader records a sample of mip 0 at (0.1, 0.1) of a 64x64 texture (4 mips) in a MIN_MIP
// map with 16x16 regions; decoded, region (0,0) says 0, the other 15 0xff (never sampled). (A coarser mip's sample
// marks a region of that mip's texels, as NVIDIA's hardware does: mip 2 would cover all 16.)
static void testFeedback12(D12 &d, const D3D12_FEATURE_DATA_D3D12_OPTIONS7 &o7)
{
   const char *name = "Sampler feedback (MIN_MIP map, decoded)";
   ID3D12Device8 *dev8 = nullptr;
   ID3D12GraphicsCommandList1 *list1 = nullptr;
   if (o7.SamplerFeedbackTier == D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED ||
       FAILED(d.dev->QueryInterface(__uuidof(ID3D12Device8), (void **)&dev8)) ||
       FAILED(d.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList1), (void **)&list1))) {
      report(Status::Skip, name, "no sampler feedback");
      release(dev8);
      return;
   }
   if (!startTest(name)) {
      release(list1);
      release(dev8);
      return;
   }
   std::string detail;
   ID3D12Resource *tex = nullptr, *fb = nullptr, *decoded = nullptr, *readback = nullptr;
   ID3D12DescriptorHeap *gpuHeap = nullptr, *cpuHeap = nullptr;
   ID3D12RootSignature *rs = nullptr;
   ID3D12PipelineState *pso = nullptr;
   bool ok = false;
   do {
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
      D3D12_RESOURCE_DESC1 rd = {};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      rd.Width = rd.Height = 64;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 4;
      rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rd.SampleDesc.Count = 1;
      HRESULT h = dev8->CreateCommittedResource2(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 nullptr, nullptr, __uuidof(ID3D12Resource), (void **)&tex);
      if (FAILED(h)) { detail = "texture: " + hrs(h); break; }
      rd.Format = (DXGI_FORMAT)189;   // DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE (newer than MinGW's headers)
      rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      rd.SamplerFeedbackMipRegion = {16, 16, 1};
      h = dev8->CreateCommittedResource2(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                         nullptr, __uuidof(ID3D12Resource), (void **)&fb);
      if (FAILED(h)) { detail = "feedback map: " + hrs(h); break; }
      decoded = texture12(d, 4, 4, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
      readback = buffer12(d, 256 * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!decoded || !readback) { detail = "decode target"; break; }

      // t0, u0 (the feedback map's UAV) in a table; a point sampler
      D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
      d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&gpuHeap);
      hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
      d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&cpuHeap);
      if (!gpuHeap || !cpuHeap) { detail = "descriptor heaps"; break; }
      UINT inc = d.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      D3D12_CPU_DESCRIPTOR_HANDLE gcpu = gpuHeap->GetCPUDescriptorHandleForHeapStart(), ccpu = cpuHeap->GetCPUDescriptorHandleForHeapStart();
      D3D12_GPU_DESCRIPTOR_HANDLE ggpu = gpuHeap->GetGPUDescriptorHandleForHeapStart();
      d.dev->CreateShaderResourceView(tex, nullptr, gcpu);
      dev8->CreateSamplerFeedbackUnorderedAccessView(tex, fb, {gcpu.ptr + inc});
      dev8->CreateSamplerFeedbackUnorderedAccessView(tex, fb, {ccpu.ptr + inc});
      D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
      D3D12_ROOT_PARAMETER param = {};
      param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      param.DescriptorTable = {2, ranges};
      D3D12_STATIC_SAMPLER_DESC samp = {};
      samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
      samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      samp.MaxLOD = D3D12_FLOAT32_MAX;
      D3D12_ROOT_SIGNATURE_DESC rsd = {1, &param, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
      rs = rootSignature12(d, rsd, detail);
      if (!rs) break;
      D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
      pd.pRootSignature = rs;
      pd.CS = {dxil_feedback_cs, sizeof(dxil_feedback_cs)};
      h = d.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso);
      if (FAILED(h)) { detail = "CreateComputePipelineState: " + hrs(h); break; }

      // clear (to "never sampled"), record, decode, read back
      d.list->SetDescriptorHeaps(1, &gpuHeap);
      const UINT zero[4] = {};
      d.list->ClearUnorderedAccessViewUint({ggpu.ptr + inc}, {ccpu.ptr + inc}, fb, zero, 0, nullptr);
      D3D12_RESOURCE_BARRIER uav = {};
      uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      uav.UAV.pResource = fb;
      d.list->ResourceBarrier(1, &uav);
      d.list->SetComputeRootSignature(rs);
      d.list->SetPipelineState(pso);
      d.list->SetComputeRootDescriptorTable(0, ggpu);
      d.list->Dispatch(1, 1, 1);
      barrier12(d.list, fb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      // a MIN_MIP map decodes whole: source subresource UINT_MAX (the runtime insists)
      list1->ResolveSubresourceRegion(decoded, 0, 0, 0, fb, UINT_MAX, nullptr, DXGI_FORMAT_R8_UINT,
                                      D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
      barrier12(d.list, decoded, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
      dst.pResource = readback;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UINT, 4, 4, 1, 256};
      src.pResource = decoded;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      h = d.list->Close();
      if (FAILED(h)) {
         detail = "Close: " + hrs(h);
         d.alloc->Reset();
         d.list->Reset(d.alloc, nullptr);
         break;
      }
      ID3D12CommandList *lists[] = {d.list};
      d.queue->ExecuteCommandLists(1, lists);
      bool done = waitIdle12(d, d.queue);
      d.alloc->Reset();
      d.list->Reset(d.alloc, nullptr);
      if (!done) { detail = "waiting failed (device: " + hrs(d.dev->GetDeviceRemovedReason()) + ")"; break; }
      void *p;
      readback->Map(0, nullptr, &p);
      std::string got;
      ok = true;
      for (int y = 0; y < 4; y++)
         for (int x = 0; x < 4; x++) {
            int v = ((BYTE *)p)[y * 256 + x];
            got += fmt("%s%d", got.empty() ? "" : ",", v);
            ok = ok && v == (x == 0 && y == 0 ? 0 : 0xff);
         }
      readback->Unmap(0, nullptr);
      detail = "decoded " + got;
      if (!ok) detail += " (expected 0, then 255s)";
   } while (false);
   if (!ok) detail += debugMessages12(d);
   check(ok, name, detail);
   release(pso);
   release(rs);
   release(cpuHeap);
   release(gpuHeap);
   for (ID3D12Resource *r : {tex, fb, decoded, readback}) release(r);
   release(list1);
   release(dev8);
}

// Programmable sample positions (tier 1): a 4x MSAA rectangle whose right edge crosses pixel column 16 a quarter in;
// of the standard positions only one sample is left of it (resolved: 1/4 white), moved all to X = -6 (1/16ths from the
// center) all four are; back to the standard ones after SetSamplePositions(0, ...)
static void testSamplePositions12(D12 &d, ID3D12RootSignature *rs, const D3D12_FEATURE_DATA_D3D12_OPTIONS2 &o2)
{
   const char *name = "Programmable sample positions (4x MSAA edge coverage)";
   ID3D12GraphicsCommandList1 *list1 = nullptr;
   if (o2.ProgrammableSamplePositionsTier == D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED ||
       FAILED(d.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList1), (void **)&list1))) {
      report(Status::Skip, name, "no programmable sample positions");
      return;
   }
   if (!startTest(name)) {
      release(list1);
      return;
   }
   static const char *VS = "float4 main(uint id : SV_VertexID) : SV_Position {"
                           " const float e = 0.015625;"   // 16.25 of 32 pixels
                           " float2 c[6] = {float2(-1, -1), float2(-1, 1), float2(e, -1), float2(e, -1), float2(-1, 1), float2(e, 1)};"
                           " return float4(c[id], 0.5, 1); }";
   static const char *PS = "float4 main() : SV_Target { return float4(1, 1, 1, 1); }";
   std::string detail;
   ID3DBlob *vsb = compileHlsl(VS, "vs_5_0", &detail), *psb = compileHlsl(PS, "ps_5_0", &detail);
   ID3D12PipelineState *pso = nullptr;
   ID3D12Resource *msaa = nullptr;
   ID3D12DescriptorHeap *rtvHeap = nullptr;
   int got[3] = {-1, -1, -1};
   bool ok = false;
   do {
      if (!vsb || !psb) break;
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
      pd.pRootSignature = rs;
      pd.VS = {vsb->GetBufferPointer(), vsb->GetBufferSize()};
      pd.PS = {psb->GetBufferPointer(), psb->GetBufferSize()};
      pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pd.SampleMask = 0xffffffff;
      pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pd.RasterizerState.DepthClipEnable = TRUE;
      pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pd.NumRenderTargets = 1;
      pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      pd.SampleDesc.Count = 4;
      HRESULT h = d.dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso);
      if (FAILED(h)) { detail = "CreateGraphicsPipelineState: " + hrs(h); break; }
      D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
      D3D12_RESOURCE_DESC rd = {};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      rd.Width = rd.Height = Target12::size;
      rd.DepthOrArraySize = rd.MipLevels = 1;
      rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rd.SampleDesc.Count = 4;
      rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_CLEAR_VALUE cv = {DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 1}};
      h = d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                         __uuidof(ID3D12Resource), (void **)&msaa);
      D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      if (SUCCEEDED(h)) d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&rtvHeap);
      if (!msaa || !rtvHeap) { detail = "MSAA target: " + hrs(h); break; }
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
      d.dev->CreateRenderTargetView(msaa, nullptr, rtv);

      // standard, moved, standard again
      D3D12_SAMPLE_POSITION left[4] = {{-6, -6}, {-6, -2}, {-6, 2}, {-6, 6}};
      int phase;
      for (phase = 0; phase < 3; phase++) {
         Target12 t;
         std::vector<uint32_t> px;
         if (!target12(d, t)) break;
         const float black[4] = {0, 0, 0, 1};
         d.list->ClearRenderTargetView(rtv, black, 0, nullptr);
         d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
         d.list->SetGraphicsRootSignature(rs);
         d.list->SetPipelineState(pso);
         d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
         if (phase == 1) list1->SetSamplePositions(4, 1, left);
         if (phase == 2) list1->SetSamplePositions(0, 0, nullptr);
         d.list->DrawInstanced(6, 1, 0, 0);
         barrier12(d.list, msaa, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
         barrier12(d.list, t.tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST);
         d.list->ResolveSubresource(t.tex, 0, msaa, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
         barrier12(d.list, msaa, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
         barrier12(d.list, t.tex, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
         bool read = readTarget12(d, t, px);
         t.release();
         if (!read) break;
         got[phase] = px[16 * Target12::size + 16] & 0xff;
      }
      if (phase < 3) { detail = "rendering or reading back failed"; break; }
      ok = near8(got[0], 64, 3) && got[1] == 255 && near8(got[2], 64, 3);
      detail = fmt("edge pixel %d (standard), %d (moved), %d (standard again); expected 64, 255, 64", got[0], got[1], got[2]);
   } while (false);
   if (!ok) detail += debugMessages12(d);
   check(ok, name, detail);
   release(rtvHeap);
   release(msaa);
   release(pso);
   release(vsb);
   release(psb);
   release(list1);
}

// WriteBufferImmediate: two 32-bit values written from the command list (both modes), read back
static void testWriteImmediate12(D12 &d)
{
   const char *name = "WriteBufferImmediate (breadcrumb writes)";
   D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof o3);
   ID3D12GraphicsCommandList2 *list2 = nullptr;
   if (!(o3.WriteBufferImmediateSupportFlags & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT) ||
       FAILED(d.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList2), (void **)&list2))) {
      report(Status::Skip, name, "no WriteBufferImmediate on direct lists");
      return;
   }
   if (!startTest(name)) {
      release(list2);
      return;
   }
   std::string detail;
   ID3D12Resource *buf = buffer12(d, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *readback = buffer12(d, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   bool ok = false;
   if (buf && readback) {
      D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[2] = {{buf->GetGPUVirtualAddress(), 0x12345678},
                                                        {buf->GetGPUVirtualAddress() + 4, 0x9abcdef0}};
      D3D12_WRITEBUFFERIMMEDIATE_MODE modes[2] = {D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
                                                  D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT};
      list2->WriteBufferImmediate(2, params, modes);
      barrier12(d.list, buf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      d.list->CopyBufferRegion(readback, 0, buf, 0, 8);
      if (submit12(d)) {
         void *p;
         readback->Map(0, nullptr, &p);
         uint32_t v[2];
         memcpy(v, p, sizeof v);
         readback->Unmap(0, nullptr);
         ok = v[0] == 0x12345678 && v[1] == 0x9abcdef0;
         detail = fmt("0x%08x 0x%08x", v[0], v[1]);
      } else {
         detail = "submitting or waiting failed";
      }
   } else {
      detail = "buffers";
   }
   check(ok, name, detail);
   release(buf);
   release(readback);
   release(list2);
}

#include "dxil/dxil_sm68_vs.h"
#include "dxil/dxil_sm68_ps.h"

// Shader model 6.8 (6.7 too): a draw starting at vertex 5, instance 7, whose vertex shader reads them back
// (SV_StartVertexLocation/SV_StartInstanceLocation) for the pixel shader: (5, 7, 128) everywhere
static const D3D_SHADER_MODEL SM_6_8 = (D3D_SHADER_MODEL)0x68;   // newer than MinGW's d3d12.h

static void testSm68(D12 &d, ID3D12RootSignature *rs, D3D_SHADER_MODEL highest)
{
   const char *name = "Shader model 6.8 (start vertex and instance in a vertex shader)";
   if (highest < SM_6_8) {
      report(Status::Skip, name, "no shader model 6.8");
      return;
   }
   if (!startTest(name)) return;
   D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
   pd.pRootSignature = rs;
   pd.VS = {dxil_sm68_vs, sizeof(dxil_sm68_vs)};
   pd.PS = {dxil_sm68_ps, sizeof(dxil_sm68_ps)};
   pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
   pd.SampleMask = 0xffffffff;
   pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
   pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
   pd.RasterizerState.DepthClipEnable = TRUE;
   pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
   pd.NumRenderTargets = 1;
   pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
   pd.SampleDesc.Count = 1;
   ID3D12PipelineState *pso = nullptr;
   HRESULT h = d.dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso);
   Target12 t;
   std::vector<uint32_t> px;
   std::string detail = FAILED(h) ? "CreateGraphicsPipelineState: " + hrs(h) : "";
   bool ok = SUCCEEDED(h) && target12(d, t);
   if (ok) {
      d.list->SetGraphicsRootSignature(rs);
      d.list->SetPipelineState(pso);
      d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      d.list->DrawInstanced(3, 1, 5, 7);
      ok = readTarget12(d, t, px) && expectRgba(px[16 * Target12::size + 16], 5, 7, 128, detail, 1, 16, 16);
   }
   check(ok, name, detail.empty() && !ok ? "rendering or reading back failed" : detail);
   t.release();
   release(pso);
}

// The Vulkan-on-12 caps (DDI 0102): newer than MinGW's d3d12.h
static const D3D12_FEATURE FEATURE_OPTIONS19 = (D3D12_FEATURE)48;
struct Options19 {
   BOOL MismatchingOutputDimensionsSupported;
   UINT SupportedSampleCountsWithNoOutputs;
   BOOL PointSamplingAddressesNeverRoundUp;
   BOOL RasterizerDesc2Supported;
   BOOL NarrowQuadrilateralLinesSupported;
   BOOL AnisoFilterWithPointMipSupported;
   UINT MaxSamplerDescriptorHeapSize;
   UINT MaxSamplerDescriptorHeapSizeWithStaticSamplers;
   UINT MaxViewDescriptorHeapSize;
   BOOL ComputeOnlyCustomHeapSupported;
};

// Reads back the first uint32 of a UAV buffer (in UNORDERED_ACCESS) after running the list
static bool readUav12(D12 &d, ID3D12Resource *uav, uint32_t &value)
{
   ID3D12Resource *rb = buffer12(d, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   if (!rb) return false;
   barrier12(d.list, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
   d.list->CopyBufferRegion(rb, 0, uav, 0, 4);
   void *p = nullptr;
   bool ok = submit12(d) && SUCCEEDED(rb->Map(0, nullptr, &p));
   if (ok) {
      memcpy(&value, p, 4);
      rb->Unmap(0, nullptr);
   }
   rb->Release();
   return ok;
}

// Expanded sampler heaps: a shader-visible heap of 4096 samplers, a border-color sampler in its last slot (a clamping
// one in its first), a compute shader sampling a 1x1 texture (0.25) outside it through the last: the border's 1.0
static void testSamplerHeap12(D12 &d, const Options19 &o19)
{
   const char *name = "Sampler heaps of 4096 descriptors";
   if (o19.MaxSamplerDescriptorHeapSize < 4096) {
      optional(false, name, fmt("not supported: at most %u samplers", o19.MaxSamplerDescriptorHeapSize));
      return;
   }
   if (!startTest(name)) return;
   std::string detail;
   D3D12_DESCRIPTOR_RANGE srv = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                          smp = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
   D3D12_ROOT_PARAMETER params[3] = {};
   params[0].ParameterType = params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
   params[0].DescriptorTable = {1, &srv};
   params[1].DescriptorTable = {1, &smp};
   params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
   D3D12_ROOT_SIGNATURE_DESC desc = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
   ID3D12RootSignature *rs = rootSignature12(d, desc, detail);
   ID3DBlob *cs = compileHlsl("Texture2D<float> t : register(t0); SamplerState s : register(s0);"
                              "RWStructuredBuffer<float> o : register(u0);"
                              "[numthreads(1, 1, 1)] void main() { o[0] = t.SampleLevel(s, float2(-1.5, -1.5), 0); }",
                              "cs_5_0", &detail);
   ID3D12PipelineState *pso = nullptr;
   if (rs && cs) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
      cd.pRootSignature = rs;
      cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
      HRESULT h = d.dev->CreateComputePipelineState(&cd, __uuidof(ID3D12PipelineState), (void **)&pso);
      if (FAILED(h)) detail = "CreateComputePipelineState: " + hrs(h);
   }
   ID3D12DescriptorHeap *views = nullptr, *samplers = nullptr;
   D3D12_DESCRIPTOR_HEAP_DESC vd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0},
                              sd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4096, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
   d.dev->CreateDescriptorHeap(&vd, __uuidof(ID3D12DescriptorHeap), (void **)&views);
   HRESULT hs = d.dev->CreateDescriptorHeap(&sd, __uuidof(ID3D12DescriptorHeap), (void **)&samplers);
   if (FAILED(hs)) detail = "CreateDescriptorHeap (4096 samplers): " + hrs(hs);
   ID3D12Resource *tex = texture12(d, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *up = buffer12(d, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
   ID3D12Resource *uav = buffer12(d, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
   uint32_t got = 0;
   bool ok = pso && views && samplers && tex && up && uav;
   if (ok) {
      float *p = nullptr;
      up->Map(0, nullptr, (void **)&p);
      *p = 0.25f;
      up->Unmap(0, nullptr);
      D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
      dst.pResource = tex;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.pResource = up;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256};
      d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      barrier12(d.list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      d.dev->CreateShaderResourceView(tex, nullptr, views->GetCPUDescriptorHandleForHeapStart());
      D3D12_SAMPLER_DESC clamp = {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                  D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1,
                                  D3D12_COMPARISON_FUNC_NEVER, {0, 0, 0, 0}, 0, 1000};
      D3D12_SAMPLER_DESC border = clamp;
      border.AddressU = border.AddressV = border.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
      border.BorderColor[0] = border.BorderColor[1] = border.BorderColor[2] = border.BorderColor[3] = 1;
      UINT inc = d.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
      D3D12_CPU_DESCRIPTOR_HANDLE last = samplers->GetCPUDescriptorHandleForHeapStart();
      d.dev->CreateSampler(&clamp, last);
      last.ptr += 4095 * (SIZE_T)inc;
      d.dev->CreateSampler(&border, last);
      D3D12_GPU_DESCRIPTOR_HANDLE lastGpu = samplers->GetGPUDescriptorHandleForHeapStart();
      lastGpu.ptr += 4095 * (UINT64)inc;
      ID3D12DescriptorHeap *heaps[] = {views, samplers};
      d.list->SetDescriptorHeaps(2, heaps);
      d.list->SetComputeRootSignature(rs);
      d.list->SetPipelineState(pso);
      d.list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
      d.list->SetComputeRootDescriptorTable(1, lastGpu);
      d.list->SetComputeRootUnorderedAccessView(2, uav->GetGPUVirtualAddress());
      d.list->Dispatch(1, 1, 1);
      ok = readUav12(d, uav, got);
      float f;
      memcpy(&f, &got, 4);
      if (ok) detail = fmt("sampled %.2f through sampler 4095 (expected 1.00, its border; 0.25 is the clamping sampler)", f);
      else detail = "running or reading back failed";
      ok = ok && f == 1.0f;
   }
   check(ok, name, detail + debugMessages12(d));
   release(uav);
   release(up);
   release(tex);
   release(samplers);
   release(views);
   release(pso);
   release(cs);
   release(rs);
}

// Rendering without outputs at the pipeline's sample count: no render target or depth, SampleDesc.Count 4, a pixel
// shader ORing its coverage into a UAV: all four samples (0xf)
static void testNoOutputSamples12(D12 &d, const Options19 &o19)
{
   const char *name = "Rendering without outputs at 4 samples (UAV-only)";
   if (!(o19.SupportedSampleCountsWithNoOutputs & 4)) {
      optional(false, name, fmt("not supported: sample counts 0x%x", o19.SupportedSampleCountsWithNoOutputs));
      return;
   }
   if (!startTest(name)) return;
   std::string detail;
   D3D12_ROOT_PARAMETER param = {};
   param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
   D3D12_ROOT_SIGNATURE_DESC desc = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
   ID3D12RootSignature *rs = rootSignature12(d, desc, detail);
   ID3DBlob *vsb = compileHlsl(VS12, "vs_5_0", &detail);
   ID3DBlob *psb = compileHlsl("RWByteAddressBuffer o : register(u0);"
                               "void main(float4 p : SV_Position, uint c : SV_Coverage) { uint was; o.InterlockedOr(0, c, was); }",
                               "ps_5_0", &detail);
   ID3D12PipelineState *pso = nullptr;
   if (rs && vsb && psb) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
      pd.pRootSignature = rs;
      pd.VS = {vsb->GetBufferPointer(), vsb->GetBufferSize()};
      pd.PS = {psb->GetBufferPointer(), psb->GetBufferSize()};
      pd.SampleMask = 0xffffffff;
      pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pd.RasterizerState.DepthClipEnable = TRUE;
      pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pd.SampleDesc.Count = 4;
      HRESULT h = d.dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso);
      if (FAILED(h)) detail = "CreateGraphicsPipelineState: " + hrs(h);
   }
   ID3D12Resource *uav = buffer12(d, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
   uint32_t got = 0;
   bool ok = pso && uav;
   if (ok) {
      D3D12_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
      D3D12_RECT sc = {0, 0, 4, 4};
      d.list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
      d.list->RSSetViewports(1, &vp);
      d.list->RSSetScissorRects(1, &sc);
      d.list->SetGraphicsRootSignature(rs);
      d.list->SetPipelineState(pso);
      d.list->SetGraphicsRootUnorderedAccessView(0, uav->GetGPUVirtualAddress());
      d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      d.list->DrawInstanced(3, 1, 0, 0);
      ok = readUav12(d, uav, got);
      detail = ok ? fmt("coverage 0x%x (expected 0xf)", got) : "running or reading back failed";
      ok = ok && got == 0xf;
   }
   check(ok, name, detail + debugMessages12(d));
   release(uav);
   release(pso);
   release(psb);
   release(vsb);
   release(rs);
}

// Enhanced barriers: a texture made in the UNORDERED_ACCESS layout (CreateCommittedResource3) and a buffer, both
// written by a compute shader, then a texture barrier (to COPY_SOURCE) and a buffer barrier (compute to copy) before
// copying them back: the texture's pixel (3, 2) is (191, 128, 128) and the buffer holds 3 * 2 + 1
static void testEnhancedBarriers12(D12 &d)
{
   const char *name = "Enhanced barriers (texture layouts, buffer and texture barriers)";
   D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12);
   ID3D12Device10 *dev10 = nullptr;
   ID3D12GraphicsCommandList7 *list7 = nullptr;
   if (!o12.EnhancedBarriersSupported || FAILED(d.dev->QueryInterface(__uuidof(ID3D12Device10), (void **)&dev10)) ||
       FAILED(d.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList7), (void **)&list7))) {
      optional(false, name);
      release(dev10);
      release(list7);
      return;
   }
   if (!startTest(name)) {
      release(dev10);
      release(list7);
      return;
   }
   std::string detail;
   D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
   D3D12_ROOT_PARAMETER params[2] = {};
   params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
   params[0].DescriptorTable = {1, &range};
   params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
   params[1].Descriptor.ShaderRegister = 1;
   D3D12_ROOT_SIGNATURE_DESC desc = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
   ID3D12RootSignature *rs = rootSignature12(d, desc, detail);
   ID3DBlob *cs = compileHlsl("RWTexture2D<float4> t : register(u0); RWStructuredBuffer<uint> b : register(u1);"
                              "[numthreads(4, 4, 1)] void main(uint2 id : SV_DispatchThreadID)"
                              "{ t[id] = float4(id.x / 4.0, id.y / 4.0, 0.5, 1); b[id.y * 4 + id.x] = id.x * id.y + 1; }",
                              "cs_5_0", &detail);
   ID3D12PipelineState *pso = nullptr;
   if (rs && cs) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
      cd.pRootSignature = rs;
      cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
      HRESULT h = d.dev->CreateComputePipelineState(&cd, __uuidof(ID3D12PipelineState), (void **)&pso);
      if (FAILED(h)) detail = "CreateComputePipelineState: " + hrs(h);
   }
   D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
   D3D12_RESOURCE_DESC1 td = {};
   td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   td.Width = td.Height = 4;
   td.DepthOrArraySize = td.MipLevels = 1;
   td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   td.SampleDesc.Count = 1;
   td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
   ID3D12Resource *tex = nullptr;
   HRESULT ht = dev10->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr,
                                               nullptr, 0, nullptr, __uuidof(ID3D12Resource), (void **)&tex);
   if (FAILED(ht)) detail = "CreateCommittedResource3: " + hrs(ht);
   ID3D12Resource *buf = buffer12(d, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
   ID3D12Resource *rb = buffer12(d, 2048, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12DescriptorHeap *heap = nullptr;
   D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
   d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&heap);
   bool ok = pso && tex && buf && rb && heap;
   if (ok) {
      d.dev->CreateUnorderedAccessView(tex, nullptr, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
      ID3D12DescriptorHeap *heaps[] = {heap};
      d.list->SetDescriptorHeaps(1, heaps);
      d.list->SetComputeRootSignature(rs);
      d.list->SetPipelineState(pso);
      d.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
      d.list->SetComputeRootUnorderedAccessView(1, buf->GetGPUVirtualAddress());
      d.list->Dispatch(1, 1, 1);
      D3D12_TEXTURE_BARRIER tb = {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COPY,
                                  D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                  D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_COPY_SOURCE, tex,
                                  {0, 1, 0, 1, 0, 1}, D3D12_TEXTURE_BARRIER_FLAG_NONE};
      D3D12_BUFFER_BARRIER bb = {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COPY,
                                 D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE, buf, 0, UINT64_MAX};
      D3D12_BARRIER_GROUP groups[2] = {};
      groups[0].Type = D3D12_BARRIER_TYPE_TEXTURE;
      groups[0].NumBarriers = 1;
      groups[0].pTextureBarriers = &tb;
      groups[1].Type = D3D12_BARRIER_TYPE_BUFFER;
      groups[1].NumBarriers = 1;
      groups[1].pBufferBarriers = &bb;
      list7->Barrier(2, groups);
      D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
      dst.pResource = rb;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256};
      src.pResource = tex;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      d.list->CopyBufferRegion(rb, 1024, buf, 0, 64);
      uint8_t *p = nullptr;
      ok = submit12(d) && SUCCEEDED(rb->Map(0, nullptr, (void **)&p));
      if (ok) {
         uint32_t px, v;
         memcpy(&px, p + 2 * 256 + 3 * 4, 4);
         memcpy(&v, p + 1024 + (2 * 4 + 3) * 4, 4);
         rb->Unmap(0, nullptr);
         ok = expectRgba(px, 191, 128, 128, detail, 2, 3, 2) && v == 7;
         detail += fmt("%sbuffer[11] %u (expected 7)", detail.empty() ? "" : "; ", v);
      } else {
         detail = "running or reading back failed";
      }
   }
   check(ok, name, detail + debugMessages12(d));
   release(heap);
   release(rb);
   release(buf);
   release(tex);
   release(pso);
   release(cs);
   release(rs);
   release(list7);
   release(dev10);
}

// Relaxed format casting: an R32_UINT texture made with R8G8B8A8_UNORM among its castable formats
// (CreateCommittedResource3), written as R8G8B8A8_UNORM by a compute shader: (1, 0.5, 0, 1) reads back as 0xff0080ff
// (green 127 or 128: 127.5 may round either way)
static void testCastableFormats12(D12 &d)
{
   const char *name = "Relaxed format casting (castable format lists)";
   D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12);
   ID3D12Device10 *dev10 = nullptr;
   if (!o12.RelaxedFormatCastingSupported || FAILED(d.dev->QueryInterface(__uuidof(ID3D12Device10), (void **)&dev10))) {
      optional(false, name);
      release(dev10);
      return;
   }
   if (!startTest(name)) {
      release(dev10);
      return;
   }
   std::string detail;
   D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
   D3D12_ROOT_PARAMETER param = {};
   param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
   param.DescriptorTable = {1, &range};
   D3D12_ROOT_SIGNATURE_DESC desc = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
   ID3D12RootSignature *rs = rootSignature12(d, desc, detail);
   ID3DBlob *cs = compileHlsl("RWTexture2D<unorm float4> t : register(u0);"
                              "[numthreads(4, 4, 1)] void main(uint2 id : SV_DispatchThreadID) { t[id] = float4(1, 0.5, 0, 1); }",
                              "cs_5_0", &detail);
   ID3D12PipelineState *pso = nullptr;
   if (rs && cs) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
      cd.pRootSignature = rs;
      cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
      HRESULT h = d.dev->CreateComputePipelineState(&cd, __uuidof(ID3D12PipelineState), (void **)&pso);
      if (FAILED(h)) detail = "CreateComputePipelineState: " + hrs(h);
   }
   D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
   D3D12_RESOURCE_DESC1 td = {};
   td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   td.Width = td.Height = 4;
   td.DepthOrArraySize = td.MipLevels = 1;
   td.Format = DXGI_FORMAT_R32_UINT;
   td.SampleDesc.Count = 1;
   td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
   DXGI_FORMAT casts[] = {DXGI_FORMAT_R8G8B8A8_UNORM};
   ID3D12Resource *tex = nullptr;
   HRESULT ht = dev10->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr,
                                               nullptr, 1, casts, __uuidof(ID3D12Resource), (void **)&tex);
   if (FAILED(ht)) detail = "CreateCommittedResource3 (castable R8G8B8A8_UNORM): " + hrs(ht);
   ID3D12Resource *rb = buffer12(d, 1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12DescriptorHeap *heap = nullptr;
   D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
   d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&heap);
   bool ok = pso && tex && rb && heap;
   if (ok) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_UAV_DIMENSION_TEXTURE2D};
      d.dev->CreateUnorderedAccessView(tex, nullptr, &ud, heap->GetCPUDescriptorHandleForHeapStart());
      ID3D12DescriptorHeap *heaps[] = {heap};
      d.list->SetDescriptorHeaps(1, heaps);
      d.list->SetComputeRootSignature(rs);
      d.list->SetPipelineState(pso);
      d.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
      d.list->Dispatch(1, 1, 1);
      barrier12(d.list, tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
      dst.pResource = rb;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 4, 4, 1, 256};
      src.pResource = tex;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      uint8_t *p = nullptr;
      ok = submit12(d) && SUCCEEDED(rb->Map(0, nullptr, (void **)&p));
      if (ok) {
         uint32_t v;
         memcpy(&v, p + 256 + 4, 4);
         rb->Unmap(0, nullptr);
         ok = (v & 0xffff00ff) == 0xff0000ff && near8((v >> 8) & 0xff, 128, 1);
         detail = fmt("texel (1, 1) 0x%08x as R32_UINT (expected 0xff0080ff)", v);
      } else {
         detail = "running or reading back failed";
      }
   }
   check(ok, name, detail + debugMessages12(d));
   release(heap);
   release(rb);
   release(tex);
   release(pso);
   release(cs);
   release(rs);
   release(dev10);
}

// DDI 0110's caps: ExecuteIndirect's tier (11: 1.1, incrementing constants) and shader model 6.8's comparison
// sampling with gradients and bias; newer than MinGW's d3d12.h
static const D3D12_FEATURE FEATURE_OPTIONS21 = (D3D12_FEATURE)53;
struct Options21 {
   UINT WorkGraphsTier;
   UINT ExecuteIndirectTier;
   BOOL SampleCmpGradientAndBiasSupported;
   BOOL ExtendedCommandInfoSupported;
};

#include "dxil/dxil_sm68cmp_cs.h"

// SampleCmpGrad: a 1x1 depth of 0.5 compared (LESS) at 0.25 and 0.75 in a cs_6_8: 1 and 0
static void testSampleCmpGrad12(D12 &d, const Options21 &o21)
{
   const char *name = "Shader model 6.8 comparison sampling with gradients (SampleCmpGrad)";
   if (!o21.SampleCmpGradientAndBiasSupported) {
      optional(false, name);
      return;
   }
   if (!startTest(name)) return;
   std::string detail;
   D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
   D3D12_ROOT_PARAMETER params[2] = {};
   params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
   params[0].DescriptorTable = {1, &range};
   params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
   D3D12_STATIC_SAMPLER_DESC smp = {};
   smp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
   smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
   smp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
   smp.MaxLOD = 1000;
   D3D12_ROOT_SIGNATURE_DESC desc = {2, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
   ID3D12RootSignature *rs = rootSignature12(d, desc, detail);
   ID3D12PipelineState *pso = nullptr;
   if (rs) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
      cd.pRootSignature = rs;
      cd.CS = {dxil_sm68cmp_cs, sizeof(dxil_sm68cmp_cs)};
      HRESULT h = d.dev->CreateComputePipelineState(&cd, __uuidof(ID3D12PipelineState), (void **)&pso);
      if (FAILED(h)) detail = "CreateComputePipelineState: " + hrs(h);
   }
   ID3D12Resource *tex = texture12(d, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *up = buffer12(d, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
   ID3D12Resource *uav = buffer12(d, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
   ID3D12Resource *rb = buffer12(d, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12DescriptorHeap *heap = nullptr;
   D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
   d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&heap);
   bool ok = pso && tex && up && uav && rb && heap;
   if (ok) {
      float *p = nullptr;
      up->Map(0, nullptr, (void **)&p);
      *p = 0.5f;
      up->Unmap(0, nullptr);
      D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
      dst.pResource = tex;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.pResource = up;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256};
      d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      barrier12(d.list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      d.dev->CreateShaderResourceView(tex, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
      ID3D12DescriptorHeap *heaps[] = {heap};
      d.list->SetDescriptorHeaps(1, heaps);
      d.list->SetComputeRootSignature(rs);
      d.list->SetPipelineState(pso);
      d.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
      d.list->SetComputeRootUnorderedAccessView(1, uav->GetGPUVirtualAddress());
      d.list->Dispatch(1, 1, 1);
      barrier12(d.list, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      d.list->CopyBufferRegion(rb, 0, uav, 0, 8);
      float v[2] = {-1, -1};
      void *m = nullptr;
      ok = submit12(d) && SUCCEEDED(rb->Map(0, nullptr, &m));
      if (ok) {
         memcpy(v, m, sizeof v);
         rb->Unmap(0, nullptr);
      }
      detail = ok ? fmt("0.5 compared at 0.25: %.2f, at 0.75: %.2f (expected 1.00, 0.00)", v[0], v[1])
                  : "running or reading back failed";
      ok = ok && v[0] == 1.0f && v[1] == 0.0f;
   }
   check(ok, name, detail + debugMessages12(d));
   release(heap);
   release(rb);
   release(uav);
   release(up);
   release(tex);
   release(pso);
   release(rs);
}

// ExecuteIndirect with state changes: 4 dispatches at most, 3 by the count buffer, each setting root constants
// (index, value) before writing out[index] = value. With tier 1.1 the index is an incrementing constant (the
// command's number) instead of an argument. vkd3d-proton runs both on device-generated commands where it has them
static void testExecuteIndirectConstants12(D12 &d, const Options21 &o21)
{
   bool incrementing = o21.ExecuteIndirectTier >= 11;
   std::string name = std::string("ExecuteIndirect with root constants and a count buffer") +
                      (incrementing ? " (incrementing constants)" : "");
   if (!startTest(name)) return;
   std::string detail;
   D3D12_ROOT_PARAMETER params[2] = {};
   params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
   params[0].Constants = {0, 0, 2};
   params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
   D3D12_ROOT_SIGNATURE_DESC desc = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
   ID3D12RootSignature *rs = rootSignature12(d, desc, detail);
   ID3DBlob *cs = compileHlsl("cbuffer C : register(b0) { uint idx; uint val; }; RWStructuredBuffer<uint> o : register(u0);"
                              "[numthreads(1, 1, 1)] void main() { o[idx] = val; }",
                              "cs_5_0", &detail);
   ID3D12PipelineState *pso = nullptr;
   if (rs && cs) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
      cd.pRootSignature = rs;
      cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
      HRESULT h = d.dev->CreateComputePipelineState(&cd, __uuidof(ID3D12PipelineState), (void **)&pso);
      if (FAILED(h)) detail = "CreateComputePipelineState: " + hrs(h);
   }
   // the arguments: (index, value) or, incrementing, (value), then the dispatch
   D3D12_INDIRECT_ARGUMENT_DESC args[3] = {};
   UINT n = 0, stride;
   if (incrementing) {
      args[n].Type = (D3D12_INDIRECT_ARGUMENT_TYPE)11; // INCREMENTING_CONSTANT, newer than MinGW's d3d12.h
      args[n++].Constant = {0, 0, 1};
      args[n].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
      args[n++].Constant = {0, 1, 1};
      stride = 16;
   } else {
      args[n].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
      args[n++].Constant = {0, 0, 2};
      stride = 20;
   }
   args[n++].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
   D3D12_COMMAND_SIGNATURE_DESC sd = {stride, n, args, 0};
   ID3D12CommandSignature *sig = nullptr;
   if (pso) {
      HRESULT h = d.dev->CreateCommandSignature(&sd, rs, __uuidof(ID3D12CommandSignature), (void **)&sig);
      if (FAILED(h)) detail = "CreateCommandSignature: " + hrs(h);
   }
   ID3D12Resource *argBuf = buffer12(d, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
   ID3D12Resource *uav = buffer12(d, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
   ID3D12Resource *rb = buffer12(d, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   bool ok = sig && argBuf && uav && rb;
   if (ok) {
      UINT *p = nullptr;
      argBuf->Map(0, nullptr, (void **)&p);
      memset(p, 0, 256);
      UINT *a = p;
      for (UINT i = 0; i < 4; i++) {
         if (!incrementing) *a++ = i;
         *a++ = 10 * (i + 1);
         *a++ = 1;
         *a++ = 1;
         *a++ = 1;
      }
      p[60] = 3; // the count, at byte 240
      argBuf->Unmap(0, nullptr);
      d.list->SetComputeRootSignature(rs);
      d.list->SetPipelineState(pso);
      d.list->SetComputeRootUnorderedAccessView(1, uav->GetGPUVirtualAddress());
      d.list->ExecuteIndirect(sig, 4, argBuf, 0, argBuf, 240);
      barrier12(d.list, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      d.list->CopyBufferRegion(rb, 0, uav, 0, 16);
      void *m = nullptr;
      ok = submit12(d) && SUCCEEDED(rb->Map(0, nullptr, &m));
      UINT out[4] = {};
      if (ok) {
         memcpy(out, m, sizeof out);
         rb->Unmap(0, nullptr);
      }
      detail = ok ? fmt("out = %u, %u, %u, %u (expected 10, 20, 30, 0)", out[0], out[1], out[2], out[3])
                  : "running or reading back failed";
      ok = ok && out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 0;
   }
   check(ok, name, detail + debugMessages12(d));
   release(rb);
   release(uav);
   release(argBuf);
   release(sig);
   release(pso);
   release(cs);
   release(rs);
}

// ExecuteIndirect drawing with state changes: two indexed draws, each with its own vertex buffer (the left and the
// right half of the target), the index buffer, and a root constant picking the color (red, green)
static void testExecuteIndirectDraws12(D12 &d)
{
   const char *name = "ExecuteIndirect drawing with vertex/index buffer and root constant changes";
   if (!startTest(name)) return;
   std::string detail;
   D3D12_ROOT_PARAMETER param = {};
   param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
   param.Constants = {0, 0, 1};
   param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
   D3D12_ROOT_SIGNATURE_DESC desc = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
   ID3D12RootSignature *rs = rootSignature12(d, desc, detail);
   ID3DBlob *vs = compileHlsl("float4 main(float2 p : POSITION) : SV_Position { return float4(p, 0.5, 1); }", "vs_5_0",
                              &detail);
   ID3DBlob *ps = compileHlsl("cbuffer C : register(b0) { uint c; };"
                              "float4 main() : SV_Target { return c ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1); }",
                              "ps_5_0", &detail);
   ID3D12PipelineState *pso = nullptr;
   if (rs && vs && ps) {
      D3D12_INPUT_ELEMENT_DESC el = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
      pd.pRootSignature = rs;
      pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
      pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
      pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pd.SampleMask = 0xffffffff;
      pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pd.InputLayout = {&el, 1};
      pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pd.NumRenderTargets = 1;
      pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      pd.SampleDesc.Count = 1;
      HRESULT h = d.dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso);
      if (FAILED(h)) detail = "CreateGraphicsPipelineState: " + hrs(h);
   }
   D3D12_INDIRECT_ARGUMENT_DESC args[4] = {};
   args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
   args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
   args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
   args[2].Constant = {0, 0, 1};
   args[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
   struct Command {
      D3D12_VERTEX_BUFFER_VIEW vb;
      D3D12_INDEX_BUFFER_VIEW ib;
      UINT color;
      D3D12_DRAW_INDEXED_ARGUMENTS draw;
   };
   D3D12_COMMAND_SIGNATURE_DESC sd = {sizeof(Command), 4, args, 0};
   ID3D12CommandSignature *sig = nullptr;
   if (pso) {
      HRESULT h = d.dev->CreateCommandSignature(&sd, rs, __uuidof(ID3D12CommandSignature), (void **)&sig);
      if (FAILED(h)) detail = "CreateCommandSignature: " + hrs(h);
   }
   ID3D12Resource *geo = buffer12(d, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
   ID3D12Resource *argBuf = buffer12(d, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
   Target12 t;
   std::vector<uint32_t> px;
   bool ok = sig && geo && argBuf && target12(d, t);
   if (ok) {
      // the quads (left, right) at 0 and 32, the indices at 64
      const float quads[16] = {-1, -1, -1, 1, 0, -1, 0, 1, 0, -1, 0, 1, 1, -1, 1, 1};
      const UINT16 idx[6] = {0, 1, 2, 2, 1, 3};
      BYTE *g = nullptr;
      geo->Map(0, nullptr, (void **)&g);
      memcpy(g, quads, sizeof quads);
      memcpy(g + 64, idx, sizeof idx);
      geo->Unmap(0, nullptr);
      D3D12_GPU_VIRTUAL_ADDRESS va = geo->GetGPUVirtualAddress();
      Command *c = nullptr;
      argBuf->Map(0, nullptr, (void **)&c);
      for (UINT i = 0; i < 2; i++)
         c[i] = {{va + 32 * i, 32, 8}, {va + 64, 12, DXGI_FORMAT_R16_UINT}, 1 - i, {6, 1, 0, 0, 0}};
      argBuf->Unmap(0, nullptr);
      d.list->SetGraphicsRootSignature(rs);
      d.list->SetPipelineState(pso);
      d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      d.list->ExecuteIndirect(sig, 2, argBuf, 0, nullptr, 0);
      ok = readTarget12(d, t, px) && expectRgba(px[16 * Target12::size + 8], 255, 0, 0, detail, 3, 8, 16) &&
           expectRgba(px[16 * Target12::size + 24], 0, 255, 0, detail, 3, 24, 16);
   }
   check(ok, name, detail + debugMessages12(d));
   t.release();
   release(argBuf);
   release(geo);
   release(sig);
   release(pso);
   release(ps);
   release(vs);
   release(rs);
}

// Work graphs (newer than MinGW's d3d12.h): what the test uses of the API. Methods returning a struct take the result's
// address after this, as MSVC's COM ABI passes it (directml.inc)
static const D3D12_STATE_SUBOBJECT_TYPE SUBOBJECT_WORK_GRAPH = (D3D12_STATE_SUBOBJECT_TYPE)13;
static const D3D12_STATE_OBJECT_TYPE STATE_OBJECT_EXECUTABLE = (D3D12_STATE_OBJECT_TYPE)4;
struct NodeId { LPCWSTR Name; UINT ArrayIndex; };
struct WorkGraphDesc {
   LPCWSTR ProgramName;
   UINT Flags; // 1: include all available nodes
   UINT NumEntrypoints;
   const NodeId *pEntrypoints;
   UINT NumExplicitlyDefinedNodes;
   const void *pExplicitlyDefinedNodes;
};
struct ProgramId { UINT64 OpaqueData[4]; };
struct WorkGraphMemory { UINT64 MinSizeInBytes, MaxSizeInBytes; UINT SizeGranularityInBytes; };
struct SetProgramDesc {
   UINT Type; // 5: work graph
   ProgramId ProgramIdentifier;
   UINT Flags; // 1: initialize the backing memory
   D3D12_GPU_VIRTUAL_ADDRESS_RANGE BackingMemory;
   D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE NodeLocalRootArgumentsTable;
};
struct NodeCpuInput {
   UINT EntrypointIndex;
   UINT NumRecords;
   const void *pRecords;
   UINT64 RecordStrideInBytes;
};
struct DispatchGraphDesc {
   UINT Mode; // 0: one entry point's records from the CPU
   NodeCpuInput NodeCPUInput; // (the API's union: 8-byte aligned)
};
struct ID3D12StateObjectProperties1 : ID3D12StateObjectProperties {
   virtual ProgramId *STDMETHODCALLTYPE GetProgramIdentifier(ProgramId *ret, LPCWSTR program) = 0;
};
struct ID3D12WorkGraphProperties : IUnknown {
   virtual UINT STDMETHODCALLTYPE GetNumWorkGraphs() = 0;
   virtual LPCWSTR STDMETHODCALLTYPE GetProgramName(UINT) = 0;
   virtual UINT STDMETHODCALLTYPE GetWorkGraphIndex(LPCWSTR) = 0;
   virtual UINT STDMETHODCALLTYPE GetNumNodes(UINT) = 0;
   virtual NodeId *STDMETHODCALLTYPE GetNodeID(NodeId *ret, UINT, UINT) = 0;
   virtual UINT STDMETHODCALLTYPE GetNodeIndex(UINT, NodeId) = 0;
   virtual UINT STDMETHODCALLTYPE GetNodeLocalRootArgumentsTableIndex(UINT, UINT) = 0;
   virtual UINT STDMETHODCALLTYPE GetNumEntrypoints(UINT) = 0;
   virtual NodeId *STDMETHODCALLTYPE GetEntrypointID(NodeId *ret, UINT, UINT) = 0;
   virtual UINT STDMETHODCALLTYPE GetEntrypointIndex(UINT, NodeId) = 0;
   virtual UINT STDMETHODCALLTYPE GetEntrypointRecordSizeInBytes(UINT, UINT) = 0;
   virtual void STDMETHODCALLTYPE GetWorkGraphMemoryRequirements(UINT, WorkGraphMemory *) = 0;
};
struct ID3D12GraphicsCommandList10 : ID3D12GraphicsCommandList7 {
   virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT, UINT) = 0;
   virtual void STDMETHODCALLTYPE RSSetDepthBias(FLOAT, FLOAT, FLOAT) = 0;
   virtual void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(UINT) = 0;
   virtual void STDMETHODCALLTYPE SetProgram(const SetProgramDesc *) = 0;
   virtual void STDMETHODCALLTYPE DispatchGraph(const DispatchGraphDesc *) = 0;
};
__CRT_UUID_DECL(ID3D12StateObjectProperties1, 0x460caac7, 0x1d24, 0x446a, 0xa1, 0x84, 0xca, 0x67, 0xdb, 0x49, 0x41, 0x38)
__CRT_UUID_DECL(ID3D12WorkGraphProperties, 0x065acf71, 0xf863, 0x4b89, 0x82, 0xf4, 0x02, 0xe4, 0xd5, 0x88, 0x67, 0x57)
__CRT_UUID_DECL(ID3D12GraphicsCommandList10, 0x7013c015, 0xd161, 0x4b63, 0xa0, 0x8c, 0x23, 0x85, 0x52, 0xdd, 0x8a, 0xcc)

#include "dxil/dxil_wg_lib.h"

// A work graph (dxil/wg.hlsl): Entry's 16 threads each send Leaf a record (output[i] = 100 + i * i), and Direct, the
// second entry point (its index from the runtime), gets 3 records from the CPU (output[32 + k] = 7 + k)
static void testWorkGraph12(D12 &d, const Options21 &o21)
{
   const char *name = "Work graphs (a broadcasting node feeding a thread node, two entry points)";
   ID3D12Device5 *dev5 = nullptr;
   ID3D12GraphicsCommandList10 *list10 = nullptr;
   if (o21.WorkGraphsTier < 10 || FAILED(d.dev->QueryInterface(__uuidof(ID3D12Device5), (void **)&dev5)) ||
       FAILED(d.list->QueryInterface(__uuidof(ID3D12GraphicsCommandList10), (void **)&list10))) {
      optional(false, name, fmt("tier %u.%u", o21.WorkGraphsTier / 10, o21.WorkGraphsTier % 10));
      release(dev5);
      release(list10);
      return;
   }
   if (!startTest(name)) {
      release(dev5);
      release(list10);
      return;
   }
   std::string detail;
   D3D12_ROOT_PARAMETER param = {};
   param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
   D3D12_ROOT_SIGNATURE_DESC desc = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
   ID3D12RootSignature *rs = rootSignature12(d, desc, detail);
   ID3D12StateObject *so = nullptr;
   ID3D12StateObjectProperties1 *props = nullptr;
   ID3D12WorkGraphProperties *wg = nullptr;
   ID3D12Resource *uav = buffer12(d, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
   ID3D12Resource *rb = buffer12(d, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *backing = nullptr;
   bool ok = false;
   do {
      if (!rs || !uav || !rb) break;
      D3D12_DXIL_LIBRARY_DESC lib = {{dxil_wg_lib, sizeof(dxil_wg_lib)}, 0, nullptr};
      D3D12_GLOBAL_ROOT_SIGNATURE gs = {rs};
      WorkGraphDesc graph = {L"graph", 1, 0, nullptr, 0, nullptr};
      D3D12_STATE_SUBOBJECT subs[] = {
         {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
         {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gs},
         {SUBOBJECT_WORK_GRAPH, &graph},
      };
      D3D12_STATE_OBJECT_DESC sd = {STATE_OBJECT_EXECUTABLE, 3, subs};
      HRESULT h = dev5->CreateStateObject(&sd, __uuidof(ID3D12StateObject), (void **)&so);
      if (FAILED(h)) { detail = "CreateStateObject: " + hrs(h); break; }
      if (FAILED(so->QueryInterface(__uuidof(ID3D12StateObjectProperties1), (void **)&props)) ||
          FAILED(so->QueryInterface(__uuidof(ID3D12WorkGraphProperties), (void **)&wg))) {
         detail = "no work graph properties";
         break;
      }
      UINT index = wg->GetWorkGraphIndex(L"graph");
      WorkGraphMemory mem = {};
      wg->GetWorkGraphMemoryRequirements(index, &mem);
      UINT entry = wg->GetEntrypointIndex(index, {L"Entry", 0}), direct = wg->GetEntrypointIndex(index, {L"Direct", 0});
      detail = fmt("backing memory %llu-%llu bytes, entry points %u/%u", (unsigned long long)mem.MinSizeInBytes,
                   (unsigned long long)mem.MaxSizeInBytes, entry, direct);
      if (mem.MinSizeInBytes) {
         backing = buffer12(d, mem.MinSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
         if (!backing) { detail += ", no backing memory"; break; }
      }
      SetProgramDesc set = {};
      set.Type = 5;
      props->GetProgramIdentifier(&set.ProgramIdentifier, L"graph");
      set.Flags = 1;
      if (backing) set.BackingMemory = {backing->GetGPUVirtualAddress(), mem.MinSizeInBytes};
      list10->SetComputeRootSignature(rs);
      list10->SetComputeRootUnorderedAccessView(0, uav->GetGPUVirtualAddress());
      list10->SetProgram(&set);
      UINT base = 100;
      DispatchGraphDesc dg = {0, {entry, 1, &base, sizeof base}};
      list10->DispatchGraph(&dg);
      UINT records[3][2] = {{0, 7}, {1, 8}, {2, 9}};
      dg = {0, {direct, 3, records, sizeof records[0]}};
      list10->DispatchGraph(&dg);
      barrier12(d.list, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      d.list->CopyBufferRegion(rb, 0, uav, 0, 256);
      void *m = nullptr;
      if (!submit12(d) || FAILED(rb->Map(0, nullptr, &m))) { detail += ", running or reading back failed"; break; }
      UINT out[64];
      memcpy(out, m, sizeof out);
      rb->Unmap(0, nullptr);
      ok = true;
      for (UINT i = 0; i < 16; i++) ok = ok && out[i] == 100 + i * i;
      for (UINT k = 0; k < 3; k++) ok = ok && out[32 + k] == 7 + k;
      detail += fmt(", output[0, 5, 15] = %u, %u, %u (expected 100, 125, 325), [32..34] = %u, %u, %u (expected 7, 8, 9)",
                    out[0], out[5], out[15], out[32], out[33], out[34]);
   } while (false);
   check(ok, name, detail + debugMessages12(d));
   release(backing);
   release(rb);
   release(uav);
   release(wg);
   release(props);
   release(so);
   release(rs);
   release(list10);
   release(dev5);
}

// A flip-model swap chain of the given format on the queue, cleared and presented, read back from the window
static void testPresent12(D12 &d, IDXGIFactory4 *factory, DXGI_FORMAT format, const char *name)
{
   if (!startTest(name)) return;
   HWND w = createWindow("dxtest d3d12 present", 64, 64, true);
   DXGI_SWAP_CHAIN_DESC1 sd = {};
   sd.Width = 64;
   sd.Height = 64;
   sd.Format = format;
   sd.SampleDesc.Count = 1;
   sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   sd.BufferCount = 2;
   sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
   IDXGISwapChain1 *sc1 = nullptr;
   IDXGISwapChain3 *sc = nullptr;
   HRESULT sh = factory ? factory->CreateSwapChainForHwnd(d.queue, w, &sd, nullptr, nullptr, &sc1) : E_FAIL;
   if (sc1) sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&sc);
   ID3D12DescriptorHeap *rtvs = nullptr;
   D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
   d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&rtvs);
   UINT inc = d.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
   ID3D12Resource *buffers[2] = {};
   for (UINT i = 0; sc && rtvs && i < 2; i++) {
      sc->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&buffers[i]);
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += i * inc;
      if (buffers[i]) d.dev->CreateRenderTargetView(buffers[i], nullptr, rtv);
   }
   HRESULT ph = S_OK;
   for (int frame = 0; frame < 10 && buffers[0] && buffers[1]; frame++) {
      UINT i = sc->GetCurrentBackBufferIndex();
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += i * inc;
      barrier12(d.list, buffers[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
      const float color[4] = {0.25f, 0, 0.75f, 1};
      d.list->ClearRenderTargetView(rtv, color, 0, nullptr);
      barrier12(d.list, buffers[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
      submit12(d);
      HRESULT h1 = sc->Present(0, 0);
      if (FAILED(h1)) ph = h1;
      waitIdle12(d, d.queue);
      pumpMessages();
      Sleep(30);
   }
   Sleep(300);
   COLORREF c = 0;
   bool ok = sc && SUCCEEDED(ph) && windowPixel(w, 32, 32, c) && near8(GetRValue(c), 64, 8) &&
             near8(GetGValue(c), 0, 8) && near8(GetBValue(c), 191, 8);
   check(ok, name,
         !sc ? "CreateSwapChainForHwnd: " + hrs(sh)
             : FAILED(ph) ? "Present: " + hrs(ph)
                          : fmt("window shows (%d,%d,%d), expected (64,0,191)", GetRValue(c), GetGValue(c), GetBValue(c)));
   release(buffers[0]);
   release(buffers[1]);
   release(rtvs);
   release(sc);
   release(sc1);
   DestroyWindow(w);
}

static void groupD3D12()
{
   const char *TESTS[] = {"Feature level 11_0 or higher", "Shader model 6.0 or higher", "Command queues, lists and fences",
                          "Root signatures and pipeline states", "Rendering a triangle", "Compute shaders and UAV buffers",
                          "Copy queues", "Bundles", "ExecuteIndirect (indirect draws)", "Descriptor heaps and texture sampling",
                          "Present to a window (DXGI swap chain)", "Pipeline libraries (pipeline state caches)"};
   typedef HRESULT(WINAPI * PFN_Create)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
   HMODULE lib = LoadLibraryA("d3d12.dll");
   auto create = lib ? (PFN_Create)GetProcAddress(lib, "D3D12CreateDevice") : nullptr;
   if (!create) {
      if (windows10OrLater()) check(false, "Direct3D 12 runtime (d3d12.dll)");
      else optional(false, "Direct3D 12 runtime (d3d12.dll)", "Windows 10 or later has it");
      for (auto t : TESTS) report(Status::Skip, t, "no Direct3D 12");
      return;
   }
   IDXGIFactory4 *factory = nullptr;
   CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&factory);
   IDXGIAdapter1 *adapter = nullptr;
   if (factory) {
      if (g_warp) {
         factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), (void **)&adapter);
      } else {
         IDXGIAdapter1 *a;
         for (UINT i = 0; !adapter && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            a->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) a->Release();
            else adapter = a;
         }
      }
   }
   // DXTEST_D3D12_DEBUG=1: the debug layer (Graphics Tools), its messages in failing tests' details
   typedef HRESULT(WINAPI * PFN_GetDebug)(REFIID, void **);
   auto getDebug = getenv("DXTEST_D3D12_DEBUG") ? (PFN_GetDebug)GetProcAddress(lib, "D3D12GetDebugInterface") : nullptr;
   ID3D12Debug *debug = nullptr;
   if (getDebug && SUCCEEDED(getDebug(__uuidof(ID3D12Debug), (void **)&debug))) debug->EnableDebugLayer();
   release(debug);
   D12 d;
   HRESULT h = adapter ? create(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&d.dev) : E_FAIL;
   std::string why = !adapter ? "no hardware adapter"
                     : h == E_NOINTERFACE || h == DXGI_ERROR_UNSUPPORTED
                        ? "the GPU's driver has no Direct3D 12 support (" + hrs(h) + ")"
                        : hrs(h);
   if (FAILED(h) && (h == E_NOINTERFACE || h == DXGI_ERROR_UNSUPPORTED)) missing("Create a Direct3D 12 device", why);
   else check(SUCCEEDED(h), "Create a Direct3D 12 device", SUCCEEDED(h) ? "" : why);
   if (FAILED(h)) {
      for (auto t : TESTS) report(Status::Skip, t, "no Direct3D 12 device");
      release(adapter);
      release(factory);
      return;
   }

   // Feature levels and tiers
   D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                 D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
   D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {ARRAYSIZE(levels), levels, D3D_FEATURE_LEVEL_11_0};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
   check(fl.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_11_0, TESTS[0], fmt("highest %s", flName(fl.MaxSupportedFeatureLevel)));
   optional(fl.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_0, "Feature level 12_0", fmt("highest %s", flName(fl.MaxSupportedFeatureLevel)));
   optional(fl.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_1, "Feature level 12_1");
   optional(fl.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_2, "Feature level 12_2 (DirectX 12 Ultimate)");
   D3D12_FEATURE_DATA_SHADER_MODEL sm = {SM_6_8};
   while (FAILED(d.dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) && sm.HighestShaderModel > D3D_SHADER_MODEL_5_1) {
      sm.HighestShaderModel = (D3D_SHADER_MODEL)(sm.HighestShaderModel - 1);
   }
   check(sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0, TESTS[1], fmt("highest %d.%d", sm.HighestShaderModel >> 4, sm.HighestShaderModel & 15));
   optional(sm.HighestShaderModel >= SM_6_8, "Shader model 6.8");
   D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
   report(Status::Info, "Resource binding tier", fmt("tier %d", (int)o.ResourceBindingTier));
   optional(o.TiledResourcesTier != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED, "Tiled resources");
   optional(o.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED, "Conservative rasterization");
   optional(o.ROVsSupported, "Rasterizer ordered views");
   optional(o.DoublePrecisionFloatShaderOps, "Double precision in shaders");
   D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1);
   optional(o1.WaveOps, "Wave intrinsics", o1.WaveOps ? fmt("%u-%u lanes", o1.WaveLaneCountMin, o1.WaveLaneCountMax) : "");
   D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2 = {};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof o2);
   optional(o2.ProgrammableSamplePositionsTier != D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED, "Programmable sample positions");
   D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
   optional(o5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED, "DirectX Raytracing (DXR)");
   D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof o6);
   optional(o6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED, "Variable rate shading");
   D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
   d.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7);
   optional(o7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED, "Mesh shaders");
   optional(o7.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED, "Sampler feedback");

   // The command machinery
   D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
   d.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&d.queue);
   d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&d.alloc);
   if (d.alloc) d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&d.list);
   d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&d.fence);
   d.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
   bool machinery = d.queue && d.alloc && d.list && d.fence && submit12(d);
   check(machinery, TESTS[2]);
   if (!machinery) return;

   std::string detail;
   D3D12_ROOT_SIGNATURE_DESC empty = {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
   ID3D12RootSignature *rs = rootSignature12(d, empty, detail);
   ID3D12PipelineState *pso = rs ? graphicsPso12(d, rs, "float4 main(float4 p : SV_Position) : SV_Target "
                                                         "{ return p.x < 16 ? float4(1, 0.5, 0, 1) : float4(0, 1, 0, 1); }", detail) : nullptr;
   check(rs && pso, TESTS[3], detail);
   auto pixelIs = [](const std::vector<uint32_t> &px, int x, int y, int r, int g, int b, std::string &det) {
      return expectRgba(px[y * Target12::size + x], r, g, b, det, 3, x, y);
   };
   if (rs && pso && startTest(TESTS[4])) {
      detail.clear();
      Target12 t;
      std::vector<uint32_t> px;
      bool ok = target12(d, t);
      if (ok) {
         d.list->SetGraphicsRootSignature(rs);
         d.list->SetPipelineState(pso);
         d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
         d.list->DrawInstanced(3, 1, 0, 0);
         ok = readTarget12(d, t, px) && pixelIs(px, 8, 16, 255, 128, 0, detail) && pixelIs(px, 24, 16, 0, 255, 0, detail);
      }
      check(ok, TESTS[4], detail);
      t.release();
   } else if (!(rs && pso)) {
      report(Status::Skip, TESTS[4], "no pipeline");
   }

   // Compute: a root UAV, each thread writes its index squared
   if (startTest(TESTS[5])) {
      detail.clear();
      D3D12_ROOT_PARAMETER param = {};
      param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_SIGNATURE_DESC desc = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
      ID3D12RootSignature *crs = rootSignature12(d, desc, detail);
      ID3DBlob *csb = compileHlsl("RWStructuredBuffer<uint> o : register(u0); [numthreads(64, 1, 1)] "
                                  "void main(uint i : SV_DispatchThreadID) { o[i] = i * i + 7; }", "cs_5_0", &detail);
      ID3D12PipelineState *cpso = nullptr;
      if (crs && csb) {
         D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
         cd.pRootSignature = crs;
         cd.CS = {csb->GetBufferPointer(), csb->GetBufferSize()};
         d.dev->CreateComputePipelineState(&cd, __uuidof(ID3D12PipelineState), (void **)&cpso);
      }
      ID3D12Resource *uav = buffer12(d, 256 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      ID3D12Resource *rb = buffer12(d, 256 * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      bool ok = crs && cpso && uav && rb;
      if (ok) {
         d.list->SetComputeRootSignature(crs);
         d.list->SetPipelineState(cpso);
         d.list->SetComputeRootUnorderedAccessView(0, uav->GetGPUVirtualAddress());
         d.list->Dispatch(4, 1, 1);
         barrier12(d.list, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
         d.list->CopyResource(rb, uav);
         ok = submit12(d);
         uint32_t *p = nullptr;
         D3D12_RANGE range = {0, 1024};
         if (ok && SUCCEEDED(rb->Map(0, &range, (void **)&p))) {
            int bad = 0;
            for (uint32_t i = 0; i < 256; i++) bad += p[i] != i * i + 7;
            if (bad) detail = fmt("%d of 256 values wrong", bad);
            ok = !bad;
            D3D12_RANGE none = {0, 0};
            rb->Unmap(0, &none);
         } else {
            ok = false;
         }
      }
      check(ok, TESTS[5], detail);

      // Pipeline libraries, the pipeline caches engines keep on disk: store the pipeline, serialize the library,
      // create one from the data and load the pipeline back from it
      if (cpso && startTest(TESTS[11])) {
         ID3D12Device1 *dev1 = nullptr;
         ID3D12PipelineLibrary *pl = nullptr, *pl2 = nullptr;
         ID3D12PipelineState *loaded = nullptr;
         HRESULT h = d.dev->QueryInterface(__uuidof(ID3D12Device1), (void **)&dev1);
         if (SUCCEEDED(h)) h = dev1->CreatePipelineLibrary(nullptr, 0, __uuidof(ID3D12PipelineLibrary), (void **)&pl);
         if (SUCCEEDED(h)) h = pl->StorePipeline(L"compute", cpso);
         std::vector<char> blob(SUCCEEDED(h) ? pl->GetSerializedSize() : 0);
         if (SUCCEEDED(h)) h = pl->Serialize(blob.data(), blob.size());
         if (SUCCEEDED(h)) h = dev1->CreatePipelineLibrary(blob.data(), blob.size(), __uuidof(ID3D12PipelineLibrary), (void **)&pl2);
         D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
         cd.pRootSignature = crs;
         cd.CS = {csb->GetBufferPointer(), csb->GetBufferSize()};
         if (SUCCEEDED(h)) h = pl2->LoadComputePipeline(L"compute", &cd, __uuidof(ID3D12PipelineState), (void **)&loaded);
         HRESULT removed = d.dev->GetDeviceRemovedReason();
         check(SUCCEEDED(h) && loaded && removed == S_OK, TESTS[11],
               FAILED(h) ? hrs(h) : removed != S_OK ? "the device was removed (" + hrs(removed) + ")"
                                                   : fmt("%zu bytes serialized", blob.size()));
         release(loaded);
         release(pl2);
         release(pl);
         release(dev1);
      }
      release(crs);
      release(csb);
      release(cpso);
      release(uav);
      release(rb);
   }

   // Copy queue: an upload buffer copied to a default buffer on a copy queue, read back on the direct queue
   if (startTest(TESTS[6])) {
      detail.clear();
      ID3D12CommandQueue *cq = nullptr;
      ID3D12CommandAllocator *ca = nullptr;
      ID3D12GraphicsCommandList *cl = nullptr;
      D3D12_COMMAND_QUEUE_DESC cqd = {D3D12_COMMAND_LIST_TYPE_COPY, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
      d.dev->CreateCommandQueue(&cqd, __uuidof(ID3D12CommandQueue), (void **)&cq);
      d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, __uuidof(ID3D12CommandAllocator), (void **)&ca);
      if (ca) d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, ca, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&cl);
      ID3D12Resource *up = buffer12(d, 4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      ID3D12Resource *def = buffer12(d, 4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
      ID3D12Resource *rb = buffer12(d, 4096, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      bool ok = cq && cl && up && def && rb;
      if (ok) {
         uint32_t *p = nullptr;
         up->Map(0, nullptr, (void **)&p);
         for (uint32_t i = 0; i < 1024; i++) p[i] = i * 2654435761u;
         up->Unmap(0, nullptr);
         cl->CopyResource(def, up);
         cl->Close();
         ID3D12CommandList *lists[] = {cl};
         cq->ExecuteCommandLists(1, lists);
         ok = waitIdle12(d, cq);
         barrier12(d.list, def, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
         d.list->CopyResource(rb, def);
         ok = ok && submit12(d) && SUCCEEDED(rb->Map(0, nullptr, (void **)&p));
         if (ok) {
            int bad = 0;
            for (uint32_t i = 0; i < 1024; i++) bad += p[i] != i * 2654435761u;
            if (bad) detail = fmt("%d of 1024 values wrong", bad);
            ok = !bad;
            rb->Unmap(0, nullptr);
         }
      }
      check(ok, TESTS[6], detail);
      release(cq);
      release(ca);
      release(cl);
      release(up);
      release(def);
      release(rb);
   }

   // Bundles and ExecuteIndirect draw the triangle of the pipeline above
   if (!(rs && pso)) {
      report(Status::Skip, TESTS[7], "no pipeline");
      report(Status::Skip, TESTS[8], "no pipeline");
   }
   if (rs && pso && startTest(TESTS[7])) {
      detail.clear();
      ID3D12CommandAllocator *ba = nullptr;
      ID3D12GraphicsCommandList *bundle = nullptr;
      d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, __uuidof(ID3D12CommandAllocator), (void **)&ba);
      if (ba) d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, ba, pso, __uuidof(ID3D12GraphicsCommandList), (void **)&bundle);
      Target12 t;
      std::vector<uint32_t> px;
      bool ok = bundle && target12(d, t);
      if (ok) {
         bundle->SetGraphicsRootSignature(rs);
         bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
         bundle->DrawInstanced(3, 1, 0, 0);
         bundle->Close();
         d.list->SetGraphicsRootSignature(rs);
         d.list->ExecuteBundle(bundle);
         ok = readTarget12(d, t, px) && pixelIs(px, 8, 16, 255, 128, 0, detail) && pixelIs(px, 24, 16, 0, 255, 0, detail);
      }
      check(ok, TESTS[7], detail);
      t.release();
      release(bundle);
      release(ba);
   }
   if (rs && pso && startTest(TESTS[8])) {
      std::vector<uint32_t> px;
      detail.clear();
      D3D12_INDIRECT_ARGUMENT_DESC arg = {};
      arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
      D3D12_COMMAND_SIGNATURE_DESC sd = {sizeof(D3D12_DRAW_ARGUMENTS), 1, &arg, 0};
      ID3D12CommandSignature *sig = nullptr;
      d.dev->CreateCommandSignature(&sd, nullptr, __uuidof(ID3D12CommandSignature), (void **)&sig);
      ID3D12Resource *args = buffer12(d, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      Target12 t2;
      bool ok = sig && args && target12(d, t2);
      if (ok) {
         D3D12_DRAW_ARGUMENTS *a = nullptr;
         args->Map(0, nullptr, (void **)&a);
         *a = {3, 1, 0, 0};
         args->Unmap(0, nullptr);
         d.list->SetGraphicsRootSignature(rs);
         d.list->SetPipelineState(pso);
         d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
         d.list->ExecuteIndirect(sig, 1, args, 0, nullptr, 0);
         ok = readTarget12(d, t2, px) && pixelIs(px, 8, 16, 255, 128, 0, detail) && pixelIs(px, 24, 16, 0, 255, 0, detail);
      }
      check(ok, TESTS[8], detail);
      t2.release();
      release(sig);
      release(args);
   }

   // Descriptor heaps: a 2x2 texture uploaded and sampled through a descriptor table and a static sampler
   if (startTest(TESTS[9])) {
      detail.clear();
      D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
      D3D12_ROOT_PARAMETER param = {};
      param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      param.DescriptorTable = {1, &range};
      param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_STATIC_SAMPLER_DESC smp = {};
      smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
      smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      smp.MaxLOD = 1000;
      smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_ROOT_SIGNATURE_DESC desc = {1, &param, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
      ID3D12RootSignature *trs = rootSignature12(d, desc, detail);
      ID3D12PipelineState *tpso = trs ? graphicsPso12(d, trs, "Texture2D t : register(t0); SamplerState s : register(s0);"
                                                              "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target "
                                                              "{ return t.Sample(s, uv); }", detail) : nullptr;
      ID3D12Resource *tex = texture12(d, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      ID3D12Resource *up = buffer12(d, 512, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      ID3D12DescriptorHeap *heap = nullptr;
      D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
      d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&heap);
      Target12 t;
      std::vector<uint32_t> px;
      bool ok = trs && tpso && tex && up && heap && target12(d, t);
      if (ok) {
         const uint32_t texels[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};
         char *p = nullptr;
         up->Map(0, nullptr, (void **)&p);
         memcpy(p, texels, 8);
         memcpy(p + 256, texels + 2, 8);
         up->Unmap(0, nullptr);
         D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
         dst.pResource = tex;
         dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
         src.pResource = up;
         src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
         src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256};
         d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
         barrier12(d.list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
         d.dev->CreateShaderResourceView(tex, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
         d.list->SetGraphicsRootSignature(trs);
         d.list->SetPipelineState(tpso);
         ID3D12DescriptorHeap *heaps[] = {heap};
         d.list->SetDescriptorHeaps(1, heaps);
         d.list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
         d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
         d.list->DrawInstanced(3, 1, 0, 0);
         ok = readTarget12(d, t, px) && pixelIs(px, 8, 8, 255, 0, 0, detail) && pixelIs(px, 24, 8, 0, 255, 0, detail) &&
              pixelIs(px, 8, 24, 0, 0, 255, detail) && pixelIs(px, 24, 24, 255, 255, 255, detail);
      }
      check(ok, TESTS[9], detail);
      t.release();
      release(trs);
      release(tpso);
      release(tex);
      release(up);
      release(heap);
   }
   // Presenting, in the formats swap chains use most (the BGRA one differently shared with DWM: converted on the GPU)
   if (!interactiveDesktop()) {
      report(Status::Skip, TESTS[10], "no interactive desktop");
   } else {
      testPresent12(d, factory, DXGI_FORMAT_R8G8B8A8_UNORM, TESTS[10]);
      testPresent12(d, factory, DXGI_FORMAT_B8G8R8A8_UNORM, "Present to a window (DXGI swap chain, BGRA)");
   }
   if (rs) testVrs12(d, rs, o6);
   if (rs) testMesh12(d, rs, o7);
   testRt12(d, o5);
   testFeedback12(d, o7);
   if (rs) testSamplePositions12(d, rs, o2);
   testWriteImmediate12(d);
   if (rs) testSm68(d, rs, sm.HighestShaderModel);
   Options19 o19 = {};
   d.dev->CheckFeatureSupport(FEATURE_OPTIONS19, &o19, sizeof o19);
   testSamplerHeap12(d, o19);
   testNoOutputSamples12(d, o19);
   testEnhancedBarriers12(d);
   testCastableFormats12(d);
   Options21 o21 = {};
   d.dev->CheckFeatureSupport(FEATURE_OPTIONS21, &o21, sizeof o21);
   optional(o21.ExecuteIndirectTier >= 11, "ExecuteIndirect tier 1.1 (incrementing constants)",
            fmt("tier %u.%u", o21.ExecuteIndirectTier / 10, o21.ExecuteIndirectTier % 10));
   testExecuteIndirectConstants12(d, o21);
   testExecuteIndirectDraws12(d);
   if (sm.HighestShaderModel >= SM_6_8) testSampleCmpGrad12(d, o21);
   if (sm.HighestShaderModel >= SM_6_8) testWorkGraph12(d, o21);
   release(rs);
   release(pso);
   release(d.list);
   release(d.alloc);
   release(d.queue);
   release(d.fence);
   release(d.dev);
   release(adapter);
   release(factory);
}
