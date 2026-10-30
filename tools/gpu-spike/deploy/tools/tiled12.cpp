// tiled12 [rgba8|bc7] [width] [height] [mips]: a Direct3D 12 reserved texture (CreateReservedResource) mapped onto a
// heap with UpdateTileMappings, filled from an upload buffer and copied back to a readback buffer; prints whether the
// bytes came back (GravityMark's textures are reserved BC7 ones). Build: x86_64-w64-mingw32-g++ -O2 -static
// tiled12.cpp -o tiled12.exe -ld3d12 -ldxgi
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x)                                                                                                       \
   do {                                                                                                               \
      HRESULT hr_ = (x);                                                                                              \
      if (FAILED(hr_)) {                                                                                              \
         printf("%s: 0x%08lx\n", #x, (unsigned long)hr_);                                                            \
         return 1;                                                                                                    \
      }                                                                                                               \
   } while (0)

int main(int argc, char **argv)
{
   setvbuf(stdout, nullptr, _IONBF, 0);
   bool bc7 = argc > 1 && !strcmp(argv[1], "bc7");
   UINT w = argc > 2 ? atoi(argv[2]) : 512, h = argc > 3 ? atoi(argv[3]) : 512, mips = argc > 4 ? atoi(argv[4]) : 1;
   ID3D12Device *dev;
   CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
   D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
   dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
   printf("tiled resources tier %d\n", (int)opts.TiledResourcesTier);
   D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
   ID3D12CommandQueue *queue;
   CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
   ID3D12CommandAllocator *alloc;
   CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
   ID3D12GraphicsCommandList *list;
   CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)));
   ID3D12Fence *fence;
   CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

   D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, (UINT16)mips,
                             bc7 ? DXGI_FORMAT_BC7_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                             D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, D3D12_RESOURCE_FLAG_NONE};
   ID3D12Resource *tex;
   CHECK(dev->CreateReservedResource(&td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
   UINT tiles = 0;
   D3D12_PACKED_MIP_INFO packed;
   D3D12_TILE_SHAPE shape;
   dev->GetResourceTiling(tex, &tiles, &packed, &shape, nullptr, 0, nullptr);
   printf("%s %ux%u, %u mips: %u tiles (%ux%u texels each), packed mips %u from %u\n", bc7 ? "BC7" : "RGBA8", w, h,
          mips, tiles, shape.WidthInTexels, shape.HeightInTexels, packed.NumPackedMips, packed.NumStandardMips);

   D3D12_HEAP_DESC hd = {(UINT64)tiles * 65536, {D3D12_HEAP_TYPE_DEFAULT}, 0,
                         D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
   ID3D12Heap *heap;
   CHECK(dev->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
   // the whole texture onto the heap, in order
   D3D12_TILED_RESOURCE_COORDINATE start = {};
   D3D12_TILE_REGION_SIZE region = {tiles, FALSE};
   D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;
   UINT offset = 0;
   queue->UpdateTileMappings(tex, 1, &start, &region, heap, 1, &flag, &offset, &tiles, D3D12_TILE_MAPPING_FLAG_NONE);

   std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(mips);
   std::vector<UINT> rows(mips);
   std::vector<UINT64> rowSize(mips);
   UINT64 total = 0;
   dev->GetCopyableFootprints(&td, 0, mips, 0, fp.data(), rows.data(), rowSize.data(), &total);
   D3D12_HEAP_PROPERTIES up = {D3D12_HEAP_TYPE_UPLOAD}, rb = {D3D12_HEAP_TYPE_READBACK};
   D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                             D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
   ID3D12Resource *upload, *readback;
   CHECK(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&upload)));
   CHECK(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&readback)));
   unsigned char *p;
   CHECK(upload->Map(0, nullptr, (void **)&p));
   for (UINT64 i = 0; i < total; i++) p[i] = (unsigned char)(i * 7 + (i >> 9) + 1);
   upload->Unmap(0, nullptr);

   for (UINT m = 0; m < mips; m++) {
      D3D12_TEXTURE_COPY_LOCATION dst = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, src = {upload};
      dst.SubresourceIndex = m;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = fp[m];
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
   }
   D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
   b.Transition.pResource = tex;
   b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
   b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
   list->ResourceBarrier(1, &b);
   for (UINT m = 0; m < mips; m++) {
      D3D12_TEXTURE_COPY_LOCATION src = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, dst = {readback};
      src.SubresourceIndex = m;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = fp[m];
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
   }
   CHECK(list->Close());
   ID3D12CommandList *lists[] = {list};
   queue->ExecuteCommandLists(1, lists);
   queue->Signal(fence, 1);
   HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
   fence->SetEventOnCompletion(1, ev);
   if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) {
      printf("timeout; device removed reason 0x%08lx\n", (unsigned long)dev->GetDeviceRemovedReason());
      return 1;
   }

   unsigned char *q;
   CHECK(upload->Map(0, nullptr, (void **)&p));
   CHECK(readback->Map(0, nullptr, (void **)&q));
   int bad = 0;
   for (UINT m = 0; m < mips; m++) {
      UINT64 mismatched = 0, zero = 0;
      for (UINT r = 0; r < rows[m]; r++)
         for (UINT64 c = 0; c < rowSize[m]; c++) {
            UINT64 i = fp[m].Offset + (UINT64)r * fp[m].Footprint.RowPitch + c;
            mismatched += p[i] != q[i];
            zero += q[i] == 0;
         }
      UINT64 n = (UINT64)rows[m] * rowSize[m];
      if (mismatched)
         bad++;
      if (mismatched || m == 0)
         printf("mip %u (%ux%u): %llu of %llu bytes differ, %llu zero\n", m, fp[m].Footprint.Width,
                fp[m].Footprint.Height, (unsigned long long)mismatched, (unsigned long long)n,
                (unsigned long long)zero);
   }
   printf("%s; device removed reason 0x%08lx\n", bad ? "FAIL" : "PASS", (unsigned long)dev->GetDeviceRemovedReason());
   return bad ? 1 : 0;
}
