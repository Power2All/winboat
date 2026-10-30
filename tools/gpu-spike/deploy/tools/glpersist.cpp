// glpersist.exe <MB> <storageflags:mapflags (hex)>... | c<count>: glBufferStorage + glMapBufferRange of a buffer of
// that size per flag pair (array and shader storage targets), or <count> persistent buffers made, mapped and deleted
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>
typedef void (APIENTRY *PFNGLGENBUFFERS)(GLsizei, GLuint*);
typedef void (APIENTRY *PFNGLBINDBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLBUFFERSTORAGE)(GLenum, ptrdiff_t, const void*, GLbitfield);
typedef void (APIENTRY *PFNGLDELETEBUFFERS)(GLsizei, const GLuint*);
typedef void* (APIENTRY *PFNGLMAPBUFFERRANGE)(GLenum, ptrdiff_t, ptrdiff_t, GLbitfield);
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBS)(HDC, HGLRC, const int*);
int main(int argc, char** argv) {
    HWND w = CreateWindowA("STATIC", "gl", WS_POPUP, 0, 0, 16, 16, 0, 0, 0, 0);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc); wglMakeCurrent(dc, rc);
    auto ca = (PFNWGLCREATECONTEXTATTRIBS)wglGetProcAddress("wglCreateContextAttribsARB");
    int attrs[] = {0x2091, 4, 0x2092, 6, 0x9126, 1, 0};
    HGLRC core = ca(dc, 0, attrs); wglMakeCurrent(dc, core);
    printf("%s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    auto gen = (PFNGLGENBUFFERS)wglGetProcAddress("glGenBuffers");
    auto bind = (PFNGLBINDBUFFER)wglGetProcAddress("glBindBuffer");
    auto storage = (PFNGLBUFFERSTORAGE)wglGetProcAddress("glBufferStorage");
    auto map = (PFNGLMAPBUFFERRANGE)wglGetProcAddress("glMapBufferRange");
    auto del = (PFNGLDELETEBUFFERS)wglGetProcAddress("glDeleteBuffers");
    if (argv[1][0] == 'c') {  // c<count>: create, map, delete in a loop
        int n = atoi(argv[1] + 1), bad = 0;
        for (int i = 0; i < n; i++) {
            GLuint b; gen(1, &b); bind(0x90D2, b);
            storage(0x90D2, 65536 * (1 + i % 16), nullptr, 0xC2);
            void* p = map(0x90D2, 0, 65536, 0xC2);
            if (!p && bad++ < 5) printf("cycle %d: map failed\n", i);
            glFinish();
            del(1, &b);
        }
        printf("%d cycles, %d failed\n", n, bad);
        return 0;
    }
    // args: MB storageflags:mapflags ...
    double mb = atof(argv[1]); ptrdiff_t size = (ptrdiff_t)(mb * 1048576);
    for (int i = 2; i < argc; i++) {
        GLbitfield sf = strtoul(argv[i], nullptr, 16), mf = strtoul(strchr(argv[i], ':') + 1, nullptr, 16);
        for (GLenum target : {0x8892u /*ARRAY*/, 0x90D2u /*SHADER_STORAGE*/}) {
            GLuint b; gen(1, &b); bind(target, b);
            storage(target, size, nullptr, sf);
            GLenum e1 = glGetError();
            void* p = map(target, 0, size, mf);
            printf("%.2f MB storage 0x%x map 0x%x target 0x%x: storage err 0x%x, map %p err 0x%x\n", mb, sf, mf, target, e1, p, glGetError());
            fflush(stdout);
        }
    }
    return 0;
}
