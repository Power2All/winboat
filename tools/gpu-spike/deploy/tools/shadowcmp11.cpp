// shadowcmp11: Direct3D 11 comparison sampling of a depth Texture2DArray (D32_FLOAT, 2 layers cleared to 0.25 and
// 0.75; an R32_FLOAT view, a GREATER_EQUAL comparison sampler, SampleCmpLevelZero) from a compute shader, also with a
// plain sampler on the same view in the same shader (GravityMark's shadow lookups do both); prints each result
// against the expected one. Build: x86_64-w64-mingw32-g++ -O2 -static shadowcmp11.cpp -o shadowcmp11.exe -ld3d11
// -ld3dcompiler_47 -ldxgi
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

#define CHECK(x)                                                                                                       \
   do {                                                                                                               \
      HRESULT hr_ = (x);                                                                                              \
      if (FAILED(hr_)) {                                                                                              \
         printf("%s: 0x%08lx\n", #x, (unsigned long)hr_);                                                            \
         return 1;                                                                                                    \
      }                                                                                                               \
   } while (0)

static const char *cs_src =
   "Texture2DArray<float> shadow : register(t3);\n"
   "SamplerComparisonState cmp : register(s1);\n"
   "SamplerState plain : register(s0);\n"
   "RWStructuredBuffer<float> outp : register(u0);\n"
   "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
   "   float refs[3] = { 0.1, 0.5, 0.9 };\n"
   "   for (uint l = 0; l < 2; l++) {\n"
   "      for (uint r = 0; r < 3; r++)\n"
   "         outp[l * 4 + r] = shadow.SampleCmpLevelZero(cmp, float3(0.5, 0.5, l), refs[r]);\n"
   "      outp[l * 4 + 3] = shadow.SampleLevel(plain, float3(0.5, 0.5, l), 0);\n"
   "   }\n"
   "}\n";

int main()
{
   setvbuf(stdout, nullptr, _IONBF, 0);
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
   CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                           &ctx));
   D3D11_TEXTURE2D_DESC td = {64, 64, 1, 2, DXGI_FORMAT_R32_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0};
   ID3D11Texture2D *tex;
   CHECK(dev->CreateTexture2D(&td, nullptr, &tex));
   const float clears[2] = {0.25f, 0.75f};
   for (UINT l = 0; l < 2; l++) {
      D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
      dd.Format = DXGI_FORMAT_D32_FLOAT;
      dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
      dd.Texture2DArray.FirstArraySlice = l;
      dd.Texture2DArray.ArraySize = 1;
      ID3D11DepthStencilView *dsv;
      CHECK(dev->CreateDepthStencilView(tex, &dd, &dsv));
      ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, clears[l], 0);
      dsv->Release();
   }
   D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
   sd.Format = DXGI_FORMAT_R32_FLOAT;
   sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
   sd.Texture2DArray.MipLevels = 1;
   sd.Texture2DArray.ArraySize = 2;
   ID3D11ShaderResourceView *srv;
   CHECK(dev->CreateShaderResourceView(tex, &sd, &srv));
   D3D11_SAMPLER_DESC cmpd = {D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
                              D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1,
                              D3D11_COMPARISON_GREATER_EQUAL, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
   D3D11_SAMPLER_DESC plaind = cmpd;
   plaind.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
   plaind.ComparisonFunc = D3D11_COMPARISON_NEVER;
   ID3D11SamplerState *cmp, *plain;
   CHECK(dev->CreateSamplerState(&cmpd, &cmp));
   CHECK(dev->CreateSamplerState(&plaind, &plain));

   ID3DBlob *code, *err = nullptr;
   if (FAILED(D3DCompile(cs_src, strlen(cs_src), "cs", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
      printf("compile: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
      return 1;
   }
   ID3D11ComputeShader *cs;
   CHECK(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
   D3D11_BUFFER_DESC bd = {8 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                           D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
   ID3D11Buffer *buf, *stage;
   CHECK(dev->CreateBuffer(&bd, nullptr, &buf));
   D3D11_BUFFER_DESC sbd = {8 * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
   CHECK(dev->CreateBuffer(&sbd, nullptr, &stage));
   ID3D11UnorderedAccessView *uav;
   CHECK(dev->CreateUnorderedAccessView(buf, nullptr, &uav));

   ctx->CSSetShader(cs, nullptr, 0);
   ctx->CSSetShaderResources(3, 1, &srv);
   ID3D11SamplerState *samplers[2] = {plain, cmp};
   ctx->CSSetSamplers(0, 2, samplers);
   ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
   ctx->Dispatch(1, 1, 1);
   ctx->CopyResource(stage, buf);
   D3D11_MAPPED_SUBRESOURCE m;
   CHECK(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m));
   const float *r = (const float *)m.pData;
   const float refs[3] = {0.1f, 0.5f, 0.9f};
   int bad = 0;
   for (int l = 0; l < 2; l++) {
      for (int k = 0; k < 3; k++) {
         float want = refs[k] >= clears[l] ? 1.0f : 0.0f;
         bad += r[l * 4 + k] != want;
         printf("layer %d ref %.1f >= %.2f: %g (want %g)\n", l, refs[k], clears[l], r[l * 4 + k], want);
      }
      bad += r[l * 4 + 3] != clears[l];
      printf("layer %d plain: %g (want %g)\n", l, r[l * 4 + 3], clears[l]);
   }
   ctx->Unmap(stage, 0);
   printf("%s\n", bad ? "FAIL" : "PASS");
   return bad ? 1 : 0;
}
