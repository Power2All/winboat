// dx-test's work graph (lib_6_8): Entry (broadcasting, 2 groups of 8 threads) sends each thread a record for Leaf (a
// thread node), which writes output[index]; Direct, a second entry point, writes output[32 + index]
struct EntryRecord { uint base; };
struct LeafRecord { uint index; uint value; };
RWStructuredBuffer<uint> output : register(u0);

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeDispatchGrid(2, 1, 1)]
[NumThreads(8, 1, 1)]
void Entry(DispatchNodeInputRecord<EntryRecord> input, uint id : SV_DispatchThreadID,
           [MaxRecords(8)] NodeOutput<LeafRecord> Leaf)
{
   ThreadNodeOutputRecords<LeafRecord> rec = Leaf.GetThreadNodeOutputRecords(1);
   rec.Get().index = id;
   rec.Get().value = input.Get().base + id * id;
   rec.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Leaf(ThreadNodeInputRecord<LeafRecord> input)
{
   output[input.Get().index] = input.Get().value;
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void Direct(ThreadNodeInputRecord<LeafRecord> input)
{
   output[32 + input.Get().index] = input.Get().value;
}
