// bufinit11: Direct3D 11 buffers created with initial data (a pattern), copied to a staging buffer and read back, for
// combinations of bind flags, raw views, sizes, and with or without a UAV view made and a dispatch of no groups
// between; prints each that doesn't come back as written.
// Build: x86_64-w64-mingw32-g++ -O2 -static bufinit11.cpp -o bufinit11.exe -ld3d11 -ldxgi
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
   setvbuf(stdout, nullptr, _IONBF, 0);
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
   if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                nullptr, &ctx))) {
      printf("no device\n");
      return 1;
   }
   const UINT binds[] = {D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_UNORDERED_ACCESS,
                         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS};
   const UINT sizes[] = {4096, 65536, 1 << 20, 16 << 20};
   int bad = 0, total = 0;
   for (UINT bind : binds)
      for (int raw = 0; raw < 2; raw++)
         for (UINT size : sizes)
            for (int view = 0; view < 2; view++) {
               if (raw && !(bind & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)))
                  continue;
               if (view && !(bind & D3D11_BIND_UNORDERED_ACCESS))
                  continue;
               std::vector<UINT> data(size / 4);
               for (UINT i = 0; i < data.size(); i++)
                  data[i] = i * 2654435761u + size;
               D3D11_BUFFER_DESC bd = {size, D3D11_USAGE_DEFAULT, bind, 0,
                                       raw ? (UINT)D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : 0u, 0};
               D3D11_SUBRESOURCE_DATA init = {data.data(), 0, 0};
               ID3D11Buffer *b, *st;
               if (FAILED(dev->CreateBuffer(&bd, &init, &b))) {
                  printf("create bind 0x%x raw %d size %u failed\n", bind, raw, size);
                  bad++;
                  continue;
               }
               ID3D11UnorderedAccessView *uav = nullptr;
               if (view) {
                  D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
                  ud.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_UINT;
                  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                  ud.Buffer.NumElements = size / 4;
                  ud.Buffer.Flags = raw ? D3D11_BUFFER_UAV_FLAG_RAW : 0;
                  dev->CreateUnorderedAccessView(b, &ud, &uav);
                  ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
               }
               D3D11_BUFFER_DESC sd = {size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
               dev->CreateBuffer(&sd, nullptr, &st);
               ctx->CopyResource(st, b);
               D3D11_MAPPED_SUBRESOURCE m;
               total++;
               if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                  printf("map bind 0x%x raw %d size %u failed\n", bind, raw, size);
                  bad++;
               } else {
                  const UINT *r = (const UINT *)m.pData;
                  UINT wrong = 0, first = ~0u;
                  for (UINT i = 0; i < data.size(); i++)
                     if (r[i] != data[i]) {
                        if (!wrong)
                           first = i;
                        wrong++;
                     }
                  if (wrong) {
                     printf("bind 0x%x raw %d size %u uav %d: %u of %u dwords wrong (first %u: 0x%08x, want 0x%08x)\n",
                            bind, raw, size, view, wrong, (UINT)data.size(), first, r[first], data[first]);
                     bad++;
                  }
                  ctx->Unmap(st, 0);
               }
               if (uav) {
                  ID3D11UnorderedAccessView *none = nullptr;
                  ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
                  uav->Release();
               }
               st->Release();
               b->Release();
            }
   // The same, with all of them alive at once (csreplay's 520 MB of raw UAV buffers read back wrong)
   // bufinit11 [MB ...]: the sizes of the buffers alive at once, default csreplay's
   std::vector<UINT> big = {4096, 65536, 1 << 20, 16 << 20, 64 << 20, 160 << 20, 256 << 20};
   if (argc > 1) {
      big.clear();
      for (int i = 1; i < argc; i++)
         big.push_back(atoi(argv[i]) << 20);
   }
   const int nbig = (int)big.size();
   std::vector<ID3D11Buffer *> bufs(nbig);
   for (int i = 0; i < nbig; i++) {
      std::vector<UINT> data(big[i] / 4);
      for (UINT j = 0; j < data.size(); j++)
         data[j] = j * 2654435761u + big[i];
      D3D11_BUFFER_DESC bd = {big[i], D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                              D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
      D3D11_SUBRESOURCE_DATA init = {data.data(), 0, 0};
      if (FAILED(dev->CreateBuffer(&bd, &init, &bufs[i]))) {
         printf("bulk create %u failed\n", big[i]);
         return 1;
      }
   }
   for (int i = 0; i < nbig; i++) {
      D3D11_BUFFER_DESC sd = {big[i], D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
      ID3D11Buffer *st;
      dev->CreateBuffer(&sd, nullptr, &st);
      ctx->CopyResource(st, bufs[i]);
      D3D11_MAPPED_SUBRESOURCE m;
      total++;
      if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
         printf("bulk map %u failed\n", big[i]);
         bad++;
      } else {
         const UINT *r = (const UINT *)m.pData;
         UINT wrong = 0, first = ~0u;
         for (UINT j = 0; j < big[i] / 4; j++)
            if (r[j] != j * 2654435761u + big[i]) {
               if (!wrong)
                  first = j;
               wrong++;
            }
         if (wrong)
            printf("bulk size %u: %u of %u dwords wrong (first %u: 0x%08x)\n", big[i], wrong, big[i] / 4, first,
                   r[first]);
         bad += wrong != 0;
         ctx->Unmap(st, 0);
      }
      st->Release();
   }
   printf("%d of %d wrong\n%s\n", bad, total, bad ? "FAIL" : "PASS");
   return bad ? 1 : 0;
}
