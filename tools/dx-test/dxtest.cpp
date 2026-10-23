// WinBoat DirectX test
//
// Tests the Direct3D 9, 10, 11 and 12 (and Direct2D and Vulkan) functions and features one by one on the GPU (the default hardware adapter,
// in WinBoat the virtual GPU) and prints a checklist: a check mark for every test that passed, a cross for every one
// that failed. Rendering tests read the pixels back: Direct3D 10/11/12 tests compare them with Microsoft's software
// renderer (WARP), Direct3D 9 tests (no WARP there) with the expected colors.
//
// Each group of tests runs in a child process with a timeout, so a crashing or hanging driver shows up as failed
// tests instead of taking the whole report down. The 32-bit groups run winboat-dxtest32.exe (next to this one).
//
//   winboat-dxtest.exe [--out DIR] [--timeout SECONDS] [--only GROUP,...] [--warp] [--check] [--gui] [--list]
//
//   --warp   runs everything on WARP instead of the GPU (checks the tests themselves: all should pass but D3D9)
//   --check  the GPU check installed with the driver: one quick group per API, what the GPU doesn't offer shown as
//            "not supported" rather than failed, an overview first; the report goes to %LOCALAPPDATA%\WinBoat\GPU Check
//            and opens when done
//   --gui    the GPU check in a window (gui.inc): a row per API, green when it works, red when it doesn't, grey when
//            the GPU doesn't offer it; the report a button away
//
// Results: dxtest-report.html, dxtest-report.md and dxtest-report.json in DIR (default: the current folder).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define INITGUID
// D3D12 methods returning structures (descriptor handles, resource descriptions) with the right calling convention
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <shellapi.h>
#include <GL/gl.h>
#include <d3d9.h>
#include <dxva2api.h>
#include <d3d10_1.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d2d1_3.h>
#include <d2d1effects_2.h>
#include <dwrite.h>
#include <dxgi1_6.h>
// Vulkan: vulkan-1.dll is loaded at run time (vulkan.inc)
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// ---------------------------------------------------------------------------------------------------------------
// Results (a child process prints one line per test; the parent collects them)

enum class Status { Pass, Warn, Fail, Info, Skip };

static const char *statusName(Status s)
{
   switch (s) {
   case Status::Pass: return "PASS";
   case Status::Warn: return "WARN";
   case Status::Fail: return "FAIL";
   case Status::Info: return "INFO";
   case Status::Skip: return "SKIP";
   }
   return "INFO";
}

static std::string sanitize(std::string s)
{
   for (char &c : s) {
      if (c == '\t' || c == '\r' || c == '\n') c = ' ';
   }
   return s;
}

// After a crash the group runs again from the test after the one that crashed: startTest() skips the tests until
// then, and nothing is reported twice
static std::string g_resumeAfter;
static bool g_skipping = false;

#define NO_HLSL_COMPILER "no HLSL compiler (D3DCompiler_47.dll; Windows 7: KB4019990)"

static void report(Status s, const std::string &name, const std::string &detail = "")
{
   if (g_skipping) return;
   // Windows 7 without KB4019990: the test couldn't run, the GPU didn't fail it
   if (s == Status::Fail && detail.rfind(NO_HLSL_COMPILER, 0) == 0) s = Status::Skip;
   printf("@@R\t%s\t%s\t%s\n", statusName(s), sanitize(name).c_str(), sanitize(detail).c_str());
   fflush(stdout);
}

static void check(bool pass, const std::string &name, const std::string &detail = "")
{
   report(pass ? Status::Pass : Status::Fail, name, detail);
}

// Marks the start of a test (the parent knows which one crashed); false while resuming past an earlier crash
static bool startTest(const std::string &name)
{
   if (g_skipping) {
      if (name == g_resumeAfter) g_skipping = false;
      return false;
   }
   printf("@@T\t%s\n", sanitize(name).c_str());
   fflush(stdout);
   return true;
}

// An optional feature: a warning when missing, not a failure
static void optional(bool supported, const std::string &name, const std::string &detail = "")
{
   report(supported ? Status::Pass : Status::Warn, name, supported ? detail : (detail.empty() ? "not supported (optional)" : detail));
}

// Windows 10 or later: Direct3D 12, DirectML, flip-discard swap chains and our Vulkan driver need it (before that a
// missing one is optional, not a failure)
static bool windows10OrLater()
{
   static int major = -1;
   if (major < 0) {
      typedef LONG(WINAPI * PFN_GetVersion)(OSVERSIONINFOW *);
      OSVERSIONINFOW os = {sizeof(os)};
      auto get = (PFN_GetVersion)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
      major = get && get(&os) == 0 ? (int)os.dwMajorVersion : 10;
   }
   return major >= 10;
}

static std::string fmt(const char *format, ...)
{
   char buf[2048];
   va_list args;
   va_start(args, format);
   vsnprintf(buf, sizeof buf, format, args);
   va_end(args);
   return buf;
}

static std::string hrs(HRESULT h) { return fmt("HRESULT 0x%08lX", static_cast<unsigned long>(h)); }

static std::string narrow(const wchar_t *w)
{
   char buf[512];
   WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
   return buf;
}

// Options for the child processes
static bool g_check = false;     // the GPU check (--check): what the GPU can do, for users rather than for driver work

// An API or device the GPU doesn't offer: a failure in the driver test, "not supported" in the GPU check, where it
// tells what the GPU can do rather than what's broken
static void missing(const std::string &name, const std::string &detail)
{
   report(g_check ? Status::Warn : Status::Fail, name, detail);
}

static bool g_warp = false;      // the "GPU" tests run on WARP (checks the tests)

// The highest Direct3D feature level the GPU's driver offers (Windows 7's: 10_0), asked once. (Not 11_1 in the list:
// a runtime without it, Windows 7's, refuses the whole list)
static D3D_FEATURE_LEVEL gpuFeatureLevel()
{
   static const D3D_FEATURE_LEVEL level = [] {
      const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                          D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1};
      D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
      D3D11CreateDevice(nullptr, g_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                        ARRAYSIZE(levels), D3D11_SDK_VERSION, nullptr, &got, nullptr);
      return got;
   }();
   return level;
}

// A check that needs a feature level: one the GPU's driver doesn't offer is "not supported" in the GPU check (a
// failure in the driver test); a GPU that offers it fails as always
static void checkAtLevel(D3D_FEATURE_LEVEL needed, bool pass, const std::string &name, const std::string &detail = "")
{
   D3D_FEATURE_LEVEL has = gpuFeatureLevel();
   if (!pass && has < needed) {
      missing(name, fmt("needs feature level %d_%d; the GPU's driver offers %d_%d", needed >> 12, (needed >> 8) & 0xf,
                        has >> 12, (has >> 8) & 0xf));
   } else {
      check(pass, name, detail);
   }
}

static std::string g_outDir = ".";

// DXTEST_HASH=1: each compared test's GPU and WARP output as hashes in dxtest-hashes.txt (to tell, across Windows
// versions, whether the GPU's result or WARP's changed)
static void hashOutputs(const std::string &name, const std::vector<uint32_t> &gpu, const std::vector<uint32_t> &warp)
{
   if (!getenv("DXTEST_HASH")) return;
   auto fnv = [](const std::vector<uint32_t> &v) {
      uint64_t h = 1469598103934665603ull;
      for (uint32_t x : v) h = (h ^ x) * 1099511628211ull;
      return h;
   };
   FILE *f = fopen((g_outDir + "\\dxtest-hashes.txt").c_str(), "a");
   if (!f) return;
   fprintf(f, "%s%s\tgpu=%016llx\twarp=%016llx\n", name.c_str(), sizeof(void *) == 4 ? " (32-bit)" : "",
           (unsigned long long)fnv(gpu), (unsigned long long)fnv(warp));
   fclose(f);
}

template <typename T> static void release(T *&p)
{
   if (p) {
      p->Release();
      p = nullptr;
   }
}

// ---------------------------------------------------------------------------------------------------------------
// Shared helpers

// D3DCompiler_47 looked up rather than imported: Windows 7 has it only with KB4019990 (the SDK's is Windows 8's), and
// without it the program didn't start at all
static HRESULT compileD3D(const void *src, SIZE_T size, const char *name, const char *entry, const char *profile,
                          UINT flags, ID3DBlob **code, ID3DBlob **errors)
{
   static auto compile = (pD3DCompile)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
   if (!compile) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
   return compile(src, size, name, nullptr, nullptr, entry, profile, flags, 0, code, errors);
}

static ID3DBlob *compileHlsl(const std::string &src, const char *profile, std::string *error = nullptr,
                             const char *entry = "main")
{
   ID3DBlob *code = nullptr, *errors = nullptr;
   HRESULT hr = compileD3D(src.data(), src.size(), "test", entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &errors);
   if (hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) && error) *error = NO_HLSL_COMPILER;
   if (errors) {
      if (error) *error = std::string((const char *)errors->GetBufferPointer(), errors->GetBufferSize());
      errors->Release();
   }
   if (FAILED(hr) && error && error->empty()) *error = hrs(hr);
   return SUCCEEDED(hr) ? code : nullptr;
}

static void pumpMessages()
{
   MSG msg;
   while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }
}

static HWND createWindow(const char *title, int width, int height, bool visible)
{
   static bool registered = false;
   if (!registered) {
      WNDCLASSA wc = {};
      wc.lpfnWndProc = DefWindowProcA;
      wc.hInstance = GetModuleHandleA(nullptr);
      wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
      wc.lpszClassName = "WinBoatDxTest";
      RegisterClassA(&wc);
      registered = true;
   }
   RECT r = {0, 0, width, height};
   AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
   HWND hwnd = CreateWindowA("WinBoatDxTest", title, WS_OVERLAPPEDWINDOW, 60, 60, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
   if (hwnd && visible) {
      // on top of everything: Windows throttles and doesn't capture windows that are covered
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
      SetForegroundWindow(hwnd);
      UpdateWindow(hwnd);
      pumpMessages();
   }
   return hwnd;
}

// A pixel of a window's client area as the desktop shows it (PrintWindow: DWM's copy of the window; without DWM's
// composition, Windows 7's basic theme, the screen's pixel: PrintWindow leaves out what Direct3D presented there)
static bool windowPixel(HWND hwnd, int x, int y, COLORREF &out)
{
   typedef HRESULT(WINAPI * PFN_DwmIsCompositionEnabled)(BOOL *);
   HMODULE dwmapi = LoadLibraryA("dwmapi.dll");
   auto composed = dwmapi ? (PFN_DwmIsCompositionEnabled)GetProcAddress(dwmapi, "DwmIsCompositionEnabled") : nullptr;
   BOOL composition = TRUE;
   if (composed && SUCCEEDED(composed(&composition)) && !composition) {
      POINT p = {x, y};
      ClientToScreen(hwnd, &p);
      HDC sdc = GetDC(nullptr);
      out = GetPixel(sdc, p.x, p.y);
      ReleaseDC(nullptr, sdc);
      return out != CLR_INVALID;
   }
   RECT wr;
   GetWindowRect(hwnd, &wr);
   POINT origin = {0, 0};
   ClientToScreen(hwnd, &origin);
   HDC wdc = GetDC(hwnd);
   HDC mdc = CreateCompatibleDC(wdc);
   HBITMAP bmp = CreateCompatibleBitmap(wdc, wr.right - wr.left, wr.bottom - wr.top);
   HGDIOBJ old = SelectObject(mdc, bmp);
   bool ok = PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT) != 0;
   if (ok) out = GetPixel(mdc, origin.x - wr.left + x, origin.y - wr.top + y);
   SelectObject(mdc, old);
   DeleteObject(bmp);
   DeleteDC(mdc);
   ReleaseDC(hwnd, wdc);
   return ok;
}

static bool interactiveDesktop()
{
   HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_READ);
   if (!desk) return false;
   CloseDesktop(desk);
   return true;
}

static bool near8(int a, int b, int tolerance) { return std::abs(a - b) <= tolerance; }

static uint16_t toHalf(float f)
{
   uint32_t x;
   memcpy(&x, &f, 4);
   uint32_t sign = (x >> 16) & 0x8000, exp = (x >> 23) & 0xff, mant = x & 0x7fffff;
   if (exp == 0) return (uint16_t)sign;
   int e = (int)exp - 127 + 15;
   if (e >= 31) return (uint16_t)(sign | 0x7c00);
   if (e <= 0) return (uint16_t)sign;
   return (uint16_t)(sign | (e << 10) | (mant >> 13));
}

// ---------------------------------------------------------------------------------------------------------------
// The tests

#include "system.inc"
#include "d3d9.inc"
#include "d3d9ex.inc"
#include "d3d10.inc"
#include "d3d11.inc"
#include "d3d12.inc"
#include "d2d.inc"
#include "vulkan.inc"
#include "directml.inc"
#include "opencl.inc"
#include "opengl.inc"
#include "video.inc"

// ---------------------------------------------------------------------------------------------------------------
// Parent: runs every group in a child process and writes the checklist

struct Group {
   const char *id;
   const char *title;
   void (*run)();
   bool bits32;   // run by winboat-dxtest32.exe
   bool check;    // part of the GPU check (--check: the quick groups, one per API)
};

static const Group GROUPS[] = {
   {"system", "System and adapter", groupSystem, false, true},
   {"d3d9", "Direct3D 9", groupD3D9, false, true},
   {"d3d9-32", "Direct3D 9 (32-bit programs)", groupD3D9, true, false},
   {"d3d9-ex", "Direct3D 9Ex, sharing, GDI and DXVA2 video", groupD3D9Ex, false, true},
   {"d3d9-ex-32", "Direct3D 9Ex, sharing, GDI and DXVA2 video (32-bit programs)", groupD3D9Ex, true, false},
   {"d3d10", "Direct3D 10 and 10.1", groupD3D10, false, true},
   {"d3d11-device", "Direct3D 11: devices, feature levels and optional features", groupD3D11Device, false, true},
   {"d3d11-formats", "Direct3D 11: format support (compared with WARP)", groupD3D11Formats, false, false},
   {"d3d11-render", "Direct3D 11: rendering features", groupD3D11Render, false, true},
   {"d3d11-render-32", "Direct3D 11: rendering features (32-bit programs)", groupD3D11Render, true, false},
   {"d3d11-shaders", "Direct3D 11: shader model 4 and 5 (vertex and pixel shaders)", groupD3D11Shaders, false, false},
   {"d3d11-compute", "Direct3D 11: compute shaders and UAVs", groupD3D11Compute, false, true},
   {"d3d11-tess", "Direct3D 11: tessellation (hull and domain shaders)", groupD3D11Tess, false, true},
   {"d3d11-msaa", "Direct3D 11: indirect draws, MSAA and depth", groupD3D11Msaa, false, false},
   {"d3d11-present", "Direct3D 11: presenting to a window", groupD3D11Present, false, true},
   {"d2d", "Direct2D (on the Direct3D 11 device, compared with WARP)", groupD2D, false, true},
   {"d2d-32", "Direct2D (32-bit programs)", groupD2D, true, false},
   {"d3d12", "Direct3D 12", groupD3D12, false, true},
   {"d3d12-32", "Direct3D 12 (32-bit programs)", groupD3D12, true, false},
   {"directml", "Machine learning (DirectML on Direct3D 12)", groupDirectML, false, true},
   {"opencl", "OpenCL", groupOpenCL, false, true},
   {"vulkan", "Vulkan", groupVulkan, false, true},
   {"vulkan-32", "Vulkan (32-bit programs)", groupVulkan, true, false},
   {"opengl", "OpenGL", groupOpenGL, false, true},
   {"opengl-32", "OpenGL (32-bit programs)", groupOpenGL, true, false},
   {"video", "Video decoding (Direct3D 11 / DXVA)", groupVideo, false, true},
};

struct Result {
   std::string group, status, name, detail;
};

static std::vector<std::string> splitTabs(const std::string &line)
{
   std::vector<std::string> parts;
   size_t start = 0, pos;
   while ((pos = line.find('\t', start)) != std::string::npos) {
      parts.push_back(line.substr(start, pos - start));
      start = pos + 1;
   }
   parts.push_back(line.substr(start));
   return parts;
}

static std::string selfDir()
{
   char self[MAX_PATH];
   GetModuleFileNameA(nullptr, self, MAX_PATH);
   std::string dir(self);
   return dir.substr(0, dir.find_last_of("\\/"));
}

static void runGroupOnce(const Group &group, DWORD timeoutMs, std::vector<Result> &results, const std::string &resumeAfter,
                         std::string &crashedIn);

// Runs a group; when its process crashes or hangs, the test it was in fails and the group goes on after it
static void runGroup(const Group &group, DWORD timeoutMs, std::vector<Result> &results)
{
   std::string resume;
   for (int attempt = 0; attempt < 12; attempt++) {
      std::string crashedIn;
      runGroupOnce(group, timeoutMs, results, resume, crashedIn);
      if (crashedIn.empty() || crashedIn == resume) return;
      resume = crashedIn;
   }
}

static void runGroupOnce(const Group &group, DWORD timeoutMs, std::vector<Result> &results, const std::string &resumeAfter,
                         std::string &crashedIn)
{
   std::string exe;
   if (group.bits32) {
      exe = selfDir() + "\\winboat-dxtest32.exe";
      // (or in 32\: Windows 8.1's install, where each one has its Universal CRT next to it)
      if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = selfDir() + "\\32\\winboat-dxtest32.exe";
      if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
         results.push_back({group.id, "SKIP", "32-bit tests", "winboat-dxtest32.exe is not next to this program (or in 32\\)"});
         return;
      }
   } else {
      char self[MAX_PATH];
      GetModuleFileNameA(nullptr, self, MAX_PATH);
      exe = self;
   }
   std::string cmd = fmt("\"%s\" --group %s --out \"%s\"%s%s", exe.c_str(), group.id, g_outDir.c_str(),
                         g_warp ? " --warp" : "", g_check ? " --check" : "");
   if (!resumeAfter.empty()) cmd += " --resume \"" + resumeAfter + "\"";

   SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
   HANDLE readPipe, writePipe;
   CreatePipe(&readPipe, &writePipe, &sa, 0);
   SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
   STARTUPINFOA si = {};
   si.cb = sizeof(si);
   si.dwFlags = STARTF_USESTDHANDLES;
   si.hStdOutput = writePipe;
   /* not the pipe: drivers write to stderr from their own threads (vkd3d-proton's info lines), which split a result
    * line now and then and dropped that test from the report */
   HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
   si.hStdError = nul;
   si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
   PROCESS_INFORMATION pi = {};
   if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      results.push_back({group.id, "FAIL", "Start the test process", fmt("error %lu", GetLastError())});
      CloseHandle(readPipe);
      CloseHandle(writePipe);
      CloseHandle(nul);
      return;
   }
   CloseHandle(writePipe);
   CloseHandle(nul);

   std::string output;
   auto drain = [&] {
      DWORD available = 0;
      while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available) {
         char buf[4096];
         DWORD got = 0;
         if (!ReadFile(readPipe, buf, std::min<DWORD>(available, sizeof(buf)), &got, nullptr) || !got) break;
         output.append(buf, got);
      }
   };
   ULONGLONG start = GetTickCount64();
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
   std::string current;   // the test that started last
   while (pos < output.size()) {
      size_t end = output.find('\n', pos);
      if (end == std::string::npos) end = output.size();
      std::string line = output.substr(pos, end - pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      auto parts = splitTabs(line);
      if (parts.size() >= 4 && parts[0] == "@@R") {
         results.push_back({group.id, parts[1], parts[2], parts[3]});
      } else if (parts.size() >= 2 && parts[0] == "@@T") {
         current = parts[1];
      }
      pos = end + 1;
   }
   size_t produced = results.size() - before;
   // the test that was running, unless it reported already
   bool reported = !current.empty() && results.size() > before && results.back().name == current;
   std::string where = current.empty() || reported ? "" : current;
   if (timedOut) {
      results.push_back({group.id, "FAIL", where.empty() ? "Tests finished in time" : where,
                         fmt("hung: the test process was stopped after %lu s", timeoutMs / 1000)});
   } else if (exitCode != 0) {
      results.push_back({group.id, "FAIL", where.empty() ? "Tests ran without crashing" : where,
                         fmt("crashed (0x%08lX), the group went on after it", exitCode)});
   }
   if ((timedOut || exitCode != 0) && !current.empty()) crashedIn = current;
   (void)produced;
}

static const char *statusIcon(const std::string &s)
{
   if (s == "PASS") return "\xE2\x9C\x85";               // check mark
   if (s == "FAIL") return "\xE2\x9D\x8C";               // cross
   if (s == "WARN") return "\xE2\x9A\xA0\xEF\xB8\x8F";   // warning: an optional feature is missing
   if (s == "SKIP") return "\xE2\x8F\xAD\xEF\xB8\x8F";   // skipped: couldn't be tested
   return "\xE2\x84\xB9\xEF\xB8\x8F";                    // information
}

static std::string htmlEscape(const std::string &s)
{
   std::string out;
   for (char c : s) {
      if (c == '<') out += "&lt;";
      else if (c == '>') out += "&gt;";
      else if (c == '&') out += "&amp;";
      else if (c == '"') out += "&quot;";
      else out += c;
   }
   return out;
}

static std::string jsonEscape(const std::string &s)
{
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

static bool writeFile(const std::string &path, const std::string &content)
{
   FILE *f = fopen(path.c_str(), "wb");
   if (!f) return false;
   fwrite(content.data(), 1, content.size(), f);
   fclose(f);
   return true;
}

// A group's results counted: PASS, WARN, FAIL, INFO, SKIP (every group's with nullptr)
static std::vector<int> countResults(const std::vector<Result> &results, const char *group)
{
   int c[5] = {};
   const char *names[] = {"PASS", "WARN", "FAIL", "INFO", "SKIP"};
   for (auto &r : results) {
      if (group && r.group != group) continue;
      for (int i = 0; i < 5; i++) {
         if (r.status == names[i]) c[i]++;
      }
   }
   return std::vector<int>(c, c + 5);
}

// The overview's verdict on a finished group: works, works without some optional features, problems, not supported
enum class Verdict { Works, Optional, Problems, Unsupported };

static Verdict verdict(const std::vector<int> &c)
{
   if (c[2]) return Verdict::Problems;
   if (!c[0] && (c[1] || c[4])) return Verdict::Unsupported;
   return c[1] ? Verdict::Optional : Verdict::Works;
}

static std::string verdictText(const std::vector<int> &c)
{
   switch (verdict(c)) {
   case Verdict::Problems: return fmt("problems: %d of %d checks failed", c[2], c[0] + c[1] + c[2]);
   case Verdict::Unsupported: return "not supported by this GPU";
   case Verdict::Optional: return fmt("works (%d optional feature%s not supported)", c[1], c[1] == 1 ? "" : "s");
   default: return "works";
   }
}

// The GPU check's window (gui.inc): told of the results after every group, and of the report when done
static void (*g_onProgress)(const std::vector<Result> &results, const std::vector<const Group *> &groups, size_t done,
                            const std::string &report) = nullptr;

// Written after every group, so a crash or a kill mid-run still leaves the results so far
static int writeReport(const std::vector<Result> &results, const std::vector<const Group *> &groups, size_t done)
{
   bool complete = done == groups.size();
   SYSTEMTIME now;
   GetLocalTime(&now);
   std::string when = fmt("%04d-%02d-%02d %02d:%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);

   auto counts = [&](const char *group) { return countResults(results, group); };
   auto total = counts(nullptr);
   std::string summary = fmt("%d passed, %d failed, %d optional features missing, %d skipped", total[0], total[2],
                             total[1], total[4]);
   std::string mode = g_warp ? " (on WARP: checking the tests)" : "";
   const char *title = g_check ? "WinBoat GPU Check" : "WinBoat DirectX test";

   // At a glance: one line per group, for the GPU check's readers
   std::string glanceMd, glanceHtml;
   for (size_t g = 0; g < groups.size(); g++) {
      auto c = counts(groups[g]->id);
      std::string state;
      if (g >= done) state = "\xE2\x8F\xB3 not run yet";                                      // hourglass
      else if (verdict(c) == Verdict::Problems) state = "\xE2\x9D\x8C " + verdictText(c);
      else if (verdict(c) == Verdict::Unsupported) state = "\xE2\x9A\xA0\xEF\xB8\x8F " + verdictText(c);
      else state = "\xE2\x9C\x85 " + verdictText(c);
      glanceMd += fmt("| %s | %s |\n", groups[g]->title, state.c_str());
      glanceHtml += fmt("<tr><td class=\"n\">%s</td><td>%s</td></tr>", htmlEscape(groups[g]->title).c_str(),
                        htmlEscape(state).c_str());
   }

   std::string md = fmt("# %s%s\n\n%s, %s session\n\n**%s**\n", title, mode.c_str(), when.c_str(),
                        GetSystemMetrics(SM_REMOTESESSION) ? "Remote Desktop" : "console", summary.c_str());
   md += "\n\xE2\x9C\x85 passed \xC2\xB7 \xE2\x9D\x8C failed \xC2\xB7 \xE2\x9A\xA0\xEF\xB8\x8F optional feature not "
         "supported \xC2\xB7 \xE2\x8F\xAD\xEF\xB8\x8F skipped \xC2\xB7 \xE2\x84\xB9\xEF\xB8\x8F information\n";
   if (!complete) md += fmt("\n**Incomplete:** stopped while running \"%s\"\n", groups[done]->title);
   if (g_check) md += "\n| | |\n|---|---|\n" + glanceMd;

   std::string html = std::string("<!doctype html><html><head><meta charset=\"utf-8\"><title>") + title + "</title><style>"
      "body{font-family:Segoe UI,system-ui,sans-serif;margin:24px;background:#16181c;color:#e6e6e6;max-width:1100px}"
      "h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:28px 0 8px;border-bottom:1px solid #333;padding-bottom:4px}"
      ".sub{color:#9aa0a6}.counts{margin:12px 0;font-size:15px}table{border-collapse:collapse;width:100%}"
      "td{padding:3px 8px;vertical-align:top;border-bottom:1px solid #24272c}td.i{width:28px}td.n{width:44%}"
      "td.d{color:#9aa0a6;font-size:13px;word-break:break-word}.tag{font-size:13px;color:#9aa0a6;margin-left:8px}"
      "</style></head><body>";
   html += fmt("<h1>%s%s</h1><div class=\"sub\">%s, %s session</div>", title, htmlEscape(mode).c_str(),
               when.c_str(), GetSystemMetrics(SM_REMOTESESSION) ? "Remote Desktop" : "console");
   html += fmt("<div class=\"counts\">\xE2\x9C\x85 %d passed &nbsp; \xE2\x9D\x8C %d failed &nbsp; "
               "\xE2\x9A\xA0\xEF\xB8\x8F %d optional features missing &nbsp; \xE2\x8F\xAD\xEF\xB8\x8F %d skipped</div>",
               total[0], total[2], total[1], total[4]);
   if (!complete) html += fmt("<p><b>Incomplete:</b> stopped while running \"%s\"</p>", htmlEscape(groups[done]->title).c_str());
   if (g_check) {
      html += "<h2>At a glance</h2><table>" + glanceHtml + "</table>"
              "<p class=\"sub\">Not supported: the GPU (the host's, through WinBoat's virtual GPU) doesn't offer it, and "
              "programs fall back as they would on a GPU without it. The details of every check follow.</p>";
   }

   for (size_t g = 0; g < groups.size(); g++) {
      auto c = counts(groups[g]->id);
      std::string tally = fmt("%d passed, %d failed", c[0], c[2]);
      if (c[1]) tally += fmt(", %d optional missing", c[1]);
      if (c[4]) tally += fmt(", %d skipped", c[4]);
      md += fmt("\n## %s (%s)\n\n", groups[g]->title, tally.c_str());
      html += fmt("<h2>%s<span class=\"tag\">%s</span></h2><table>", htmlEscape(groups[g]->title).c_str(), tally.c_str());
      bool any = false;
      for (auto &r : results) {
         if (r.group != groups[g]->id) continue;
         any = true;
         md += fmt("- %s **%s**", statusIcon(r.status), r.name.c_str());
         if (!r.detail.empty()) md += ": " + r.detail;
         md += "\n";
         html += fmt("<tr><td class=\"i\">%s</td><td class=\"n\">%s</td><td class=\"d\">%s</td></tr>",
                     statusIcon(r.status), htmlEscape(r.name).c_str(), htmlEscape(r.detail).c_str());
      }
      if (!any) {
         md += g < done ? "- (no results)\n" : "- (not run yet)\n";
         html += fmt("<tr><td colspan=3 class=\"d\">%s</td></tr>", g < done ? "(no results)" : "(not run yet)");
      }
      html += "</table>";
   }
   html += "</body></html>\n";

   std::string json = "{\n  \"results\": [\n";
   for (size_t i = 0; i < results.size(); i++) {
      auto &r = results[i];
      json += fmt("    {\"group\": \"%s\", \"status\": \"%s\", \"name\": \"%s\", \"detail\": \"%s\"}%s\n",
                  jsonEscape(r.group).c_str(), r.status.c_str(), jsonEscape(r.name).c_str(),
                  jsonEscape(r.detail).c_str(), i + 1 < results.size() ? "," : "");
   }
   json += fmt("  ],\n  \"complete\": %s,\n  \"warp\": %s\n}\n", complete ? "true" : "false", g_warp ? "true" : "false");

   CreateDirectoryA(g_outDir.c_str(), nullptr);
   std::string base = g_outDir + (g_check ? "\\gpu-check" : "\\dxtest-report");
   if (!writeFile(base + ".md", md)) {
      base = selfDir() + (g_check ? "\\gpu-check" : "\\dxtest-report");
      writeFile(base + ".md", md);
   }
   writeFile(base + ".html", html);
   writeFile(base + ".json", json);
   if (complete) {
      fprintf(stderr, "\n%s\nReport: %s.html (and .md, .json)\n", summary.c_str(), base.c_str());
      if (g_check && !g_onProgress) ShellExecuteA(nullptr, "open", (base + ".html").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
   }
   if (g_onProgress) g_onProgress(results, groups, done, base + ".html");
   return total[2];
}

static int runAll(DWORD timeoutMs, const std::string &only)
{
   std::vector<const Group *> groups;
   for (const auto &g : GROUPS) {
      if (only.empty() ? !g_check || g.check : ("," + only + ",").find("," + std::string(g.id) + ",") != std::string::npos) {
         groups.push_back(&g);
      }
   }
   std::vector<Result> results;
   for (size_t i = 0; i < groups.size(); i++) {
      writeReport(results, groups, i);
      printf("\n== %s\n", groups[i]->title);
      fflush(stdout);
      size_t before = results.size();
      runGroup(*groups[i], timeoutMs, results);
      for (size_t r = before; r < results.size(); r++) {
         printf("%s %s%s%s\n", statusIcon(results[r].status), results[r].name.c_str(),
                results[r].detail.empty() ? "" : ": ", results[r].detail.c_str());
      }
      fflush(stdout);
   }
   return writeReport(results, groups, groups.size());
}

#include "gui.inc"

int main(int argc, char **argv)
{
   SetConsoleOutputCP(CP_UTF8);
   // Real pixels: a DPI-unaware program's windows are scaled by Windows (and read back scaled)
   typedef BOOL(WINAPI * PFN_SetDpiContext)(HANDLE);
   auto setDpi = (PFN_SetDpiContext)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
   if (!setDpi || !setDpi((HANDLE)-4 /* per monitor v2 */)) SetProcessDPIAware();
   // A crashing driver mustn't open error dialogs that would hang a test process
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

   std::string group, only;
   DWORD timeoutMs = 180 * 1000;
   bool gui = false;
   for (int i = 1; i < argc; i++) {
      std::string arg = argv[i];
      if (arg == "--group" && i + 1 < argc) {
         group = argv[++i];
      } else if (arg == "--out" && i + 1 < argc) {
         g_outDir = argv[++i];
      } else if (arg == "--timeout" && i + 1 < argc) {
         timeoutMs = static_cast<DWORD>(atoi(argv[++i])) * 1000;
      } else if (arg == "--only" && i + 1 < argc) {
         only = argv[++i];
         for (char &ch : only) {
            if (ch == '+') ch = ',';
         }
      } else if (arg == "--resume" && i + 1 < argc) {
         g_resumeAfter = argv[++i];
         g_skipping = true;
      } else if (arg == "--warp") {
         g_warp = true;
      } else if (arg == "--check") {
         g_check = true;
      } else if (arg == "--gui") {
         g_check = gui = true;
      } else if (arg == "--list") {
         for (const auto &g : GROUPS) printf("%-16s %s\n", g.id, g.title);
         return 0;
      } else {
         fprintf(stderr, "Usage: %s [--out DIR] [--timeout SECONDS] [--only GROUP,...] [--warp] [--check] [--gui] [--list]\n", argv[0]);
         return 2;
      }
   }

   if (group.empty()) {
      // The GPU check's report goes to the user's own folder (it runs from Program Files)
      if (g_check && g_outDir == ".") {
         char local[MAX_PATH] = "";
         GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
         std::string dir = std::string(local) + "\\WinBoat";
         CreateDirectoryA(dir.c_str(), nullptr);
         g_outDir = dir + "\\GPU Check";
      }
      return gui ? runGui(timeoutMs) : runAll(timeoutMs, only);
   }

   for (const auto &g : GROUPS) {
      if (group == g.id) {
         CoInitializeEx(nullptr, COINIT_MULTITHREADED);
         g.run();
         fflush(stdout);
         // Skip the driver's teardown: a crash while unloading would hide results already printed
         TerminateProcess(GetCurrentProcess(), 0);
      }
   }
   fprintf(stderr, "Unknown group: %s\n", group.c_str());
   return 2;
}
