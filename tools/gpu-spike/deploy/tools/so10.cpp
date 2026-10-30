// so10.exe [warp]: Direct3D 10 stream output the way GPU simulations use it (3DMark Vantage's cloth and particles):
// particles ping-ponged between two buffers, each step reading the last one's output with DrawAuto and adding 1.
// Variants: the update in a geometry shader or in the vertex shader alone (stream output without a geometry
// shader), with a draw by a shader without stream output between the steps (targets still bound), and the second
// half of the steps appending (offset -1) to a buffer the first half wrote. Prints, for each variant, the particles
// counted and their values, and whether a DrawAuto render of the result lit the expected pixels.
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
   const int N = 1000, STEPS = 10;
   static float init[N * 2][4]; // the buffers are twice N: their initial data too
   for (int i = 0; i < N; i++) {
      init[i][0] = (i % 40) / 20.0f - 0.975f; // x, y: a 40x25 grid over the target
      init[i][1] = (i / 40) / 12.5f - 0.96f;
      init[i][2] = 0;                         // the value the steps add to
      init[i][3] = 1;
   }

   // update: +1 to z, in the vertex shader (streamed out without a geometry shader) or in a geometry shader
   ID3DBlob *vsUpd = compile("float4 main(float4 p : POSITION) : POSITION { return p + float4(0, 0, 1, 0); }", "vs_4_0");
   ID3DBlob *vsPass = compile("float4 main(float4 p : POSITION) : POSITION { return p; }", "vs_4_0");
   ID3DBlob *gsUpd = compile("struct V { float4 p : POSITION; };\n"
                             "[maxvertexcount(1)] void main(point V i[1], inout PointStream<V> o) { V v; v.p = i[0].p + float4(0, 0, 1, 0); o.Append(v); }",
                             "gs_4_0");
   ID3DBlob *vsDraw = compile("float4 main(float4 p : POSITION) : SV_Position { return float4(p.xy, 0.5, 1); }", "vs_4_0");
   ID3DBlob *psDraw = compile("float4 main() : SV_Target { return float4(1, 1, 1, 1); }", "ps_4_0");
   D3D10_SO_DECLARATION_ENTRY so = {"POSITION", 0, 0, 4, 0};
   ID3D10VertexShader *vsUpdS, *vsPassS, *vsDrawS;
   ID3D10GeometryShader *gsVsSo, *gsUpdSo;
   ID3D10PixelShader *psDrawS;
   dev->CreateVertexShader(vsUpd->GetBufferPointer(), vsUpd->GetBufferSize(), &vsUpdS);
   dev->CreateVertexShader(vsPass->GetBufferPointer(), vsPass->GetBufferSize(), &vsPassS);
   dev->CreateVertexShader(vsDraw->GetBufferPointer(), vsDraw->GetBufferSize(), &vsDrawS);
   dev->CreatePixelShader(psDraw->GetBufferPointer(), psDraw->GetBufferSize(), &psDrawS);
   dev->CreateGeometryShaderWithStreamOutput(vsUpd->GetBufferPointer(), vsUpd->GetBufferSize(), &so, 1, 16, &gsVsSo);
   dev->CreateGeometryShaderWithStreamOutput(gsUpd->GetBufferPointer(), gsUpd->GetBufferSize(), &so, 1, 16, &gsUpdSo);
   D3D10_INPUT_ELEMENT_DESC el = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0};
   ID3D10InputLayout *layout;
   dev->CreateInputLayout(&el, 1, vsUpd->GetBufferPointer(), vsUpd->GetBufferSize(), &layout);

   D3D10_TEXTURE2D_DESC td = {64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET, 0, 0};
   ID3D10Texture2D *rt;
   dev->CreateTexture2D(&td, NULL, &rt);
   ID3D10RenderTargetView *rtv;
   dev->CreateRenderTargetView(rt, NULL, &rtv);
   td.Usage = D3D10_USAGE_STAGING;
   td.BindFlags = 0;
   td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
   ID3D10Texture2D *rtRead;
   dev->CreateTexture2D(&td, NULL, &rtRead);
   D3D10_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
   dev->RSSetViewports(1, &vp);

   ID3D10Buffer *readback = buffer(0, N * 16 * 2, NULL, D3D10_USAGE_STAGING, D3D10_CPU_ACCESS_READ);
   ID3D10Query *stats;
   D3D10_QUERY_DESC qd = {D3D10_QUERY_SO_STATISTICS, 0};
   dev->CreateQuery(&qd, &stats);

   static const struct { const char *name; bool gs, between, append, offset; } variants[] = {
      {"geometry shader", true, false, false, false},
      {"vertex shader only", false, false, false, false},
      {"geometry shader, draws between", true, true, false, false},
      {"vertex shader only, draws between", false, true, false, false},
      {"append (offset -1) into one buffer", true, false, true, false},
      {"second half at an offset into one buffer", true, false, false, true},
   };
   int failures = 0;
   for (const auto &v : variants) {
      ID3D10Buffer *b[2] = {buffer(D3D10_BIND_STREAM_OUTPUT | D3D10_BIND_VERTEX_BUFFER, N * 16 * 2, init),
                            buffer(D3D10_BIND_STREAM_OUTPUT | D3D10_BIND_VERTEX_BUFFER, N * 16 * 2, NULL)};
      UINT stride = 16, zero = 0;
      dev->IASetInputLayout(layout);
      dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
      int cur = 0;
      UINT64 written = 0;
      stats->Begin();
      if (v.append || v.offset) {
         // half the particles into b[1] at offset 0, the other half appended (-1): N particles, value 1
         UINT append = (UINT)-1;
         dev->IASetVertexBuffers(0, 1, &b[0], &stride, &zero);
         dev->VSSetShader(vsPassS);
         dev->GSSetShader(gsUpdSo);
         dev->PSSetShader(NULL);
         dev->SOSetTargets(1, &b[1], &zero);
         dev->Draw(N / 2, 0);
         // the second half at an explicit byte offset (3DMark Vantage's particles: new ones into a ring), or appended
         UINT half = N / 2 * 16;
         dev->SOSetTargets(1, &b[1], v.offset ? &half : &append);
         dev->Draw(N / 2, N / 2);
         cur = 1;
      } else {
         for (int s = 0; s < STEPS; s++) {
            dev->IASetVertexBuffers(0, 1, &b[cur], &stride, &zero);
            dev->SOSetTargets(1, &b[1 - cur], &zero);
            dev->VSSetShader(v.gs ? vsPassS : vsUpdS);
            dev->GSSetShader(v.gs ? gsUpdSo : gsVsSo);
            dev->PSSetShader(NULL);
            if (s == 0)
               dev->Draw(N, 0);
            else
               dev->DrawAuto();
            if (v.between) {
               // a draw by shaders without stream output, the targets still bound: writes nothing to them
               dev->OMSetRenderTargets(1, &rtv, NULL);
               dev->VSSetShader(vsDrawS);
               dev->GSSetShader(NULL);
               dev->PSSetShader(psDrawS);
               dev->Draw(N, 0);
               dev->OMSetRenderTargets(0, NULL, NULL);
            }
            ID3D10Buffer *none = NULL;
            dev->SOSetTargets(1, &none, &zero);
            cur = 1 - cur;
         }
      }
      stats->End();
      ID3D10Buffer *none = NULL;
      dev->SOSetTargets(1, &none, &zero);
      D3D10_QUERY_DATA_SO_STATISTICS st = {};
      while (stats->GetData(&st, sizeof st, 0) == S_FALSE) Sleep(1);
      written = st.NumPrimitivesWritten;

      // the values
      int expect = v.append || v.offset ? 1 : STEPS, good = 0, wrong = 0;
      float first = -1;
      dev->CopyResource(readback, b[cur]);
      float (*p)[4];
      if (SUCCEEDED(readback->Map(D3D10_MAP_READ, 0, (void **)&p))) {
         first = p[0][2];
         for (int i = 0; i < N; i++) {
            if (p[i][2] == (float)expect) good++;
            else wrong++;
         }
         readback->Unmap();
      }

      // the result drawn with DrawAuto: one lit pixel per particle cell expected
      float black[4] = {0, 0, 0, 0};
      dev->ClearRenderTargetView(rtv, black);
      dev->OMSetRenderTargets(1, &rtv, NULL);
      dev->IASetVertexBuffers(0, 1, &b[cur], &stride, &zero);
      dev->VSSetShader(vsDrawS);
      dev->GSSetShader(NULL);
      dev->PSSetShader(psDrawS);
      // (DrawAuto after a capture made at an offset draws what that capture wrote; the whole buffer counted here)
      if (v.offset)
         dev->Draw(N, 0);
      else
         dev->DrawAuto();
      dev->OMSetRenderTargets(0, NULL, NULL);
      dev->CopyResource(rtRead, rt);
      int lit = 0;
      D3D10_MAPPED_TEXTURE2D m;
      if (SUCCEEDED(rtRead->Map(0, D3D10_MAP_READ, 0, &m))) {
         for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
               if (((unsigned char *)m.pData)[y * m.RowPitch + x * 4]) lit++;
         rtRead->Unmap(0);
      }
      bool ok = good == N && lit >= 900;
      failures += !ok;
      printf("%-38s %s: %d of %d particles at %d (first %g), %llu primitives written, %d pixels lit\n", v.name,
             ok ? "ok  " : "FAIL", good, N, expect, first, (unsigned long long)written, lit);
      b[0]->Release();
      b[1]->Release();
   }
   printf("%d failures\n", failures);
   return failures;
}
