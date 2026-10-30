// csreplay: one compute dispatch replayed from captured buffers, on the hardware device or on WARP, to compare a
// shader's translation against Microsoft's reference. Takes a DXBC container and a directory with u0.bin..u7.bin (raw
// UAVs, as many as exist), cb0.bin/cb1.bin (constant buffers) and args.bin (three group counts); writes out_u<i>.bin.
// Usage: csreplay <shader.dxbc> <dir> hw|warp|12 <outdir> (12: D3D11 on 12, the shader through the D3D12 driver)
// Build: x86_64-w64-mingw32-g++ -O2 -static csreplay.cpp -o csreplay.exe -ld3d11 -ld3d12 -ldxgi
#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<char> load(const std::string &path)
{
   std::vector<char> d;
   FILE *f = fopen(path.c_str(), "rb");
   if (!f)
      return d;
   fseek(f, 0, SEEK_END);
   d.resize(ftell(f));
   fseek(f, 0, SEEK_SET);
   if (!d.empty() && fread(d.data(), 1, d.size(), f) != d.size())
      d.clear();
   fclose(f);
   return d;
}

int main(int argc, char **argv)
{
   setvbuf(stdout, nullptr, _IONBF, 0);
   if (argc < 5) {
      printf("usage: csreplay <shader.dxbc> <dir> hw|warp <outdir>\n");
      return 1;
   }
   std::string dir = argv[2], out = argv[4];
   bool warp = !strcmp(argv[3], "warp"), on12 = !strcmp(argv[3], "12");
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
   HRESULT hr;
   if (on12) {
      ID3D12Device *dev12;
      ID3D12CommandQueue *queue;
      D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
      if (FAILED(hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12))) ||
          FAILED(hr = dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
         printf("d3d12: 0x%08lx\n", (unsigned long)hr);
         return 1;
      }
      hr = D3D11On12CreateDevice(dev12, 0, &fl, 1, (IUnknown **)&queue, 1, 0, &dev, &ctx, nullptr);
   } else {
      hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                             D3D11_SDK_VERSION, &dev, nullptr, &ctx);
   }
   if (FAILED(hr)) {
      printf("device: 0x%08lx\n", (unsigned long)hr);
      return 1;
   }
   std::vector<char> code = load(argv[1]);
   ID3D11ComputeShader *cs;
   if (code.empty() || FAILED(hr = dev->CreateComputeShader(code.data(), code.size(), nullptr, &cs))) {
      printf("shader: 0x%08lx\n", (unsigned long)hr);
      return 1;
   }

   ID3D11Buffer *bufs[8] = {};
   ID3D11UnorderedAccessView *uavs[8] = {};
   UINT sizes[8] = {};
   unsigned n = 0;
   for (; n < 8; n++) {
      std::vector<char> d = load(dir + "\\u" + std::to_string(n) + ".bin");
      if (d.empty())
         break;
      d.resize((d.size() + 15) & ~size_t(15));
      sizes[n] = (UINT)d.size();
      D3D11_BUFFER_DESC bd = {sizes[n], D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                              D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
      D3D11_SUBRESOURCE_DATA init = {d.data(), 0, 0};
      if (FAILED(hr = dev->CreateBuffer(&bd, &init, &bufs[n]))) {
         printf("u%u (%u bytes): 0x%08lx\n", n, sizes[n], (unsigned long)hr);
         return 1;
      }
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
      ud.Format = DXGI_FORMAT_R32_TYPELESS;
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Buffer.NumElements = sizes[n] / 4;
      ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      if (FAILED(hr = dev->CreateUnorderedAccessView(bufs[n], &ud, &uavs[n]))) {
         printf("u%u view: 0x%08lx\n", n, (unsigned long)hr);
         return 1;
      }
   }
   ID3D11Buffer *cbs[2] = {};
   for (unsigned i = 0; i < 2; i++) {
      std::vector<char> d = load(dir + "\\cb" + std::to_string(i) + ".bin");
      if (d.empty())
         continue;
      d.resize((d.size() + 15) & ~size_t(15));
      D3D11_BUFFER_DESC bd = {(UINT)d.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
      D3D11_SUBRESOURCE_DATA init = {d.data(), 0, 0};
      if (FAILED(hr = dev->CreateBuffer(&bd, &init, &cbs[i]))) {
         printf("cb%u: 0x%08lx\n", i, (unsigned long)hr);
         return 1;
      }
   }
   std::vector<char> args = load(dir + "\\args.bin");
   if (args.size() < 12) {
      printf("no args.bin\n");
      return 1;
   }
   const UINT *g = (const UINT *)args.data();
   printf("%s: %u UAVs, dispatch %u %u %u\n", argv[3], n, g[0], g[1], g[2]);

   ctx->CSSetShader(cs, nullptr, 0);
   ctx->CSSetConstantBuffers(0, 2, cbs);
   ctx->CSSetUnorderedAccessViews(0, n, uavs, nullptr);
   ctx->Dispatch(g[0], g[1], g[2]);

   for (unsigned i = 0; i < n; i++) {
      D3D11_BUFFER_DESC sd = {sizes[i], D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
      ID3D11Buffer *st;
      if (FAILED(hr = dev->CreateBuffer(&sd, nullptr, &st))) {
         printf("staging u%u: 0x%08lx\n", i, (unsigned long)hr);
         return 1;
      }
      ctx->CopyResource(st, bufs[i]);
      D3D11_MAPPED_SUBRESOURCE m;
      if (FAILED(hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
         printf("map u%u: 0x%08lx\n", i, (unsigned long)hr);
         return 1;
      }
      FILE *f = fopen((out + "\\out_u" + std::to_string(i) + ".bin").c_str(), "wb");
      fwrite(m.pData, 1, sizes[i], f);
      fclose(f);
      ctx->Unmap(st, 0);
      st->Release();
   }
   printf("done\n");
   return 0;
}
