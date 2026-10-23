// dx-test's mesh shader pipelines (Direct3D 12, shader model 6.5): one triangle over the whole target, its color from
// the mesh shader alone (MS_SOLO), or from an amplification shader's payload (AS + MS_PAYLOAD)
struct Vertex { float4 position : SV_Position; float4 color : COLOR0; };
struct Payload { float4 color; };

// one triangle over the whole target in a color (a macro: only entry points take mesh outputs)
#define FULL_TRIANGLE(v, t, c)                            \
    SetMeshOutputCounts(3, 1);                            \
    v[0].position = float4(-1, -1, 0.5, 1);               \
    v[1].position = float4(3, -1, 0.5, 1);                \
    v[2].position = float4(-1, 3, 0.5, 1);                \
    v[0].color = c;                                       \
    v[1].color = c;                                       \
    v[2].color = c;                                       \
    t[0] = uint3(0, 1, 2);

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void MS_SOLO(out vertices Vertex v[3], out indices uint3 t[1])
{
    FULL_TRIANGLE(v, t, float4(0.25, 0.5, 0.75, 1))
}

groupshared Payload payload;

[numthreads(1, 1, 1)]
void AS()
{
    payload.color = float4(0.75, 0.5, 0.25, 1);
    DispatchMesh(1, 1, 1, payload);
}

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void MS_PAYLOAD(in payload Payload p, out vertices Vertex v[3], out indices uint3 t[1])
{
    FULL_TRIANGLE(v, t, p.color)
}

float4 PS(Vertex v) : SV_Target
{
    return v.color;
}
