// gsint10.exe [warp]: integer vertex data passed from the vertex shader to a geometry shader and streamed out, the way
// 3DMark Vantage's GPU cloth passes per-vertex flags: small integers (whose bit patterns are float denormals) and
// large ones, through a uint varying, tested (switch on the low byte) and written back. Prints the vertices whose
// streamed-out values match.
#include <windows.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static ID3D10Device *dev;

static ID3DBlob *compile(const char *src, const char *target)
{
   static pD3DCompile fn = (pD3DCompile)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
   ID3DBlob *code = NULL, *err = NULL;
   if (!fn || FAILED(fn(src, strlen(src), NULL, NULL, NULL, "main", target, 0, 0, &code, &err))) {
      printf("compile %s: %s\n", target, err ? (const char *)err->GetBufferPointer() : "no compiler");
      exit(1);
   }
   return code;
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   bool warp = argc > 1 && !strcmp(argv[1], "warp");
   if (FAILED(D3D10CreateDevice(NULL, warp ? D3D10_DRIVER_TYPE_WARP : D3D10_DRIVER_TYPE_HARDWARE, NULL, 0,
                                D3D10_SDK_VERSION, &dev))) {
      printf("no device\n");
      return 1;
   }
   const int N = 512;
   UINT data[N];
   for (int i = 0; i < N; i++)
      data[i] = i < 256 ? (UINT)i : ((UINT)i << 20) | (UINT)(i & 7); // small (denormal bits) and large values
   ID3DBlob *vs = compile("struct V { uint f : FLAGS; }; V main(V v) { return v; }", "vs_4_0");
   // per point: the value back, the case its low byte selects, and whether bit 2 is set
   ID3DBlob *gs = compile(
      "struct V { uint f : FLAGS; };\n"
      "struct O { uint4 v : VALUE; };\n"
      "[maxvertexcount(1)] void main(point V i[1], inout PointStream<O> o) {\n"
      "  uint f = i[0].f, c;\n"
      "  switch (f & 255) { case 0: c = 10; break; case 1: c = 11; break; case 2: c = 12; break; default: c = 99; break; }\n"
      "  O r; r.v = uint4(f, c, (f & 4u) ? 1u : 0u, 7u); o.Append(r); }", "gs_4_0");
   D3D10_SO_DECLARATION_ENTRY so = {"VALUE", 0, 0, 4, 0};
   ID3D10VertexShader *vss;
   ID3D10GeometryShader *gss;
   dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), &vss);
   if (FAILED(dev->CreateGeometryShaderWithStreamOutput(gs->GetBufferPointer(), gs->GetBufferSize(), &so, 1, 16, &gss))) {
      printf("gs\n");
      return 1;
   }
   D3D10_INPUT_ELEMENT_DESC el = {"FLAGS", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0};
   ID3D10InputLayout *layout;
   dev->CreateInputLayout(&el, 1, vs->GetBufferPointer(), vs->GetBufferSize(), &layout);
   D3D10_BUFFER_DESC bd = {sizeof(data), D3D10_USAGE_DEFAULT, D3D10_BIND_VERTEX_BUFFER, 0, 0};
   D3D10_SUBRESOURCE_DATA sd = {data, 0, 0};
   ID3D10Buffer *vb, *out, *rb;
   dev->CreateBuffer(&bd, &sd, &vb);
   bd.ByteWidth = N * 16;
   bd.BindFlags = D3D10_BIND_STREAM_OUTPUT;
   dev->CreateBuffer(&bd, NULL, &out);
   bd.Usage = D3D10_USAGE_STAGING;
   bd.BindFlags = 0;
   bd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
   dev->CreateBuffer(&bd, NULL, &rb);
   UINT stride = 4, zero = 0;
   dev->IASetInputLayout(layout);
   dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
   dev->IASetVertexBuffers(0, 1, &vb, &stride, &zero);
   dev->VSSetShader(vss);
   dev->GSSetShader(gss);
   dev->PSSetShader(NULL);
   dev->SOSetTargets(1, &out, &zero);
   dev->Draw(N, 0);
   ID3D10Buffer *none = NULL;
   dev->SOSetTargets(1, &none, &zero);
   dev->CopyResource(rb, out);
   int small = 0, large = 0, firstBad = -1;
   UINT (*p)[4];
   if (SUCCEEDED(rb->Map(D3D10_MAP_READ, 0, (void **)&p))) {
      for (int i = 0; i < N; i++) {
         UINT f = data[i], lo = f & 255, c = lo == 0 ? 10 : lo == 1 ? 11 : lo == 2 ? 12 : 99;
         bool ok = p[i][0] == f && p[i][1] == c && p[i][2] == ((f & 4) ? 1u : 0u) && p[i][3] == 7;
         if (ok)
            (i < 256 ? small : large)++;
         else if (firstBad < 0) {
            firstBad = i;
            printf("   first wrong vertex %d: %u %u %u %u (value %u)\n", i, p[i][0], p[i][1], p[i][2], p[i][3], f);
         }
      }
      rb->Unmap();
   }
   int failures = (small != 256) + (large != N - 256);
   printf("small values (denormal bits) %s: %d of 256\n", small == 256 ? "ok  " : "FAIL", small);
   printf("large values                 %s: %d of %d\n", large == N - 256 ? "ok  " : "FAIL", large, N - 256);
   printf("%d failures\n", failures);
   return failures;
}
