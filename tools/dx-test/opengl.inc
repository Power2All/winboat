// OpenGL: the context Windows' opengl32.dll gets from the GPU's driver (an ICD), its version, rendering and compute
// read back from a framebuffer object. "GDI Generic" is Windows' own software OpenGL 1.1: no GPU OpenGL.

typedef HGLRC(WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPUTE_SHADER 0x91B9
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_DYNAMIC_COPY 0x88EA
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_PERSISTENT_BIT 0x0040
#define GL_MAP_COHERENT_BIT 0x0080
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_ALREADY_SIGNALED 0x911A
#define GL_CONDITION_SATISFIED 0x911C
typedef struct __GLsync *GLsync;
typedef void(WINAPI *PFN_glBufferStorage)(GLenum, GLsizeiptr, const void *, GLbitfield);
typedef GLsync(WINAPI *PFN_glFenceSync)(GLenum, GLbitfield);
typedef GLenum(WINAPI *PFN_glClientWaitSync)(GLsync, GLbitfield, uint64_t);
typedef void(WINAPI *PFN_glDeleteSync)(GLsync);

#define GL_FUNCS(X)                                                                                                   \
   X(glGenFramebuffers, void, (GLsizei, GLuint *)) X(glBindFramebuffer, void, (GLenum, GLuint))                       \
   X(glGenRenderbuffers, void, (GLsizei, GLuint *)) X(glBindRenderbuffer, void, (GLenum, GLuint))                     \
   X(glRenderbufferStorage, void, (GLenum, GLenum, GLsizei, GLsizei))                                                 \
   X(glFramebufferRenderbuffer, void, (GLenum, GLenum, GLenum, GLuint)) X(glCheckFramebufferStatus, GLenum, (GLenum)) \
   X(glCreateShader, GLuint, (GLenum)) X(glShaderSource, void, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
   X(glCompileShader, void, (GLuint)) X(glGetShaderiv, void, (GLuint, GLenum, GLint *))                               \
   X(glCreateProgram, GLuint, (void)) X(glAttachShader, void, (GLuint, GLuint)) X(glLinkProgram, void, (GLuint))       \
   X(glGetProgramiv, void, (GLuint, GLenum, GLint *)) X(glUseProgram, void, (GLuint))                                 \
   X(glGenVertexArrays, void, (GLsizei, GLuint *)) X(glBindVertexArray, void, (GLuint))                               \
   X(glGenBuffers, void, (GLsizei, GLuint *)) X(glBindBuffer, void, (GLenum, GLuint))                                 \
   X(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum)) X(glBindBufferBase, void, (GLenum, GLuint, GLuint)) \
   X(glDispatchCompute, void, (GLuint, GLuint, GLuint)) X(glMemoryBarrier, void, (GLbitfield))                        \
   X(glMapBufferRange, void *, (GLenum, intptr_t, GLsizeiptr, GLbitfield)) X(glUnmapBuffer, GLboolean, (GLenum))

struct GlT {
#define GL_DECLARE(name, ret, args) ret(WINAPI *name) args = nullptr;
   GL_FUNCS(GL_DECLARE)
#undef GL_DECLARE
};

static GLuint glProgram(GlT &gl, std::initializer_list<std::pair<GLenum, const char *>> stages)
{
   GLuint prog = gl.glCreateProgram();
   for (auto &st : stages) {
      GLuint sh = gl.glCreateShader(st.first);
      gl.glShaderSource(sh, 1, &st.second, nullptr);
      gl.glCompileShader(sh);
      GLint ok = 0;
      gl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
      if (!ok) return 0;
      gl.glAttachShader(prog, sh);
   }
   gl.glLinkProgram(prog);
   GLint ok = 0;
   gl.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   return ok ? prog : 0;
}

static void groupOpenGL()
{
   const char *TESTS[] = {"OpenGL on the GPU", "OpenGL 3.3 core profile or higher", "Rendering a triangle (framebuffer object)",
                          "Compute shaders (OpenGL 4.3)", "Persistent coherent buffer maps (OpenGL 4.4)"};
   HWND hwnd = createWindow("WinBoat GPU check (OpenGL)", 64, 64, false);
   HDC dc = hwnd ? GetDC(hwnd) : nullptr;
   PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
   int format = dc ? ChoosePixelFormat(dc, &pfd) : 0;
   HGLRC legacy = format && SetPixelFormat(dc, format, &pfd) ? wglCreateContext(dc) : nullptr;
   if (!legacy || !wglMakeCurrent(dc, legacy)) {
      missing(TESTS[0], "no OpenGL context");
      for (size_t i = 1; i < ARRAYSIZE(TESTS); i++) report(Status::Skip, TESTS[i], "no OpenGL");
      if (hwnd) DestroyWindow(hwnd);
      return;
   }
   std::string renderer = (const char *)glGetString(GL_RENDERER), version = (const char *)glGetString(GL_VERSION);
   if (renderer.find("GDI Generic") != std::string::npos) {
      missing(TESTS[0], "only Windows' software OpenGL 1.1 (" + renderer + ")");
      for (size_t i = 1; i < ARRAYSIZE(TESTS); i++) report(Status::Skip, TESTS[i], "no OpenGL on the GPU");
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(legacy);
      DestroyWindow(hwnd);
      return;
   }
   check(true, TESTS[0], renderer + ", " + version);

   // The newest core profile the driver gives
   auto createAttribs = (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
   HGLRC core = nullptr;
   int major = 0, minor = 0;
   const int versions[][2] = {{4, 6}, {4, 5}, {4, 3}, {4, 0}, {3, 3}};
   for (auto &v : versions) {
      const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, v[0], WGL_CONTEXT_MINOR_VERSION_ARB, v[1],
                             WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
      if (createAttribs && (core = createAttribs(dc, nullptr, attribs))) {
         major = v[0], minor = v[1];
         break;
      }
   }
   if (core) {
      wglMakeCurrent(dc, core);
      // the version the context has (it can be newer than the one asked for)
      const char *cv = (const char *)glGetString(GL_VERSION);
      sscanf(cv, "%d.%d", &major, &minor);
      check(true, TESTS[1], fmt("%d.%d core: %s", major, minor, cv));
   } else {
      missing(TESTS[1], "the driver gives no 3.3 core context");
   }
   GlT gl;
   bool loaded = core != nullptr;
#define GLFN_LOAD(name, ret, args) gl.name = (ret(WINAPI *) args)wglGetProcAddress(#name), loaded = loaded && gl.name;
   GL_FUNCS(GLFN_LOAD)
#undef GLFN_LOAD

   if (startTest(TESTS[2])) {
      bool ok = false;
      std::string detail = loaded ? "" : "no core profile";
      if (loaded) {
         GLuint fbo, rb, vao;
         gl.glGenFramebuffers(1, &fbo);
         gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         gl.glGenRenderbuffers(1, &rb);
         gl.glBindRenderbuffer(GL_RENDERBUFFER, rb);
         gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
         gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
         GLuint prog = glProgram(gl, {{GL_VERTEX_SHADER, "#version 330 core\nvoid main() {\n"
                                                         "  vec2 p[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));\n"
                                                         "  gl_Position = vec4(p[gl_VertexID], 0, 1); }\n"},
                                        {GL_FRAGMENT_SHADER, "#version 330 core\nout vec4 c; void main() { c = vec4(0.25, 0.5, 1, 1); }\n"}});
         if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && prog) {
            glViewport(0, 0, 64, 64);
            glClearColor(1, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            gl.glUseProgram(prog);
            gl.glGenVertexArrays(1, &vao);
            gl.glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            uint8_t px[4] = {};
            glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            ok = near8(px[0], 64, 2) && near8(px[1], 128, 2) && near8(px[2], 255, 2);
            detail = fmt("pixel (%d,%d,%d), expected (64,128,255)", px[0], px[1], px[2]);
         } else {
            detail = prog ? "incomplete framebuffer" : "shaders didn't compile";
         }
      }
      check(ok, TESTS[2], detail);
   }

   if (startTest(TESTS[3])) {
      if (!loaded || major < 4 || (major == 4 && minor < 3)) {
         report(Status::Warn, TESTS[3], fmt("not supported: needs OpenGL 4.3 (the driver gives %d.%d)", major, minor));
      } else {
         GLuint prog = glProgram(gl, {{GL_COMPUTE_SHADER, "#version 430\nlayout(local_size_x = 64) in;\n"
                                                          "layout(std430, binding = 0) buffer B { uint v[]; };\n"
                                                          "void main() { uint i = gl_GlobalInvocationID.x; v[i] = i * 3u + 1u; }\n"}});
         GLuint buf;
         gl.glGenBuffers(1, &buf);
         gl.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
         gl.glBufferData(GL_SHADER_STORAGE_BUFFER, 1024 * 4, nullptr, GL_DYNAMIC_COPY);
         gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
         int bad = -1;
         if (prog) {
            gl.glUseProgram(prog);
            gl.glDispatchCompute(16, 1, 1);
            gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            const uint32_t *v = (const uint32_t *)gl.glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 1024 * 4, GL_MAP_READ_BIT);
            if (v) {
               bad = 0;
               for (uint32_t i = 0; i < 1024; i++) bad += v[i] != i * 3 + 1;
               gl.glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            }
         }
         check(bad == 0, TESTS[3], !prog ? "the compute shader didn't compile" : bad < 0 ? "no result" : bad ? fmt("%d of 1024 values wrong", bad) : "1024 invocations");
      }
   }

   // ARB_buffer_storage: one persistent, coherent map for the whole test. Twice: the CPU writes 1024 values through it,
   // a compute shader writes v * 5 + 2 behind them, a fence wait later the CPU reads them through the same map
   if (startTest(TESTS[4])) {
      auto bufferStorage = (PFN_glBufferStorage)wglGetProcAddress("glBufferStorage");
      auto fenceSync = (PFN_glFenceSync)wglGetProcAddress("glFenceSync");
      auto clientWaitSync = (PFN_glClientWaitSync)wglGetProcAddress("glClientWaitSync");
      auto deleteSync = (PFN_glDeleteSync)wglGetProcAddress("glDeleteSync");
      if (!loaded || major < 4 || (major == 4 && minor < 4) || !bufferStorage || !fenceSync || !clientWaitSync || !deleteSync) {
         report(Status::Warn, TESTS[4], fmt("not supported: needs OpenGL 4.4 (the driver gives %d.%d)", major, minor));
      } else {
         GLuint prog = glProgram(gl, {{GL_COMPUTE_SHADER, "#version 430\nlayout(local_size_x = 64) in;\n"
                                                          "layout(std430, binding = 0) buffer B { uint v[]; };\n"
                                                          "void main() { uint i = gl_GlobalInvocationID.x; v[i + 1024u] = v[i] * 5u + 2u; }\n"}});
         const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
         GLuint buf;
         gl.glGenBuffers(1, &buf);
         gl.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
         bufferStorage(GL_SHADER_STORAGE_BUFFER, 2048 * 4, nullptr, flags);
         gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
         volatile uint32_t *v = prog ? (volatile uint32_t *)gl.glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 2048 * 4, flags) : nullptr;
         int bad = -1;
         std::string why = !prog ? "the compute shader didn't compile" : !v ? "the persistent map failed" : "";
         for (uint32_t round = 1; v && round <= 2 && why.empty(); round++) {
            for (uint32_t i = 0; i < 2048; i++) v[i] = i < 1024 ? i * round : 0;
            gl.glUseProgram(prog);
            gl.glDispatchCompute(16, 1, 1);
            GLsync sync = fenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            GLenum waited = clientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
            deleteSync(sync);
            if (waited != GL_ALREADY_SIGNALED && waited != GL_CONDITION_SATISFIED) {
               why = fmt("the fence wait returned 0x%x", waited);
               break;
            }
            bad = 0;
            for (uint32_t i = 0; i < 1024; i++) bad += v[1024 + i] != i * round * 5 + 2;
            if (bad) why = fmt("round %u: %d of 1024 values wrong (first %u, expected %u)", round, bad, v[1024 + 1], round * 5 + 2);
         }
         if (v) gl.glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
         check(why.empty(), TESTS[4], why.empty() ? "2 x 1024 values through one map" : why);
      }
   }

   wglMakeCurrent(nullptr, nullptr);
   if (core) wglDeleteContext(core);
   wglDeleteContext(legacy);
   ReleaseDC(hwnd, dc);
   DestroyWindow(hwnd);
}
