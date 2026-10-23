// dx-test's shader model 6.8 comparison sampling (Direct3D 12): SampleCmpGrad (6.8, SampleCmpGradientAndBiasSupported)
// of a 1x1 depth of 0.5 with a LESS comparison sampler, at references 0.25 (passes: 1) and 0.75 (fails: 0)
Texture2D<float> depth : register(t0);
SamplerComparisonState cmp : register(s0);
RWStructuredBuffer<float> o : register(u0);

[numthreads(1, 1, 1)] void CS()
{
    o[0] = depth.SampleCmpGrad(cmp, float2(0.5, 0.5), 0.25, float2(0, 0), float2(0, 0));
    o[1] = depth.SampleCmpGrad(cmp, float2(0.5, 0.5), 0.75, float2(0, 0), float2(0, 0));
}
