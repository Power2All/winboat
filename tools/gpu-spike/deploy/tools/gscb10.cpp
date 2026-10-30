// gscb10.exe [warp]: a geometry shader's constant buffer and SV_PrimitiveID right after another program's vertex
// shader used a constant buffer (3DMark Vantage's GPU cloth: its geometry shaders read the integrate pass's constants
// and primitive ID 0 everywhere, and the cloth blew up). Program A streams out from a vertex shader reading cb1 (111);
// program B, a pass-through vertex shader and a geometry shader, streams out its own cb1 (222) and the primitive ID.
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

static ID3D10Buffer *buffer(UINT bind, UINT size, const void *data, D3D10_USAGE usage = D3D10_USAGE_DEFAULT, UINT cpu = 0)
{
   D3D10_BUFFER_DESC bd = {size, usage, bind, cpu, 0};
   D3D10_SUBRESOURCE_DATA sd = {data, 0, 0};
   ID3D10Buffer *b = NULL;
   dev->CreateBuffer(&bd, data ? &sd : NULL, &b);
   return b;
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
   const int N = 300; // points
   float pts[N][4] = {};
   ID3DBlob *vsA = compile("cbuffer C : register(b1) { float4 k; };\n"
                           "float4 main(float4 p : POSITION) : POSITION { return float4(k.x, 0, 0, 1); }", "vs_4_0");
   ID3DBlob *vsB = compile("float4 main(float4 p : POSITION) : POSITION { return p; }", "vs_4_0");
   ID3DBlob *gsB = compile("cbuffer C : register(b1) { float4 k; };\n"
                           "struct O { float4 v : VALUE; };\n"
                           "[maxvertexcount(1)] void main(point float4 i[1] : POSITION, uint id : SV_PrimitiveID, inout PointStream<O> o) {\n"
                           "  O r; r.v = float4(k.x, (float)id, 0, 1); o.Append(r); }", "gs_4_0");
   D3D10_SO_DECLARATION_ENTRY soA = {"POSITION", 0, 0, 4, 0}, soB = {"VALUE", 0, 0, 4, 0};
   ID3D10VertexShader *vA, *vB;
   ID3D10GeometryShader *gA, *gB;
   dev->CreateVertexShader(vsA->GetBufferPointer(), vsA->GetBufferSize(), &vA);
   dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), &vB);
   dev->CreateGeometryShaderWithStreamOutput(vsA->GetBufferPointer(), vsA->GetBufferSize(), &soA, 1, 16, &gA);
   if (FAILED(dev->CreateGeometryShaderWithStreamOutput(gsB->GetBufferPointer(), gsB->GetBufferSize(), &soB, 1, 16, &gB))) {
      printf("gs\n");
      return 1;
   }
   D3D10_INPUT_ELEMENT_DESC el = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0};
   ID3D10InputLayout *layout;
   dev->CreateInputLayout(&el, 1, vsB->GetBufferPointer(), vsB->GetBufferSize(), &layout);
   float a[4] = {111, 0, 0, 0}, b[4] = {222, 0, 0, 0};
   ID3D10Buffer *cbA = buffer(D3D10_BIND_CONSTANT_BUFFER, 16, a), *cbB = buffer(D3D10_BIND_CONSTANT_BUFFER, 16, b);
   ID3D10Buffer *vb = buffer(D3D10_BIND_VERTEX_BUFFER, sizeof(pts), pts);
   ID3D10Buffer *outA = buffer(D3D10_BIND_STREAM_OUTPUT, N * 16, NULL), *outB = buffer(D3D10_BIND_STREAM_OUTPUT, N * 16, NULL);
   ID3D10Buffer *rb = buffer(0, N * 16, NULL, D3D10_USAGE_STAGING, D3D10_CPU_ACCESS_READ);
   UINT stride = 16, zero = 0;
   ID3D10Buffer *none = NULL;
   dev->IASetInputLayout(layout);
   dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
   dev->IASetVertexBuffers(0, 1, &vb, &stride, &zero);
   dev->PSSetShader(NULL);
   // the constant buffers bound once, as games do: from the second round on the geometry shader's isn't "new"
   dev->VSSetConstantBuffers(1, 1, &cbA);
   dev->GSSetConstantBuffers(1, 1, &cbB);
   int failures = 0;
   for (int round = 0; round < 3; round++) {
      // program A: its vertex shader's cb1
      dev->VSSetShader(vA);
      dev->GSSetShader(gA);
      dev->SOSetTargets(1, &outA, &zero);
      dev->Draw(N, 0);
      // program B: the geometry shader's own cb1, and the primitive ID
      dev->VSSetShader(vB);
      dev->GSSetShader(gB);
      dev->SOSetTargets(1, &outB, &zero);
      dev->Draw(N, 0);
      dev->SOSetTargets(1, &none, &zero);
      dev->CopyResource(rb, outB);
      int cb = 0, id = 0;
      float (*p)[4];
      if (SUCCEEDED(rb->Map(D3D10_MAP_READ, 0, (void **)&p))) {
         for (int i = 0; i < N; i++) {
            cb += p[i][0] == 222;
            id += p[i][1] == (float)i;
         }
         if (cb != N || id != N)
            printf("   point 5: cb1 %g, primitive ID %g\n", p[5][0], p[5][1]);
         rb->Unmap();
      }
      bool ok = cb == N && id == N;
      failures += !ok;
      printf("round %d %s: geometry shader's own cb1 at %d of %d points, primitive IDs right at %d\n", round,
             ok ? "ok  " : "FAIL", cb, N, id);
   }
   printf("%d failures\n", failures);
   return failures;
}
