// dxbcdis: every DXBC container found in a file (GravityMark's shader cache), disassembled with D3DDisassemble into
// <outdir>\<index>.asm (and the container into <index>.dxbc)
// Build: x86_64-w64-mingw32-g++ -O2 -static dxbcdis.cpp -o dxbcdis.exe -ld3dcompiler_47
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
   if (argc < 3) {
      printf("usage: dxbcdis <file> <outdir>\n");
      return 1;
   }
   FILE *f = fopen(argv[1], "rb");
   if (!f)
      return 1;
   std::vector<unsigned char> d;
   unsigned char buf[65536];
   size_t n;
   while ((n = fread(buf, 1, sizeof buf, f)) > 0)
      d.insert(d.end(), buf, buf + n);
   fclose(f);
   int count = 0;
   for (size_t i = 0; i + 32 <= d.size(); i++) {
      if (memcmp(&d[i], "DXBC", 4))
         continue;
      unsigned size = *(unsigned *)&d[i + 24];
      if (size < 32 || i + size > d.size())
         continue;
      ID3DBlob *text = nullptr;
      if (FAILED(D3DDisassemble(&d[i], size, 0, nullptr, &text)))
         continue;
      char path[MAX_PATH];
      snprintf(path, sizeof path, "%s\\%04d.asm", argv[2], count);
      FILE *o = fopen(path, "wb");
      fwrite(text->GetBufferPointer(), 1, text->GetBufferSize() - 1, o);
      fclose(o);
      snprintf(path, sizeof path, "%s\\%04d.dxbc", argv[2], count);
      o = fopen(path, "wb");
      fwrite(&d[i], 1, size, o);
      fclose(o);
      text->Release();
      count++;
      i += size - 1;
   }
   printf("%d shaders\n", count);
   return 0;
}
