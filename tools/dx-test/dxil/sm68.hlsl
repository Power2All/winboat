// dx-test's shader model 6.8 pipeline (Direct3D 12): a triangle over the whole target from a vertex shader that reads
// the draw's start vertex and start instance (SV_StartVertexLocation, SV_StartInstanceLocation, 6.8); the pixel shader
// writes them as red and green, and blue from IsHelperLane (6.7: never a helper lane where it writes)
struct V { float4 p : SV_Position; nointerpolation uint2 start : START; };

V VS(uint id : SV_VertexID, int startVertex : SV_StartVertexLocation, uint startInstance : SV_StartInstanceLocation)
{
    V v;
    uint i = id - startVertex;
    float2 uv = float2((i << 1) & 2, i & 2);
    v.p = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    v.start = uint2(startVertex, startInstance);
    return v;
}

float4 PS(V v) : SV_Target
{
    return float4(v.start.x / 255.0, v.start.y / 255.0, IsHelperLane() ? 1.0 : 0.5, 1);
}
