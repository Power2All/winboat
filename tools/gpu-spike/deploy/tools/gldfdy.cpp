// gldfdy.exe: the sign of dFdy in the window's back buffer and in an FBO. A full-screen triangle with a varying that
// is its clip-space y (rising upwards): GL's dFdy is taken along window y (upwards), so dFdy of that varying and of
// gl_FragCoord.y are positive in both. GravityMark's OpenGL blit mirrors its picture on the sign of a dFdy and showed
// the scene upside down on virgl. Prints the signs read back at the bottom and top rows. Also packHalf2x16 and
// unpackHalf2x16 on known values (the host left TGSI's UP2H/PK2H out of its GLSL). Exit code = failures.
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
typedef char GLchar;
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBS)(HDC, HGLRC, const int*);
#define F(ret, name, ...) typedef ret (APIENTRY *PFN_##name)(__VA_ARGS__); static PFN_##name name;
F(GLuint, glCreateShader, GLenum)
F(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)
F(void, glCompileShader, GLuint)
F(void, glGetShaderiv, GLuint, GLenum, GLint*)
F(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)
F(GLuint, glCreateProgram, void)
F(void, glAttachShader, GLuint, GLuint)
F(void, glLinkProgram, GLuint)
F(void, glUseProgram, GLuint)
F(void, glGenVertexArrays, GLsizei, GLuint*)
F(void, glBindVertexArray, GLuint)
F(void, glGenFramebuffers, GLsizei, GLuint*)
F(void, glBindFramebuffer, GLenum, GLuint)
F(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)
#define L(name) name = (PFN_##name)wglGetProcAddress(#name)

static const int W = 64, H = 64;

static GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, 0x8B81, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); printf("shader: %s\n", log); exit(2); }
    return s;
}

// R: dFdy(vy) > 0, G: dFdy(gl_FragCoord.y) > 0, B: in the lower half of the clip space (vy < 0)
static int check(const char* where) {
    unsigned char lo[4], hi[4];
    glReadPixels(W / 2, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lo);
    glReadPixels(W / 2, H - 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, hi);
    bool ok = lo[0] > 128 && lo[1] > 128 && lo[2] > 128 && hi[0] > 128 && hi[1] > 128 && hi[2] < 128;
    printf("%s %s: bottom row dFdy(vy)>0 %d dFdy(fragcoord.y)>0 %d lower half %d | top row %d %d %d\n", ok ? "ok  " : "FAIL",
           where, lo[0] > 128, lo[1] > 128, lo[2] > 128, hi[0] > 128, hi[1] > 128, hi[2] > 128);
    return !ok;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    HWND w = CreateWindowA("STATIC", "gldfdy", WS_POPUP | WS_VISIBLE, 0, 0, W, H, 0, 0, 0, 0);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc); wglMakeCurrent(dc, rc);
    auto ca = (PFNWGLCREATECONTEXTATTRIBS)wglGetProcAddress("wglCreateContextAttribsARB");
    int attrs[] = {0x2091, 3, 0x2092, 3, 0x9126, 1, 0};
    HGLRC core = ca(dc, 0, attrs); wglMakeCurrent(dc, core);
    printf("%s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glGetShaderiv); L(glGetShaderInfoLog); L(glCreateProgram);
    L(glAttachShader); L(glLinkProgram); L(glUseProgram); L(glGenVertexArrays); L(glBindVertexArray);
    L(glGenFramebuffers); L(glBindFramebuffer); L(glFramebufferTexture2D);

    GLuint p = glCreateProgram();
    glAttachShader(p, shader(0x8B31, "#version 330\nout float vy;\nvoid main() {\n"
                                      "  vec2 c = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;\n"
                                      "  vy = c.y; gl_Position = vec4(c, 0.0, 1.0); }\n"));
    glAttachShader(p, shader(0x8B30, "#version 330\nin float vy;\nout vec4 o;\nvoid main() {\n"
                                      "  o = vec4(dFdy(vy) > 0.0 ? 1.0 : 0.0, dFdy(gl_FragCoord.y) > 0.0 ? 1.0 : 0.0, vy < 0.0 ? 1.0 : 0.0, 1.0); }\n"));
    glLinkProgram(p); glUseProgram(p);
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glViewport(0, 0, W, H);
    int failures = 0;

    glClearColor(0.5f, 0.5f, 0.5f, 1); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadBuffer(GL_BACK);
    failures += check("window back buffer");

    GLuint tex, fbo;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(0x8D40, fbo);
    glFramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, tex, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    failures += check("FBO");
    // packHalf2x16 / unpackHalf2x16 (TGSI PK2H / UP2H): known halves
    {
        GLuint q = glCreateProgram();
        glAttachShader(q, shader(0x8B31, "#version 330\nvoid main() { vec2 c = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0; gl_Position = vec4(c, 0.0, 1.0); }\n"));
        glAttachShader(q, shader(0x8B30, "#version 330\n#extension GL_ARB_shading_language_packing : require\nuniform uint u;\nuniform vec2 f;\nout vec4 o;\nvoid main() {\n"
                                          "  vec2 h = unpackHalf2x16(u);\n"
                                          "  uint p = packHalf2x16(f);\n"
                                          "  o = vec4(h.x == 1.5 ? 1.0 : 0.0, h.y == -2.0 ? 1.0 : 0.0, p == 0xc0003e00u ? 1.0 : 0.0, 1.0); }\n"));
        glLinkProgram(q); glUseProgram(q);
        typedef GLint (APIENTRY *PFNLOC)(GLuint, const GLchar*);
        typedef void (APIENTRY *PFNU1UI)(GLint, GLuint);
        typedef void (APIENTRY *PFNU2F)(GLint, GLfloat, GLfloat);
        auto loc = (PFNLOC)wglGetProcAddress("glGetUniformLocation");
        auto u1ui = (PFNU1UI)wglGetProcAddress("glUniform1ui");
        auto u2f = (PFNU2F)wglGetProcAddress("glUniform2f");
        u1ui(loc(q, "u"), 0xc0003e00u);  // halves 1.5 (0x3e00) and -2.0 (0xc000)
        u2f(loc(q, "f"), 1.5f, -2.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        unsigned char px[4];
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        bool ok = px[0] > 128 && px[1] > 128 && px[2] > 128;
        printf("%s half floats: unpackHalf2x16 x %d y %d, packHalf2x16 %d\n", ok ? "ok  " : "FAIL", px[0] > 128, px[1] > 128, px[2] > 128);
        failures += !ok;
    }
    printf("%d failures\n", failures);
    return failures;
}
