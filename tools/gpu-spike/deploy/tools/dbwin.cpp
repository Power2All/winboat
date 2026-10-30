// dbwin.exe <out.txt> <seconds>: writes every OutputDebugString message of this session (pid + text) to out.txt
#include <windows.h>
#include <stdio.h>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    HANDLE ready = CreateEventA(nullptr, FALSE, TRUE, "DBWIN_BUFFER_READY");
    HANDLE data = CreateEventA(nullptr, FALSE, FALSE, "DBWIN_DATA_READY");
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, "DBWIN_BUFFER");
    const char* view = map ? (const char*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 4096) : nullptr;
    FILE* f = fopen(argv[1], "w");
    if (!view || !f) return 1;
    DWORD end = GetTickCount() + atoi(argv[2]) * 1000;
    while ((int)(end - GetTickCount()) > 0) {
        if (WaitForSingleObject(data, 500) != WAIT_OBJECT_0) continue;
        fprintf(f, "[%lu] %.*s", *(const DWORD*)view, 4090, view + 4);
        fflush(f);
        SetEvent(ready);
    }
    return 0;
}
