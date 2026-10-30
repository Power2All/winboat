// dxgimodes: each output's display modes per format (IDXGIOutput::GetDisplayModeList), with the D3D11 driver's
// DISPLAY format support for them: DXGI lists a format's modes only where the driver says it can scan it out.
// Build: x86_64-w64-mingw32-g++ -O2 -static dxgimodes.cpp -o dxgimodes.exe -ld3d11 -ldxgi
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <vector>

int main()
{
   setvbuf(stdout, nullptr, _IONBF, 0);
   IDXGIFactory1 *f;
   if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) return 1;
   const struct { DXGI_FORMAT fmt; const char *name; } fmts[] = {
      {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"}, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB"},
      {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"}, {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB"},
      {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM"}, {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT"}};
   IDXGIAdapter1 *a;
   for (UINT ai = 0; f->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ai++) {
      DXGI_ADAPTER_DESC1 ad;
      a->GetDesc1(&ad);
      printf("adapter %u: %ls\n", ai, ad.Description);
      ID3D11Device *dev = nullptr;
      D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
      IDXGIOutput *o;
      for (UINT oi = 0; a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; oi++) {
         DXGI_OUTPUT_DESC od;
         o->GetDesc(&od);
         printf(" output %u: %ls\n", oi, od.DeviceName);
         for (auto &fm : fmts) {
            UINT support = 0, n = 0;
            if (dev) dev->CheckFormatSupport(fm.fmt, &support);
            HRESULT hr = o->GetDisplayModeList(fm.fmt, 0, &n, nullptr);
            std::vector<DXGI_MODE_DESC> m(n);
            if (n) o->GetDisplayModeList(fm.fmt, 0, &n, m.data());
            printf("  %-20s display support %d, %u modes (0x%08lx)", fm.name, !!(support & D3D11_FORMAT_SUPPORT_DISPLAY),
                   n, (unsigned long)hr);
            for (UINT i = 0; i < n && i < 40; i++)
               if (m[i].Width == 1280 && (m[i].Height == 1024 || m[i].Height == 800))
                  printf(" %ux%u@%u/%u", m[i].Width, m[i].Height, m[i].RefreshRate.Numerator, m[i].RefreshRate.Denominator);
            printf("\n");
         }
         o->Release();
      }
      if (dev) dev->Release();
      a->Release();
   }
   return 0;
}
