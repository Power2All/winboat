// fs10.exe [buffers] [seconds] [modeswitch]: a full-screen Direct3D 10 swap chain (1280x1024) whose one render target view, made
// once as games do, is cleared red, green, blue, white for a quarter of the time each and presented every frame.
// What the screen shows tells whether full-screen (flip) presents reach the display.
#include <windows.h>
#include <d3d10.h>
#include <stdio.h>
#include <stdlib.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(int argc, char **argv)
{
   int buffers = argc > 1 ? atoi(argv[1]) : 2, seconds = argc > 2 ? atoi(argv[2]) : 12;
   WNDCLASSA wc = {};
   wc.lpfnWndProc = WndProc;
   wc.hInstance = GetModuleHandleA(NULL);
   wc.lpszClassName = "fs10";
   RegisterClassA(&wc);
   HWND hwnd = CreateWindowA("fs10", "fs10", WS_POPUP | WS_VISIBLE, 0, 0, 1280, 1024, NULL, NULL, wc.hInstance, NULL);

   DXGI_SWAP_CHAIN_DESC sd = {};
   sd.BufferCount = buffers;
   sd.BufferDesc.Width = 1280;
   sd.BufferDesc.Height = 1024;
   sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   sd.OutputWindow = hwnd;
   sd.SampleDesc.Count = 1;
   sd.Windowed = FALSE;
   sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
   // modeswitch: the display mode the swap chain asks for (flip presents, as 3DMark Vantage gets)
   if (argc > 3 && atoi(argv[3])) {
      sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
      sd.BufferDesc.RefreshRate.Numerator = 60;
      sd.BufferDesc.RefreshRate.Denominator = 1;
   }
   ID3D10Device *dev;
   IDXGISwapChain *sc;
   HRESULT hr = D3D10CreateDeviceAndSwapChain(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &sd, &sc, &dev);
   if (FAILED(hr)) { printf("create: 0x%lx\n", hr); return 1; }

   // modeswitch: as games switch, made windowed then the closest mode set and full screen
   if (argc > 3 && atoi(argv[3])) {
      sc->SetFullscreenState(FALSE, NULL);
      IDXGIOutput *out = NULL;
      sc->GetContainingOutput(&out);
      DXGI_MODE_DESC want = sd.BufferDesc, mode = {};
      want.RefreshRate.Numerator = want.RefreshRate.Denominator = 0;
      hr = out ? out->FindClosestMatchingMode(&want, &mode, dev) : E_FAIL;
      printf("closest mode: 0x%lx %ux%u %u/%u\n", hr, mode.Width, mode.Height, mode.RefreshRate.Numerator,
             mode.RefreshRate.Denominator);
      sc->ResizeTarget(&mode);
      hr = sc->SetFullscreenState(TRUE, NULL);
      printf("full screen: 0x%lx\n", hr);
      sc->ResizeBuffers(buffers, mode.Width, mode.Height, mode.Format, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
      if (out) out->Release();
   }
   ID3D10Texture2D *bb;
   sc->GetBuffer(0, __uuidof(ID3D10Texture2D), (void **)&bb);
   ID3D10RenderTargetView *rtv;
   dev->CreateRenderTargetView(bb, NULL, &rtv);
   dev->OMSetRenderTargets(1, &rtv, NULL);

   static const float colors[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
   DWORD start = GetTickCount();
   int frames = 0, shown = -1;
   for (;;) {
      DWORD t = GetTickCount() - start;
      if (t >= (DWORD)seconds * 1000) break;
      int c = (int)(t * 4 / ((DWORD)seconds * 1000));
      if (c != shown) { printf("%5lu ms: %s\n", t, (const char *[]){"red", "green", "blue", "white"}[c]); fflush(stdout); shown = c; }
      dev->ClearRenderTargetView(rtv, colors[c]);
      hr = sc->Present(1, 0);
      if (FAILED(hr)) { printf("present: 0x%lx\n", hr); break; }
      frames++;
      MSG msg;
      while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
   }
   printf("%d frames\n", frames);
   sc->SetFullscreenState(FALSE, NULL);
   rtv->Release(); bb->Release(); sc->Release(); dev->Release();
   return 0;
}
