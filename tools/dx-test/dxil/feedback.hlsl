// dx-test's sampler feedback shader (cs_6_5): records a sample of mip 0 at (0.1, 0.1) in a MIN_MIP feedback map
Texture2D<float4> tex : register(t0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> feedback : register(u0);
SamplerState samp : register(s0);

[numthreads(1, 1, 1)]
void CS()
{
   feedback.WriteSamplerFeedbackLevel(tex, samp, float2(0.1, 0.1), 0);
}
