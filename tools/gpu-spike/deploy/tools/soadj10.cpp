// soadj10.exe [warp]: Direct3D 10 stream output from geometry shaders on adjacency primitives, the way 3DMark Vantage's
// GPU cloth simulates: indexed (16- and 32-bit) lines with adjacency in, a switch on per-vertex flags, four points out
// per primitive (a uint and a float3, 16 bytes), and a triangles-with-adjacency pass writing 24-byte vertices
// (position and normal). Each variant's output is checked against what the shaders compute, per vertex.
#include <windows.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

struct V { UINT flags; float p[3]; };

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   bool warp = argc > 1 && !strcmp(argv[1], "warp");
   if (FAILED(D3D10CreateDevice(NULL, warp ? D3D10_DRIVER_TYPE_WARP : D3D10_DRIVER_TYPE_HARDWARE, NULL, 0,
                                D3D10_SDK_VERSION, &dev))) {
      printf("no device\n");
      return 1;
   }
   const int W = 16, H = 16, N = W * H; // a 16x16 grid of particles
   V init[N];
   for (int i = 0; i < N; i++) {
      init[i].flags = (UINT)(i % 4) | (i % W == 0 ? 16777216u : 0u); // a case (0-3) and the pinned bit (first column)
      init[i].p[0] = (float)(i % W);
      init[i].p[1] = (float)(i / W);
      init[i].p[2] = 0;
   }
   // lines with adjacency over the rows: (i, i+1, i+2, i+3) per 4 vertices, every vertex once
   unsigned short idx16[N];
   UINT idx32[N];
   for (int i = 0; i < N; i++)
      idx16[i] = (unsigned short)i, idx32[i] = (UINT)i;

   ID3DBlob *vsPass = compile("struct V { uint f : FLAGS; float3 p : POS; }; V main(V v) { return v; }", "vs_4_0");
   // per vertex of the primitive: a case on its flags' low byte moves it; pinned ones (bit 24) stay
   ID3DBlob *gsSim = compile(
      "struct V { uint f : FLAGS; float3 p : POS; };\n"
      "cbuffer C : register(b1) { float4 k; };\n"
      "[maxvertexcount(4)] void main(lineadj V i[4], inout PointStream<V> o) {\n"
      "  for (int n = 0; n < 4; n++) { V v = i[n]; float3 d;\n"
      "    switch (v.f & 255) { case 0: d = float3(k.x, 0, 0); break; case 1: d = float3(0, k.y, 0); break;\n"
      "                         case 2: d = float3(0, 0, k.z); break; default: d = float3(k.w, k.w, k.w); break; }\n"
      "    if (!(v.f & 16777216u)) v.p += d; o.Append(v); } }", "gs_4_0");
   // normals: per triangle (with adjacency) its 3 vertices with the face normal
   ID3DBlob *gsNorm = compile(
      "struct V { uint f : FLAGS; float3 p : POS; };\n"
      "struct R { float3 p : POSITION; float3 n : NORMAL; };\n"
      "[maxvertexcount(3)] void main(triangleadj V i[6], inout PointStream<R> o) {\n"
      "  float3 n = normalize(cross(i[2].p - i[0].p, i[4].p - i[0].p));\n"
      "  for (int c = 0; c < 6; c += 2) { R r; r.p = i[c].p; r.n = n; o.Append(r); } }", "gs_4_0");

   D3D10_SO_DECLARATION_ENTRY soSim[] = {{"FLAGS", 0, 0, 1, 0}, {"POS", 0, 0, 3, 0}};
   D3D10_SO_DECLARATION_ENTRY soNorm[] = {{"POSITION", 0, 0, 3, 0}, {"NORMAL", 0, 0, 3, 0}};
   ID3D10VertexShader *vs;
   ID3D10GeometryShader *gsS, *gsN;
   HRESULT hr1 = dev->CreateVertexShader(vsPass->GetBufferPointer(), vsPass->GetBufferSize(), &vs);
   HRESULT hr2 = dev->CreateGeometryShaderWithStreamOutput(gsSim->GetBufferPointer(), gsSim->GetBufferSize(), soSim, 2, 16, &gsS);
   HRESULT hr3 = dev->CreateGeometryShaderWithStreamOutput(gsNorm->GetBufferPointer(), gsNorm->GetBufferSize(), soNorm, 2, 24, &gsN);
   if (FAILED(hr1) || FAILED(hr2) || FAILED(hr3)) { printf("shaders: 0x%lx 0x%lx 0x%lx\n", hr1, hr2, hr3); return 1; }
   D3D10_INPUT_ELEMENT_DESC el[] = {{"FLAGS", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
                                    {"POS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 4, D3D10_INPUT_PER_VERTEX_DATA, 0}};
   ID3D10InputLayout *layout;
   HRESULT hr4 = dev->CreateInputLayout(el, 2, vsPass->GetBufferPointer(), vsPass->GetBufferSize(), &layout);
   if (FAILED(hr4)) { printf("layout: 0x%lx\n", hr4); return 1; }
   float k[4] = {0.5f, 0.25f, 2.0f, -1.0f};
   ID3D10Buffer *cb = buffer(D3D10_BIND_CONSTANT_BUFFER, 16, k);
   ID3D10Buffer *ib16 = buffer(D3D10_BIND_INDEX_BUFFER, sizeof(idx16), idx16);
   ID3D10Buffer *ib32 = buffer(D3D10_BIND_INDEX_BUFFER, sizeof(idx32), idx32);
   ID3D10Buffer *readback = buffer(0, N * 24 * 2, NULL, D3D10_USAGE_STAGING, D3D10_CPU_ACCESS_READ);

   static const struct { const char *name; bool i32; int steps; } variants[] = {
      {"lineadj, 16-bit indices, 1 step", false, 1},
      {"lineadj, 32-bit indices, 1 step", true, 1},
      {"lineadj, 16-bit indices, 3 steps (ping-pong)", false, 3},
   };
   int failures = 0;
   for (const auto &v : variants) {
      ID3D10Buffer *b[2] = {buffer(D3D10_BIND_STREAM_OUTPUT | D3D10_BIND_VERTEX_BUFFER, sizeof(init), init),
                            buffer(D3D10_BIND_STREAM_OUTPUT | D3D10_BIND_VERTEX_BUFFER, sizeof(init), NULL)};
      UINT stride = 16, zero = 0;
      dev->IASetInputLayout(layout);
      dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_LINELIST_ADJ);
      dev->IASetIndexBuffer(v.i32 ? ib32 : ib16, v.i32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
      dev->VSSetShader(vs);
      dev->GSSetShader(gsS);
      dev->GSSetConstantBuffers(1, 1, &cb);
      dev->PSSetShader(NULL);
      int cur = 0;
      for (int s = 0; s < v.steps; s++) {
         dev->IASetVertexBuffers(0, 1, &b[cur], &stride, &zero);
         dev->SOSetTargets(1, &b[1 - cur], &zero);
         dev->DrawIndexedInstanced(N, 1, 0, 0, 0);
         ID3D10Buffer *none = NULL;
         dev->SOSetTargets(1, &none, &zero);
         cur = 1 - cur;
      }
      dev->CopySubresourceRegion(readback, 0, 0, 0, 0, b[cur], 0, NULL);
      int good = 0;
      V *p;
      if (SUCCEEDED(readback->Map(D3D10_MAP_READ, 0, (void **)&p))) {
         for (int i = 0; i < N; i++) {
            float e[3] = {init[i].p[0], init[i].p[1], init[i].p[2]};
            if (!(init[i].flags & 16777216u)) {
               UINT c = init[i].flags & 255;
               for (int s = 0; s < v.steps; s++) {
                  if (c == 0) e[0] += k[0]; else if (c == 1) e[1] += k[1]; else if (c == 2) e[2] += k[2];
                  else { e[0] += k[3]; e[1] += k[3]; e[2] += k[3]; }
               }
            }
            if (p[i].flags == init[i].flags && p[i].p[0] == e[0] && p[i].p[1] == e[1] && p[i].p[2] == e[2])
               good++;
            else if (good == i && i < 1000)
               printf("   first wrong vertex %d: flags %u pos %g %g %g (expected %u %g %g %g)\n", i, p[i].flags, p[i].p[0],
                      p[i].p[1], p[i].p[2], init[i].flags, e[0], e[1], e[2]);
         }
         readback->Unmap();
      }
      bool ok = good == N;
      failures += !ok;
      printf("%-46s %s: %d of %d vertices\n", v.name, ok ? "ok  " : "FAIL", good, N);

      // normals: triangles with adjacency over the result, 6 indices (0..5 of a run) per triangle
      if (v.steps == 1 && !v.i32) {
         const int T = N / 6;
         ID3D10Buffer *nb = buffer(D3D10_BIND_STREAM_OUTPUT | D3D10_BIND_VERTEX_BUFFER, T * 3 * 24, NULL);
         dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ);
         dev->IASetVertexBuffers(0, 1, &b[cur], &stride, &zero);
         dev->GSSetShader(gsN);
         dev->SOSetTargets(1, &nb, &zero);
         dev->DrawIndexedInstanced(T * 6, 1, 0, 0, 0);
         ID3D10Buffer *none = NULL;
         dev->SOSetTargets(1, &none, &zero);
         dev->CopySubresourceRegion(readback, 0, 0, 0, 0, nb, 0, NULL);
         int ngood = 0;
         float (*r)[6];
         V *src = new V[N];
         dev->CopySubresourceRegion(readback, 0, 0, 0, 0, b[cur], 0, NULL);
         if (SUCCEEDED(readback->Map(D3D10_MAP_READ, 0, (void **)&p))) {
            memcpy(src, p, sizeof(V) * N);
            readback->Unmap();
         }
         dev->CopySubresourceRegion(readback, 0, 0, 0, 0, nb, 0, NULL);
         if (SUCCEEDED(readback->Map(D3D10_MAP_READ, 0, (void **)&r))) {
            for (int t = 0; t < T; t++) {
               const float *a = src[t * 6].p, *b2 = src[t * 6 + 2].p, *c = src[t * 6 + 4].p;
               float u[3] = {b2[0] - a[0], b2[1] - a[1], b2[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
               float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
               float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
               bool okT = true;
               for (int j = 0; j < 3; j++) {
                  const float *sp = src[t * 6 + j * 2].p, *o = r[t * 3 + j];
                  for (int x = 0; x < 3; x++)
                     okT &= o[x] == sp[x] && fabsf(o[3 + x] - n[x] / l) < 1e-4f;
               }
               ngood += okT;
            }
            readback->Unmap();
         }
         delete[] src;
         failures += ngood != T;
         printf("%-46s %s: %d of %d triangles\n", "triangleadj normals, 24-byte vertices", ngood == T ? "ok  " : "FAIL",
                ngood, T);
         nb->Release();
      }
      b[0]->Release();
      b[1]->Release();
   }
   printf("%d failures\n", failures);
   return failures;
}
