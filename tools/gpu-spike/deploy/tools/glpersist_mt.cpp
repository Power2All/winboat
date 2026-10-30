// glpersist_mt.exe <threads> <MB>: each thread its own core context (sharing with the first), 4 persistent buffers
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
typedef void (APIENTRY *PFNGLGENBUFFERS)(GLsizei, GLuint*);
typedef void (APIENTRY *PFNGLBINDBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLBUFFERSTORAGE)(GLenum, ptrdiff_t, const void*, GLbitfield);
typedef void* (APIENTRY *PFNGLMAPBUFFERRANGE)(GLenum, ptrdiff_t, ptrdiff_t, GLbitfield);
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBS)(HDC, HGLRC, const int*);
static HDC dc; static HGLRC share; static PFNWGLCREATECONTEXTATTRIBS ca; static ptrdiff_t size;
static HANDLE go; static volatile LONG fails;
static DWORD WINAPI worker(void* arg) {
    int attrs[] = {0x2091, 4, 0x2092, 6, 0x9126, 1, 0};
    HGLRC rc = ca(dc, share, attrs);
    wglMakeCurrent(dc, rc);
    auto gen = (PFNGLGENBUFFERS)wglGetProcAddress("glGenBuffers");
    auto bind = (PFNGLBINDBUFFER)wglGetProcAddress("glBindBuffer");
    auto storage = (PFNGLBUFFERSTORAGE)wglGetProcAddress("glBufferStorage");
    auto map = (PFNGLMAPBUFFERRANGE)wglGetProcAddress("glMapBufferRange");
    WaitForSingleObject(go, INFINITE);
    for (int i = 0; i < 4; i++) {
        GLuint b; gen(1, &b); bind(0x90D2, b);
        storage(0x90D2, size, nullptr, 0xC2);
        void* p = map(0x90D2, 0, size, 0xC2);
        if (!p) { InterlockedIncrement(&fails); printf("thread %d buffer %d: map failed, err 0x%x\n", (int)(INT_PTR)arg, i, glGetError()); }
    }
    glFinish();
    return 0;
}
int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 8; size = (ptrdiff_t)((argc > 2 ? atof(argv[2]) : 4) * 1048576);
    HWND w = CreateWindowA("STATIC", "gl", WS_POPUP, 0, 0, 16, 16, 0, 0, 0, 0);
    dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc); wglMakeCurrent(dc, rc);
    ca = (PFNWGLCREATECONTEXTATTRIBS)wglGetProcAddress("wglCreateContextAttribsARB");
    int attrs[] = {0x2091, 4, 0x2092, 6, 0x9126, 1, 0};
    share = ca(dc, 0, attrs); wglMakeCurrent(dc, NULL);
    go = CreateEventA(0, TRUE, FALSE, 0);
    HANDLE t[64];
    for (int i = 0; i < n; i++) t[i] = CreateThread(0, 0, worker, (void*)(INT_PTR)i, 0, 0);
    Sleep(500); SetEvent(go);
    WaitForMultipleObjects(n, t, TRUE, 60000);
    printf("%d threads x 4 buffers: %ld maps failed\n", n, fails);
    return 0;
}
