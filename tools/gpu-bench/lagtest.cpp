// lagtest.exe [seconds] [log] [width height]: a game-like window for measuring a display path from the host. A
// borderless full-screen Direct3D 11 window (flip model, vsync) that shows its frame number as 10 stripes across the
// top fifth of the screen (white = 1, bit 0 on the left) and turns the rest white or black on each key press (raw
// input, so it doesn't need the focus). Logs presents per second. width/height: the display mode to use meanwhile.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>

static bool white = false;
static int presses = 0;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEKEYBOARD && !(ri.data.keyboard.Flags & RI_KEY_BREAK)) {
            if (ri.data.keyboard.VKey == VK_ESCAPE) PostQuitMessage(0);
            else if (ri.data.keyboard.VKey == VK_SPACE) { white = !white; presses++; }
        }
    } else if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 120;
    const char* logPath = argc > 2 ? argv[2] : "C:\\build\\lagtest.log";
    FILE* log = fopen(logPath, "w");
    SetProcessDPIAware();
    if (argc > 4) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = atoi(argv[3]);
        dm.dmPelsHeight = atoi(argv[4]);
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        fprintf(log, "mode %ldx%ld: %ld\n", dm.dmPelsWidth, dm.dmPelsHeight, ChangeDisplaySettingsW(&dm, CDS_FULLSCREEN));
    }
    int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"lagtest";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"lagtest", L"lagtest", WS_POPUP | WS_VISIBLE, 0, 0, W, H, nullptr,
                                nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE rid{1, 6, RIDEV_INPUTSINK, hwnd};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    ShowCursor(FALSE);

    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        fprintf(log, "no device\n");
        return 1;
    }
    IDXGIDevice* dd;
    IDXGIAdapter* ad;
    IDXGIFactory2* factory;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd);
    dd->GetAdapter(&ad);
    ad->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = W;
    sd.Height = H;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc;
    if (FAILED(factory->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc))) {
        fprintf(log, "no swap chain\n");
        return 1;
    }
    ID3D11Texture2D* bb;
    ID3D11RenderTargetView* rtv;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    dev->CreateRenderTargetView(bb, nullptr, &rtv);

    // A stripe in white and in black, copied into the back buffer
    int sw = W / 10, sh = H / 5;
    ID3D11Texture2D* stripe[2];
    for (int i = 0; i < 2; i++) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = sw;
        td.Height = sh;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        dev->CreateTexture2D(&td, nullptr, &stripe[i]);
        ID3D11RenderTargetView* v;
        dev->CreateRenderTargetView(stripe[i], nullptr, &v);
        float c[4] = {i ? 1.f : 0.f, i ? 1.f : 0.f, i ? 1.f : 0.f, 1.f};
        ctx->ClearRenderTargetView(v, c);
        v->Release();
    }
    fprintf(log, "window %dx%d\n", W, H);
    fflush(log);

    LARGE_INTEGER freq, start, now, second;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    second = start;
    unsigned frame = 0, frames = 0;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            DispatchMessageW(&msg);
        }
        float bg[4] = {white ? 1.f : 0.f, white ? 1.f : 0.f, white ? 1.f : 0.f, 1.f};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, bg);
        for (int bit = 0; bit < 10; bit++)
            ctx->CopySubresourceRegion(bb, 0, bit * sw, 0, 0, stripe[(frame >> bit) & 1], 0, nullptr);
        sc->Present(1, 0);
        frame++;
        frames++;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - second.QuadPart >= freq.QuadPart) {
            fprintf(log, "presents/s %.1f presses %d\n", frames * (double)freq.QuadPart / (now.QuadPart - second.QuadPart),
                    presses);
            fflush(log);
            frames = 0;
            second = now;
        }
        if (now.QuadPart - start.QuadPart >= (LONGLONG)seconds * freq.QuadPart) break;
    }
done:
    fprintf(log, "done %u frames\n", frame);
    fclose(log);
    if (argc > 4) ChangeDisplaySettingsW(nullptr, 0);
    return 0;
}
