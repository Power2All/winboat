// System and adapter: what the tests run on

typedef LONG(WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW *);

static void groupSystem()
{
   OSVERSIONINFOEXW os = {sizeof(os)};
   auto rtlGetVersion = (PFN_RtlGetVersion)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
   if (rtlGetVersion) rtlGetVersion(&os);
   report(Status::Info, "Windows", fmt("%lu.%lu build %lu", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber));
   report(Status::Info, "Session", GetSystemMetrics(SM_REMOTESESSION) ? "Remote Desktop (RemoteApp or desktop)" : "console");
   DEVMODEA dm = {};
   dm.dmSize = sizeof dm;
   if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
      HDC screen = GetDC(nullptr);
      int dpi = GetDeviceCaps(screen, LOGPIXELSX);
      ReleaseDC(nullptr, screen);
      report(Status::Info, "Display", fmt("%lux%lu at %lu Hz, %d%% scale", dm.dmPelsWidth, dm.dmPelsHeight,
                                           dm.dmDisplayFrequency, dpi * 100 / 96));
   }
   if (g_warp) report(Status::Info, "Mode", "--warp: the GPU tests run on WARP (software), to check the tests");

   IDXGIFactory1 *factory = nullptr;
   if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) {
      check(false, "DXGI factory");
      return;
   }
   bool hardware = false;
   IDXGIAdapter1 *adapter;
   for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
      DXGI_ADAPTER_DESC1 desc;
      adapter->GetDesc1(&desc);
      bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c);
      LARGE_INTEGER umd = {};
      std::string version = "no Direct3D 10/11 driver";
      if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
         version = fmt("driver %u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                       LOWORD(umd.LowPart));
      }
      report(Status::Info, fmt("Adapter %u", i),
             fmt("%s (vendor 0x%04x, device 0x%04x)%s, %s, %llu MB dedicated video memory, %llu MB shared",
                 narrow(desc.Description).c_str(), desc.VendorId, desc.DeviceId, software ? ", software" : "",
                 version.c_str(), (unsigned long long)desc.DedicatedVideoMemory >> 20,
                 (unsigned long long)desc.SharedSystemMemory >> 20));
      if (!software) hardware = true;
      adapter->Release();
   }
   factory->Release();
   check(hardware, "A hardware (GPU) adapter is present", hardware ? "" : "only software rendering available");
}
