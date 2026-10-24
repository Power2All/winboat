// WinBoat GPU benchmark: throughput of the things programs lean on, per API, on the default GPU (in WinBoat the
// virtual GPU). One line per result: "<api> <test>: <value> <unit>". Each test runs about --seconds (default 3) and
// waits for the GPU at its end, so queued work counts. Comparing runs before and after a driver change shows what it
// gained or cost; --warp runs Direct3D 11 on WARP (Microsoft's CPU renderer) for scale.
//
//   gpubench.exe [--seconds N] [--only d3d11|gl] [--warp]
//
// Direct3D 11: draw calls (a constant buffer update and a 3-vertex draw each, 1000 a frame), fill rate (20 full
// 1920x1080 quads a frame), texture upload (UpdateSubresource of 1920x1080 RGBA), dynamic buffer writes
// (Map WRITE_DISCARD of 4 MB), readback (CopyResource to a staging texture and Map, 1920x1080), compute (dispatches
// of 64x64 thread groups doing 256 multiply-adds a thread), present (a 1920x1080 flip-model swap chain, sync
// interval 0). OpenGL (WGL, core 3.3+): draw calls, fill rate, glTexSubImage2D upload, glReadPixels readback.
// Direct3D 12 and Vulkan presents: presentbench.exe and vkpresentbench.exe next to this.
//
// Build: tools/gpu-bench/build.sh (MinGW-w64)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <GL/gl.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static double g_seconds = 3;
static bool g_warp = false;
static int g_drawsPerFlush = 1000; // GB_DRAWS_PER_FLUSH: how many draws between flushes in the draw call tests

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

static void result(const char *api, const char *test, double value, const char *unit)
{
   printf("%s %s: %.1f %s\n", api, test, value, unit);
   fflush(stdout);
}

static HWND makeWindow(int w, int h, bool visible)
{
   WNDCLASSA wc = {};
   wc.lpfnWndProc = DefWindowProcA;
   wc.hInstance = GetModuleHandleA(nullptr);
   wc.lpszClassName = "gpubench";
   RegisterClassA(&wc);
   HWND wnd = CreateWindowA("gpubench", "gpubench", WS_POPUP, 0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
   if (visible) ShowWindow(wnd, SW_SHOW);
   pump();
   return wnd;
}

// ---------------------------------------------------------------------------------------------------------------
// Direct3D 11

static ID3DBlob *compile(const char *src, const char *entry, const char *profile)
{
   static auto fn = (pD3DCompile)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
   if (!fn) return nullptr;
   ID3DBlob *code = nullptr, *errors = nullptr;
   fn(src, strlen(src), "bench", nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
   if (errors) {
      fprintf(stderr, "%s\n", (const char *)errors->GetBufferPointer());
      errors->Release();
   }
   return code;
}

static const char *SHADERS = R"(
cbuffer C : register(b0) { float4 offset; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
   float2 p = float2((id << 1) & 2, id & 2);            // a triangle covering the target
   return float4(p * 2 - 1 + offset.xy * 0.001, 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target { return float4(offset.z, pos.x / 4096, pos.y / 4096, 1); }
RWStructuredBuffer<float> outBuf : register(u0);
[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID)
{
   float a = id.x, b = 1.0001;
   [loop] for (int i = 0; i < 256; i++) a = a * b + 0.5;
   outBuf[id.x & 65535] = a;
}
)";

struct D3D11 {
   ID3D11Device *dev = nullptr;
   ID3D11DeviceContext *ctx = nullptr;
   ID3D11Query *event = nullptr;
   void finish()
   {
      ctx->End(event);
      while (ctx->GetData(event, nullptr, 0, 0) == S_FALSE) {}
   }
};

static void benchD3D11()
{
   const char *api = g_warp ? "d3d11-warp" : "d3d11";
   D3D11 d;
   D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
   if (FAILED(D3D11CreateDevice(nullptr, g_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                D3D11_SDK_VERSION, &d.dev, nullptr, &d.ctx))) {
      printf("%s: no feature level 11_0 device\n", api);
      return;
   }
   D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
   d.dev->CreateQuery(&qd, &d.event);
   ID3DBlob *vsb = compile(SHADERS, "vs", "vs_5_0"), *psb = compile(SHADERS, "ps", "ps_5_0"), *csb = compile(SHADERS, "cs", "cs_5_0");
   if (!vsb || !psb || !csb) {
      printf("%s: no HLSL compiler (d3dcompiler_47.dll)\n", api);
      return;
   }
   ID3D11VertexShader *vs = nullptr;
   ID3D11PixelShader *ps = nullptr;
   ID3D11ComputeShader *cs = nullptr;
   d.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
   d.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
   d.dev->CreateComputeShader(csb->GetBufferPointer(), csb->GetBufferSize(), nullptr, &cs);

   const int W = 1920, H = 1080;
   D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
   ID3D11Texture2D *rt = nullptr;
   d.dev->CreateTexture2D(&td, nullptr, &rt);
   ID3D11RenderTargetView *rtv = nullptr;
   d.dev->CreateRenderTargetView(rt, nullptr, &rtv);
   D3D11_BUFFER_DESC cbd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
   ID3D11Buffer *cb = nullptr;
   d.dev->CreateBuffer(&cbd, nullptr, &cb);

   auto setup = [&](int w, int h) {
      D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
      d.ctx->RSSetViewports(1, &vp);
      d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
      d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      d.ctx->VSSetShader(vs, nullptr, 0);
      d.ctx->PSSetShader(ps, nullptr, 0);
      d.ctx->VSSetConstantBuffers(0, 1, &cb);
      d.ctx->PSSetConstantBuffers(0, 1, &cb);
   };

   // Draw calls: a tiny viewport, so the draws' cost is the API and driver path, not pixels. Three ways to change a
   // constant per draw: none, UpdateSubresource, a dynamic buffer mapped WRITE_DISCARD (what most games do)
   D3D11_BUFFER_DESC dcbd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
   ID3D11Buffer *dcb = nullptr;
   d.dev->CreateBuffer(&dcbd, nullptr, &dcb);
   const char *drawTests[] = {"draw calls (no state change)", "draw calls (UpdateSubresource of a constant buffer each)",
                              "draw calls (Map WRITE_DISCARD of a constant buffer each)"};
   for (int mode = 0; mode < 3; mode++) {
      setup(8, 8);
      if (mode == 2) {
         d.ctx->VSSetConstantBuffers(0, 1, &dcb);
         d.ctx->PSSetConstantBuffers(0, 1, &dcb);
      }
      long draws = 0;
      double start = now();
      while (now() - start < g_seconds) {
         for (int i = 0; i < g_drawsPerFlush; i++) {
            float c[4] = {(float)(i & 63), 0, (float)(i & 1), 0};
            if (mode == 1) d.ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
            if (mode == 2) {
               D3D11_MAPPED_SUBRESOURCE m;
               if (SUCCEEDED(d.ctx->Map(dcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                  memcpy(m.pData, c, sizeof(c));
                  d.ctx->Unmap(dcb, 0);
               }
            }
            d.ctx->Draw(3, 0);
         }
         d.ctx->Flush();
         draws += g_drawsPerFlush;
      }
      d.finish();
      result(api, drawTests[mode], draws / (now() - start) / 1000, "thousand/s");
   }
   // Fill rate
   {
      setup(W, H);
      long frames = 0;
      double start = now();
      while (now() - start < g_seconds) {
         for (int i = 0; i < 20; i++) d.ctx->Draw(3, 0);
         d.ctx->Flush();
         frames++;
         if (frames % 8 == 0) d.finish(); // (no runaway queue: the time counts the GPU's work)
      }
      d.finish();
      result(api, "fill rate (1920x1080 RGBA8)", frames * 20.0 * W * H / (now() - start) / 1e9, "Gpixel/s");
   }
   // Texture upload
   {
      std::vector<uint32_t> pixels(W * H, 0x80402010);
      long n = 0;
      double start = now();
      while (now() - start < g_seconds) {
         d.ctx->UpdateSubresource(rt, 0, nullptr, pixels.data(), W * 4, 0);
         if (++n % 4 == 0) d.finish();
      }
      d.finish();
      result(api, "texture upload (UpdateSubresource 1920x1080)", (double)n * W * H * 4 / (now() - start) / 1e6, "MB/s");
   }
   // Dynamic buffer writes
   {
      const UINT size = 4 << 20;
      D3D11_BUFFER_DESC bd = {size, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
      ID3D11Buffer *vb = nullptr;
      d.dev->CreateBuffer(&bd, nullptr, &vb);
      long n = 0;
      double start = now();
      while (now() - start < g_seconds) {
         D3D11_MAPPED_SUBRESOURCE m;
         if (SUCCEEDED(d.ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memset(m.pData, (int)n, size);
            d.ctx->Unmap(vb, 0);
         }
         n++;
      }
      d.finish();
      result(api, "dynamic buffer writes (Map WRITE_DISCARD 4 MB)", n * (double)size / (now() - start) / 1e6, "MB/s");
      vb->Release();
   }
   // Readback
   {
      D3D11_TEXTURE2D_DESC sd = td;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Texture2D *staging = nullptr;
      d.dev->CreateTexture2D(&sd, nullptr, &staging);
      long n = 0;
      volatile uint32_t sink = 0;
      double start = now();
      while (now() - start < g_seconds) {
         d.ctx->CopyResource(staging, rt);
         D3D11_MAPPED_SUBRESOURCE m;
         if (SUCCEEDED(d.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            sink += ((uint32_t *)m.pData)[n % (W * H)];
            d.ctx->Unmap(staging, 0);
         }
         n++;
      }
      double t = now() - start;
      result(api, "readback (copy + Map READ 1920x1080)", t / n * 1000, "ms each");
      (void)sink;
      result(api, "readback bandwidth", (double)n * W * H * 4 / t / 1e6, "MB/s");
      staging->Release();
   }
   // Compute
   {
      D3D11_BUFFER_DESC bd = {65536 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                              D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
      ID3D11Buffer *buf = nullptr;
      d.dev->CreateBuffer(&bd, nullptr, &buf);
      ID3D11UnorderedAccessView *uav = nullptr;
      d.dev->CreateUnorderedAccessView(buf, nullptr, &uav);
      d.ctx->CSSetShader(cs, nullptr, 0);
      d.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
      long n = 0;
      double start = now();
      while (now() - start < g_seconds) {
         d.ctx->Dispatch(64 * 64, 1, 1); // 262144 threads
         if (++n % 16 == 0) d.finish();
      }
      d.finish();
      double t = now() - start;
      result(api, "compute (multiply-adds)", n * 262144.0 * 256 * 2 / t / 1e9, "GFLOP/s");
      result(api, "compute dispatches", n / t, "/s");
      uav->Release();
      buf->Release();
   }
   // Present
   if (!g_warp) {
      HWND wnd = makeWindow(W, H, true);
      IDXGIDevice *dxgi = nullptr;
      IDXGIAdapter *adapter = nullptr;
      IDXGIFactory2 *factory = nullptr;
      d.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
      dxgi->GetAdapter(&adapter);
      adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
      DXGI_SWAP_CHAIN_DESC1 scd = {};
      scd.Width = W;
      scd.Height = H;
      scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      scd.SampleDesc.Count = 1;
      scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      scd.BufferCount = 3;
      scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      IDXGISwapChain1 *sc = nullptr;
      if (FAILED(factory->CreateSwapChainForHwnd(d.dev, wnd, &scd, nullptr, nullptr, &sc))) {
         // Windows 7: no flip model
         scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
         scd.BufferCount = 1;
         factory->CreateSwapChainForHwnd(d.dev, wnd, &scd, nullptr, nullptr, &sc);
      }
      if (sc) {
         long frames = 0;
         double inPresent = 0, start = now();
         while (now() - start < g_seconds) {
            ID3D11Texture2D *back = nullptr;
            sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
            ID3D11RenderTargetView *view = nullptr;
            d.dev->CreateRenderTargetView(back, nullptr, &view);
            float color[4] = {(frames & 255) / 255.0f, 0.3f, 0.6f, 1};
            d.ctx->ClearRenderTargetView(view, color);
            view->Release();
            back->Release();
            double p = now();
            sc->Present(0, 0);
            inPresent += now() - p;
            frames++;
            pump();
         }
         double t = now() - start;
         result(api, "present (1920x1080, sync interval 0)", frames / t, "fps");
         result(api, "time in Present", inPresent / frames * 1000, "ms each");
         sc->Release();
      }
      factory->Release();
      adapter->Release();
      dxgi->Release();
      DestroyWindow(wnd);
   }
}

// ---------------------------------------------------------------------------------------------------------------
// OpenGL (core profile 3.3: shaders, buffers, framebuffers through wglGetProcAddress)

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_UNIFORM_BUFFER 0x8A11
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define GL_RGBA8 0x8058

#define GL_FUNCS(X)                                                                                                \
   X(GLuint, glCreateShader, (GLenum t))                                                                          \
   X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar *const *src, const GLint *len))                   \
   X(void, glCompileShader, (GLuint s))                                                                           \
   X(GLuint, glCreateProgram, (void))                                                                             \
   X(void, glAttachShader, (GLuint p, GLuint s))                                                                  \
   X(void, glLinkProgram, (GLuint p))                                                                             \
   X(void, glUseProgram, (GLuint p))                                                                              \
   X(GLint, glGetUniformLocation, (GLuint p, const GLchar *n))                                                   \
   X(void, glUniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                                   \
   X(void, glGenVertexArrays, (GLsizei n, GLuint *a))                                                             \
   X(void, glBindVertexArray, (GLuint a))                                                                         \
   X(void, glGenFramebuffers, (GLsizei n, GLuint *f))                                                             \
   X(void, glBindFramebuffer, (GLenum t, GLuint f))                                                               \
   X(void, glFramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l))

#define GLB_DECLARE(ret, name, args) typedef ret(WINAPI *PFN_##name) args; static PFN_##name name;
GL_FUNCS(GLB_DECLARE)

static void benchGL()
{
   HWND wnd = makeWindow(64, 64, false);
   HDC dc = GetDC(wnd);
   PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
   SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
   HGLRC legacy = wglCreateContext(dc);
   wglMakeCurrent(dc, legacy);
   typedef HGLRC(WINAPI * PFN_create)(HDC, HGLRC, const int *);
   auto create = (PFN_create)wglGetProcAddress("wglCreateContextAttribsARB");
   const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                          WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
   HGLRC core = create ? create(dc, nullptr, attribs) : nullptr;
   if (!core) {
      printf("gl: no OpenGL 3.3 core context\n");
      return;
   }
   wglMakeCurrent(dc, core);
   wglDeleteContext(legacy);
#define GLB_LOAD(ret, name, args) name = (PFN_##name)wglGetProcAddress(#name);
   GL_FUNCS(GLB_LOAD)

   const char *vsrc = "#version 330 core\nuniform vec4 offset;\nvoid main() { vec2 p = vec2((gl_VertexID << 1) & 2, "
                      "gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0 + offset.xy * 0.001, 0, 1); }\n";
   const char *fsrc = "#version 330 core\nuniform vec4 offset;\nout vec4 color;\nvoid main() { color = vec4(offset.z, "
                      "gl_FragCoord.x / 4096.0, gl_FragCoord.y / 4096.0, 1); }\n";
   GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER), prog = glCreateProgram();
   glShaderSource(vs, 1, &vsrc, nullptr);
   glShaderSource(fs, 1, &fsrc, nullptr);
   glCompileShader(vs);
   glCompileShader(fs);
   glAttachShader(prog, vs);
   glAttachShader(prog, fs);
   glLinkProgram(prog);
   glUseProgram(prog);
   GLint offset = glGetUniformLocation(prog, "offset");
   GLuint vao;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   const int W = 1920, H = 1080;
   GLuint tex, fb;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   printf("gl: %s, %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));

   {
      glViewport(0, 0, 8, 8);
      long draws = 0;
      double start = now();
      while (now() - start < g_seconds) {
         for (int i = 0; i < 1000; i++) {
            glUniform4f(offset, (float)(i & 63), 0, (float)(i & 1), 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
         }
         glFlush();
         draws += 1000;
      }
      glFinish();
      result("gl", "draw calls (with a uniform update each)", draws / (now() - start) / 1000, "thousand/s");
   }
   {
      glViewport(0, 0, W, H);
      long frames = 0;
      double start = now();
      while (now() - start < g_seconds) {
         for (int i = 0; i < 20; i++) glDrawArrays(GL_TRIANGLES, 0, 3);
         if (++frames % 8 == 0) glFinish();
      }
      glFinish();
      result("gl", "fill rate (1920x1080 RGBA8)", frames * 20.0 * W * H / (now() - start) / 1e9, "Gpixel/s");
   }
   {
      std::vector<uint32_t> pixels(W * H, 0x80402010);
      long n = 0;
      double start = now();
      while (now() - start < g_seconds) {
         glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
         if (++n % 4 == 0) glFinish();
      }
      glFinish();
      result("gl", "texture upload (glTexSubImage2D 1920x1080)", (double)n * W * H * 4 / (now() - start) / 1e6, "MB/s");
   }
   {
      std::vector<uint32_t> pixels(W * H);
      long n = 0;
      double start = now();
      while (now() - start < g_seconds) {
         glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
         n++;
      }
      double t = now() - start;
      result("gl", "readback (glReadPixels 1920x1080)", t / n * 1000, "ms each");
   }
   wglMakeCurrent(nullptr, nullptr);
   wglDeleteContext(core);
   ReleaseDC(wnd, dc);
   DestroyWindow(wnd);
}

int main(int argc, char **argv)
{
   SetProcessDPIAware();
   if (const char *e = getenv("GB_DRAWS_PER_FLUSH")) g_drawsPerFlush = atoi(e) > 0 ? atoi(e) : 1000;
   std::string only;
   for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      if (a == "--seconds" && i + 1 < argc) g_seconds = atof(argv[++i]);
      else if (a == "--only" && i + 1 < argc) only = argv[++i];
      else if (a == "--warp") g_warp = true;
      else {
         fprintf(stderr, "usage: %s [--seconds N] [--only d3d11|gl] [--warp]\n", argv[0]);
         return 2;
      }
   }
   if (only.empty() || only == "d3d11") benchD3D11();
   if ((only.empty() || only == "gl") && !g_warp) benchGL();
   return 0;
}
