// vtxint10.exe [warp]: Direct3D 10 integer vertex attributes (R32_UINT, R32_SINT, R16G16_UINT) read by a vertex shader as
// integers, the bits tested and the values streamed out (3DMark Vantage's GPU cloth keeps per-vertex flags so).
// Prints, per format, the vertices whose streamed-out values match what the shader should have read.
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
   const int N = 256;
   static const struct {
      const char *name, *type;
      DXGI_FORMAT format;
      UINT size;
   } formats[] = {
      {"R32_UINT", "uint", DXGI_FORMAT_R32_UINT, 4},
      {"R32_SINT", "int", DXGI_FORMAT_R32_SINT, 4},
      {"R16G16_UINT", "uint2", DXGI_FORMAT_R16G16_UINT, 4},
   };
   int failures = 0;
   for (const auto &f : formats) {
      // the values: a flag bit (16777216, as Vantage's cloth), small numbers and a negative one for SINT
      UINT data[N];
      for (int i = 0; i < N; i++)
         data[i] = (i % 3 == 0 ? 16777216u : 0u) | (UINT)i | (f.format == DXGI_FORMAT_R32_SINT && i % 5 == 0 ? 0x80000000u : 0u);
      if (f.format == DXGI_FORMAT_R16G16_UINT)
         for (int i = 0; i < N; i++)
            data[i] = (UINT)i | ((UINT)(i * 7 + 1) << 16);
      char src[512];
      // out.x: the value's bits (x component), out.y: 1 when the flag bit is set, out.z: the second component (uint2)
      snprintf(src, sizeof(src),
               "struct O { uint4 v : VALUE; };\n"
               "O main(%s v : FLAGS) { O o; uint x = asuint(v%s); o.v = uint4(x, (x & 16777216u) ? 1u : 0u, %s, 7u); return o; }",
               f.type, f.format == DXGI_FORMAT_R16G16_UINT ? ".x" : "", f.format == DXGI_FORMAT_R16G16_UINT ? "v.y" : "0u");
      ID3DBlob *vs = compile(src, "vs_4_0");
      D3D10_SO_DECLARATION_ENTRY so = {"VALUE", 0, 0, 4, 0};
      ID3D10GeometryShader *gs;
      ID3D10VertexShader *vss;
      dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), &vss);
      dev->CreateGeometryShaderWithStreamOutput(vs->GetBufferPointer(), vs->GetBufferSize(), &so, 1, 16, &gs);
      D3D10_INPUT_ELEMENT_DESC el = {"FLAGS", 0, f.format, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0};
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
      dev->GSSetShader(gs);
      dev->PSSetShader(NULL);
      dev->SOSetTargets(1, &out, &zero);
      dev->Draw(N, 0);
      ID3D10Buffer *none = NULL;
      dev->SOSetTargets(1, &none, &zero);
      dev->CopyResource(rb, out);

      int good = 0;
      UINT first[4] = {};
      UINT (*p)[4];
      if (SUCCEEDED(rb->Map(D3D10_MAP_READ, 0, (void **)&p))) {
         memcpy(first, p[3], sizeof(first));
         for (int i = 0; i < N; i++) {
            UINT x = f.format == DXGI_FORMAT_R16G16_UINT ? (data[i] & 0xffff) : data[i];
            UINT y = f.format == DXGI_FORMAT_R16G16_UINT ? data[i] >> 16 : 0;
            if (p[i][0] == x && p[i][1] == ((x & 16777216u) ? 1u : 0u) && p[i][2] == y && p[i][3] == 7)
               good++;
         }
         rb->Unmap();
      }
      bool ok = good == N;
      failures += !ok;
      printf("%-12s %s: %d of %d vertices read right (vertex 3: %u %u %u %u)\n", f.name, ok ? "ok  " : "FAIL", good, N,
             first[0], first[1], first[2], first[3]);
      vb->Release(); out->Release(); rb->Release(); layout->Release(); gs->Release(); vss->Release();
   }
   printf("%d failures\n", failures);
   return failures;
}
