// presentbench <d3d11|d3d12> <width> <height> <seconds> [bgra]: a borderless window of that size, cleared to a changing color
// and presented (flip discard, 3 buffers, sync interval 0; R8G8B8A8, or B8G8R8A8) as fast as it goes; prints frames per second and the average
// time spent in Present. A load per frame (environment): PB_CPU_MS, a CPU spin of that many milliseconds before the
// frame's commands; PB_GPU_COPIES, that many copies of a 3840x2160 texture to another besides the window's clear
// (GPU work that no fast path skips); PB_SUBMITS, D3D12 only: those copies over that many ExecuteCommandLists. Build: x86_64-w64-mingw32-g++ -O2 -static presentbench.cpp -o presentbench.exe -ld3d11 -ld3d12
// -ldxgi -luser32
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static double now()
{
   LARGE_INTEGER c, f;
   QueryPerformanceCounter(&c);
   QueryPerformanceFrequency(&f);
   return (double)c.QuadPart / f.QuadPart;
}

static void pump()
{
   MSG m;
   while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
}

int main(int argc, char **argv)
{
   if (argc < 5) {
      fprintf(stderr, "usage: %s <d3d11|d3d12> <width> <height> <seconds>\n", argv[0]);
      return 2;
   }
   bool d12 = !strcmp(argv[1], "d3d12");
   UINT w = atoi(argv[2]), h = atoi(argv[3]);
   double seconds = atof(argv[4]);
   bool bgra = argc > 5 && !strcmp(argv[5], "bgra");
   double cpuMs = getenv("PB_CPU_MS") ? atof(getenv("PB_CPU_MS")) : 0;
   int gpuCopies = getenv("PB_GPU_COPIES") ? atoi(getenv("PB_GPU_COPIES")) : 0;
   int submits = getenv("PB_SUBMITS") ? atoi(getenv("PB_SUBMITS")) : 1;
   WNDCLASSA wc = {};
   wc.lpfnWndProc = DefWindowProcA;
   wc.hInstance = GetModuleHandleA(nullptr);
   wc.lpszClassName = "presentbench";
   RegisterClassA(&wc);
   HWND win = CreateWindowExA(0, "presentbench", "presentbench", WS_POPUP | WS_VISIBLE, 0, 0, w, h, nullptr, nullptr,
                              wc.hInstance, nullptr);
   pump();
   IDXGIFactory4 *factory = nullptr;
   CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&factory);
   DXGI_SWAP_CHAIN_DESC1 sd = {};
   sd.Width = w;
   sd.Height = h;
   sd.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
   sd.SampleDesc.Count = 1;
   sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   sd.BufferCount = 3;
   sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
   IDXGISwapChain1 *sc1 = nullptr;
   IDXGISwapChain3 *sc = nullptr;
   HRESULT hr;

   // D3D11
   ID3D11Device *dev11 = nullptr;
   ID3D11DeviceContext *ctx11 = nullptr;
   // D3D12
   ID3D12Device *dev = nullptr;
   ID3D12CommandQueue *queue = nullptr;
   ID3D12CommandAllocator *alloc[3] = {};
   ID3D12GraphicsCommandList *list = nullptr;
   ID3D12Fence *fence = nullptr;
   UINT64 fenceValues[3] = {}, fenceValue = 0;
   HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
   ID3D12DescriptorHeap *rtvs = nullptr;
   ID3D12Resource *buffers[3] = {};
   UINT inc = 0;

   if (d12) {
      hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev);
      if (FAILED(hr)) {
         printf("D3D12CreateDevice: 0x%08lx\n", (unsigned long)hr);
         return 1;
      }
      D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
      dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&queue);
      for (auto &a : alloc) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&a);
      dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList),
                             (void **)&list);
      list->Close();
      dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&fence);
      hr = factory->CreateSwapChainForHwnd(queue, win, &sd, nullptr, nullptr, &sc1);
      D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&rtvs);
      inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
   } else {
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev11,
                             nullptr, &ctx11);
      if (FAILED(hr)) {
         printf("D3D11CreateDevice: 0x%08lx\n", (unsigned long)hr);
         return 1;
      }
      hr = factory->CreateSwapChainForHwnd(dev11, win, &sd, nullptr, nullptr, &sc1);
   }
   if (FAILED(hr) || FAILED(sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&sc))) {
      printf("CreateSwapChainForHwnd: 0x%08lx\n", (unsigned long)hr);
      return 1;
   }
   if (d12) {
      for (UINT i = 0; i < 3; i++) {
         sc->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&buffers[i]);
         D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
         rtv.ptr += i * inc;
         dev->CreateRenderTargetView(buffers[i], nullptr, rtv);
      }
   }
   ID3D11RenderTargetView *rtv11 = nullptr;
   // the GPU load's target
   D3D12_RESOURCE_DESC ld = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 3840, 2160, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
   ID3D12Resource *load12[2] = {};
   ID3D11Texture2D *load11[2] = {};
   for (int k = 0; k < 2 && gpuCopies; k++) {
      if (d12) {
         D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
         dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ld, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                      __uuidof(ID3D12Resource), (void **)&load12[k]);
      } else {
         D3D11_TEXTURE2D_DESC td = {3840, 2160, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET, 0, 0};
         dev11->CreateTexture2D(&td, nullptr, &load11[k]);
      }
   }

   UINT frames = 0;
   double presentTime = 0, start = now(), end = start + seconds;
   while (now() < end) {
      float c = (frames % 256) / 255.0f;
      const float color[4] = {c, 0.25f, 1 - c, 1};
      for (double spin = now() + cpuMs / 1000; now() < spin;) {
      }
      if (d12) {
         UINT i = sc->GetCurrentBackBufferIndex();
         if (fence->GetCompletedValue() < fenceValues[i]) {
            fence->SetEventOnCompletion(fenceValues[i], event);
            WaitForSingleObject(event, 5000);
         }
         alloc[i]->Reset();
         list->Reset(alloc[i], nullptr);
         D3D12_RESOURCE_BARRIER b = {};
         b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
         b.Transition.pResource = buffers[i];
         b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
         b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
         b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
         list->ResourceBarrier(1, &b);
         D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
         rtv.ptr += i * inc;
         list->ClearRenderTargetView(rtv, color, 0, nullptr);
         for (int k = 0; k < gpuCopies; k++) {
            list->CopyResource(load12[k & 1], load12[(k + 1) & 1]);
            // the frame's earlier submissions
            if (submits > 1 && (k + 1) % ((gpuCopies + submits - 1) / submits) == 0 && k + 1 < gpuCopies) {
               list->Close();
               ID3D12CommandList *part[] = {list};
               queue->ExecuteCommandLists(1, part);
               list->Reset(alloc[i], nullptr);
            }
         }
         b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
         b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
         list->ResourceBarrier(1, &b);
         list->Close();
         ID3D12CommandList *lists[] = {list};
         queue->ExecuteCommandLists(1, lists);
         double t = now();
         hr = sc->Present(0, 0);
         presentTime += now() - t;
         queue->Signal(fence, ++fenceValue);
         fenceValues[i] = fenceValue;
      } else {
         ID3D11Texture2D *back = nullptr;
         sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
         dev11->CreateRenderTargetView(back, nullptr, &rtv11);
         ctx11->ClearRenderTargetView(rtv11, color);
         for (int k = 0; k < gpuCopies; k++) ctx11->CopyResource(load11[k & 1], load11[(k + 1) & 1]);
         rtv11->Release();
         back->Release();
         double t = now();
         hr = sc->Present(0, 0);
         presentTime += now() - t;
      }
      if (FAILED(hr)) {
         printf("Present: 0x%08lx after %u frames\n", (unsigned long)hr, frames);
         return 1;
      }
      frames++;
      pump();
   }
   double elapsed = now() - start;
   printf("%s%s %ux%u: %u frames in %.1f s = %.1f fps, Present %.2f ms average\n", d12 ? "d3d12" : "d3d11",
          bgra ? " bgra" : "", w, h, frames, elapsed, frames / elapsed, presentTime * 1000 / frames);
   if (d12) {
      queue->Signal(fence, ++fenceValue);
      fence->SetEventOnCompletion(fenceValue, event);
      WaitForSingleObject(event, 5000);
   }
   DestroyWindow(win);
   return 0;
}
