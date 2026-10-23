// WinBoat GPU check
//
// Probes every graphics path a Windows app might use (DXGI, Direct3D 9/11/12, OpenGL, Vulkan, video decode,
// on-screen presentation) and prints a checklist of what works and what doesn't. Rendering is verified by reading
// pixels back, not just by checking that a device could be created.
//
// Each group of checks runs in a child process with a timeout, so a crashing or hanging driver shows up as a
// failed check instead of taking the whole report down.
//
//   winboat-gpucheck.exe [--out DIR] [--timeout SECONDS] [--no-window] [--only GROUP,...] [--group NAME]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <setupapi.h>
#include <GL/gl.h>

#include <chrono>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------
// Results

enum class Status { Pass, Warn, Fail, Info, Skip };

static const char* statusName(Status s) {
    switch (s) {
        case Status::Pass: return "PASS";
        case Status::Warn: return "WARN";
        case Status::Fail: return "FAIL";
        case Status::Info: return "INFO";
        case Status::Skip: return "SKIP";
    }
    return "INFO";
}

static std::string sanitize(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    }
    return s;
}

// Child processes print one line per result; the parent collects them
static void report(Status s, const std::string& name, const std::string& detail = "") {
    printf("@@R\t%s\t%s\t%s\n", statusName(s), sanitize(name).c_str(), sanitize(detail).c_str());
    fflush(stdout);
}

static std::string fmt(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

static std::string hr(HRESULT h) { return fmt("HRESULT 0x%08lX", static_cast<unsigned long>(h)); }

static std::string narrow(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** operator&() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

static double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

constexpr UINT VENDOR_VIRTIO = 0x1AF4;
constexpr UINT VENDOR_MICROSOFT = 0x1414;
constexpr double PERF_SECONDS = 2.0;
constexpr int PERF_SIZE = 1024;
constexpr int PERF_TRIANGLES = 200;

static bool interactiveDesktop() {
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!desk) return false;
    CloseDesktop(desk);
    return true;
}

static void pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static HWND createWindow(const wchar_t* title, int width, int height, bool visible) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WinBoatGPUCheck";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    RECT rect = {0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(visible ? WS_EX_TOPMOST : 0, L"WinBoatGPUCheck", title, WS_OVERLAPPEDWINDOW, 100, 100,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (hwnd && visible) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        UpdateWindow(hwnd);
        pumpMessages();
    }
    return hwnd;
}

// Reads what is actually on screen in the middle of the window's client area
// Screen pixel at a fraction of the window's client area (0.5, 0.5 is the center)
static COLORREF screenPixelAt(HWND hwnd, double fx, double fy) {
    RECT rect;
    GetClientRect(hwnd, &rect);
    POINT pt = {static_cast<LONG>((rect.right - rect.left) * fx), static_cast<LONG>((rect.bottom - rect.top) * fy)};
    ClientToScreen(hwnd, &pt);
    HDC screen = GetDC(nullptr);
    COLORREF color = GetPixel(screen, pt.x, pt.y);
    ReleaseDC(nullptr, screen);
    return color;
}

static COLORREF screenPixelAtCenter(HWND hwnd) { return screenPixelAt(hwnd, 0.5, 0.5); }

static bool closeTo(int value, int expected) { return value >= expected - 8 && value <= expected + 8; }

static bool colorMatches(int r, int g, int b, int er, int eg, int eb) {
    return closeTo(r, er) && closeTo(g, eg) && closeTo(b, eb);
}

// ---------------------------------------------------------------------------------------------------------------
// Environment

typedef LONG(WINAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
typedef LONG(WINAPI* PFN_NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);

static std::string regString(HKEY root, const char* key, const char* value) {
    char buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return "";
    return buf;
}

static bool regDword(HKEY root, const char* key, const char* value, DWORD& out) {
    DWORD size = sizeof(out);
    return RegGetValueA(root, key, value, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}

static void groupEnvironment() {
    const char* cv = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    DWORD ubr = 0;
    regDword(HKEY_LOCAL_MACHINE, cv, "UBR", ubr);
    std::string product = regString(HKEY_LOCAL_MACHINE, cv, "ProductName");
    // ProductName still says "Windows 10" on Windows 11
    if (atoi(regString(HKEY_LOCAL_MACHINE, cv, "CurrentBuild").c_str()) >= 22000 && product.rfind("Windows 10", 0) == 0) {
        product.replace(8, 2, "11");
    }
    report(Status::Info, "Windows version",
           fmt("%s %s (build %s.%lu)", product.c_str(),
               regString(HKEY_LOCAL_MACHINE, cv, "DisplayVersion").c_str(),
               regString(HKEY_LOCAL_MACHINE, cv, "CurrentBuild").c_str(), static_cast<unsigned long>(ubr)));
    // 32-bit apps load the drivers' 32-bit (WOW64) user-mode parts, which a driver may not ship
    report(Status::Info, "Test build", sizeof(void*) == 8 ? "64-bit" : "32-bit (runs like older 32-bit apps and games)");

    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    bool remote = GetSystemMetrics(SM_REMOTESESSION) != 0;
    report(Status::Info, "Session",
           fmt("%s (session %lu)%s", remote ? "Remote Desktop" : "Console", static_cast<unsigned long>(session),
               interactiveDesktop() ? "" : ", no interactive desktop"));

    // SystemCodeIntegrityInformation: test-signed drivers only load when CODEINTEGRITY_OPTION_TESTSIGN is set
    struct {
        ULONG Length;
        ULONG CodeIntegrityOptions;
    } ci = {sizeof(ci), 0};
    auto query = reinterpret_cast<PFN_NtQuerySystemInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (query && query(103, &ci, sizeof(ci), nullptr) == 0) {
        bool testSigning = ci.CodeIntegrityOptions & 0x2;
        report(testSigning ? Status::Pass : Status::Warn, "Test signing",
               testSigning ? "enabled, test-signed drivers can load" : "disabled, test-signed drivers won't load");
    } else {
        report(Status::Warn, "Test signing", "could not query code integrity options");
    }

    DWORD hwBeforeSw = 0;
    bool hasPolicy = regDword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services",
                              "bEnumerateHWBeforeSW", hwBeforeSw);
    report(hasPolicy && hwBeforeSw ? Status::Pass : (remote ? Status::Fail : Status::Warn),
           "RDP sessions may use the hardware GPU (bEnumerateHWBeforeSW)",
           hasPolicy && hwBeforeSw ? "policy set" : "policy not set, RDP sessions render in software");

    DISPLAY_DEVICEA dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); i++) {
        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
            report(Status::Info, "Active display", fmt("%s on %s%s", dd.DeviceName, dd.DeviceString,
                                                        dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE ? " (primary)" : ""));
        }
        dd.cb = sizeof(dd);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Device Manager view of the display adapters

static std::string deviceProperty(HDEVINFO set, SP_DEVINFO_DATA& data, DWORD property) {
    char buf[512] = {};
    if (!SetupDiGetDeviceRegistryPropertyA(set, &data, property, nullptr, reinterpret_cast<PBYTE>(buf), sizeof(buf),
                                           nullptr)) {
        return "";
    }
    return buf;  // first string of REG_MULTI_SZ values
}

static void groupPnp() {
    static const GUID displayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    HDEVINFO set = SetupDiGetClassDevsA(&displayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        report(Status::Fail, "Enumerate display devices", fmt("error %lu", GetLastError()));
        return;
    }

    bool foundVirtio = false;
    SP_DEVINFO_DATA data = {};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); i++) {
        std::string desc = deviceProperty(set, data, SPDRP_DEVICEDESC);
        std::string hwid = deviceProperty(set, data, SPDRP_HARDWAREID);
        std::string service = deviceProperty(set, data, SPDRP_SERVICE);
        ULONG status = 0, problem = 0;
        CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0);
        bool isVirtio = hwid.find("VEN_1AF4&DEV_1050") != std::string::npos;
        std::string detail = fmt("%s, driver service '%s'", hwid.c_str(), service.c_str());

        if (!isVirtio) {
            report(Status::Info, desc, detail);
            continue;
        }
        foundVirtio = true;
        if (problem != 0) {
            report(Status::Fail, "virtio-gpu device: " + desc, fmt("Device Manager error %lu, %s", problem, detail.c_str()));
        } else if (_stricmp(service.c_str(), "BasicDisplay") == 0) {
            report(Status::Fail, "virtio-gpu device: " + desc, "uses the Microsoft Basic Display driver, no 3D driver installed");
        } else {
            report(Status::Pass, "virtio-gpu device: " + desc, detail);
        }
    }
    SetupDiDestroyDeviceInfoList(set);

    if (!foundVirtio) {
        report(Status::Fail, "virtio-gpu device present", "no PCI\\VEN_1AF4&DEV_1050 display device (is GPU=Y set?)");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// DXGI

static bool isSoftwareAdapter(const DXGI_ADAPTER_DESC1& desc) {
    return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (desc.VendorId == VENDOR_MICROSOFT && desc.DeviceId == 0x8C);
}

// The adapter the GPU checks run on: the virtio-gpu if Windows has a render driver for it, else any hardware adapter
static bool pickHardwareAdapter(IDXGIAdapter1** out, DXGI_ADAPTER_DESC1* outDesc) {
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    IDXGIAdapter1* fallback = nullptr;
    DXGI_ADAPTER_DESC1 fallbackDesc = {};
    Com<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (isSoftwareAdapter(desc)) continue;
        if (desc.VendorId == VENDOR_VIRTIO) {
            if (fallback) fallback->Release();
            *outDesc = desc;
            *out = adapter.p;
            adapter.p = nullptr;
            return true;
        }
        if (!fallback) {
            fallback = adapter.p;
            fallbackDesc = desc;
            adapter.p = nullptr;
        }
    }
    if (!fallback) return false;
    *out = fallback;
    *outDesc = fallbackDesc;
    return true;
}

static void groupDxgi() {
    Com<IDXGIFactory1> factory;
    HRESULT h = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(h)) {
        report(Status::Fail, "Create DXGI factory", hr(h));
        return;
    }

    bool virtioRender = false;
    Com<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        LARGE_INTEGER umd = {};
        bool hasUmd = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
        std::string detail =
            fmt("%04X:%04X, %llu MB dedicated, %llu MB shared%s%s", desc.VendorId, desc.DeviceId,
                static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20),
                static_cast<unsigned long long>(desc.SharedSystemMemory >> 20),
                isSoftwareAdapter(desc) ? ", software" : "",
                hasUmd ? fmt(", D3D user-mode driver %u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                             HIWORD(umd.LowPart), LOWORD(umd.LowPart))
                             .c_str()
                       : ", no D3D user-mode driver");

        if (desc.VendorId == VENDOR_VIRTIO) {
            virtioRender = hasUmd;
            report(hasUmd ? Status::Pass : Status::Fail, fmt("DXGI adapter %u: ", i) + narrow(desc.Description), detail);
        } else {
            report(Status::Info, fmt("DXGI adapter %u: ", i) + narrow(desc.Description), detail);
        }
    }
    if (!virtioRender) {
        report(Status::Fail, "virtio-gpu usable for rendering", "no DXGI adapter with a D3D driver for the virtio-gpu");
    }

    Com<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6)))) {
        Com<IDXGIAdapter1> preferred;
        if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                            IID_PPV_ARGS(&preferred)))) {
            DXGI_ADAPTER_DESC1 desc;
            preferred->GetDesc1(&desc);
            report(Status::Info, "High-performance GPU preference", narrow(desc.Description));
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Direct3D 11

typedef HRESULT(WINAPI* PFN_D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                          LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static ID3DBlob* compileShader(const char* source, const char* entry, const char* target, std::string& error) {
    static PFN_D3DCompileFn compile = nullptr;
    if (!compile) {
        HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
        if (module) compile = reinterpret_cast<PFN_D3DCompileFn>(GetProcAddress(module, "D3DCompile"));
    }
    if (!compile) {
        error = "d3dcompiler_47.dll not available";
        return nullptr;
    }
    ID3DBlob* blob = nullptr;
    Com<ID3DBlob> errors;
    HRESULT h = compile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &errors);
    if (FAILED(h)) {
        error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : hr(h);
        return nullptr;
    }
    return blob;
}

static const char* TRIANGLE_HLSL = R"(
struct VSOut { float4 pos : SV_Position; };
VSOut vs(uint id : SV_VertexID) {
    float2 p[3] = { float2(0.0, 0.8), float2(0.8, -0.8), float2(-0.8, -0.8) };
    VSOut o;
    o.pos = float4(p[id], 0.0, 1.0);
    return o;
}
float4 ps(VSOut i) : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }
)";

// The way most apps draw: positions from a vertex buffer through an input layout
static const char* TRIANGLE_VB_HLSL = R"(
float4 vs(float2 pos : POSITION) : SV_Position { return float4(pos, 0.0, 1.0); }
float4 ps(float4 pos : SV_Position) : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }
)";

static const char* COMPUTE_HLSL = R"(
RWStructuredBuffer<uint> buf : register(u0);
[numthreads(64, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) { buf[id.x] = id.x * 3 + 1; }
)";

static const char* featureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        case D3D_FEATURE_LEVEL_9_3: return "9_3";
        case D3D_FEATURE_LEVEL_9_2: return "9_2";
        case D3D_FEATURE_LEVEL_9_1: return "9_1";
        default: return "unknown";
    }
}

static HRESULT createD3D11Device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, ID3D11Device** device,
                                 ID3D11DeviceContext** context, D3D_FEATURE_LEVEL* level) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
                                  D3D_FEATURE_LEVEL_9_1};
    HRESULT h = D3D11CreateDevice(adapter, type, nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device,
                                  level, context);
    if (h == E_INVALIDARG) {  // runtimes without 11_1
        h = D3D11CreateDevice(adapter, type, nullptr, 0, levels + 1, ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, device,
                              level, context);
    }
    return h;
}

struct D3D11Pipeline {
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11InputLayout> layout;  // vertex buffer variant only
    Com<ID3D11Buffer> vertices;
};

static bool createTrianglePipeline(ID3D11Device* device, D3D11Pipeline& pipeline, bool vertexBuffer,
                                   std::string& error) {
    const char* source = vertexBuffer ? TRIANGLE_VB_HLSL : TRIANGLE_HLSL;
    Com<ID3DBlob> vsBlob, psBlob;
    vsBlob.p = compileShader(source, "vs", "vs_4_0", error);
    if (!vsBlob) return false;
    psBlob.p = compileShader(source, "ps", "ps_4_0", error);
    if (!psBlob) return false;
    HRESULT h = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &pipeline.vs);
    if (SUCCEEDED(h)) {
        h = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pipeline.ps);
    }
    if (SUCCEEDED(h) && vertexBuffer) {
        D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        h = device->CreateInputLayout(&element, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      &pipeline.layout);
        if (SUCCEEDED(h)) {
            const float positions[] = {0.0f, 0.8f, 0.8f, -0.8f, -0.8f, -0.8f};
            D3D11_BUFFER_DESC desc = {sizeof(positions), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA data = {positions, 0, 0};
            h = device->CreateBuffer(&desc, &data, &pipeline.vertices);
        }
    }
    if (FAILED(h)) error = "create pipeline: " + hr(h);
    return SUCCEEDED(h);
}

static void bindTriangle(ID3D11DeviceContext* ctx, D3D11Pipeline& pipeline, int size) {
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(pipeline.layout);
    UINT stride = 2 * sizeof(float), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &pipeline.vertices.p, &stride, &offset);
    ctx->VSSetShader(pipeline.vs, nullptr, 0);
    ctx->PSSetShader(pipeline.ps, nullptr, 0);
}

static bool createRenderTarget(ID3D11Device* device, int size, ID3D11Texture2D** texture, ID3D11RenderTargetView** rtv) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = size;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, texture)) &&
           SUCCEEDED(device->CreateRenderTargetView(*texture, nullptr, rtv));
}

// Clears to blue, optionally draws a red triangle, reads the result back. Returns false if it couldn't run.
static bool d3d11Render(ID3D11Device* device, ID3D11DeviceContext* ctx, D3D11Pipeline* pipeline, uint8_t corner[4],
                        uint8_t center[4], std::string& error) {
    const int size = 64;
    Com<ID3D11Texture2D> target;
    Com<ID3D11RenderTargetView> rtv;
    if (!createRenderTarget(device, size, &target, &rtv)) {
        error = "could not create a render target";
        return false;
    }

    const float blue[4] = {0, 0, 1, 1};
    ctx->OMSetRenderTargets(1, &rtv.p, nullptr);
    ctx->ClearRenderTargetView(rtv, blue);
    if (pipeline) {
        bindTriangle(ctx, *pipeline, size);
        ctx->Draw(3, 0);
    }

    D3D11_TEXTURE2D_DESC desc;
    target->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    HRESULT h = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(h)) {
        error = "create staging texture: " + hr(h);
        return false;
    }
    ctx->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE mapped;
    h = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(h)) {
        error = "map: " + hr(h);
        return false;
    }
    auto pixel = [&](int x, int y) {
        return static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4;
    };
    memcpy(corner, pixel(1, 1), 4);
    memcpy(center, pixel(size / 2, size / 2), 4);
    ctx->Unmap(staging, 0);
    return true;
}

// Clear, then a red triangle drawn two ways (vertex buffer, and generated from SV_VertexID); returns whether the
// vertex buffer draw worked, which the benchmark uses
static bool d3d11RenderTest(ID3D11Device* device, ID3D11DeviceContext* ctx, D3D_FEATURE_LEVEL level) {
    uint8_t corner[4], center[4];
    std::string error;
    if (!d3d11Render(device, ctx, nullptr, corner, center, error)) {
        report(Status::Fail, "Clear + read back", error);
        return false;
    }
    report(colorMatches(corner[0], corner[1], corner[2], 0, 0, 255) ? Status::Pass : Status::Fail, "Clear + read back",
           fmt("pixel rgb(%d,%d,%d)", corner[0], corner[1], corner[2]));
    if (level < D3D_FEATURE_LEVEL_10_0) return false;

    bool vertexBufferOk = false;
    for (bool vertexBuffer : {true, false}) {
        const char* name = vertexBuffer ? "Draw a triangle from a vertex buffer + read back"
                                        : "Draw a triangle generated in the shader (SV_VertexID) + read back";
        D3D11Pipeline pipeline;
        if (!createTrianglePipeline(device, pipeline, vertexBuffer, error) ||
            !d3d11Render(device, ctx, &pipeline, corner, center, error)) {
            report(Status::Fail, name, error);
            continue;
        }
        bool ok = colorMatches(corner[0], corner[1], corner[2], 0, 0, 255) &&
                  colorMatches(center[0], center[1], center[2], 255, 0, 0);
        report(ok ? Status::Pass : Status::Fail, name,
               fmt("clear pixel rgb(%d,%d,%d), triangle pixel rgb(%d,%d,%d)", corner[0], corner[1], corner[2],
                   center[0], center[1], center[2]));
        if (vertexBuffer) vertexBufferOk = ok;
    }
    return vertexBufferOk;
}

static void d3d11ComputeTest(ID3D11Device* device, ID3D11DeviceContext* ctx, D3D_FEATURE_LEVEL level) {
    const char* target = "cs_5_0";
    if (level < D3D_FEATURE_LEVEL_11_0) {
        D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS options = {};
        device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &options, sizeof(options));
        if (!options.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x) {
            report(Status::Skip, "Compute shader", fmt("not supported at feature level %s", featureLevelName(level)));
            return;
        }
        target = "cs_4_0";
    }

    std::string error;
    Com<ID3DBlob> blob;
    blob.p = compileShader(COMPUTE_HLSL, "cs", target, error);
    if (!blob) {
        report(Status::Fail, "Compute shader", error);
        return;
    }
    Com<ID3D11ComputeShader> cs;
    HRESULT h = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
    if (FAILED(h)) {
        report(Status::Fail, "Compute shader", "create: " + hr(h));
        return;
    }

    const UINT count = 256;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = count * 4;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;
    Com<ID3D11Buffer> buffer, staging;
    Com<ID3D11UnorderedAccessView> uav;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = count;
    h = device->CreateBuffer(&bd, nullptr, &buffer);
    if (SUCCEEDED(h)) h = device->CreateUnorderedAccessView(buffer, &ud, &uav);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(h)) h = device->CreateBuffer(&bd, nullptr, &staging);
    if (FAILED(h)) {
        report(Status::Fail, "Compute shader", "create buffers: " + hr(h));
        return;
    }

    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav.p, nullptr);
    ctx->Dispatch(count / 64, 1, 1);
    ctx->CopyResource(staging, buffer);
    D3D11_MAPPED_SUBRESOURCE mapped;
    h = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(h)) {
        report(Status::Fail, "Compute shader", "map: " + hr(h));
        return;
    }
    const uint32_t* values = static_cast<const uint32_t*>(mapped.pData);
    UINT wrong = 0;
    for (UINT i = 0; i < count; i++) {
        if (values[i] != i * 3 + 1) wrong++;
    }
    ctx->Unmap(staging, 0);
    report(wrong ? Status::Fail : Status::Pass, fmt("Compute shader (%s) + read back", target),
           wrong ? fmt("%u of %u values wrong", wrong, count) : fmt("%u values correct", count));
}

// Overlapping full-size triangles into a 1024x1024 target, the same workload for every API
static double d3d11Perf(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    Com<ID3D11Texture2D> target;
    Com<ID3D11RenderTargetView> rtv;
    D3D11Pipeline pipeline;
    std::string error;
    if (!createRenderTarget(device, PERF_SIZE, &target, &rtv) ||
        !createTrianglePipeline(device, pipeline, true, error)) {
        return -1;
    }
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    Com<ID3D11Query> query;
    device->CreateQuery(&qd, &query);
    auto sync = [&] {
        ctx->End(query);
        while (ctx->GetData(query, nullptr, 0, 0) == S_FALSE) {
        }
    };

    const float black[4] = {0, 0, 0, 1};
    ctx->OMSetRenderTargets(1, &rtv.p, nullptr);
    bindTriangle(ctx, pipeline, PERF_SIZE);
    sync();
    int frames = 0;
    auto start = std::chrono::steady_clock::now();
    while (secondsSince(start) < PERF_SECONDS) {
        ctx->ClearRenderTargetView(rtv, black);
        ctx->DrawInstanced(3, PERF_TRIANGLES, 0, 0);
        if (++frames % 8 == 0) sync();
    }
    sync();
    return frames / secondsSince(start);
}

static void d3d11Suite(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, const std::string& label) {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    HRESULT h = createD3D11Device(adapter, type, &device, &ctx, &level);
    if (FAILED(h)) {
        report(Status::Fail, "Create Direct3D 11 device on " + label, hr(h));
        return;
    }
    report(Status::Pass, "Create Direct3D 11 device on " + label, fmt("feature level %s", featureLevelName(level)));
    bool drawOk = d3d11RenderTest(device, ctx, level);
    d3d11ComputeTest(device, ctx, level);
    if (!drawOk) {
        if (level >= D3D_FEATURE_LEVEL_10_0) {
            report(Status::Skip, "Fill-rate benchmark", "the vertex buffer triangle didn't render, so it would measure nothing");
        }
    } else {
        double fps = d3d11Perf(device, ctx);
        if (fps >= 0) {
            report(Status::Info, "Fill-rate benchmark",
                   fmt("%.1f frames/s (%dx%d, %d overlapping triangles)", fps, PERF_SIZE, PERF_SIZE, PERF_TRIANGLES));
            printf("@@M\t%s\t%.3f\n", type == D3D_DRIVER_TYPE_WARP ? "warp_fps" : "gpu_fps", fps);
        }
    }
}

static void groupD3D11() {
    Com<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc;
    if (!pickHardwareAdapter(&adapter, &desc)) {
        report(Status::Fail, "Hardware adapter", "Windows has no hardware render adapter, only software rendering");
        return;
    }
    d3d11Suite(adapter, D3D_DRIVER_TYPE_UNKNOWN, narrow(desc.Description));
}

static void groupD3D11Default() {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    HRESULT h = createD3D11Device(nullptr, D3D_DRIVER_TYPE_HARDWARE, &device, &ctx, &level);
    if (FAILED(h)) {
        report(Status::Fail, "Default hardware device", hr(h));
        return;
    }
    Com<IDXGIDevice> dxgiDevice;
    Com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        adapter->GetDesc(&desc);
    }
    bool software = desc.VendorId == VENDOR_MICROSOFT;
    report(software ? Status::Fail : Status::Pass, "Apps asking for \"a hardware device\" get",
           fmt("%s, feature level %s%s", narrow(desc.Description).c_str(), featureLevelName(level),
               software ? " (software rendering)" : ""));
}

static void groupWarp() { d3d11Suite(nullptr, D3D_DRIVER_TYPE_WARP, "WARP (software)"); }

// ---------------------------------------------------------------------------------------------------------------
// Direct3D 9

// Reads one pixel of a render target (through a system-memory copy); returns false if that failed
static bool d3d9ReadPixel(IDirect3DDevice9* device, IDirect3DSurface9* target, int x, int y, uint8_t rgb[3],
                          std::string& error) {
    D3DSURFACE_DESC desc;
    target->GetDesc(&desc);
    Com<IDirect3DSurface9> copy;
    HRESULT h = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy,
                                                    nullptr);
    if (SUCCEEDED(h)) h = device->GetRenderTargetData(target, copy);
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(h)) h = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(h)) {
        error = "read back: " + hr(h);
        return false;
    }
    const uint8_t* px = static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch + x * 4;  // BGRX
    rgb[0] = px[2];
    rgb[1] = px[1];
    rgb[2] = px[0];
    copy->UnlockRect();
    return true;
}

// Clears to blue, runs draw(), and checks for a red triangle in the middle of the 64x64 back buffer
static void d3d9DrawTest(IDirect3DDevice9* device, const char* name, const std::function<HRESULT()>& draw) {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0);
    HRESULT h = device->BeginScene();
    if (SUCCEEDED(h)) {
        h = draw();
        device->EndScene();
    }
    if (FAILED(h)) {
        report(Status::Fail, name, hr(h));
        return;
    }
    Com<IDirect3DSurface9> backBuffer;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    uint8_t corner[3], center[3];
    std::string error;
    if (!d3d9ReadPixel(device, backBuffer, 1, 1, corner, error) ||
        !d3d9ReadPixel(device, backBuffer, 32, 40, center, error)) {
        report(Status::Fail, name, error);
        return;
    }
    bool ok = colorMatches(corner[0], corner[1], corner[2], 0, 0, 255) &&
              colorMatches(center[0], center[1], center[2], 255, 0, 0);
    report(ok ? Status::Pass : Status::Fail, name,
           fmt("clear pixel rgb(%d,%d,%d), triangle pixel rgb(%d,%d,%d)", corner[0], corner[1], corner[2], center[0],
               center[1], center[2]));
}

static const char* D3D9_HLSL = R"(
float4 vs(float4 pos : POSITION) : POSITION { return pos; }
float4 ps() : COLOR { return float4(1.0, 0.0, 0.0, 1.0); }
)";

static void groupD3D9() {
    Com<IDirect3D9> d3d;
    d3d.p = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        report(Status::Fail, "Direct3DCreate9", "failed");
        return;
    }
    UINT adapters = d3d->GetAdapterCount();
    for (UINT i = 0; i < adapters; i++) {
        D3DADAPTER_IDENTIFIER9 id = {};
        d3d->GetAdapterIdentifier(i, 0, &id);
        report(Status::Info, fmt("Adapter %u", i),
               fmt("%s (%04lX:%04lX, %s)", id.Description, static_cast<unsigned long>(id.VendorId),
                   static_cast<unsigned long>(id.DeviceId), id.Driver));
    }

    const int size = 64;
    HWND hwnd = createWindow(L"WinBoat GPU check (D3D9)", size, size, false);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = size;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow = hwnd;
    // Most games ask for hardware vertex processing; fall back to the runtime's software vertex processing
    Com<IDirect3DDevice9> device;
    const char* vertexProcessing = "hardware";
    HRESULT h = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
    if (FAILED(h)) {
        vertexProcessing = "software";
        h = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                              D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
    }
    if (FAILED(h)) {
        // What the runtime itself thinks of the adapter, to tell a driver's caps from a device creation failure
        D3DDISPLAYMODE mode = {};
        HRESULT modeResult = d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
        D3DCAPS9 halCaps = {};
        report(Status::Fail, "Create HAL device",
               fmt("%s; display mode %s %ux%u format %u, CheckDeviceType %s, GetDeviceCaps %s", hr(h).c_str(),
                   hr(modeResult).c_str(), mode.Width, mode.Height, static_cast<unsigned>(mode.Format),
                   hr(d3d->CheckDeviceType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format, D3DFMT_X8R8G8B8, TRUE))
                       .c_str(),
                   hr(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &halCaps)).c_str()));
        DestroyWindow(hwnd);
        return;
    }
    D3DCAPS9 caps = {};
    device->GetDeviceCaps(&caps);
    D3DADAPTER_IDENTIFIER9 id = {};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    report(Status::Pass, "Create HAL device",
           fmt("on %s, %s vertex processing, vertex shader %lu.%lu, pixel shader %lu.%lu", id.Description,
               vertexProcessing, D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion),
               D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion), D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion),
               D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion)));

    // Clear only
    {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0);
        Com<IDirect3DSurface9> backBuffer;
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        uint8_t px[3];
        std::string error;
        if (!d3d9ReadPixel(device, backBuffer, 32, 32, px, error)) {
            report(Status::Fail, "Clear + read back", error);
        } else {
            report(colorMatches(px[0], px[1], px[2], 255, 0, 0) ? Status::Pass : Status::Fail, "Clear + read back",
                   fmt("pixel rgb(%d,%d,%d)", px[0], px[1], px[2]));
        }
    }

    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    const DWORD red = D3DCOLOR_XRGB(255, 0, 0);

    // Fixed function, pre-transformed vertices (screen coordinates): the simplest draw
    {
        struct Vertex { float x, y, z, rhw; DWORD color; };
        const Vertex v[] = {{32, 4, 0.5f, 1, red}, {60, 60, 0.5f, 1, red}, {4, 60, 0.5f, 1, red}};
        d3d9DrawTest(device, "Fixed function: draw pre-transformed triangle + read back", [&] {
            device->SetVertexShader(nullptr);
            device->SetPixelShader(nullptr);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            return device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vertex));
        });
    }

    // Fixed function vertex pipeline (identity world/view/projection transforms) from a vertex buffer
    {
        struct Vertex { float x, y, z; DWORD color; };
        const Vertex v[] = {{0, 0.9f, 0.5f, red}, {0.9f, -0.9f, 0.5f, red}, {-0.9f, -0.9f, 0.5f, red}};
        Com<IDirect3DVertexBuffer9> vb;
        void* data = nullptr;
        h = device->CreateVertexBuffer(sizeof(v), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT,
                                       &vb, nullptr);
        if (SUCCEEDED(h)) h = vb->Lock(0, 0, &data, 0);
        if (SUCCEEDED(h)) {
            memcpy(data, v, sizeof(v));
            vb->Unlock();
        }
        if (FAILED(h)) {
            report(Status::Fail, "Fixed function: transform a triangle from a vertex buffer + read back",
                   "vertex buffer: " + hr(h));
        } else {
            D3DMATRIX identity = {};
            identity._11 = identity._22 = identity._33 = identity._44 = 1;
            d3d9DrawTest(device, "Fixed function: transform a triangle from a vertex buffer + read back", [&] {
                device->SetTransform(D3DTS_WORLD, &identity);
                device->SetTransform(D3DTS_VIEW, &identity);
                device->SetTransform(D3DTS_PROJECTION, &identity);
                device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
                device->SetStreamSource(0, vb, 0, sizeof(Vertex));
                return device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
            });
        }
    }

    // Fixed function texturing: a red 2x2 managed texture (the runtime uploads it), white vertices
    {
        struct Vertex { float x, y, z, rhw; DWORD color; float u, v; };
        const DWORD white = D3DCOLOR_XRGB(255, 255, 255);
        const Vertex v[] = {{32, 4, 0.5f, 1, white, 0.5f, 0}, {60, 60, 0.5f, 1, white, 1, 1},
                            {4, 60, 0.5f, 1, white, 0, 1}};
        Com<IDirect3DTexture9> texture;
        h = device->CreateTexture(2, 2, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(h)) h = texture->LockRect(0, &locked, nullptr, 0);
        if (SUCCEEDED(h)) {
            for (int y = 0; y < 2; y++) {
                DWORD* row = reinterpret_cast<DWORD*>(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch);
                row[0] = row[1] = red;
            }
            texture->UnlockRect(0);
        }
        if (FAILED(h)) {
            report(Status::Fail, "Fixed function: textured triangle + read back", "texture: " + hr(h));
        } else {
            d3d9DrawTest(device, "Fixed function: textured triangle + read back", [&] {
                device->SetTexture(0, texture);
                device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                HRESULT r = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vertex));
                device->SetTexture(0, nullptr);
                return r;
            });
        }
    }

    // Vertex + pixel shaders (shader model 2.0)
    std::string error;
    Com<ID3DBlob> vsBlob, psBlob;
    vsBlob.p = compileShader(D3D9_HLSL, "vs", "vs_2_0", error);
    if (vsBlob) psBlob.p = compileShader(D3D9_HLSL, "ps", "ps_2_0", error);
    Com<IDirect3DVertexShader9> vs;
    Com<IDirect3DPixelShader9> ps;
    Com<IDirect3DVertexDeclaration9> decl;
    const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                          D3DDECL_END()};
    h = psBlob ? S_OK : E_FAIL;
    if (SUCCEEDED(h)) h = device->CreateVertexShader(static_cast<const DWORD*>(vsBlob->GetBufferPointer()), &vs);
    if (SUCCEEDED(h)) h = device->CreatePixelShader(static_cast<const DWORD*>(psBlob->GetBufferPointer()), &ps);
    if (SUCCEEDED(h)) h = device->CreateVertexDeclaration(elements, &decl);
    const float triangle[] = {0, 0.9f, 0.5f, 0.9f, -0.9f, 0.5f, -0.9f, -0.9f, 0.5f};
    if (FAILED(h)) {
        report(Status::Fail, "Shaders (vs_2_0/ps_2_0): draw a triangle + read back",
               psBlob ? "create shaders: " + hr(h) : "compile: " + error);
    } else {
        d3d9DrawTest(device, "Shaders (vs_2_0/ps_2_0): draw a triangle + read back", [&] {
            device->SetVertexDeclaration(decl);
            device->SetVertexShader(vs);
            device->SetPixelShader(ps);
            return device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, 3 * sizeof(float));
        });

        // Fill rate: the same workload as the other APIs, into a 1024x1024 render target
        Com<IDirect3DSurface9> target, previous;
        Com<IDirect3DQuery9> query;
        h = device->CreateRenderTarget(PERF_SIZE, PERF_SIZE, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target,
                                       nullptr);
        if (SUCCEEDED(h)) h = device->CreateQuery(D3DQUERYTYPE_EVENT, &query);
        if (SUCCEEDED(h)) {
            device->GetRenderTarget(0, &previous);
            device->SetRenderTarget(0, target);
            D3DVIEWPORT9 vp = {0, 0, PERF_SIZE, PERF_SIZE, 0, 1};
            device->SetViewport(&vp);
            std::vector<float> many;
            for (int i = 0; i < PERF_TRIANGLES; i++) many.insert(many.end(), triangle, triangle + 9);
            auto sync = [&] {
                query->Issue(D3DISSUE_END);
                while (query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                }
            };
            sync();
            int frames = 0;
            auto start = std::chrono::steady_clock::now();
            while (secondsSince(start) < PERF_SECONDS) {
                device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
                device->BeginScene();
                device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, PERF_TRIANGLES, many.data(), 3 * sizeof(float));
                device->EndScene();
                if (++frames % 8 == 0) sync();
            }
            sync();
            double fps = frames / secondsSince(start);
            device->SetRenderTarget(0, previous);
            report(Status::Info, "Fill-rate benchmark",
                   fmt("%.1f frames/s (%dx%d, %d overlapping triangles)", fps, PERF_SIZE, PERF_SIZE, PERF_TRIANGLES));
            printf("@@M\td3d9_fps\t%.3f\n", fps);
        } else {
            report(Status::Fail, "Fill-rate benchmark", hr(h));
        }
    }
    device.reset();
    DestroyWindow(hwnd);
}

// ---------------------------------------------------------------------------------------------------------------
// Direct3D 12

static void groupD3D12() {
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        report(Status::Fail, "Create DXGI factory", "failed");
        return;
    }
    bool anyHardware = false;
    Com<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (isSoftwareAdapter(desc)) continue;
        // Passing no output pointer only tests whether a device could be created
        HRESULT h = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
        bool ok = SUCCEEDED(h);
        anyHardware |= ok;
        report(ok ? Status::Pass : (desc.VendorId == VENDOR_VIRTIO ? Status::Warn : Status::Info),
               "Direct3D 12 on " + narrow(desc.Description), ok ? "supported (feature level 11_0+)" : hr(h));
    }
    if (!anyHardware) report(Status::Warn, "Direct3D 12 on any GPU", "not available, D3D12 apps won't run");
}

// ---------------------------------------------------------------------------------------------------------------
// OpenGL

#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA8_ 0x8058
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

typedef void(APIENTRY* PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void(APIENTRY* PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY* PFN_glCheckFramebufferStatus)(GLenum);
typedef HGLRC(WINAPI* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
typedef BOOL(WINAPI* PFN_wglSwapIntervalEXT)(int);

struct GLContext {
    HWND hwnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    PIXELFORMATDESCRIPTOR pfd = {};

    ~GLContext() {
        wglMakeCurrent(nullptr, nullptr);
        if (rc) wglDeleteContext(rc);
        if (dc) ReleaseDC(hwnd, dc);
        if (hwnd) DestroyWindow(hwnd);
    }

    bool create(int size, bool visible, std::string& error) {
        hwnd = createWindow(L"WinBoat GPU check (OpenGL)", size, size, visible);
        dc = GetDC(hwnd);
        PIXELFORMATDESCRIPTOR want = {};
        want.nSize = sizeof(want);
        want.nVersion = 1;
        want.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        want.iPixelType = PFD_TYPE_RGBA;
        want.cColorBits = 32;
        want.cDepthBits = 24;
        want.cStencilBits = 8;
        int format = ChoosePixelFormat(dc, &want);
        if (!format || !SetPixelFormat(dc, format, &want)) {
            error = fmt("no OpenGL pixel format (error %lu)", GetLastError());
            return false;
        }
        DescribePixelFormat(dc, format, sizeof(pfd), &pfd);
        rc = wglCreateContext(dc);
        if (!rc || !wglMakeCurrent(dc, rc)) {
            error = fmt("wglCreateContext/wglMakeCurrent failed (error %lu)", GetLastError());
            return false;
        }
        return true;
    }

    // Generic (non-accelerated) pixel formats come from Microsoft's software renderer
    bool isGeneric() const { return (pfd.dwFlags & PFD_GENERIC_FORMAT) && !(pfd.dwFlags & PFD_GENERIC_ACCELERATED); }
};

struct GLFramebuffer {
    GLuint fbo = 0, texture = 0;
    bool create(int size) {
        auto gen = reinterpret_cast<PFN_glGenFramebuffers>(wglGetProcAddress("glGenFramebuffers"));
        auto bind = reinterpret_cast<PFN_glBindFramebuffer>(wglGetProcAddress("glBindFramebuffer"));
        auto attach = reinterpret_cast<PFN_glFramebufferTexture2D>(wglGetProcAddress("glFramebufferTexture2D"));
        auto check = reinterpret_cast<PFN_glCheckFramebufferStatus>(wglGetProcAddress("glCheckFramebufferStatus"));
        if (!gen || !bind || !attach || !check) return false;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gen(1, &fbo);
        bind(GL_FRAMEBUFFER, fbo);
        attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        return check(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
};

static const GLfloat TRIANGLE_VERTICES[] = {0.0f, 0.8f, 0.8f, -0.8f, -0.8f, -0.8f};

static void glDrawTriangles(int count) {
    static std::vector<GLfloat> vertices;
    if (vertices.size() != static_cast<size_t>(count) * 6) {
        vertices.clear();
        for (int i = 0; i < count; i++) vertices.insert(vertices.end(), TRIANGLE_VERTICES, TRIANGLE_VERTICES + 6);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, vertices.data());
    glDrawArrays(GL_TRIANGLES, 0, count * 3);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static std::string maxCoreProfile(HDC dc) {
    auto createAttribs =
        reinterpret_cast<PFN_wglCreateContextAttribsARB>(wglGetProcAddress("wglCreateContextAttribsARB"));
    if (!createAttribs) return "";
    const int versions[][2] = {{4, 6}, {4, 5}, {4, 3}, {4, 0}, {3, 3}, {3, 2}};
    for (auto& v : versions) {
        int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, v[0], WGL_CONTEXT_MINOR_VERSION_ARB, v[1],
                         WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
        HGLRC rc = createAttribs(dc, nullptr, attribs);
        if (rc) {
            wglDeleteContext(rc);
            return fmt("%d.%d", v[0], v[1]);
        }
    }
    return "";
}

static void groupOpenGL() {
    GLContext gl;
    std::string error;
    if (!gl.create(64, false, error)) {
        report(Status::Fail, "Create OpenGL context", error);
        return;
    }
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* glsl = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
    std::string desc = fmt("%s / %s / OpenGL %s", vendor ? vendor : "?", renderer ? renderer : "?", version ? version : "?");
    bool software = gl.isGeneric() || (renderer && strstr(renderer, "GDI Generic"));
    report(software ? Status::Fail : Status::Pass, "Hardware OpenGL driver (ICD)",
           software ? desc + " (Microsoft software renderer)" : desc);
    if (glsl) report(Status::Info, "GLSL version", glsl);

    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    int extensionCount = 0;
    for (const char* p = extensions; p && *p; p++) {
        if (*p == ' ') extensionCount++;
    }
    report(Status::Info, "Extensions", fmt("%d", extensionCount));

    std::string core = maxCoreProfile(gl.dc);
    report(core.empty() ? Status::Warn : Status::Pass, "Core profile context",
           core.empty() ? "no OpenGL 3.2+ core profile" : "up to " + core);

    while (glGetError() != GL_NO_ERROR) {
        // GL_SHADING_LANGUAGE_VERSION and friends are invalid enums on OpenGL 1.1, don't blame the render test
    }
    GLFramebuffer fb;
    bool useFbo = fb.create(64);
    if (!useFbo) glDrawBuffer(GL_BACK);
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 0, 0);
    glDrawTriangles(1);
    glFinish();
    uint8_t center[4] = {}, corner[4] = {};
    if (!useFbo) glReadBuffer(GL_BACK);
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    std::string detail = fmt("%sclear pixel rgb(%d,%d,%d), triangle pixel rgb(%d,%d,%d)", useFbo ? "" : "back buffer, ",
                             corner[0], corner[1], corner[2], center[0], center[1], center[2]);
    report(colorMatches(corner[0], corner[1], corner[2], 0, 0, 255) &&
                   colorMatches(center[0], center[1], center[2], 255, 0, 0)
               ? Status::Pass
               : Status::Fail,
           "Draw a triangle + read back", detail);
    GLenum glError = glGetError();
    if (glError != GL_NO_ERROR) report(Status::Warn, "glGetError after render test", fmt("0x%04X", glError));

    if (useFbo) {
        GLFramebuffer perfFb;
        if (perfFb.create(PERF_SIZE)) {
            glViewport(0, 0, PERF_SIZE, PERF_SIZE);
            glFinish();
            int frames = 0;
            auto start = std::chrono::steady_clock::now();
            while (secondsSince(start) < PERF_SECONDS) {
                glClear(GL_COLOR_BUFFER_BIT);
                glDrawTriangles(PERF_TRIANGLES);
                if (++frames % 8 == 0) glFinish();
            }
            glFinish();
            double fps = frames / secondsSince(start);
            report(Status::Info, "Fill-rate benchmark",
                   fmt("%.1f frames/s (%dx%d, %d overlapping triangles)", fps, PERF_SIZE, PERF_SIZE, PERF_TRIANGLES));
            printf("@@M\tgl_fps\t%.3f\n", fps);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Vulkan (loaded dynamically, the loader isn't always installed)

typedef int32_t VkResultT;
typedef void* VkInstanceT;
typedef void* VkPhysicalDeviceT;
typedef void (*PFN_vkVoid)(void);
typedef PFN_vkVoid(__stdcall* PFN_vkGetInstanceProcAddrT)(VkInstanceT, const char*);

struct VkApplicationInfoT {
    uint32_t sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};
struct VkInstanceCreateInfoT {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    const VkApplicationInfoT* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

// Which virtio-gpu capsets (host renderer protocols) the viogpu3d kernel driver sees: Vulkan needs Venus (4), which
// the host only offers with QEMU's venus=on. Read from the driver's private adapter info (VIOGPU_ADAPTERINFO).
static void reportVirtioGpuCapsets() {
    struct AdapterInfo { UINT hAdapter; LUID luid; ULONG sources; BOOL precisePresent; };
    struct EnumAdapters2 { ULONG count; AdapterInfo* adapters; };
    struct QueryAdapterInfo { UINT hAdapter; UINT type; void* data; UINT size; };
    struct CloseAdapter { UINT hAdapter; };
    struct VioGpuAdapterInfo { ULONGLONG iam; UINT flags; ULONGLONG capsets; };
    typedef LONG(APIENTRY * PFN_enum)(EnumAdapters2*);
    typedef LONG(APIENTRY * PFN_query)(QueryAdapterInfo*);
    typedef LONG(APIENTRY * PFN_close)(CloseAdapter*);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    auto enumAdapters = reinterpret_cast<PFN_enum>(GetProcAddress(gdi, "D3DKMTEnumAdapters2"));
    auto query = reinterpret_cast<PFN_query>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    auto close = reinterpret_cast<PFN_close>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    if (!enumAdapters || !query || !close) return;
    AdapterInfo adapters[16];
    EnumAdapters2 list = {16, adapters};
    if (enumAdapters(&list) != 0) return;
    bool found = false;
    for (ULONG i = 0; i < list.count; i++) {
        VioGpuAdapterInfo info = {};
        QueryAdapterInfo q = {adapters[i].hAdapter, 0 /* KMTQAITYPE_UMDRIVERPRIVATE */, &info, sizeof(info)};
        if (!found && query(&q) == 0 && info.iam == 0x56696f475055ull /* VIOGPU_IAM */) {
            found = true;
            static const char* names[] = {"", "virgl", "virgl2", "gfxstream-vulkan", "venus", "cross-domain", "drm"};
            std::string capsets;
            for (int id = 1; id < 64; id++) {
                if (!(info.capsets & (1ull << id))) continue;
                if (!capsets.empty()) capsets += ", ";
                capsets += id < 7 ? names[id] : fmt("%d", id);
            }
            bool venus = info.capsets & (1ull << 4);
            report(venus ? Status::Info : Status::Warn, "virtio-gpu capsets",
                   capsets + (venus ? "" : " (no Venus: the host needs QEMU's venus=on for Vulkan)"));
        }
        CloseAdapter c = {adapters[i].hAdapter};
        close(&c);
    }
}

static void groupVulkan() {
    reportVirtioGpuCapsets();
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) {
        report(Status::Warn, "Vulkan loader", "vulkan-1.dll not installed, Vulkan apps won't run");
        return;
    }
    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddrT>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    typedef VkResultT(__stdcall * PFN_create)(const VkInstanceCreateInfoT*, const void*, VkInstanceT*);
    typedef void(__stdcall * PFN_destroy)(VkInstanceT, const void*);
    typedef VkResultT(__stdcall * PFN_enumerate)(VkInstanceT, uint32_t*, VkPhysicalDeviceT*);
    typedef void(__stdcall * PFN_props)(VkPhysicalDeviceT, void*);
    auto create = getProc ? reinterpret_cast<PFN_create>(getProc(nullptr, "vkCreateInstance")) : nullptr;
    if (!create) {
        report(Status::Fail, "Vulkan loader", "vkCreateInstance not found");
        return;
    }

    VkApplicationInfoT app = {0, nullptr, "winboat-gpucheck", 1, nullptr, 0, (1u << 22)};  // Vulkan 1.0
    VkInstanceCreateInfoT info = {1, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    VkInstanceT instance = nullptr;
    VkResultT result = create(&info, nullptr, &instance);
    if (result != 0) {
        report(Status::Fail, "Create Vulkan instance", fmt("VkResult %d (no Vulkan driver?)", result));
        return;
    }
    auto destroy = reinterpret_cast<PFN_destroy>(getProc(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_enumerate>(getProc(instance, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_props>(getProc(instance, "vkGetPhysicalDeviceProperties"));

    uint32_t count = 0;
    enumerate(instance, &count, nullptr);
    std::vector<VkPhysicalDeviceT> devices(count);
    if (count) enumerate(instance, &count, devices.data());
    bool anyGpu = false;
    static const char* types[] = {"other", "integrated GPU", "discrete GPU", "virtual GPU", "CPU"};
    for (auto dev : devices) {
        alignas(8) uint8_t buf[4096] = {};  // VkPhysicalDeviceProperties: apiVersion, driverVersion, vendorID, deviceID, deviceType, deviceName[256]
        props(dev, buf);
        uint32_t api, vendor, device;
        int32_t type;
        memcpy(&api, buf, 4);
        memcpy(&vendor, buf + 8, 4);
        memcpy(&device, buf + 12, 4);
        memcpy(&type, buf + 16, 4);
        const char* name = reinterpret_cast<const char*>(buf + 20);
        bool gpu = type >= 1 && type <= 3;
        anyGpu |= gpu;
        report(gpu ? Status::Pass : Status::Info, fmt("Vulkan device: %s", name),
               fmt("%s, %04X:%04X, Vulkan %u.%u.%u", type >= 0 && type <= 4 ? types[type] : "?", vendor, device,
                   api >> 22, (api >> 12) & 0x3FF, api & 0xFFF));
    }
    if (!anyGpu) report(Status::Warn, "Vulkan on a GPU", count ? "only CPU devices" : "no Vulkan devices");
    destroy(instance, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// Video decode

static void groupVideo() {
    Com<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc;
    if (!pickHardwareAdapter(&adapter, &desc)) {
        report(Status::Skip, "Hardware video decode", "no hardware adapter");
        return;
    }
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    if (FAILED(createD3D11Device(adapter, D3D_DRIVER_TYPE_UNKNOWN, &device, &ctx, &level))) {
        report(Status::Skip, "Hardware video decode", "no Direct3D 11 device");
        return;
    }
    Com<ID3D11VideoDevice> video;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&video)))) {
        report(Status::Warn, "Hardware video decode", "driver exposes no video device");
        return;
    }
    static const GUID h264 = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
    static const GUID hevc = {0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};
    UINT profiles = video->GetVideoDecoderProfileCount();
    bool hasH264 = false, hasHevc = false;
    for (UINT i = 0; i < profiles; i++) {
        GUID g;
        if (FAILED(video->GetVideoDecoderProfile(i, &g))) continue;
        hasH264 |= IsEqualGUID(g, h264);
        hasHevc |= IsEqualGUID(g, hevc);
    }
    report(profiles ? Status::Pass : Status::Warn, "Hardware video decode profiles", fmt("%u", profiles));
    report(hasH264 ? Status::Pass : Status::Warn, "H.264 decode", hasH264 ? "supported" : "not supported");
    report(hasHevc ? Status::Pass : Status::Info, "HEVC decode", hasHevc ? "supported" : "not supported");
}

// ---------------------------------------------------------------------------------------------------------------
// On-screen presentation: renders into a visible window and checks what actually reaches the screen

static bool noWindows = false;
static std::string outDir = ".";

// Saves what the screen shows over the window's client area, to tell a failed present from a failed capture
static void saveScreenshot(HWND hwnd, const char* name) {
    RECT rect;
    GetClientRect(hwnd, &rect);
    POINT origin = {0, 0};
    ClientToScreen(hwnd, &origin);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(mem, bitmap);
    BitBlt(mem, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY);
    SelectObject(mem, old);

    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;  // bottom-up, as BMP files store it
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    DWORD stride = ((width * 3) + 3) & ~3u;
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);
    GetDIBits(mem, bitmap, 0, height, pixels.data(), reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    BITMAPFILEHEADER file = {};
    file.bfType = 0x4D42;  // "BM"
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    std::string path = outDir + "\\screenshot-" + name + ".bmp";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(&file, sizeof(file), 1, f);
    fwrite(&info, sizeof(info), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
    report(Status::Info, "Screenshot of the failed check", path);
}

static bool canShowWindows() {
    if (noWindows) {
        report(Status::Skip, "On-screen tests", "disabled with --no-window");
        return false;
    }
    if (!interactiveDesktop()) {
        report(Status::Skip, "On-screen tests", "no interactive desktop, run from the console or an RDP session");
        return false;
    }
    return true;
}

static void presentD3D11(DXGI_SWAP_EFFECT effect, const char* label) {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    HRESULT h = createD3D11Device(nullptr, D3D_DRIVER_TYPE_HARDWARE, &device, &ctx, &level);
    if (FAILED(h)) {
        report(Status::Fail, std::string("Present (") + label + ")", "create device: " + hr(h));
        return;
    }
    Com<IDXGIDevice> dxgiDevice;
    Com<IDXGIAdapter> adapter;
    Com<IDXGIFactory2> factory;
    device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    const int width = 320, height = 240;
    HWND hwnd = createWindow(L"WinBoat GPU check (Direct3D 11)", width, height, true);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = (effect == DXGI_SWAP_EFFECT_FLIP_DISCARD || effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) ? 2 : 1;
    sd.SwapEffect = effect;
    Com<IDXGISwapChain1> swapChain;
    h = factory ? factory->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr, &swapChain) : E_FAIL;
    if (FAILED(h)) {
        report(Status::Fail, std::string("Present (") + label + ")", "create swap chain: " + hr(h));
        DestroyWindow(hwnd);
        return;
    }

    auto frame = [&](const float color[4]) {
        Com<ID3D11Texture2D> buffer;
        Com<ID3D11RenderTargetView> rtv;
        swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
        device->CreateRenderTargetView(buffer, nullptr, &rtv);
        ctx->OMSetRenderTargets(1, &rtv.p, nullptr);
        ctx->ClearRenderTargetView(rtv, color);
        return swapChain->Present(0, 0);
    };

    const float green[4] = {0, 1, 0, 1};
    HRESULT presentResult = S_OK;
    for (int i = 0; i < 5 && SUCCEEDED(presentResult); i++) {
        presentResult = frame(green);
        pumpMessages();
    }
    Sleep(300);
    DwmFlush();
    pumpMessages();
    COLORREF seen = screenPixelAtCenter(hwnd);
    bool ok = SUCCEEDED(presentResult) && colorMatches(GetRValue(seen), GetGValue(seen), GetBValue(seen), 0, 255, 0);
    report(ok ? Status::Pass : Status::Fail, std::string("Present (") + label + ") reaches the screen",
           fmt("Present %s, screen shows rgb(%d,%d,%d), expected green", SUCCEEDED(presentResult) ? "ok" : hr(presentResult).c_str(),
               GetRValue(seen), GetGValue(seen), GetBValue(seen)));
    if (!ok) saveScreenshot(hwnd, effect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "d3d11-flip" : "d3d11-blt");

    int frames = 0;
    auto start = std::chrono::steady_clock::now();
    const float colors[2][4] = {{1, 0, 1, 1}, {0, 1, 1, 1}};
    while (secondsSince(start) < PERF_SECONDS && SUCCEEDED(frame(colors[frames % 2]))) {
        frames++;
        pumpMessages();
    }
    report(Status::Info, std::string("Present rate (") + label + ")", fmt("%.1f frames/s", frames / secondsSince(start)));
    swapChain.reset();
    DestroyWindow(hwnd);
}

static void groupPresentD3D11() {
    if (!canShowWindows()) return;
    presentD3D11(DXGI_SWAP_EFFECT_FLIP_DISCARD, "flip model");
    presentD3D11(DXGI_SWAP_EFFECT_DISCARD, "blt model");
}

static void groupPresentGL() {
    if (!canShowWindows()) return;
    GLContext gl;
    std::string error;
    if (!gl.create(320, true, error)) {
        report(Status::Fail, "OpenGL SwapBuffers", error);
        return;
    }
    auto swapInterval = reinterpret_cast<PFN_wglSwapIntervalEXT>(wglGetProcAddress("wglSwapIntervalEXT"));
    if (swapInterval) swapInterval(0);

    // Green top half, red bottom half (GL's y axis points up), so an upside-down present is caught too
    glViewport(0, 0, 320, 320);
    for (int i = 0; i < 5; i++) {
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 320, 160);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        SwapBuffers(gl.dc);
        pumpMessages();
    }
    Sleep(300);
    DwmFlush();
    pumpMessages();
    COLORREF top = screenPixelAt(gl.hwnd, 0.5, 0.25), bottom = screenPixelAt(gl.hwnd, 0.5, 0.75);
    bool topOk = colorMatches(GetRValue(top), GetGValue(top), GetBValue(top), 0, 255, 0);
    bool bottomOk = colorMatches(GetRValue(bottom), GetGValue(bottom), GetBValue(bottom), 255, 0, 0);
    bool flipped = colorMatches(GetRValue(top), GetGValue(top), GetBValue(top), 255, 0, 0) &&
                   colorMatches(GetRValue(bottom), GetGValue(bottom), GetBValue(bottom), 0, 255, 0);
    report(topOk && bottomOk ? Status::Pass : Status::Fail, "OpenGL SwapBuffers reaches the screen",
           fmt("screen shows rgb(%d,%d,%d) on top and rgb(%d,%d,%d) below, expected green over red%s", GetRValue(top),
               GetGValue(top), GetBValue(top), GetRValue(bottom), GetGValue(bottom), GetBValue(bottom),
               flipped ? " (upside down)" : ""));
    if (!(topOk && bottomOk)) saveScreenshot(gl.hwnd, "opengl");

    int frames = 0;
    auto start = std::chrono::steady_clock::now();
    while (secondsSince(start) < PERF_SECONDS) {
        glClearColor(frames % 2, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(gl.dc);
        frames++;
        pumpMessages();
    }
    report(Status::Info, "OpenGL SwapBuffers rate",
           fmt("%.1f frames/s%s", frames / secondsSince(start), swapInterval ? ", vsync off" : ", no swap interval control"));
}

static void groupPresentD3D9() {
    if (!canShowWindows()) return;
    Com<IDirect3D9> d3d;
    d3d.p = Direct3DCreate9(D3D_SDK_VERSION);
    const int width = 320, height = 240;
    HWND hwnd = createWindow(L"WinBoat GPU check (Direct3D 9)", width, height, true);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    Com<IDirect3DDevice9> device;
    HRESULT h = d3d ? d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device)
                    : E_FAIL;
    if (FAILED(h)) {
        report(Status::Fail, "Present", "create device: " + hr(h));
        DestroyWindow(hwnd);
        return;
    }
    HRESULT presentResult = S_OK;
    for (int i = 0; i < 5 && SUCCEEDED(presentResult); i++) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0);
        presentResult = device->Present(nullptr, nullptr, nullptr, nullptr);
        pumpMessages();
    }
    Sleep(300);
    DwmFlush();
    pumpMessages();
    COLORREF seen = screenPixelAtCenter(hwnd);
    bool ok = SUCCEEDED(presentResult) && colorMatches(GetRValue(seen), GetGValue(seen), GetBValue(seen), 0, 255, 0);
    report(ok ? Status::Pass : Status::Fail, "Present reaches the screen",
           fmt("Present %s, screen shows rgb(%d,%d,%d), expected green",
               SUCCEEDED(presentResult) ? "ok" : hr(presentResult).c_str(), GetRValue(seen), GetGValue(seen),
               GetBValue(seen)));
    if (!ok) saveScreenshot(hwnd, "d3d9");

    int frames = 0;
    auto start = std::chrono::steady_clock::now();
    while (secondsSince(start) < PERF_SECONDS) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, frames % 2 ? D3DCOLOR_XRGB(255, 0, 255) : D3DCOLOR_XRGB(0, 255, 255),
                      1.0f, 0);
        if (FAILED(device->Present(nullptr, nullptr, nullptr, nullptr))) break;
        frames++;
        pumpMessages();
    }
    report(Status::Info, "Present rate", fmt("%.1f frames/s", frames / secondsSince(start)));
    device.reset();
    DestroyWindow(hwnd);
}

// ---------------------------------------------------------------------------------------------------------------
// Parent: runs every group in a child process and writes the checklist

struct Group {
    const char* id;
    const char* title;
    void (*run)();
};

static const Group GROUPS[] = {
    {"env", "Environment", groupEnvironment},
    {"pnp", "Display devices (Device Manager)", groupPnp},
    {"dxgi", "DXGI adapters", groupDxgi},
    {"d3d11", "Direct3D 11 on the GPU", groupD3D11},
    {"d3d11-default", "Direct3D 11 default adapter (what apps get)", groupD3D11Default},
    {"warp", "Software baseline (WARP)", groupWarp},
    {"d3d9", "Direct3D 9", groupD3D9},
    {"d3d12", "Direct3D 12", groupD3D12},
    {"opengl", "OpenGL", groupOpenGL},
    {"vulkan", "Vulkan", groupVulkan},
    {"video", "Video decode (DXVA)", groupVideo},
    {"present-d3d11", "On-screen presentation: Direct3D 11", groupPresentD3D11},
    {"present-gl", "On-screen presentation: OpenGL", groupPresentGL},
    {"present-d3d9", "On-screen presentation: Direct3D 9", groupPresentD3D9},
};

struct Result {
    std::string group, status, name, detail;
};

static std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0, pos;
    while ((pos = line.find('\t', start)) != std::string::npos) {
        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    parts.push_back(line.substr(start));
    return parts;
}

// Drivers (Mesa among them) log through OutputDebugString, which nobody sees without a debugger attached. The parent
// listens on the DBWIN buffer, like DebugView does, and adds each check process's messages to its group.
struct DebugMessages {
    CRITICAL_SECTION lock;
    std::vector<std::pair<DWORD, std::string>> messages;
    HANDLE bufferReady = nullptr, dataReady = nullptr;
    const char* view = nullptr;  // the writer's process id, then the text

    void start() {
        InitializeCriticalSection(&lock);
        bufferReady = CreateEventA(nullptr, FALSE, TRUE, "DBWIN_BUFFER_READY");
        dataReady = CreateEventA(nullptr, FALSE, FALSE, "DBWIN_DATA_READY");
        HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, "DBWIN_BUFFER");
        if (!bufferReady || !dataReady || !mapping) return;
        view = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 4096));
        if (view) CloseHandle(CreateThread(nullptr, 0, listen, this, 0, nullptr));
    }

    static DWORD WINAPI listen(void* param) {
        auto* self = static_cast<DebugMessages*>(param);
        while (WaitForSingleObject(self->dataReady, INFINITE) == WAIT_OBJECT_0) {
            DWORD pid = *reinterpret_cast<const DWORD*>(self->view);
            const char* text = self->view + sizeof(DWORD);
            std::string message(text, strnlen(text, 4096 - sizeof(DWORD)));
            EnterCriticalSection(&self->lock);
            self->messages.emplace_back(pid, message);
            LeaveCriticalSection(&self->lock);
            SetEvent(self->bufferReady);
        }
        return 0;
    }

    std::vector<std::string> take(DWORD pid) {
        std::vector<std::string> taken;
        if (!view) return taken;
        Sleep(100);  // the last messages can arrive just after the process exits
        EnterCriticalSection(&lock);
        for (auto it = messages.begin(); it != messages.end();) {
            if (it->first == pid) {
                taken.push_back(it->second);
                it = messages.erase(it);
            } else {
                ++it;
            }
        }
        LeaveCriticalSection(&lock);
        return taken;
    }
};

static DebugMessages debugMessages;

// Runs one group in a child process; returns its result lines (plus a failure if it crashed or hung)
static void runGroup(const Group& group, DWORD timeoutMs, std::vector<Result>& results,
                     std::vector<std::pair<std::string, double>>& metrics) {
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string cmd = fmt("\"%s\" --group %s --out \"%s\"%s", self, group.id, outDir.c_str(),
                          noWindows ? " --no-window" : "");

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe, writePipe;
    CreatePipe(&readPipe, &writePipe, &sa, 0);
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        results.push_back({group.id, "FAIL", "Start check process", fmt("error %lu", GetLastError())});
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return;
    }
    CloseHandle(writePipe);

    std::string output;
    auto drain = [&] {
        DWORD available = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buf[4096];
            DWORD read = 0;
            if (!ReadFile(readPipe, buf, std::min<DWORD>(available, sizeof(buf)), &read, nullptr) || !read) break;
            output.append(buf, read);
        }
    };
    auto start = GetTickCount64();
    bool timedOut = false;
    while (WaitForSingleObject(pi.hProcess, 50) == WAIT_TIMEOUT) {
        drain();
        if (GetTickCount64() - start > timeoutMs) {
            TerminateProcess(pi.hProcess, 0xDEAD);
            WaitForSingleObject(pi.hProcess, 5000);
            timedOut = true;
            break;
        }
    }
    drain();
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readPipe);

    size_t before = results.size();
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto parts = splitTabs(line);
        if (parts.size() >= 4 && parts[0] == "@@R") {
            results.push_back({group.id, parts[1], parts[2], parts[3]});
        } else if (parts.size() >= 3 && parts[0] == "@@M") {
            metrics.emplace_back(parts[1], atof(parts[2].c_str()));
        }
        pos = end + 1;
    }
    size_t produced = results.size() - before;

    std::string last;
    int shown = 0;
    for (std::string message : debugMessages.take(pi.dwProcessId)) {
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.pop_back();
        // Skip the D3D10 user-mode driver's resource creation trace (CreateResource(...) plus indented fields)
        if (message.empty() || message == last || message.rfind("CreateResource(", 0) == 0 ||
            message.rfind("  ", 0) == 0) {
            continue;
        }
        last = message;
        if (++shown > 40) {
            results.push_back({group.id, "INFO", "Driver messages", "more messages left out"});
            break;
        }
        if (message.size() > 300) message = message.substr(0, 300) + "...";
        results.push_back({group.id, "INFO", "Driver message", message});
    }

    if (timedOut) {
        results.push_back({group.id, "FAIL", "Checks finished in time",
                           fmt("hung, killed after %lu s (after %zu checks)", timeoutMs / 1000, produced)});
    } else if (exitCode != 0) {
        results.push_back({group.id, "FAIL", "Checks ran without crashing",
                           fmt("process crashed with 0x%08lX after %zu checks", exitCode, produced)});
    }
}

static const char* statusIcon(const std::string& s) {
    if (s == "PASS") return "\xE2\x9C\x85";          // white heavy check mark
    if (s == "FAIL") return "\xE2\x9D\x8C";          // cross mark
    if (s == "WARN") return "\xE2\x9A\xA0\xEF\xB8\x8F";  // warning sign
    if (s == "SKIP") return "\xE2\x8F\xAD\xEF\xB8\x8F";  // next track
    return "\xE2\x84\xB9\xEF\xB8\x8F";                   // information
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += fmt("\\u%04x", c);
        } else {
            out += c;
        }
    }
    return out;
}

// Written after every group, so a crash, a kill or an RDP session ending mid-run still leaves the results so far
static int writeReport(const std::string& outDir, const std::vector<Result>& results,
                       const std::vector<std::pair<std::string, double>>& metrics, size_t groupsDone) {
    bool complete = groupsDone == ARRAYSIZE(GROUPS);
    int counts[5] = {};
    for (auto& r : results) {
        const char* names[] = {"PASS", "WARN", "FAIL", "INFO", "SKIP"};
        for (int i = 0; i < 5; i++) {
            if (r.status == names[i]) counts[i]++;
        }
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    std::string md = fmt("# WinBoat GPU check%s\n\n%04d-%02d-%02d %02d:%02d, %s session\n\n", sizeof(void*) == 8 ? "" : " (32-bit)", now.wYear, now.wMonth,
                         now.wDay, now.wHour, now.wMinute, GetSystemMetrics(SM_REMOTESESSION) ? "Remote Desktop" : "console");
    md += fmt("**%d passed, %d warnings, %d failed** (%d info, %d skipped)\n", counts[0], counts[1], counts[2],
              counts[3], counts[4]);
    if (!complete) {
        md += fmt("\n**Incomplete:** %zu of %zu groups done, stopped while running \"%s\"\n", groupsDone,
                  ARRAYSIZE(GROUPS), GROUPS[groupsDone].title);
    }

    double gpuFps = 0, warpFps = 0, glFps = 0, d3d9Fps = 0;
    for (auto& m : metrics) {
        if (m.first == "gpu_fps") gpuFps = m.second;
        if (m.first == "warp_fps") warpFps = m.second;
        if (m.first == "d3d9_fps") d3d9Fps = m.second;
        if (m.first == "gl_fps") glFps = m.second;
    }
    if (gpuFps > 0 && warpFps > 0) {
        md += fmt("\nDirect3D 11 on the GPU is **%.1fx** the speed of software rendering (WARP)", gpuFps / warpFps);
        if (glFps > 0) md += fmt("; OpenGL is %.1fx", glFps / warpFps);
        if (d3d9Fps > 0) md += fmt("; Direct3D 9 is %.1fx", d3d9Fps / warpFps);
        md += ".\n";
    }

    for (const auto& group : GROUPS) {
        md += fmt("\n## %s\n\n", group.title);
        for (auto& r : results) {
            if (r.group != group.id) continue;
            md += fmt("- %s **%s**", statusIcon(r.status), r.name.c_str());
            if (!r.detail.empty()) md += ": " + r.detail;
            md += "\n";
        }
    }

    std::string json = "{\n  \"results\": [\n";
    for (size_t i = 0; i < results.size(); i++) {
        auto& r = results[i];
        json += fmt("    {\"group\": \"%s\", \"status\": \"%s\", \"name\": \"%s\", \"detail\": \"%s\"}%s\n",
                    jsonEscape(r.group).c_str(), r.status.c_str(), jsonEscape(r.name).c_str(),
                    jsonEscape(r.detail).c_str(), i + 1 < results.size() ? "," : "");
    }
    json += fmt("  ],\n  \"complete\": %s,\n  \"metrics\": {", complete ? "true" : "false");
    for (size_t i = 0; i < metrics.size(); i++) {
        json += fmt("%s\"%s\": %.3f", i ? ", " : "", jsonEscape(metrics[i].first).c_str(), metrics[i].second);
    }
    json += "}\n}\n";

    auto write = [](const std::string& path, const std::string& content) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        fwrite(content.data(), 1, content.size(), f);
        fclose(f);
        return true;
    };
    CreateDirectoryA(outDir.c_str(), nullptr);
    std::string mdPath = outDir + "\\gpucheck-report.md", jsonPath = outDir + "\\gpucheck-report.json";
    bool written = write(mdPath, md) && write(jsonPath, json);
    if (!written) {
        // e.g. a mangled --out path from an RDP RemoteApp command line: fall back to the exe's folder
        char self[MAX_PATH];
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string exeDir(self);
        exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
        fprintf(stderr, "Could not write to %s, using %s\n", outDir.c_str(), exeDir.c_str());
        mdPath = exeDir + "\\gpucheck-report.md";
        jsonPath = exeDir + "\\gpucheck-report.json";
        written = write(mdPath, md) && write(jsonPath, json);
    }

    if (complete) {
        fwrite(md.data(), 1, md.size(), stdout);
        if (written) fprintf(stderr, "\nReport written to %s and %s\n", mdPath.c_str(), jsonPath.c_str());
    }
    return counts[2];
}

// only: comma-separated group ids to run (empty: all)
static int runAll(const std::string& outDir, DWORD timeoutMs, const std::string& only) {
    debugMessages.start();
    std::vector<Result> results;
    std::vector<std::pair<std::string, double>> metrics;
    for (size_t i = 0; i < ARRAYSIZE(GROUPS); i++) {
        writeReport(outDir, results, metrics, i);
        if (!only.empty() && ("," + only + ",").find("," + std::string(GROUPS[i].id) + ",") == std::string::npos) {
            continue;
        }
        fprintf(stderr, "Running %s...\n", GROUPS[i].title);
        runGroup(GROUPS[i], timeoutMs, results, metrics);
    }
    return writeReport(outDir, results, metrics, ARRAYSIZE(GROUPS));
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // Don't let a crashing driver open error dialogs that would hang the child process
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    std::string group, only;
    DWORD timeoutMs = 90 * 1000;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--group" && i + 1 < argc) {
            group = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeoutMs = static_cast<DWORD>(atoi(argv[++i])) * 1000;
        } else if (arg == "--only" && i + 1 < argc) {
            // '+' works too: FreeRDP's /app option splits on commas
            only = argv[++i];
            for (char& ch : only) {
                if (ch == '+') ch = ',';
            }
        } else if (arg == "--no-window") {
            noWindows = true;
        } else {
            fprintf(stderr, "Usage: %s [--out DIR] [--timeout SECONDS] [--no-window] [--only GROUP,...] [--group NAME]\nGroups:", argv[0]);
            for (const auto& g : GROUPS) fprintf(stderr, " %s", g.id);
            fprintf(stderr, "\n");
            return 2;
        }
    }

    if (group.empty()) return runAll(outDir, timeoutMs, only);

    for (const auto& g : GROUPS) {
        if (group == g.id) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            g.run();
            fflush(stdout);
            // Skip driver teardown: a crash while unloading would hide results that were already printed
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
    fprintf(stderr, "Unknown group: %s\n", group.c_str());
    return 2;
}
