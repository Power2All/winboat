// Shader model 6.6's 64-bit atomics, as Unreal Engine 5's Nanite and virtual shadow maps use them: 64 threads
// InterlockedMax a 64-bit value (the thread's number + 1 in the high half, 100 - its number in the low half) into a
// R32G32_UINT texture reached through the descriptor heap (ResourceDescriptorHeap) and into a raw buffer at 8; thread
// 0 then stores the texture's value at 0
RWByteAddressBuffer buf : register(u0);

[numthreads(64, 1, 1)]
void CS(uint i : SV_GroupIndex)
{
   RWTexture2D<uint64_t> tex = ResourceDescriptorHeap[0];
   uint64_t v = ((uint64_t)(i + 1) << 32) | (100 - i);
   InterlockedMax(tex[uint2(0, 0)], v);
   buf.InterlockedMax64(8, v);
   DeviceMemoryBarrierWithGroupSync();
   if (i == 0) buf.Store<uint64_t>(0, tex[uint2(0, 0)]);
}
