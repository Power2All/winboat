// dx-test's ray tracing library (lib_6_3): ray i of 2 starts at x = 0 (hits the triangle at z = 1) or x = 5 (misses);
// each writes its payload to output[i]. The hit group's local root signature has one constant (the alpha).
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<float4> output : register(u0);
cbuffer Local : register(b0) { float alpha; };

struct Payload { float4 color; };

[shader("raygeneration")]
void RayGen()
{
   uint i = DispatchRaysIndex().x;
   RayDesc ray;
   ray.Origin = float3(i == 0 ? 0 : 5, 0, 0);
   ray.Direction = float3(0, 0, 1);
   ray.TMin = 0;
   ray.TMax = 10;
   Payload p = { float4(0, 0, 0, 0) };
   TraceRay(scene, RAY_FLAG_NONE, 0xff, 0, 1, 0, ray, p);
   output[i] = p.color;
}

[shader("miss")]
void Miss(inout Payload p)
{
   p.color = float4(0.75, 0, 0.25, 1);
}

[shader("closesthit")]
void Hit(inout Payload p, BuiltInTriangleIntersectionAttributes a)
{
   p.color = float4(a.barycentrics, RayTCurrent(), alpha);
}
