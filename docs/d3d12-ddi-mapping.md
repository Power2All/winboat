# D3D12 DDI 0052 → vkd3d-proton API mapping reference

Working reference for the WinBoat D3D12 UMD (stage C). Target: `D3D12DDI_SUPPORTED_0052`
(interface R5, build 2). The UMD receives DDI calls from the MS D3D12 runtime and replays
them on vkd3d-proton's COM API objects (`ID3D12Device*`, `ID3D12GraphicsCommandList*`, ...).

Sources used (all read-only):
- `~/.cache/claude-winboat/wdk-inc/d3d12umddi.h` (tables: device `D3D12DDI_DEVICE_FUNCS_CORE_0052` @7404,
  command list `D3D12DDI_COMMAND_LIST_FUNCS_3D_0052` @7548, queue `D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001` @2729),
  `dxgiddi.h` (`DXGI1_4_DDI_BASE_FUNCTIONS` @737), `d3d10umddi.h`.
- `~/.cache/claude-winboat/wdk/d3d11TokenizedProgramFormat.hpp` (`D3D10_SB_NAME`, needed by `d3d10umddi.h`; it is
  NOT in `wdk-inc/`, which is required to compile the DDI headers).
- vkd3d-proton `include/vkd3d_d3d12.idl`, `include/vkd3d.h`, `libs/vkd3d/*.c`, `libs/vkd3d-shader/*.c`.
- MS sample `graphics-driver-samples/compute-only-sample/cosumd12/` (implements 0052; compute-only, so many
  graphics entries there are `UNEXPECTED_DDI()` stubs).

Confidence markers: **[hdr]** = read from the header, **[sample]** = observed in cosumd12, **[vkd3d]** = read in
vkd3d-proton source, **[infer]** = inference / must be verified on a real runtime.

---

## 0. Key facts (read this first)

1. **Every DDI object lives in runtime-allocated memory.** `pfnCalcPrivate*Size(args)` returns a byte count, the
   runtime allocates it, then `pfnCreate*(..., hX)` where `hX.pDrvPrivate` points to that memory; the driver does
   placement-new there. `pfnDestroy*` must run the destructor but must NOT free the memory. [hdr][sample]
2. **Handles are tiny structs.** `D3D10DDI_H(T)` = `struct T { void* pDrvPrivate; }` (driver objects);
   `D3D10DDI_HRT(T)` = `struct T { const void* handle; }` (runtime objects: `D3D12DDI_HRTCOMMANDQUEUE`,
   `D3D12DDI_HRTCOMMANDLIST`, `D3D12DDI_HRTPIPELINESTATE`, `D3D12DDI_HRTTABLE`, `D3D12DDI_HRTRESOURCE`, ...).
   Store the HRT handles you are given; they are the first argument of runtime callbacks
   (`pfnSetCommandListDDITableCb(hRTCommandList, hRTTable)`, `pfnCreateContextCb(hRTCommandQueue, ...)`,
   `pfnSetErrorCb(hRTDevice, hr)`, `pfnSetCommandListErrorCb`, `pfnAllocateCb` with `hResource = hRTResource`).
3. **CPU/GPU descriptor handles, descriptor sizes and GPU VAs are defined by the driver** and the runtime passes
   them back unchanged. So the cheapest correct mapping is: return vkd3d-proton's own values
   (`GetCPUDescriptorHandleForHeapStart`, `GetGPUDescriptorHandleForHeapStart`,
   `GetDescriptorHandleIncrementSize`, `ID3D12Resource::GetGPUVirtualAddress`) and pass every DDI handle/VA straight
   through to vkd3d. No translation tables needed.
4. **Shaders arrive stripped**: `D3D12DDIARG_CREATE_SHADER_0026.pShaderCode` is the raw program chunk
   (`_In_reads_(pShaderCode[1])` — dword[1] is the length in DWORDs: SM4/5 token stream or DXIL program header),
   not a DXBC container, and signatures arrive as register/sysval lists **without semantic names**. vkd3d-proton
   requires a DXBC container (`DXBC` magic, ISGN/OSGN, SHEX/DXIL) and matches input layouts by semantic name.
   => the UMD must synthesize a container + signatures with invented names (section 4). [hdr][vkd3d][infer]
5. **Input layouts have no semantic names either** (`D3D12DDIARG_INPUT_ELEMENT_DESC.InputRegister`), and
   **view descs lose array/MS-ness** (`D3D12DDI_RD_TEXTURE2D` covers 2D/2DArray/2DMS/2DMSArray). Both need
   reconstruction.
6. **Fences are runtime-managed** (KM monitored fences). The driver never sees `ID3D12CommandQueue::Signal/Wait`
   values to implement; the runtime signals/waits on the KM context(s) the driver created for the queue
   (`pfnCreateContextCb(hRTCommandQueue, ...)`). If vkd3d work is not submitted through that KM context, runtime
   fence signals will race ahead of the GPU work. This is the biggest architectural constraint (section 2d).
7. **Several enums are NOT numerically identical** even though most are. Not identical:
   `D3D12DDI_CPU_PAGE_PROPERTY` (API = DDI+1), `D3D12DDI_MEMORY_POOL` (API = DDI+1), `D3D12DDI_HEAP_FLAGS`
   (allow-flags vs API deny-flags), `D3D12DDI_RESOURCE_FLAGS_0003` (bit layout differs),
   `D3D12DDI_DESCRIPTOR_HEAP_FLAGS` (SHADER_VISIBLE = 0x2 vs 0x1), `D3D12DDI_COMMAND_QUEUE_FLAGS` (bitmask vs list
   type enum), `CheckFormatSupport` output bits (D3D10 DDI bits, not `D3D12_FORMAT_SUPPORT1`), `D3D10_SB_NAME`
   tess-factor values vs `D3D_NAME`. Table in 0.1.

### 0.1 Enum / struct compatibility table

| DDI type | API type | Identical? |
|---|---|---|
| `D3D12DDI_RESOURCE_TYPE` (BUFFER=1..TEX3D=4) | `D3D12_RESOURCE_DIMENSION` | yes |
| `D3D12DDI_TEXTURE_LAYOUT` (0..3) | `D3D12_TEXTURE_LAYOUT` | yes (0x100+ device-dependent has no API value) |
| `D3D12DDI_RESOURCE_STATES` | `D3D12_RESOURCE_STATES` | yes (all bits incl. video, SRS 0x01000000, RTAS 0x400000) |
| `D3D12DDI_RESOURCE_FLAGS_0003` | `D3D12_RESOURCE_FLAGS` | **no** — see `pfnCreateHeapAndResource` |
| `D3D12DDI_HEAP_FLAGS` | `D3D12_HEAP_FLAGS` | **no** — DDI says what is *allowed* |
| `D3D12DDI_CPU_PAGE_PROPERTY` (NOT_AVAILABLE=0,WC=1,WB=2) | `D3D12_CPU_PAGE_PROPERTY` (UNKNOWN=0,NA=1,WC=2,WB=3) | **no**, API = DDI+1 |
| `D3D12DDI_MEMORY_POOL` (L0=0,L1=1) | `D3D12_MEMORY_POOL` (UNKNOWN=0,L0=1,L1=2) | **no**, API = DDI+1 |
| `D3D12DDI_DESCRIPTOR_HEAP_TYPE` | `D3D12_DESCRIPTOR_HEAP_TYPE` | yes |
| `D3D12DDI_DESCRIPTOR_HEAP_FLAGS` (CPU_VISIBLE=1, SHADER_VISIBLE=2) | `D3D12_DESCRIPTOR_HEAP_FLAGS` (SHADER_VISIBLE=1) | **no** |
| `D3D12DDI_COMMAND_LIST_TYPE` (DIRECT=0,BUNDLE=1) + `D3D12DDI_COMMAND_QUEUE_FLAGS` | `D3D12_COMMAND_LIST_TYPE` (DIRECT,BUNDLE,COMPUTE,COPY) | **no** |
| `D3D12DDI_FILTER`, `D3D12DDI_TEXTURE_ADDRESS_MODE`, `D3D12DDI_COMPARISON_FUNC`, `D3D12DDI_STATIC_BORDER_COLOR` | `D3D12_*` | yes |
| `D3D12DDI_BLEND`, `_BLEND_OP`, `_LOGIC_OP`, `_STENCIL_OP`, `_FILL_MODE`, `_CULL_MODE`, `_DEPTH_WRITE_MASK`, `_CONSERVATIVE_RASTERIZATION_MODE`, `_INPUT_CLASSIFICATION` | `D3D12_*` | yes |
| `D3D12DDI_PRIMITIVE_TOPOLOGY` | `D3D_PRIMITIVE_TOPOLOGY` | yes (DDI has TRIANGLEFAN=6, no API value) |
| `D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE`, `_INDEX_BUFFER_STRIP_CUT_VALUE` | `D3D12_*` | yes |
| `D3D12DDI_QUERY_TYPE`, `D3D12DDI_QUERY_HEAP_TYPE`, `D3D12DDI_PREDICATION_OP` | `D3D12_*` | yes |
| `D3D12DDI_RESOLVE_MODE`, `D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032` | `D3D12_*` | yes |
| `D3D12DDI_ROOT_PARAMETER_TYPE`, `_SHADER_VISIBILITY`, `_DESCRIPTOR_RANGE_TYPE`, `_ROOT_SIGNATURE_FLAGS`, `_DESCRIPTOR_RANGE_FLAGS` (incl. 0x10000), `_ROOT_DESCRIPTOR_FLAGS` | `D3D12_*` | yes |
| `D3D12DDI_ROOT_SIGNATURE_VERSION_1_1` (=2) | `D3D_ROOT_SIGNATURE_VERSION_1_1` (=2) | yes |
| `D3D12DDI_INDIRECT_ARGUMENT_TYPE` + `D3D12DDI_INDIRECT_ARGUMENT_DESC` | `D3D12_INDIRECT_ARGUMENT_*` | yes (layout too) |
| `D3D12DDI_TILE_RANGE_FLAGS`, `_TILE_MAPPING_FLAGS`, `_TILE_COPY_FLAGS`, `D3D12DDI_TILED_RESOURCE_COORDINATE`, `D3D12DDI_TILE_REGION_SIZE` | `D3D12_*` | yes (layout too) |
| `D3D12DDI_CREATE_DEPTH_STENCIL_VIEW_FLAGS` (RO depth 1, RO stencil 2) | `D3D12_DSV_FLAGS` | yes |
| `D3D12DDI_BUFFER_SRV_FLAGS` / `_BUFFER_UAV_FLAGS` (RAW=1) | `D3D12_BUFFER_SRV_FLAGS` / `_UAV_FLAGS` | yes |
| `Shader4ComponentMapping` encoding (`D3D12DDI_ENCODE_SHADER_4_COMPONENT_MAPPING`, always-set bit 1<<12) | `D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING` | yes |
| `D3D10_DDI_CLEAR_DEPTH`=1 / `_STENCIL`=2 | `D3D12_CLEAR_FLAGS` | yes |
| `D3D10_SB_NAME` 0..10 | `D3D_NAME` 0..10 | yes; **11..22 differ** (per-component tess factors) |
| `D3D10_SB_REGISTER_COMPONENT_TYPE`, `D3D11_SB_OPERAND_MIN_PRECISION` | `D3D_REGISTER_COMPONENT_TYPE`, `D3D_MIN_PRECISION` | yes |

Layout-identical structs (can be `reinterpret_cast`): `D3D12DDI_VIEWPORT`↔`D3D12_VIEWPORT`, `D3D12DDI_RECT`(=RECT)↔`D3D12_RECT`,
`D3D12DDI_BOX`↔`D3D12_BOX`, `D3D12DDI_INDEX_BUFFER_VIEW`, `D3D12DDI_VERTEX_BUFFER_VIEW`, `D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW`,
`D3D12DDI_CONSTANT_BUFFER_VIEW_DESC` (has explicit `Padding`; same 16-byte layout), `D3D12DDI_SAMPLER_DESC`,
`D3D12DDI_STATIC_SAMPLER`↔`D3D12_STATIC_SAMPLER_DESC`, `D3D12DDI_CLEAR_VALUES`↔`D3D12_CLEAR_VALUE`,
`D3D12DDIARG_DISCARD_RESOURCE_0003`↔`D3D12_DISCARD_REGION`, `D3D12DDI_SAMPLE_POSITION`,
`D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032`, `D3D12DDI_RENDER_TARGET_BLEND_DESC`, `D3D12DDI_DEPTH_STENCILOP_DESC`,
the whole root-signature 1.1 tree (section 3). Prefix-compatible: `D3D12DDI_BLEND_DESC_0010` (API desc + trailing
`LibraryReference`). NOT compatible: `D3D12DDI_DEPTH_STENCIL_DESC_0025`, `D3D12DDI_RASTERIZER_DESC_0010`,
`D3D12DDIARG_INPUT_ELEMENT_DESC`, all view-create args, `D3D12DDIARG_RESOURCE_BARRIER_0022`.

### 0.2 Our driver objects (suggested)

`H(x)` below means `static_cast<Obj*>(x.pDrvPrivate)`. Each object stores the vkd3d COM pointer(s):

| DDI handle | Our object holds |
|---|---|
| `D3D12DDI_HDEVICE` | `ID3D12Device*` (latest iface, e.g. `ID3D12Device5`), callbacks, `hRTDevice`, `hRTTable` for cmd lists |
| `D3D12DDI_HCOMMANDQUEUE` | `ID3D12CommandQueue*`, `hRTCommandQueue`, KM context handle(s), internal vkd3d `ID3D12Fence*` for sync |
| `D3D12DDI_HCOMMANDPOOL_0040` | `ID3D12CommandAllocator*` per list type (lazily created) |
| `D3D12DDI_HCOMMANDRECORDER_0040` | `QueueFlags`, pointer to current target pool |
| `D3D12DDI_HCOMMANDLIST` | `ID3D12GraphicsCommandList*` (cast up to `...List4`), `hRTCommandList`, API type |
| `D3D12DDI_HHEAP` | `ID3D12Heap*` *or* (committed) borrowed `ID3D12Resource*`; CPU ptr for `MapHeap` |
| `D3D12DDI_HRESOURCE` | `ID3D12Resource*`, `D3D12_RESOURCE_DESC`, owning heap ptr, `hRTResource` |
| `D3D12DDI_HDESCRIPTORHEAP` | `ID3D12DescriptorHeap*` |
| `D3D12DDI_HROOTSIGNATURE` | `ID3D12RootSignature*` |
| `D3D12DDI_HSHADER` | synthesized DXBC container bytes + stage + (SO decl) |
| `D3D12DDI_HELEMENTLAYOUT` / `HBLENDSTATE` / `HRASTERIZERSTATE` / `HDEPTHSTENCILSTATE` | converted API desc (`D3D12_INPUT_ELEMENT_DESC[]`, `D3D12_BLEND_DESC`, `D3D12_RASTERIZER_DESC`, `D3D12_DEPTH_STENCIL_DESC1`) |
| `D3D12DDI_HPIPELINESTATE` | `ID3D12PipelineState*`, `hRTPipelineState` |
| `D3D12DDI_HQUERYHEAP` | `ID3D12QueryHeap*` |
| `D3D12DDI_HCOMMANDSIGNATURE` | `ID3D12CommandSignature*` |
| `D3D12DDI_HFENCE` | nothing useful (copy of `D3D12DDI_FENCE` placements) |

---

## 1. Function tables

Format per entry: **`pfnName`** — `PFN type` — `RET (params)`; then args, API mapping, conversion, what the
runtime relies on. "no API" = no ID3D12 equivalent.

### 1.1 `D3D12DDI_DEVICE_FUNCS_CORE_0052` (105 entries, table order)

1. **`pfnCheckFormatSupport`** — `PFND3D12DDI_CHECKFORMATSUPPORT` — `VOID (D3D12DDI_HDEVICE, DXGI_FORMAT, _Out_ UINT*)`
   - API: `ID3D12Device::CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, D3D12_FEATURE_DATA_FORMAT_SUPPORT{Format, Support1, Support2})`.
   - Conv: output is **D3D10/11 DDI format bits** (`d3d10umddi.h`), not `D3D12_FORMAT_SUPPORT1`:
     `D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE`(1)←S1 `SHADER_SAMPLE`(0x200); `_RENDERTARGET`(2)←S1 `RENDER_TARGET`(0x4000);
     `_BLENDABLE`(4)←S1 `BLENDABLE`(0x8000); `_MULTISAMPLE_RENDERTARGET`(8)←S1 `MULTISAMPLE_RENDERTARGET`(0x200000);
     `_MULTISAMPLE_LOAD`(0x10)←S1 `MULTISAMPLE_LOAD`(0x400000); `D3D11_1DDI_FORMAT_SUPPORT_DECODER_OUTPUT`(0x20)←S1 0x08000000;
     `_VIDEO_PROCESSOR_OUTPUT`(0x40)←S1 0x10000000; `_VIDEO_PROCESSOR_INPUT`(0x80)←S1 0x20000000; `_VERTEX_BUFFER`(0x100)←S1 `IA_VERTEX_BUFFER`(2);
     `_UAV_WRITES`(0x200)←S1 `TYPED_UNORDERED_ACCESS_VIEW`(0x02000000); `_BUFFER`(0x400)←S1 `BUFFER`(1); `_VIDEO_ENCODER`(0x1000)←S1 0x40000000;
     `_OUTPUT_MERGER_LOGIC_OP`(0x2000)←S2 0x100; `_SHADER_GATHER`(0x4000)←S1 0x800000; `_MULTIPLANE_OVERLAY`(0x8000)←S2 0x4000;
     `D3DWDDM1_3DDI_FORMAT_SUPPORT_TILED`(0x10000)←S2 0x200; `D3DWDDM2_0DDI_FORMAT_SUPPORT_UAV_READS`(0x20000)←S2 `UAV_TYPED_LOAD`(0x40);
     `D3D11_1DDI_FORMAT_SUPPORT_DISPLAY`(0x200000)←S1 `DISPLAY`(0x80000). Unsupported → 0
     (`D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED` 0x80000000 is documented only for `R10G10B10_XR_BIAS_A2_UNORM`).
   - Runtime relies on it to build API format-support answers (texture dims etc. come from runtime tables). [hdr][sample]
2. **`pfnCheckMultisampleQualityLevels`** — `PFND3D12DDI_CHECKMULTISAMPLEQUALITYLEVELS` — `VOID (D3D12DDI_HDEVICE hDevice, DXGI_FORMAT Format, UINT SampleCount, D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS Flags, _Out_ UINT* pNumQualityLevels)`
   - API: `CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, {Format, SampleCount, Flags, NumQualityLevels})`.
     Flags identical (`..._TILED_RESOURCE`=1). Return 0 levels if unsupported.
3. **`pfnGetMipPacking`** — `PFND3D12DDI_GETMIPPACKING` — `VOID (D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hTiledResource, _Out_ UINT* pNumPackedMips, _Out_ UINT* pNumTilesForPackedMips)`
   - API: `ID3D12Device::GetResourceTiling(res, &numTiles, &packed, &tileShape, &subCount, 0, subTilings)`; output
     `packed.NumPackedMips`, `packed.NumTilesForPackedMips`. Only called for reserved resources.
4. **`pfnCalcPrivateElementLayoutSize`** — `PFND3D12DDI_CALCPRIVATEELEMENTLAYOUTSIZE_0010` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATEELEMENTLAYOUT_0010*)`
   - Args: `D3D12DDIARG_CREATEELEMENTLAYOUT_0010 { CONST D3D12DDIARG_INPUT_ELEMENT_DESC* pVertexElements; UINT NumElements; D3D12DDI_LIBRARY_REFERENCE_0010 LibraryReference; }`
   - Return `sizeof(obj) + NumElements*sizeof(D3D12_INPUT_ELEMENT_DESC)` (store the array inline; names are synthetic constants).
5. **`pfnCreateElementLayout`** — `PFND3D12DDI_CREATEELEMENTLAYOUT_0010` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATEELEMENTLAYOUT_0010*, D3D12DDI_HELEMENTLAYOUT)`
   - `D3D12DDIARG_INPUT_ELEMENT_DESC { UINT InputSlot; UINT AlignedByteOffset; DXGI_FORMAT Format; D3D12DDI_INPUT_CLASSIFICATION InputSlotClass; UINT InstanceDataStepRate; UINT InputRegister; }`
     — **no SemanticName/SemanticIndex**; instead `InputRegister` = VS input register (v#).
   - API: none directly; becomes `D3D12_INPUT_LAYOUT_DESC` inside the PSO. Convert each to
     `D3D12_INPUT_ELEMENT_DESC{ SemanticName="WBATTR", SemanticIndex=InputRegister, Format, InputSlot, AlignedByteOffset, InputSlotClass, InstanceDataStepRate }`
     and emit the same (`"WBATTR"`, register) pairs in the synthesized VS ISGN (section 4). vkd3d maps attribute
     `location = signature_element->register_index` found by name/index (state.c ~5410). `0xFFFFFFFF` offsets
     (`D3D12_APPEND_ALIGNED_ELEMENT`) are handled by vkd3d if the runtime passes them through. No return value.
6. **`pfnDestroyElementLayout`** — `PFND3D12DDI_DESTROYELEMENTLAYOUT` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HELEMENTLAYOUT)` — run destructor.
7. **`pfnCalcPrivateBlendStateSize`** — `PFND3D12DDI_CALCPRIVATEBLENDSTATESIZE_0010` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDI_BLEND_DESC_0010*)`
8. **`pfnCreateBlendState`** — `PFND3D12DDI_CREATEBLENDSTATE_0010` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDI_BLEND_DESC_0010*, D3D12DDI_HBLENDSTATE)`
   - `D3D12DDI_BLEND_DESC_0010 { BOOL AlphaToCoverageEnable; BOOL IndependentBlendEnable; D3D12DDI_RENDER_TARGET_BLEND_DESC RenderTarget[8]; D3D12DDI_LIBRARY_REFERENCE_0010 LibraryReference; }`
   - `D3D12DDI_RENDER_TARGET_BLEND_DESC` = `D3D12_RENDER_TARGET_BLEND_DESC` layout (BlendEnable, LogicOpEnable, SrcBlend, DestBlend, BlendOp, SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha, LogicOp, UINT8 RenderTargetWriteMask), enums identical.
   - Store `D3D12_BLEND_DESC` = memcpy of the first `sizeof(D3D12_BLEND_DESC)` bytes.
9. **`pfnDestroyBlendState`** — `PFND3D12DDI_DESTROYBLENDSTATE` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HBLENDSTATE)`
10. **`pfnCalcPrivateDepthStencilStateSize`** — `PFND3D12DDI_CALCPRIVATEDEPTHSTENCILSTATESIZE_0025` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDI_DEPTH_STENCIL_DESC_0025*)`
11. **`pfnCreateDepthStencilState`** — `PFND3D12DDI_CREATEDEPTHSTENCILSTATE_0025` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDI_DEPTH_STENCIL_DESC_0025*, D3D12DDI_HDEPTHSTENCILSTATE)`
    - `D3D12DDI_DEPTH_STENCIL_DESC_0025 { BOOL DepthEnable; D3D12DDI_DEPTH_WRITE_MASK DepthWriteMask; D3D12DDI_COMPARISON_FUNC DepthFunc; BOOL StencilEnable; BOOL FrontEnable; BOOL BackEnable; UINT8 StencilReadMask; UINT8 StencilWriteMask; D3D12DDI_DEPTH_STENCILOP_DESC FrontFace; D3D12DDI_DEPTH_STENCILOP_DESC BackFace; D3D12DDI_LIBRARY_REFERENCE_0010 LibraryReference; BOOL DepthBoundsTestEnable; }`
    - Differs from API: extra `FrontEnable`/`BackEnable` (no API field; [infer] runtime derives them; if
      `StencilEnable && !FrontEnable` set FrontFace to KEEP/KEEP/KEEP/ALWAYS, same for back), and
      `DepthBoundsTestEnable` → needs `D3D12_DEPTH_STENCIL_DESC1` (pipeline-state-stream subobject
      `D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1`). Store a `D3D12_DEPTH_STENCIL_DESC1`.
12. **`pfnDestroyDepthStencilState`** — `PFND3D12DDI_DESTROYDEPTHSTENCILSTATE` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HDEPTHSTENCILSTATE)`
13. **`pfnCalcPrivateRasterizerStateSize`** — `PFND3D12DDI_CALCPRIVATERASTERIZERSTATESIZE_0010` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDI_RASTERIZER_DESC_0010*)`
14. **`pfnCreateRasterizerState`** — `PFND3D12DDI_CREATERASTERIZERSTATE_0010` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDI_RASTERIZER_DESC_0010*, D3D12DDI_HRASTERIZERSTATE)`
    - `D3D12DDI_RASTERIZER_DESC_0010 { FillMode; CullMode; BOOL FrontCounterClockwise; INT DepthBias; FLOAT DepthBiasClamp; FLOAT SlopeScaledDepthBias; BOOL DepthClipEnable; BOOL ScissorEnable; BOOL MultisampleEnable; BOOL AntialiasedLineEnable; UINT ForcedSampleCount; D3D12DDI_CONSERVATIVE_RASTERIZATION_MODE ConservativeRasterizationMode; D3D12DDI_LIBRARY_REFERENCE_0010 LibraryReference; }`
    - Differs: extra `ScissorEnable` (D3D12 always scissors; ignore). Copy remaining fields to `D3D12_RASTERIZER_DESC` field by field.
15. **`pfnDestroyRasterizerState`** — `PFND3D12DDI_DESTROYRASTERIZERSTATE` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HRASTERIZERSTATE)`
16. **`pfnCalcPrivateShaderSize`** — `PFND3D12DDI_CALC_PRIVATE_SHADER_SIZE_0026` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_SHADER_0026*)`
    - Return `sizeof(obj)`; allocate the synthesized container on our own heap (its size depends on signatures), or
      compute the exact container size here and keep it inline.
17. **`pfnCreateVertexShader`** — `PFND3D12DDI_CREATE_SHADER_0026` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_SHADER_0026*, D3D12DDI_HSHADER)`
    - `D3D12DDIARG_CREATE_SHADER_0026 { D3D12DDI_HROOTSIGNATURE hRootSignature; CONST UINT* pShaderCode; union { CONST D3D12DDIARG_STAGE_IO_SIGNATURES* Standard; CONST D3D12DDIARG_TESSELLATION_IO_SIGNATURES* Tessellation; CONST D3D12DDIARG_MESH_IO_SIGNATURES* Mesh; } IOSignatures; D3D12DDI_CREATE_SHADER_FLAGS Flags; D3D12DDI_LIBRARY_REFERENCE_0010 LibraryReference; D3D12DDI_SHADERCACHE_HASH ShaderCodeHash; }`
    - API: none (D3D12 API has no shader objects). Build + keep a DXBC container (section 4); consumed by
      `pfnCreatePipelineState`. Copy everything — `pShaderCode` is not guaranteed to outlive the call [sample copies].
      `ShaderCodeHash` (16 bytes) is a runtime-provided cache key. `Flags`: `ENABLE_SHADER_TRACING`=1, `DISABLE_OPTIMIZATION_0024`=2 (ignore).
18. **`pfnCreatePixelShader`** — same PFN/args as 17 (`IOSignatures.Standard`).
19. **`pfnCreateGeometryShader`** — same as 17 (`IOSignatures.Standard`).
20. **`pfnCreateComputeShader`** — same as 17 (`IOSignatures.Standard`, normally empty signatures).
21. **`pfnCalcPrivateGeometryShaderWithStreamOutput`** — `PFND3D12DDI_CALC_PRIVATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026*)`
22. **`pfnCreateGeometryShaderWithStreamOutput`** — `PFND3D12DDI_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026*, D3D12DDI_HSHADER)`
    - `{ D3D12DDIARG_CREATE_SHADER_0026 CreateShader; CONST D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY* pOutputStreamDecl; UINT NumEntries; CONST UINT* BufferStridesInBytes; UINT NumStrides; UINT RasterizedStream; }`
    - `D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY { UINT Stream; UINT OutputSlot; UINT RegisterIndex; BYTE RegisterMask; }` — register-based.
    - API: becomes `D3D12_STREAM_OUTPUT_DESC` of the PSO: each entry → `D3D12_SO_DECLARATION_ENTRY{ Stream, SemanticName = synthetic name of output register RegisterIndex (same scheme as OSGN), SemanticIndex, StartComponent = ctz(RegisterMask), ComponentCount = popcount(RegisterMask), OutputSlot }`;
      `RegisterIndex == D3D12_SO_DDI_REGISTER_INDEX_DENOTING_GAP` (0xffffffff) → gap entry (`SemanticName = NULL`, ComponentCount = popcount). `RasterizedStream` may be `D3D12_SO_NO_RASTERIZED_STREAM`.
      [infer, D3D10/11 DDI convention] `CreateShader.pShaderCode == NULL` means "SO on VS/DS output, no GS".
23. **`pfnCalcPrivateTessellationShaderSize`** — `PFND3D12DDI_CALC_PRIVATE_SHADER_SIZE_0026` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_SHADER_0026*)` (HS/DS; `IOSignatures.Tessellation`).
24. **`pfnCreateHullShader`** — `PFND3D12DDI_CREATE_SHADER_0026` — as 17, with `D3D12DDIARG_TESSELLATION_IO_SIGNATURES { pInputSignature/NumInputSignatureEntries; pOutputSignature/Num...; pPatchConstantSignature/NumPatchConstantSignatureEntries }` (entries `D3D12DDIARG_SIGNATURE_ENTRY_0012`) → ISGN/OSGN/PCSG.
25. **`pfnCreateDomainShader`** — as 24.
26. **`pfnDestroyShader`** — `PFND3D12DDI_DESTROYSHADER` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HSHADER)` — free container.
27. **`pfnCalcPrivateCommandQueueSize`** — `PFND3D12DDI_CALCPRIVATECOMMANDQUEUESIZE_0050` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATECOMMANDQUEUE_0050*)`
28. **`pfnCreateCommandQueue`** — `PFND3D12DDI_CREATECOMMANDQUEUE_0050` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATECOMMANDQUEUE_0050*, D3D12DDI_HCOMMANDQUEUE, D3D12DDI_HRTCOMMANDQUEUE)`
    - `D3D12DDIARG_CREATECOMMANDQUEUE_0050 { D3D12DDI_COMMAND_QUEUE_FLAGS QueueFlags; UINT NodeMask; D3D12DDI_COMMAND_QUEUE_CREATION_FLAGS QueueCreationFlags; D3D12DDI_HSCHEDULINGGROUP_0050 SchedulingGroup; }`
    - API: `ID3D12Device::CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC{Type, Priority, Flags=NONE, NodeMask})`.
      Type: `QueueFlags & D3D12DDI_COMMAND_QUEUE_FLAG_3D`(1)→`DIRECT`; `_COMPUTE`(2)→`COMPUTE`; `_COPY`(4)→`COPY`;
      video flags → fail `E_INVALIDARG`. Priority: `D3D12DDI_COMMAND_QUEUE_CREATION_FLAG_GLOBAL_REALTIME_PRIORITY` →
      `D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME`, else NORMAL (DDI has no HIGH; [infer] runtime folds it).
    - Also: store `hRTCommandQueue`; create the KM context with `pfnCreateContextCb(hRTCommandQueue, &D3DDDICB_CREATECONTEXT)`
      (or `pfnCreateContextVirtualCb` / `pfnCreateHwQueueCb`) — the runtime uses it for fences/present (section 2d) [sample].
    - Ret: HRESULT; failure fails `ID3D12Device::CreateCommandQueue`.
29. **`pfnDestroyCommandQueue`** — `PFND3D12DDI_DESTROYCOMMANDQUEUE` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE)` — release queue, `pfnDestroyContextCb`.
30. **`pfnCalcPrivateCommandPoolSize`** — `PFND3D12DDI_CALC_PRIVATE_COMMAND_POOL_SIZE_0040` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_COMMAND_POOL_0040*)`
31. **`pfnCreateCommandPool`** — `PFND3D12DDI_CREATE_COMMAND_POOL_0040` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_COMMAND_POOL_0040*, D3D12DDI_HCOMMANDPOOL_0040)`
    - `D3D12DDIARG_CREATE_COMMAND_POOL_0040 { D3D12DDI_COMMAND_POOL_FLAGS PoolFlags; }` (only `_NONE`). **No list type.**
    - API: `ID3D12Device::CreateCommandAllocator(type, ...)` — type unknown here, so create lazily per list type on
      first `pfnResetCommandList` (section 2c). Ret S_OK.
32. **`pfnDestroyCommandPool`** — `PFND3D12DDI_DESTROY_COMMAND_POOL_0040` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDPOOL_0040)` — release allocators.
33. **`pfnResetCommandPool`** — `PFND3D12DDI_RESET_COMMAND_POOL_0040` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDPOOL_0040)`
    - API: `ID3D12CommandAllocator::Reset()` on each allocator of the pool (= app's `ID3D12CommandAllocator::Reset`).
      Errors → `pfnSetErrorCb`.
34. **`pfnCalcPrivatePipelineStateSize`** — `PFND3D12DDI_CALC_PRIVATE_PIPELINE_STATE_SIZE_0033` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_PIPELINE_STATE_0033*)`
35. **`pfnCreatePipelineState`** — `PFND3D12DDI_CREATE_PIPELINE_STATE_0033` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_PIPELINE_STATE_0033*, D3D12DDI_HPIPELINESTATE, D3D12DDI_HRTPIPELINESTATE)`
    - Args + mapping: section 4. API: `ID3D12Device2::CreatePipelineState(D3D12_PIPELINE_STATE_STREAM_DESC)` (needed
      for DEPTH_STENCIL1 / VIEW_INSTANCING), or `CreateGraphicsPipelineState` / `CreateComputePipelineState`.
    - Ret: HRESULT (`E_INVALIDARG`/`E_OUTOFMEMORY` propagate to the app). Keep `hRTPipelineState`.
36. **`pfnDestroyPipelineState`** — `PFND3D12DDI_DESTROY_PIPELINE_STATE` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HPIPELINESTATE)`
37. **`pfnCalcPrivateCommandListSize`** — `PFND3D12DDI_CALC_PRIVATE_COMMAND_LIST_SIZE_0040` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_COMMAND_LIST_0040*)`
38. **`pfnCreateCommandList`** — `PFND3D12DDI_CREATE_COMMAND_LIST_0040` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_COMMAND_LIST_0040*, D3D12DDI_HCOMMANDLIST, D3D12DDI_HRTCOMMANDLIST)`
    - `D3D12DDIARG_CREATE_COMMAND_LIST_0040 { D3D12DDI_COMMAND_LIST_TYPE Type; D3D12DDI_COMMAND_QUEUE_FLAGS QueueFlags; UINT64 ID; D3D12DDI_COMMAND_LIST_FLAGS CommandListFlags; UINT NodeMask; }`
      — **no allocator, no initial PSO**.
    - API type: `Type==BUNDLE`→`D3D12_COMMAND_LIST_TYPE_BUNDLE`; else from QueueFlags (3D→DIRECT, COMPUTE→COMPUTE, COPY→COPY).
      API: `ID3D12Device4::CreateCommandList1(NodeMask, type, D3D12_COMMAND_LIST_FLAG_NONE, IID_ID3D12GraphicsCommandList, ...)` → created **closed**, no allocator (matches DDI: first `pfnResetCommandList` opens it).
    - **Must call** `pfnSetCommandListDDITableCb(hRTCommandList, hRTTable)` with the `D3D12DDI_HRTTABLE` received in
      `pfnFillDdiTable(D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D, ..., uTableNum, hTable)` [sample]. Ret HRESULT.
39. **`pfnDestroyCommandList`** — `PFND3D12DDI_DESTROYCOMMANDLIST` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST)`
40. **`pfnCalcPrivateFenceSize`** — `PFND3D12DDI_CALCPRIVATEFENCESIZE` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_FENCE*)`
41. **`pfnCreateFence`** — `PFND3D12DDI_CREATEFENCE` — `HRESULT (D3D12DDI_HDEVICE, D3D12DDI_HFENCE, _In_ CONST D3D12DDIARG_CREATE_FENCE*)` — **note handle-before-args order**.
    - `D3D12DDIARG_CREATE_FENCE { UINT FenceCount; D3D12DDI_FENCE* Fences; }`, `D3D12DDI_FENCE { D3D12DDI_FENCE_PLACEMENT FenceValue; D3D12DDI_FENCE_PLACEMENT FenceMonitoredValue; D3D12DDI_FENCE_FLAGS Flags; }`, placement = `{ D3DGPU_VIRTUAL_ADDRESS BaseAddress; }` (KM monitored-fence GPU VAs; meaningless for vkd3d).
    - API: no — `ID3D12Fence` is implemented by the runtime on KM sync objects. Minimal: store args, S_OK [sample].
42. **`pfnDestroyFence`** — `PFND3D12DDI_DESTROYFENCE` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HFENCE)`
43. **`pfnCalcPrivateDescriptorHeapSize`** — `PFND3D12DDI_CALC_PRIVATE_DESCRIPTOR_HEAP_SIZE_0001` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001*)`
44. **`pfnCreateDescriptorHeap`** — `PFND3D12DDI_CREATE_DESCRIPTOR_HEAP_0001` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001*, D3D12DDI_HDESCRIPTORHEAP)`
    - `{ D3D12DDI_DESCRIPTOR_HEAP_TYPE Type; UINT NumDescriptors; D3D12DDI_DESCRIPTOR_HEAP_FLAGS Flags; UINT NodeMask; }`
    - API: `ID3D12Device::CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC{Type, NumDescriptors, Flags', NodeMask})`,
      `Flags' = (Flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE /*0x2*/) ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE /*0x1*/ : NONE`.
      Ret HRESULT.
45. **`pfnDestroyDescriptorHeap`** — `PFND3D12DDI_DESTROY_DESCRIPTOR_HEAP` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HDESCRIPTORHEAP)`
46. **`pfnGetDescriptorSizeInBytes`** — `PFND3D12DDI_GET_DESCRIPTOR_SIZE_IN_BYTES` — `UINT (D3D12DDI_HDEVICE, D3D12DDI_DESCRIPTOR_HEAP_TYPE)`
    - API: `ID3D12Device::GetDescriptorHandleIncrementSize(type)` (enum identical). **Runtime returns this to apps**
      as the increment; must be consistent with the handles from 47/48.
47. **`pfnGetCPUDescriptorHandleForHeapStart`** — `PFND3D12DDI_GET_CPU_DESCRIPTOR_HANDLE_FOR_HEAP_START` — `D3D12DDI_CPU_DESCRIPTOR_HANDLE (D3D12DDI_HDEVICE, D3D12DDI_HDESCRIPTORHEAP)`
    - API: `ID3D12DescriptorHeap::GetCPUDescriptorHandleForHeapStart()`; return `{ .ptr = api.ptr }` (`SIZE_T ptr`).
48. **`pfnGetGPUDescriptorHandleForHeapStart`** — `PFND3D12DDI_GET_GPU_DESCRIPTOR_HANDLE_FOR_HEAP_START` — `D3D12DDI_GPU_DESCRIPTOR_HANDLE (D3D12DDI_HDEVICE, D3D12DDI_HDESCRIPTORHEAP)`
    - API: `ID3D12DescriptorHeap::GetGPUDescriptorHandleForHeapStart()` (`UINT64 ptr`). Only for shader-visible heaps.
49. **`pfnCreateShaderResourceView`** — `PFND3D12DDI_CREATE_SHADER_RESOURCE_VIEW_0002` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002*, _In_ D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)`
    - `{ D3D12DDI_HRESOURCE hDrvResource; DXGI_FORMAT Format; D3D12DDI_RESOURCE_DIMENSION ResourceDimension; UINT Shader4ComponentMapping; union { Buffer{UINT64 FirstElement; UINT NumElements; UINT StructureByteStride; D3D12DDI_BUFFER_SRV_FLAGS Flags;}; Tex1D{MostDetailedMip, FirstArraySlice, MipLevels, ArraySize, FLOAT ResourceMinLODClamp}; Tex2D(_0002){MostDetailedMip, FirstArraySlice, MipLevels, ArraySize, PlaneSlice, ResourceMinLODClamp}; Tex3D{MostDetailedMip, MipLevels, ResourceMinLODClamp}; TexCube{MostDetailedMip, MipLevels, First2DArrayFace, NumCubes, ResourceMinLODClamp}; RaytracingAccelerationStructure(_0042){D3D12DDI_GPU_VIRTUAL_ADDRESS Location}; } }`
    - `D3D12DDI_RESOURCE_DIMENSION`: BUFFER=1, TEXTURE1D=2, TEXTURE2D=3, TEXTURE3D=4, TEXTURECUBE=5, RTAS_0042=6 — **not**
      `D3D12_SRV_DIMENSION`. Reconstruct (resource desc from our resource object):
      BUFFER→`BUFFER`; TEX1D→`TEXTURE1DARRAY` if resource `DepthOrArraySize>1` else `TEXTURE1D`;
      TEX2D→ if `SampleDesc.Count>1`: `TEXTURE2DMSARRAY`/`TEXTURE2DMS` else `TEXTURE2DARRAY`/`TEXTURE2D` (array iff resource is an array);
      TEX3D→`TEXTURE3D`; TEXCUBE→`TEXTURECUBEARRAY` if resource `DepthOrArraySize>6` else `TEXTURECUBE`;
      RTAS→`RAYTRACING_ACCELERATION_STRUCTURE` (hDrvResource NULL). The shader's Texture2D vs Texture2DArray choice
      is lost — heuristic can mismatch view type vs SPIR-V `Arrayed` (works on most Vulkan drivers, technically UB).
    - API: `ID3D12Device::CreateShaderResourceView(res_or_NULL, &desc, {DestDescriptor.ptr})`. Mapping value and
      flags identical. No return.
50. **`pfnCreateConstantBufferView`** — `PFND3D12DDI_CREATE_CONSTANT_BUFFER_VIEW` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDI_CONSTANT_BUFFER_VIEW_DESC*, _In_ D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)`
    - `{ D3D12DDI_GPU_VIRTUAL_ADDRESS BufferLocation; UINT SizeInBytes; UINT Padding; }` → `ID3D12Device::CreateConstantBufferView((D3D12_CONSTANT_BUFFER_VIEW_DESC*)p, {ptr})`. BufferLocation is a vkd3d VA (we returned it).
51. **`pfnCreateSampler`** — `PFND3D12DDI_CREATE_SAMPLER` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_SAMPLER*, _In_ D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)`
    - `D3D12DDIARG_CREATE_SAMPLER { CONST D3D12DDI_SAMPLER_DESC* pSamplerDesc; }` (extra indirection);
      `D3D12DDI_SAMPLER_DESC` = `D3D12_SAMPLER_DESC` layout. → `ID3D12Device::CreateSampler((D3D12_SAMPLER_DESC*)a->pSamplerDesc, {ptr})`.
52. **`pfnCreateUnorderedAccessView`** — `PFND3D12DDI_CREATE_UNORDERED_ACCESS_VIEW_0002` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002*, _In_ D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)`
    - `{ hDrvResource; Format; ResourceDimension /*never TexCube*/; union { Buffer{D3D12DDI_HRESOURCE hDrvCounterResource; UINT64 FirstElement; UINT NumElements; UINT StructureByteStride; UINT64 CounterOffsetInBytes; D3D12DDI_BUFFER_UAV_FLAGS Flags;}; Tex1D{MipSlice, FirstArraySlice, ArraySize}; Tex2D(_0002){MipSlice, FirstArraySlice, ArraySize, PlaneSlice}; Tex3D{MipSlice, FirstW, WSize}; } }`
    - → `ID3D12Device::CreateUnorderedAccessView(res, H(Buffer.hDrvCounterResource) (buffer only, else NULL), &desc, {ptr})`; dimension rebuilt as for SRV (1D/1DARRAY, 2D/2DARRAY, 3D with `FirstWSlice=FirstW`).
53. **`pfnCreateRenderTargetView`** — `PFND3D12DDI_CREATE_RENDER_TARGET_VIEW_0002` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002*, _In_ D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)`
    - `{ hDrvResource; Format; ResourceDimension; union { Buffer{UINT64 FirstElement; UINT NumElements}; Tex1D{MipSlice, FirstArraySlice, ArraySize}; Tex2D(_0002){MipSlice, FirstArraySlice, ArraySize, PlaneSlice}; Tex3D{MipSlice, FirstW, WSize}; TexCube{MipSlice, FirstArraySlice, ArraySize}; } }`
    - → `ID3D12Device::CreateRenderTargetView(res, &desc, {ptr})`; TexCube → `TEXTURE2DARRAY`; MS from resource sample count.
54. **`pfnCreateDepthStencilView`** — `PFND3D12DDI_CREATE_DEPTH_STENCIL_VIEW` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW*, _In_ D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)`
    - `{ hDrvResource; Format; ResourceDimension; D3D12DDI_CREATE_DEPTH_STENCIL_VIEW_FLAGS Flags; union { Tex1D{MipSlice, FirstArraySlice, ArraySize}; Tex2D{MipSlice, FirstArraySlice, ArraySize}; TexCube{MipSlice, FirstArraySlice, ArraySize}; } }`
    - → `ID3D12Device::CreateDepthStencilView(res, &D3D12_DEPTH_STENCIL_VIEW_DESC{Format, dim, Flags (identical), ...}, {ptr})`.
55. **`pfnCalcPrivateRootSignatureSize`** — `PFND3D12DDI_CALC_PRIVATE_ROOT_SIGNATURE_SIZE_0013` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013*)`
56. **`pfnCreateRootSignature`** — `PFND3D12DDI_CREATE_ROOT_SIGNATURE_0013` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013*, D3D12DDI_HROOTSIGNATURE)`
    - Deserialized 1.1 desc → serialize with vkd3d → `CreateRootSignature`. Section 3. Ret HRESULT.
57. **`pfnDestroyRootSignature`** — `PFND3D12DDI_DESTROY_ROOT_SIGNATURE` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HROOTSIGNATURE)`
58. **`pfnMapHeap`** — `PFND3D12DDI_MAPHEAP` — `HRESULT (D3D12DDI_HDEVICE, D3D12DDI_HHEAP, _Out_ VOID**)`
    - Heap-level map (the runtime implements `ID3D12Resource::Map(sub)` as heap pointer + offset from
      `pfnCheckSubresourceInfo`). Section 2a. Return a stable pointer to byte 0 of the heap (committed: of the resource). S_OK.
59. **`pfnUnmapHeap`** — `PFND3D12DDI_UNMAPHEAP` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HHEAP)` — no-op (vkd3d keeps host-visible memory persistently mapped).
60. **`pfnCalcPrivateHeapAndResourceSizes`** — `PFND3D12DDI_CALCPRIVATEHEAPANDRESOURCESIZES_0030` — `D3D12DDI_HEAP_AND_RESOURCE_SIZES (D3D12DDI_HDEVICE, _In_opt_ CONST D3D12DDIARG_CREATEHEAP_0001*, _In_opt_ CONST D3D12DDIARG_CREATERESOURCE_0003*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030)`
    - Return `{ SIZE_T Heap; SIZE_T Resource; }` = sizeof our heap/resource objects (sample returns both unconditionally).
61. **`pfnCreateHeapAndResource`** — `PFND3D12DDI_CREATEHEAPANDRESOURCE_0030` — `HRESULT (D3D12DDI_HDEVICE, _In_opt_ CONST D3D12DDIARG_CREATEHEAP_0001*, D3D12DDI_HHEAP, D3D12DDI_HRTRESOURCE, _In_opt_ CONST D3D12DDIARG_CREATERESOURCE_0003*, _In_opt_ CONST D3D12DDI_CLEAR_VALUES*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030, D3D12DDI_HRESOURCE)`
    - `D3D12DDIARG_CREATEHEAP_0001 { UINT64 ByteSize; UINT64 Alignment; D3D12DDI_MEMORY_POOL MemoryPool; D3D12DDI_CPU_PAGE_PROPERTY CPUPageProperty; D3D12DDI_HEAP_FLAGS Flags; UINT CreationNodeMask; UINT VisibleNodeMask; }`
      — no heap *type*, always "custom" properties.
    - `D3D12DDIARG_CREATERESOURCE_0003 { D3D12DDIARG_BUFFER_PLACEMENT ReuseBufferGPUVA; D3D12DDI_RESOURCE_TYPE ResourceType; UINT64 Width; UINT Height; UINT16 DepthOrArraySize; UINT16 MipLevels; DXGI_FORMAT Format; DXGI_SAMPLE_DESC SampleDesc; D3D12DDI_TEXTURE_LAYOUT Layout; D3D12DDI_RESOURCE_FLAGS_0003 Flags; D3D12DDI_RESOURCE_STATES InitialResourceState; CONST D3D12DDIARG_ROW_MAJOR_RESOURCE_LAYOUT* pRowMajorLayout; }`
      — **no `Alignment`**; `ReuseBufferGPUVA.BaseAddress.UMD = { D3D12DDI_HRESOURCE hResource; UINT64 Offset; }` carries the
      placement offset inside the heap [sample uses `.Offset` only].
    - Three cases + API mapping, heap property/flag conversion, resource flag conversion: section 2a.
    - `D3D12DDI_CLEAR_VALUES` = `D3D12_CLEAR_VALUE`. Protected session: ignore (NULL).
    - Ret HRESULT; runtime then calls `pfnCheckResourceVirtualAddress` (buffers) and keeps the VA.
62. **`pfnDestroyHeapAndResource`** — `PFND3D12DDI_DESTROYHEAPANDRESOURCE` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HHEAP, D3D12DDI_HRESOURCE)`
    - Either handle may be NULL (`pDrvPrivate==NULL`); destroy whichever is non-NULL (placed resource: resource only;
      `CreateHeap` heap: heap only; committed: both) [sample].
63. **`pfnMakeResident`** — `PFND3D12DDI_MAKERESIDENT_0001` — `HRESULT (D3D12DDI_HDEVICE, D3D12DDIARG_MAKERESIDENT_0001*)`
    - `{ UINT NumAdapters; CONST D3D12DDI_HRTPAGINGQUEUE* pRTPagingQueue; UINT NumObjects; CONST D3D12DDI_HANDLE_AND_TYPE* pObjects; D3DDDI_MAKERESIDENT_FLAGS Flags; UINT64* pPagingFenceValue /*out*/; UINT WaitMask /*out*/; }`,
      `D3D12DDI_HANDLE_AND_TYPE { VOID* Handle; D3D12DDI_HANDLETYPE Type; }` (`D3D12DDI_HT_HEAP`=25, `_0012_RESOURCE`=34, `_DESCRIPTOR_HEAP`=24, ...).
    - API equivalent `ID3D12Device::MakeResident` exists but vkd3d is effectively no-op. Minimal: S_OK, `WaitMask=0`
      (only E_PENDING makes the runtime wait on paging fences). Sample translates to KM `pfnMakeResidentCb`.
64. **`pfnEvict`** — `PFND3D12DDI_EVICT2` — `HRESULT (D3D12DDI_HDEVICE, CONST D3D12DDIARG_EVICT*)` — `{ NumObjects; pObjects; D3DDDI_EVICT_FLAGS Flags; }` → S_OK no-op.
65. **`pfnCalcPrivateOpenedHeapAndResourceSizes`** — `PFND3D12DDI_CALCPRIVATEOPENEDHEAPANDRESOURCESIZES_0043` — `D3D12DDI_HEAP_AND_RESOURCE_SIZES (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_OPENHEAP_0003*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030)`
66. **`pfnOpenHeapAndResource`** — `PFND3D12DDI_OPENHEAPANDRESOURCE_0043` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_OPENHEAP_0003*, D3D12DDI_HHEAP, D3D12DDI_HRTRESOURCE, D3D12DDI_HPROTECTEDRESOURCESESSION_0030, D3D12DDI_HRESOURCE)`
    - `D3D12DDIARG_OPENHEAP_0003 { UINT NumAllocations; D3DDDI_OPENALLOCATIONINFO* pOpenAllocationInfo; D3D12DDI_HKMRESOURCE hKMResource; VOID* pPrivateDriverData; UINT PrivateDriverDataSize; D3D12DDI_RESOURCE_STATES InitialResourceState; }`
    - API: `ID3D12Device::OpenSharedHandle` — but input is KM allocation private data, so only meaningful if our
      resources are backed by KM allocations. Minimal: return `E_OUTOFMEMORY` ("allowed and doesn't trigger device
      removal" per sample comment) or `E_NOTIMPL`.
67. **`pfnCopyDescriptors`** — `PFND3D12DDI_COPY_DESCRIPTORS_0003` — `VOID (D3D12DDI_HDEVICE, _In_ UINT NumDestDescriptorRanges, _In_reads_(NumDestDescriptorRanges) CONST D3D12DDI_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts, _In_reads_opt_(NumDestDescriptorRanges) CONST UINT* pDestDescriptorRangeSizes /*NULL = all 1*/, _In_ UINT NumSrcDescriptorRanges, _In_reads_(NumSrcDescriptorRanges) CONST D3D12DDI_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts, _In_reads_opt_(NumSrcDescriptorRanges) CONST UINT* pSrcDescriptorRangeSizes /*NULL = all 1*/, _In_ D3D12DDI_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)`
    - API: `ID3D12Device::CopyDescriptors(...)` — identical argument list; cast handle arrays.
68. **`pfnCopyDescriptorsSimple`** — `PFND3D12DDI_COPY_DESCRIPTORS_SIMPLE_0003` — `VOID (D3D12DDI_HDEVICE, _In_ UINT NumDescriptors, _In_ D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart, _In_ D3D12DDI_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, _In_ D3D12DDI_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)`
    - API: `ID3D12Device::CopyDescriptorsSimple(n, dst, src, type)`.
69. **`pfnCalcPrivateQueryHeapSize`** — `PFND3D12DDI_CALC_PRIVATE_QUERY_HEAP_SIZE_0001` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_QUERY_HEAP_0001*)`
70. **`pfnCreateQueryHeap`** — `PFND3D12DDI_CREATE_QUERY_HEAP_0001` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_QUERY_HEAP_0001*, D3D12DDI_HQUERYHEAP)`
    - `{ D3D12DDI_QUERY_HEAP_TYPE Type; UINT Count; UINT NodeMask; }` = `D3D12_QUERY_HEAP_DESC` → `ID3D12Device::CreateQueryHeap`.
71. **`pfnDestroyQueryHeap`** — `PFND3D12DDI_DESTROY_QUERY_HEAP` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HQUERYHEAP)`
72. **`pfnCalcPrivateCommandSignatureSize`** — `PFND3D12DDI_CALC_PRIVATE_COMMAND_SIGNATURE_SIZE_0001` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001*)`
73. **`pfnCreateCommandSignature`** — `PFND3D12DDI_CREATE_COMMAND_SIGNATURE_0001` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001*, D3D12DDI_HCOMMANDSIGNATURE)`
    - `{ UINT ByteStride; UINT NumArgumentDescs; const D3D12DDI_INDIRECT_ARGUMENT_DESC* pArgumentDescs; D3D12DDI_HROOTSIGNATURE hRootSignature; UINT NodeMask; }`
    - → `ID3D12Device::CreateCommandSignature(&D3D12_COMMAND_SIGNATURE_DESC{ByteStride, NumArgumentDescs, (cast)pArgumentDescs, NodeMask}, H(hRootSignature)->rs or NULL, ...)`.
74. **`pfnDestroyCommandSignature`** — `PFND3D12DDI_DESTROY_COMMAND_SIGNATURE` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDSIGNATURE)`
75. **`pfnCheckResourceVirtualAddress`** — `PFND3D12DDI_CHECKRESOURCEVIRTUALADDRESS` — `D3D12DDI_GPU_VIRTUAL_ADDRESS (D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE)`
    - **This is how the GPU VA reaches the runtime/app** (`ID3D12Resource::GetGPUVirtualAddress`). API:
      `ID3D12Resource::GetGPUVirtualAddress()` (buffers; 0 for textures). All later DDI VAs (VB/IB/SO views, root
      CBV/SRV/UAV, CBV desc, WriteBufferImmediate) are app offsets added to this value → pass through unchanged.
76. **`pfnCheckResourceAllocationInfo`** — `PFND3D12DDI_CHECKRESOURCEALLOCATIONINFO_0022` — `VOID (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATERESOURCE_0003*, D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS, UINT32 AlignmentRestriction, UINT VisibleNodeMask, _Out_ D3D12DDI_RESOURCE_ALLOCATION_INFO_0022*)`
    - Out: `{ UINT64 ResourceDataSize; UINT64 AdditionalDataHeaderSize; UINT64 AdditionalDataSize; UINT32 ResourceDataAlignment; UINT32 AdditionalDataHeaderAlignment; UINT32 AdditionalDataAlignment; D3D12DDI_TEXTURE_LAYOUT Layout; UINT8 MipLevelSwizzleTransition[5]; UINT8 PlaneSliceSwizzleTransition[2]; }`
    - API: `ID3D12Device::GetResourceAllocationInfo(VisibleNodeMask, 1, &desc)` with `desc.Alignment = AlignmentRestriction`
      (this is where the API `Alignment` goes). Fill `ResourceDataSize=SizeInBytes`, `ResourceDataAlignment=Alignment`,
      Additional* = 0 with alignments 1 (sample), `Layout` = requested layout, buffers `D3D12DDI_TL_ROW_MAJOR` [sample];
      opaque textures: [infer] `D3D12DDI_TL_UNDEFINED`. **Runtime relies on size/alignment** for `GetResourceAllocationInfo`
      and placed-resource validation.
77. **`pfnCheckSubresourceInfo`** — `PFND3D12DDI_CHECKSUBRESOURCEINFO` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE, UINT Subresource, _Out_ D3D12DDI_SUBRESOURCE_INFO*)`
    - Out: `{ UINT64 Offset; UINT64 RowStride; UINT64 DepthStride; UINT16 RowBytePreSwizzleOffset; UINT16 ColumnPreSwizzleOffset; UINT16 DepthPreSwizzleOffset; }`
    - Layout of the subresource inside the mapped heap memory (used for `Map(sub)`, `WriteToSubresource`/`ReadFromSubresource`).
      Buffers: `{0, Width, Width}` (sample zero-fills). CPU-visible textures (CUSTOM heaps): vkd3d has no API exposing
      the real `vkGetImageSubresourceLayout`; options: forbid CPU-visible textures, or add a vkd3d private export. No API equivalent.
78. **`pfnCheckExistingResourceAllocationInfo`** — `PFND3D12DDI_CHECKEXISITINGRESOURCEALLOCATIONINFO_0022` (sic) — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE, _Out_ D3D12DDI_RESOURCE_ALLOCATION_INFO_0022*)` — as 76 for `resource->GetDesc()`.
79. **`pfnOfferResources`** — `PFND3D12DDI_OFFERRESOURCES` — `HRESULT (D3D12DDI_HDEVICE, CONST D3D12DDIARG_OFFERRESOURCES*)` — `{ NumObjects; pObjects; D3DDDI_OFFER_PRIORITY Priority; }`. API `ID3D12Device::OfferResources` exists; minimal S_OK (sample: E_NOTIMPL).
80. **`pfnReclaimResources`** — `PFND3D12DDI_RECLAIMRESOURCES_0001` — `HRESULT (D3D12DDI_HDEVICE, D3D12DDIARG_RECLAIMRESOURCES_0001*)` — `{ NumAdapters; pRTPagingQueue; NumObjects; pObjects; BOOL* pDiscarded; UINT64* pPagingFenceValue; UINT WaitMask; }`. Minimal: `pDiscarded[i]=FALSE`, `WaitMask=0`, S_OK.
81. **`pfnGetImplicitPhysicalAdapterMask`** — `PFND3D12DDI_GETIMPLICITPHYSICALADAPTERMASK` — `UINT (D3D12DDI_HDEVICE)` — no API. Return 1 (single node) — sample returns 0.
82. **`pfnGetPresentPrivateDriverDataSize`** — `PFND3D12DDI_GET_PRESENT_PRIVATE_DRIVER_DATA_SIZE` — `UINT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_PRESENT_0001*)` — no API. Return 0.
83. **`pfnQueryNodeMap`** — `PFND3D12DDI_QUERY_NODE_MAP` — `VOID (D3D12DDI_HDEVICE, UINT NumPhysicalAdapters, _Out_writes_(NumPhysicalAdapters) UINT* pMap)` — no API. `pMap[i]=i` [sample].
84. **`pfnRetrieveShaderComment`** — `PFND3D12DDI_RETRIEVE_SHADER_COMMENT_0003` — `HRESULT (D3D12DDI_HDEVICE, D3D12DDI_HPIPELINESTATE, _Out_writes_z_(*CharacterCountIncludingNullTerminator) WCHAR* pBuffer, _Inout_ SIZE_T* CharacterCountIncludingNullTerminator)` — no API (driver disassembly for tools). `E_NOTIMPL` [sample].
85. **`pfnCheckResourceAllocationHandle`** — `PFND3D12DDI_CHECKRESOURCEALLOCATIONHANDLE` — `D3DKMT_HANDLE (D3D12DDI_HDEVICE, D3D10DDI_HRESOURCE)` — no API; KM allocation backing the resource (interop/sharing/present). Return 0 when not KM-backed [sample].
86. **`pfnCalcPrivatePipelineLibrarySize`** — `PFND3D12DDI_CALC_PRIVATE_PIPELINE_LIBRARY_SIZE_0010` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010*)` — `{ SIZE_T InitialDataSize; CONST VOID* pInitialData; }`
87. **`pfnCreatePipelineLibrary`** — `PFND3D12DDI_CREATE_PIPELINE_LIBRARY_0010` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010*, D3D12DDI_HPIPELINELIBRARY)`
    - API: `ID3D12Device1::CreatePipelineLibrary(pInitialData, size, ...)` (vkd3d implements it). The runtime keeps the
      name→index map; the driver only stores pipelines by `PipelineIndex`. Simplest correct: an empty driver library
      (store nothing, serialize a tiny header) — PSO creation still receives the full desc [infer]. Or return
      `DXGI_ERROR_UNSUPPORTED` (apps must handle it).
88. **`pfnDestroyPipelineLibrary`** — `PFND3D12DDI_DESTROY_PIPELINE_LIBRARY_0010` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HPIPELINELIBRARY)`
89. **`pfnAddPipelineStateToLibrary`** — `PFND3D12DDI_ADD_PIPELINE_STATE_TO_LIBRARY_0010` — `HRESULT (D3D12DDI_HDEVICE hDevice, D3D12DDI_HPIPELINELIBRARY hLibrary, D3D12DDI_HPIPELINESTATE hPipelineState, UINT PipelineIndex)` — ~`ID3D12PipelineLibrary::StorePipeline` (by index, not name). Minimal S_OK.
90. **`pfnCalcSerializedLibrarySize`** — `PFND3D12DDI_CALC_SERIALIZED_LIBRARY_SIZE_0010` — `SIZE_T (D3D12DDI_HDEVICE hDevice, D3D12DDI_HPIPELINELIBRARY hLibrary)` — ~`GetSerializedSize`. Runtime allocates this many bytes for 91.
91. **`pfnSerializeLibrary`** — `PFND3D12DDI_SERIALIZE_LIBRARY_0010` — `HRESULT (D3D12DDI_HDEVICE hDevice, D3D12DDI_HPIPELINELIBRARY hLibrary, _Out_ VOID *pBlob)` — ~`Serialize`.
92. **`pfnGetDebugAllocationInfo`** — `PFND3D12DDI_GET_DEBUG_ALLOCATION_INFO_0014` — `VOID (D3D12DDI_HDEVICE hDevice, D3D12DDI_HANDLE_AND_TYPE Object, _Inout_ UINT* pNumVirtualAddressInfos, _Out_writes_to_opt_ D3D12DDI_DEBUG_VIRTUAL_ADDRESS_ALLOCATION_INFO_0012* pVirtualAddressInfos, _Inout_ UINT* pNumKMTInfos, _Out_writes_to_opt_ D3D12DDI_DEBUG_KMT_ALLOCATION_INFO_0014* pKMTInfos)` — no API (DRED/debug). Set both counts to 0.
93. **`pfnCalcPrivateCommandRecorderSize`** — `PFND3D12DDI_CALC_PRIVATE_COMMAND_RECORDER_SIZE_0040` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_COMMAND_RECORDER_0040*)`
94. **`pfnCreateCommandRecorder`** — `PFND3D12DDI_CREATE_COMMAND_RECORDER_0040` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATE_COMMAND_RECORDER_0040*, D3D12DDI_HCOMMANDRECORDER_0040)`
    - `{ D3D12DDI_COMMAND_QUEUE_FLAGS QueueFlags; D3D12DDI_COMMAND_RECORDER_FLAGS RecorderFlags /*NONE*/; }`. No API object; store QueueFlags. S_OK.
95. **`pfnDestroyCommandRecorder`** — `PFND3D12DDI_DESTROY_COMMAND_RECORDER_0040` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDRECORDER_0040)`
96. **`pfnCommandRecorderSetCommandPoolAsTarget`** — `PFND3D12DDI_COMMAND_RECORDER_SET_COMMAND_POOL_AS_TARGET_0040` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDRECORDER_0040, D3D12DDI_HCOMMANDPOOL_0040)`
    - Recorder remembers the pool; the next `pfnResetCommandList` with this recorder records into that pool [sample]. Section 2c.
97. **`pfnCalcPrivateSchedulingGroupSize`** — `PFND3D12DDI_CALCPRIVATESCHEDULINGGROUPSIZE_0050` — `SIZE_T (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATESCHEDULINGGROUP_0050*)` — `{ UINT NodeMask; }`; return a small size.
98. **`pfnCreateSchedulingGroup`** — `PFND3D12DDI_CREATESCHEDULINGGROUP_0050` — `HRESULT (D3D12DDI_HDEVICE, _In_ CONST D3D12DDIARG_CREATESCHEDULINGGROUP_0050*, D3D12DDI_HSCHEDULINGGROUP_0050, D3D12DDI_HRTSCHEDULINGGROUP_0050)` — no API (runtime-internal, HW scheduling). Sample returns `E_NOINTERFACE`; only used if caps advertise it.
99. **`pfnDestroySchedulingGroup`** — `PFND3D12DDI_DESTROYSCHEDULINGGROUP_0050` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HSCHEDULINGGROUP_0050)`
100. **`pfnEnumerateMetaCommands`** — `PFND3D12DDI_ENUMERATE_META_COMMANDS_0052` — `HRESULT (D3D12DDI_HDEVICE, _Inout_ UINT* pNumMetaCommands, _Out_writes_opt_(*pNumMetaCommands) D3D12DDIARG_META_COMMAND_DESC* pDescs)`
     - `D3D12DDIARG_META_COMMAND_DESC { GUID Id; LPCWSTR Name; D3D12DDI_GRAPHICS_STATES InitializationDirtyState; D3D12DDI_GRAPHICS_STATES ExecutionDirtyState; }`. API: `ID3D12Device5::EnumerateMetaCommands`. Minimal: `*pNumMetaCommands=0`, S_OK.
101. **`pfnEnumerateMetaCommandParameters`** — `PFND3D12DDI_ENUMERATE_META_COMMAND_PARAMETERS_0052` — `HRESULT (D3D12DDI_HDEVICE, GUID CommandId, D3D12DDI_META_COMMAND_PARAMETER_STAGE Stage, _Inout_ UINT* pParameterCount, _Out_writes_opt_(*pParameterCount) D3D12DDIARG_META_COMMAND_PARAMETER_DESC* pParameterDescs)` — `ID3D12Device5::EnumerateMetaCommandParameters`; minimal `E_INVALIDARG`.
102. **`pfnCalcPrivateMetaCommandSize`** — `PFND3D12DDI_CALC_PRIVATE_META_COMMAND_SIZE_0052` — `SIZE_T (D3D12DDI_HDEVICE, GUID CommandId, UINT NodeMask, CONST void* pCreationParameters, SIZE_T CreationParametersDataSizeInBytes)`
103. **`pfnCreateMetaCommand`** — `PFND3D12DDI_CREATE_META_COMMAND_0052` — `HRESULT (D3D12DDI_HDEVICE, GUID CommandId, UINT NodeMask, CONST void* pCreationParameters, SIZE_T CreationParametersDataSizeInBytes, D3D12DDI_HMETACOMMAND_0052, D3D12DDI_HRTMETACOMMAND_0052)` — `ID3D12Device5::CreateMetaCommand`; minimal `E_INVALIDARG`.
104. **`pfnDestroyMetaCommand`** — `PFND3D12DDI_DESTROY_META_COMMAND_0052` — `VOID (D3D12DDI_HDEVICE, D3D12DDI_HMETACOMMAND_0052)`
105. **`pfnGetMetaCommandRequiredParameterInfo`** — `PFND3D12DDI_GET_META_COMMAND_REQUIRED_PARAMETER_INFO_0052` — `VOID (D3D12DDI_HMETACOMMAND_0052, D3D12DDI_META_COMMAND_PARAMETER_STAGE Stage, UINT ParameterIndex, _Out_ D3D12DDIARG_META_COMMAND_REQUIRED_PARAMETER_INFO*)` — **no device param**. `ID3D12MetaCommand::GetRequiredParameterResourceSize`. Unreachable if none enumerated.

### 1.2 `D3D12DDI_COMMAND_LIST_FUNCS_3D_0052` (60 entries, table order)

All take `D3D12DDI_HCOMMANDLIST` first and return `VOID`. Errors: `pfnSetCommandListErrorCb(hRTCommandList, hr)`.
`L` = our list's `ID3D12GraphicsCommandList*` (QI'd up to `...List4` once at creation). The same table type is
used for DIRECT/COMPUTE/COPY/BUNDLE lists (table type `D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D`=1).

1. **`pfnCloseCommandList`** — `PFND3D12DDI_CLOSECOMMANDLIST` — `(D3D12DDI_HCOMMANDLIST)` → `L->Close()`; on failure `pfnSetCommandListErrorCb`. [infer] runtime returns its own HRESULT to the app (deferred errors reported via the callback).
2. **`pfnResetCommandList`** — `PFND3D12DDI_RESETCOMMANDLIST_0040` — `(D3D12DDI_HCOMMANDLIST, _In_ CONST D3D12DDIARG_RESETCOMMANDLIST_0040*)`
   - `D3D12DDIARG_RESETCOMMANDLIST_0040 { D3D12DDI_HCOMMANDRECORDER_0040 hDrvCommandRecorder; UINT64 ID; D3D12DDI_COMMAND_LIST_FLAGS CommandListFlags; }` — **no allocator, no initial PSO**.
   - → `L->Reset(pool->AllocatorFor(listType), NULL)` where `pool = H(hDrvCommandRecorder)->targetPool`. If the app
     passed an initial PSO, [infer] the runtime follows with `pfnSetPipelineState`. Section 2c.
3. **`pfnDrawInstanced`** — `PFND3D12DDI_DRAWINSTANCED` — `(D3D12DDI_HCOMMANDLIST, UINT, UINT, UINT, UINT)` → `L->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation)`.
4. **`pfnDrawIndexedInstanced`** — `PFND3D12DDI_DRAWINDEXEDINSTANCED` — `(D3D12DDI_HCOMMANDLIST, UINT, UINT, UINT, INT, UINT)` → `L->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation)`.
5. **`pfnDispatch`** — `PFND3D12DDI_DISPATCH` — `(D3D12DDI_HCOMMANDLIST, UINT, UINT, UINT)` → `L->Dispatch(x, y, z)`.
6. **`pfnClearUnorderedAccessViewUint`** — `PFND3D12DDI_CLEAR_UNORDERED_ACCESS_VIEW_UINT_0003` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12DDI_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, D3D12DDI_HRESOURCE hDrvResource, CONST UINT[4], UINT NumRects, _In_reads_(NumRects) CONST D3D12DDI_RECT* pRects)` → `L->ClearUnorderedAccessViewUint(gpu, cpu, res, values, NumRects, pRects)` (identical).
7. **`pfnClearUnorderedAccessViewFloat`** — `PFND3D12DDI_CLEAR_UNORDERED_ACCESS_VIEW_FLOAT_0003` — same with `CONST FLOAT[4]` → `L->ClearUnorderedAccessViewFloat`.
8. **`pfnClearRenderTargetView`** — `PFND3D12DDI_CLEAR_RENDER_TARGET_VIEW_0003` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, CONST FLOAT[4], UINT NumRects, _In_reads_(NumRects) CONST D3D12DDI_RECT* pRects)` → `L->ClearRenderTargetView`.
9. **`pfnClearDepthStencilView`** — `PFND3D12DDI_CLEAR_DEPTH_STENCIL_VIEW_0003` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, UINT /*flags*/, FLOAT /*depth*/, UINT8 /*stencil*/, UINT NumRects, _In_reads_(NumRects) CONST D3D12DDI_RECT* pRects)` → `L->ClearDepthStencilView(h, (D3D12_CLEAR_FLAGS)flags, depth, stencil, n, rects)`; flags = `D3D10_DDI_CLEAR_DEPTH`(1)|`_STENCIL`(2), identical.
10. **`pfnDiscardResource`** — `PFND3D12DDI_DISCARD_RESOURCE_0003` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HRESOURCE hDrvResource, _In_opt_ CONST D3D12DDIARG_DISCARD_RESOURCE_0003*)` — `{ UINT NumRects; CONST D3D12DDI_RECT* pRects; UINT FirstSubresource; UINT NumSubresources; }` = `D3D12_DISCARD_REGION` → `L->DiscardResource(res, (D3D12_DISCARD_REGION*)p)`.
11. **`pfnCopyTextureRegion`** — `PFND3D12DDI_COPYTEXTUREREGION_0003` — `(D3D12DDI_HCOMMANDLIST, _In_ CONST D3D12DDIARG_BUFFER_PLACEMENT* /*dst*/, D3D12DDIARG_PLACED_RESOURCE /*dst layout*/, UINT DstX, UINT DstY, UINT DstZ, _In_ CONST D3D12DDIARG_BUFFER_PLACEMENT* /*src*/, D3D12DDIARG_PLACED_RESOURCE /*src layout*/, _In_opt_ CONST D3D12DDI_BOX*)`
    - `D3D12DDIARG_BUFFER_PLACEMENT { union { D3D12DDIARG_HRESOURCE_PLACEMENT UMD; } BaseAddress; }`, `UMD = { D3D12DDI_HRESOURCE hResource; UINT64 Offset; }`;
      `D3D12DDIARG_PLACED_RESOURCE { D3D12DDI_RESOURCE_LAYOUT Layout; CONST VOID* pLayout; }`;
      `D3D12DDI_RESOURCE_LAYOUT`: `D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED`=1 → `pLayout` = `D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT { Format; PhysicalWidth; PhysicalHeight; PhysicalDepth /*in blocks*/; Pitch; SlicePitch; }`;
      `_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED`=2 → `D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT { Format; VirtualWidth/Height/Depth (texels); PhysicalWidth/Height/Depth; Pitch; SlicePitch; }`;
      `_RL_SELECT_SUBRESOURCE`=0x40 → `UMD.Offset` **is the subresource index**.
    - → `L->CopyTextureRegion(&dst, DstX, DstY, DstZ, &src, (D3D12_BOX*)pSrcBox)` with
      SELECT_SUBRESOURCE → `{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex=(UINT)Offset}`;
      PITCHED → `{res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint={Offset, {Format, Width, Height, Depth, RowPitch=Pitch}}}`
      (virtual: Width=VirtualWidth...; physical: texels = blocks × block dim for BC formats).
12. **`pfnResourceCopy`** — `PFND3D12DDI_RESOURCECOPY` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HRESOURCE /*dst*/, D3D12DDI_HRESOURCE /*src*/)` → `L->CopyResource(dst, src)`.
13. **`pfnCopyTiles`** — `PFND3D12DDI_COPYTILES` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HRESOURCE hResource, _In_ const D3D12DDI_TILED_RESOURCE_COORDINATE* pRegionStartCoord, _In_ const D3D12DDI_TILE_REGION_SIZE* pRegionSize, D3D12DDI_HRESOURCE hBuffer, UINT64 BufferStartOffsetInBytes, D3D12DDI_TILE_COPY_FLAGS)` → `L->CopyTiles(...)` (identical order/structs).
14. **`pfnCopyBufferRegion`** — `PFND3D12DDI_COPYBUFFERREGION_0003` — `(D3D12DDI_HCOMMANDLIST, D3D12DDIARG_BUFFER_PLACEMENT Dst, D3D12DDIARG_BUFFER_PLACEMENT Src, UINT64 SrcBytes)` (by value) → `L->CopyBufferRegion(H(Dst.UMD.hResource), Dst.UMD.Offset, H(Src.UMD.hResource), Src.UMD.Offset, SrcBytes)`.
15. **`pfnResourceResolveSubresource`** — `PFND3D12DDI_RESOURCERESOLVESUBRESOURCE` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HRESOURCE /*dst*/, UINT /*dstSub*/, D3D12DDI_HRESOURCE /*src*/, UINT /*srcSub*/, DXGI_FORMAT)` → `L->ResolveSubresource`.
16. **`pfnExecuteBundle`** — `PFND3D12DDI_EXECUTE_BUNDLE` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HCOMMANDLIST /*bundle*/)` → `L->ExecuteBundle(H(bundle)->list)`.
17. **`pfnExecuteIndirect`** — `PFND3D12DDI_EXECUTE_INDIRECT` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HCOMMANDSIGNATURE, UINT MaxCommandCount, D3D12DDIARG_BUFFER_PLACEMENT ArgumentBuffer, D3D12DDIARG_BUFFER_PLACEMENT CountBuffer)` → `L->ExecuteIndirect(sig, Max, argRes, argOff, countRes_or_NULL, countOff)`.
18. **`pfnResourceBarrier`** — `PFND3D12DDI_RESOURCEBARRIER_0022` — `(D3D12DDI_HCOMMANDLIST, UINT Count, _In_reads_(Count) CONST D3D12DDIARG_RESOURCE_BARRIER_0022*)`
    - `D3D12DDIARG_RESOURCE_BARRIER_0022 { D3D12DDI_RESOURCE_BARRIER_TYPE Type; D3D12DDI_RESOURCE_BARRIER_FLAGS Flags; union { D3D12DDI_RESOURCE_TRANSITION_BARRIER_0003 Transition{hResource, Subresource, StateBefore, StateAfter}; D3D12DDI_RESOURCE_RANGED_BARRIER_0022 Ranged{hResource, Subresource, D3D12DDI_RANGE Range{Begin, End}}; D3D12DDI_RESOURCE_UAV_BARRIER UAV{hResource}; }; }`
    - Types: TRANSITION=0, ALIASING=1 (deprecated, no union member in _0022), UAV=2, `_0022_RANGED`=3.
      Flags: BEGIN_ONLY=1, END_ONLY=2 (identical to API), `_0022_ATOMIC_COPY`=4, `_0022_ALIASING`=8 (DDI-only).
    - Convert array → `D3D12_RESOURCE_BARRIER[]` → `L->ResourceBarrier`. TRANSITION: states/subresource identical
      (`0xffffffff` = all). UAV: `{UAV, pResource}` (NULL ok). RANGED with `_0022_ALIASING` flag → API aliasing barrier
      `{pResourceBefore=NULL, pResourceAfter=res}` [infer: how the runtime encodes API aliasing barriers]; other RANGED
      (atomic copy) → drop or full UAV barrier. Mask DDI-only flag bits out of API `Flags`.
19. **`pfnBlt`** — `PFND3D12DDI_BLT` — `(D3D12DDI_HCOMMANDLIST, _In_ CONST D3D12DDIARG_BLT*)` — `{ hDstResource; DstSubresource; DstLeft/Top/Right/Bottom; hSrcResource; SrcSubresource; SrcLeft/Top/Right/Bottom; DXGI_DDI_ARG_BLT_FLAGS Flags; DXGI_DDI_MODE_ROTATION Rotate; }`. No API (DXGI present helper, stretch + format convert + rotate). Same size/format/no rotation → `CopyTextureRegion`; otherwise needs a driver-internal blit shader.
20. **`pfnPresent`** — `PFND3D12DDI_PRESENT_0051` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HCOMMANDQUEUE, _In_ CONST D3D12DDIARG_PRESENT_0001*, _Out_ D3D12DDI_PRESENT_0051*, _Out_opt_ D3D12DDI_PRESENT_CONTEXTS_0051*, _Out_opt_ D3D12DDI_PRESENT_HWQUEUES_0051*)`
    - In: `{ CONST D3D12DDI_ARG_PRESENTSURFACE* phSurfacesToPresent /*{hSurface, SubResourceIndex}*/; UINT SurfacesToPresent; D3D12DDI_HRESOURCE hDstResource; UINT DstSubResourceIndex; DXGI_DDI_PRESENT_FLAGS Flags; DXGI_DDI_FLIP_INTERVAL_TYPE FlipInterval; D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceID; CONST RECT* pDirtyRects; UINT DirtyRects; UINT PrivateDriverDataSize; VOID* pPrivateDriverData; BOOL OptimizeForComposition; }`
    - Out: `D3D12DDI_PRESENT_0051 { D3DKMT_HANDLE BroadcastSrcAllocation[D3DDDI_MAX_BROADCAST_CONTEXT+1]; D3DKMT_HANDLE BroadcastDstAllocation[...]; BOOL AddedGpuWork; UINT BackBufferMultiplicity; BOOL SyncIntervalOverrideValid; DXGI_DDI_FLIP_INTERVAL_TYPE SyncIntervalOverride; }`,
      `D3D12DDI_PRESENT_CONTEXTS_0051 { HANDLE hContext; UINT BroadcastContextCount; HANDLE BroadcastContext[...]; }`, `D3D12DDI_PRESENT_HWQUEUES_0051 { UINT BroadcastQueueCount; HANDLE hHwQueues[...]; }`.
    - No API. Driver records any blit, reports the **KM allocation handles** of the source/destination and the KM
      context/HW queue; the runtime then performs the KM present. Requires swapchain buffers to be KM allocations —
      outside what vkd3d provides; needs WinBoat's own present path (cf. stage B fast present).
21. **`pfnBeginQuery`** — `PFND3D12DDI_BEGIN_END_QUERY_0003` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HQUERYHEAP, D3D12DDI_QUERY_TYPE, UINT)` → `L->BeginQuery(heap, type, index)`.
22. **`pfnEndQuery`** — same PFN → `L->EndQuery`.
23. **`pfnResolveQueryData`** — `PFND3D12DDI_RESOLVE_QUERY_DATA` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HQUERYHEAP, D3D12DDI_QUERY_TYPE, UINT StartElement, UINT ElementCount, D3D12DDI_HRESOURCE hDrvDestinationBuffer, UINT64 DestinationOffset)` → `L->ResolveQueryData`.
24. **`pfnSetPredication`** — `PFND3D12DDI_SET_PREDICATION` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HRESOURCE, UINT64, D3D12DDI_PREDICATION_OP)` → `L->SetPredication(res_or_NULL, offset, op)`.
25. **`pfnIaSetTopology`** — `PFND3D12DDI_IA_SETTOPOLOGY_0003` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_PRIMITIVE_TOPOLOGY)` → `L->IASetPrimitiveTopology((D3D_PRIMITIVE_TOPOLOGY)t)`.
26. **`pfnRsSetViewports`** — `PFND3D12DDI_RS_SETVIEWPORTS_0003` — `(D3D12DDI_HCOMMANDLIST, UINT Count, _In_reads_(Count) CONST D3D12DDI_VIEWPORT*)` → `L->RSSetViewports`.
27. **`pfnRsSetScissorRects`** — `PFND3D12DDI_RS_SETSCISSORRECTS_0003` — `(D3D12DDI_HCOMMANDLIST, UINT Count, _In_reads_(Count) CONST D3D12DDI_RECT*)` → `L->RSSetScissorRects`.
28. **`pfnOmSetBlendFactor`** — `PFND3D12DDI_OM_SETBLENDFACTOR` — `(D3D12DDI_HCOMMANDLIST, CONST FLOAT[4])` → `L->OMSetBlendFactor`.
29. **`pfnOmSetStencilRef`** — `PFND3D12DDI_OM_SETSTENCILREF` — `(D3D12DDI_HCOMMANDLIST, UINT)` → `L->OMSetStencilRef`.
30. **`pfnSetPipelineState`** — `PFND3D12DDI_SET_PIPELINE_STATE` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HPIPELINESTATE)` → `L->SetPipelineState(pso)` (NULL allowed only for ClearState-like resets [infer]).
31. **`pfnSetDescriptorHeaps`** — `PFND3D12DDI_SET_DESCRIPTOR_HEAPS_0003` — `(D3D12DDI_HCOMMANDLIST, _In_ UINT NumDescriptorHeaps, _In_reads_(NumDescriptorHeaps) D3D12DDI_HDESCRIPTORHEAP*)` → build `ID3D12DescriptorHeap*[]` → `L->SetDescriptorHeaps`.
32. **`pfnSetComputeRootSignature`** — `PFND3D12DDI_SET_ROOT_SIGNATURE` — `(D3D12DDI_HCOMMANDLIST, _In_ D3D12DDI_HROOTSIGNATURE)` → `L->SetComputeRootSignature`.
33. **`pfnSetGraphicsRootSignature`** — same PFN → `L->SetGraphicsRootSignature`.
34. **`pfnSetComputeRootDescriptorTable`** — `PFND3D12DDI_SET_ROOT_DESCRIPTOR_TABLE` — `(D3D12DDI_HCOMMANDLIST, _In_ UINT RootParameterIndex, _In_ D3D12DDI_GPU_DESCRIPTOR_HANDLE BaseDescriptor)` → `L->SetComputeRootDescriptorTable(i, {ptr})`.
35. **`pfnSetGraphicsRootDescriptorTable`** — same PFN → `L->SetGraphicsRootDescriptorTable`.
36. **`pfnSetComputeRoot32BitConstant`** — `PFND3D12DDI_SET_ROOT_32BIT_CONSTANT` — `(D3D12DDI_HCOMMANDLIST, UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)` → `L->SetComputeRoot32BitConstant`.
37. **`pfnSetGraphicsRoot32BitConstant`** — same PFN → `L->SetGraphicsRoot32BitConstant`.
38. **`pfnSetComputeRoot32BitConstants`** — `PFND3D12DDI_SET_ROOT_32BIT_CONSTANTS_0003` — `(D3D12DDI_HCOMMANDLIST, UINT RootParameterIndex, UINT Num32BitValuesToSet, CONST void* pSrcData, UINT DestOffsetIn32BitValues)` → `L->SetComputeRoot32BitConstants`.
39. **`pfnSetGraphicsRoot32BitConstants`** — same PFN → `L->SetGraphicsRoot32BitConstants`.
40. **`pfnSetComputeRootConstantBufferView`** — `PFND3D12DDI_SET_ROOT_BUFFER_VIEW` — `(D3D12DDI_HCOMMANDLIST, UINT RootParameterIndex, _In_ D3D12DDI_GPU_VIRTUAL_ADDRESS BufferLocation)` → `L->SetComputeRootConstantBufferView`.
41. **`pfnSetGraphicsRootConstantBufferView`** — same PFN → `L->SetGraphicsRootConstantBufferView`.
42. **`pfnSetComputeRootShaderResourceView`** — same PFN → `L->SetComputeRootShaderResourceView`.
43. **`pfnSetGraphicsRootShaderResourceView`** — same PFN → `L->SetGraphicsRootShaderResourceView`.
44. **`pfnSetComputeRootUnorderedAccessView`** — same PFN → `L->SetComputeRootUnorderedAccessView`.
45. **`pfnSetGraphicsRootUnorderedAccessView`** — same PFN → `L->SetGraphicsRootUnorderedAccessView`.
46. **`pfnIASetIndexBuffer`** — `PFND3D12DDI_IA_SET_INDEX_BUFFER` — `(D3D12DDI_HCOMMANDLIST, _In_ CONST D3D12DDI_INDEX_BUFFER_VIEW* pDesc)` → `L->IASetIndexBuffer((D3D12_INDEX_BUFFER_VIEW*)pDesc)` (NULL → unbind).
47. **`pfnIASetVertexBuffers`** — `PFND3D12DDI_IA_SET_VERTEX_BUFFERS_0003` — `(D3D12DDI_HCOMMANDLIST, _In_ UINT StartSlot, _In_ UINT NumViews, _In_reads_opt_(NumViews) CONST D3D12DDI_VERTEX_BUFFER_VIEW* pViews)` → `L->IASetVertexBuffers`.
48. **`pfnSOSetTargets`** — `PFND3D12DDI_SO_SET_TARGETS_0003` — `(D3D12DDI_HCOMMANDLIST, _In_ UINT StartSlot, _In_ UINT NumViews, _In_reads_opt_(NumViews) CONST D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW* pViews)` → `L->SOSetTargets`.
49. **`pfnOMSetRenderTargets`** — `PFND3D12DDI_OM_SET_RENDER_TARGETS_0003` — `(D3D12DDI_HCOMMANDLIST, _In_ UINT NumRenderTargetDescriptors, _In_ CONST D3D12DDI_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, _In_ BOOL RTsSingleHandleToDescriptorRange, _In_opt_ CONST D3D12DDI_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)` → `L->OMSetRenderTargets` (identical).
50. **`pfnSetMarker`** — `PFND3D12DDI_SET_MARKER` — `(D3D12DDI_HCOMMANDLIST, UINT64)` — **not** PIX `SetMarker`; runtime-generated breadcrumb ID (only with `D3D12DDI_COMMAND_LIST_FLAG_ENABLE_MARKERS`). No API; no-op.
51. **`pfnClearRootArguments`** — `PFND3D12DDI_CLEAR_ROOT_ARGUMENTS` — `(D3D12DDI_HCOMMANDLIST)` — no API. [infer] part of the runtime's `ClearState` emulation (invalidate root args/signatures). No-op is safe: vkd3d resets root args on the next `Set*RootSignature`.
52. **`pfnAtomicCopyBufferRegion`** — `PFND3D12DDI_COPYBUFFERREGION_0003` — same signature as 14. API `AtomicCopyBufferUINT/UINT64` are **stubs in vkd3d** (command.c ~17372) → use `L->CopyBufferRegion`.
53. **`pfnOMSetDepthBounds`** — `PFND3D12DDI_OM_SETDEPTHBOUNDS_0025` — `(D3D12DDI_HCOMMANDLIST, FLOAT Min, FLOAT Max)` → `ID3D12GraphicsCommandList1::OMSetDepthBounds`.
54. **`pfnSetSamplePositions`** — `PFND3D12DDI_SETSAMPLEPOSITIONS_0027` — `(D3D12DDI_HCOMMANDLIST, UINT NumSamplesPerPixel, UINT NumPixels, D3D12DDI_SAMPLE_POSITION* pSamplePositions)` → `ID3D12GraphicsCommandList1::SetSamplePositions` (vkd3d stub — don't advertise programmable sample positions).
55. **`pfnResourceResolveSubresourceRegion`** — `PFND3D12DDI_RESOURCERESOLVESUBRESOURCEREGION_0027` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HRESOURCE DstResource, UINT DstSubresource, UINT DstX, UINT DstY, D3D12DDI_HRESOURCE SrcResource, UINT SrcSubresource, D3D12DDI_RECT* pSrcRect, DXGI_FORMAT Format, D3D12DDI_RESOLVE_MODE ResolveMode)` → `ID3D12GraphicsCommandList1::ResolveSubresourceRegion` (identical order).
56. **`pfnSetProtectedResourceSession`** — `PFND3D12DDI_SETPROTECTEDRESOURCESESSION_0030` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HPROTECTEDRESOURCESESSION_0030)` — vkd3d stub; no-op.
57. **`pfnWriteBufferImmediate`** — `PFND3D12DDI_WRITEBUFFERIMMEDIATE_0032` — `(D3D12DDI_HCOMMANDLIST, UINT Count, _In_reads_(Count) CONST D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032* pParams /*{Dst VA, UINT32 Value}*/, _In_reads_opt_(Count) CONST D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032* pModes)` → `ID3D12GraphicsCommandList2::WriteBufferImmediate` (identical).
58. **`pfnSetViewInstanceMask`** — `PFND3D12DDI_SETVIEWINSTANCEMASK_0033` — `(D3D12DDI_HCOMMANDLIST, UINT Mask)` → `ID3D12GraphicsCommandList1::SetViewInstanceMask`.
59. **`pfnInitializeMetaCommand`** — `PFND3D12DDI_INITIALIZE_META_COMMAND_0052` — `(D3D12DDI_HCOMMANDLIST, D3D12DDI_HMETACOMMAND_0052, _In_ CONST void*, _In_ SIZE_T)` → `ID3D12GraphicsCommandList4::InitializeMetaCommand` (unreachable if 0 meta commands enumerated).
60. **`pfnExecuteMetaCommand`** — `PFND3D12DDI_EXECUTE_META_COMMAND_0052` — same → `ID3D12GraphicsCommandList4::ExecuteMetaCommand`.

(Entry count check: 2 lifecycle + 58 commands = 60 distinct names; the header table has 60 named fields —
`pfnBeginQuery`/`pfnEndQuery` and the 12 root-param setters share PFN types.)

### 1.3 `D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001` (7 slots)

1. **`pfnExecuteCommandLists`** — `PFND3D12DDI_EXECUTECOMMANDLISTS` — `VOID (D3D12DDI_HCOMMANDQUEUE, UINT Count, _In_reads_(Count) CONST D3D12DDI_HCOMMANDLIST* pCommandLists)`
   - Receives **our** command-list handles (closed lists). → build `ID3D12CommandList*[]` → `ID3D12CommandQueue::ExecuteCommandLists`.
     Then make runtime fence ordering hold (section 2d). No return; errors → `pfnSetErrorCb` (device removed).
2. **`pfnUnused`** — `void*` — set `nullptr`.
3. **`pfnUnused2`** — `void*` — set `nullptr`.
4. **`pfnUpdateTileMappings`** — `PFND3D12DDI_UPDATETILEMAPPINGS` — `VOID (D3D12DDI_HCOMMANDQUEUE, D3D12DDI_HRESOURCE, UINT NumTiledResourceRegions, _In_reads_(NumTiledResourceRegions) const D3D12DDI_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoords, _In_reads_opt_(NumTiledResourceRegions) const D3D12DDI_TILE_REGION_SIZE* pResourceRegionSizes, D3D12DDI_HHEAP, UINT NumRanges, _In_reads_opt_(NumRanges) const D3D12DDI_TILE_RANGE_FLAGS*, _In_reads_opt_(NumRanges) const UINT* pHeapStartOffsets, _In_reads_opt_(NumRanges) const UINT* pRangeTileCounts, D3D12DDI_TILE_MAPPING_FLAGS)`
   - → `ID3D12CommandQueue::UpdateTileMappings(res, n, coords, sizes, H(heap)->heap_or_NULL, NumRanges, flags, offsets, counts, flags)` (identical order).
5. **`pfnCopyTileMappings`** — `PFND3D12DDI_COPYTILEMAPPINGS` — `VOID (D3D12DDI_HCOMMANDQUEUE, D3D12DDI_HRESOURCE hDstResource, _In_ const D3D12DDI_TILED_RESOURCE_COORDINATE* pDstRegionStartCoord, D3D12DDI_HRESOURCE hSrcResource, _In_ const D3D12DDI_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoord, _In_ const D3D12DDI_TILE_REGION_SIZE*, D3D12DDI_TILE_MAPPING_FLAGS)` → `ID3D12CommandQueue::CopyTileMappings`.
6. **`pfnSignalFence`** — `PFND3D12DDI_SIGNAL_FENCE` — `void (D3D12DDI_HCOMMANDQUEUE, D3D12DDIARG_FENCE_OPERATION*)`
   - `D3D12DDIARG_FENCE_OPERATION { D3D12DDI_HFENCE Fence; UINT64 Value; UINT PhysicalAdapterMask /*Out*/; }`.
   - Not `ID3D12CommandQueue::Signal` itself: the runtime performs the signal on the queue's KM context; this callback
     lets the driver pick broadcast adapters (LDA). Sample: "only needed for MultiGPU/LDA support", empty body.
     Set `PhysicalAdapterMask = 1`. Possible hook to flush/order vkd3d work before the runtime's KM signal [infer].
7. **`pfnWaitForFence`** — `PFND3D12DDI_WAIT_FOR_FENCE` — `void (D3D12DDI_HCOMMANDQUEUE, D3D12DDIARG_FENCE_OPERATION*)` — same idea for `Wait`. Set mask 1.

### 1.4 `DXGI1_4_DDI_BASE_FUNCTIONS` (21 slots, `dxgiddi.h`)

All are `HRESULT (__stdcall*)(ARG*)`. `hDevice` is `DXGI_DDI_HDEVICE` (= our device `pDrvPrivate` as `UINT_PTR`),
resources are `DXGI_DDI_HRESOURCE` (= our resource `pDrvPrivate`). None has an ID3D12 equivalent; vkd3d's own
DXGI swapchain (`swapchain.c`) is not reachable through the DDI. For D3D12 the real present goes through the command
list `pfnPresent` (1.2 #20); these are legacy/auxiliary. The sample returns `E_NOTIMPL` for all.

1. **`pfnPresent`** — `(DXGI_DDI_ARG_PRESENT*)` — `{ hDevice; hSurfaceToPresent; SrcSubResourceIndex; hDstResource; DstSubResourceIndex; void* pDXGIContext; DXGI_DDI_PRESENT_FLAGS Flags; DXGI_DDI_FLIP_INTERVAL_TYPE FlipInterval; }` — [infer] unused by D3D12 (uses list `pfnPresent`). `E_NOTIMPL`.
2. **`pfnGetGammaCaps`** — `(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS*)` — `{ hDevice; DXGI_GAMMA_CONTROL_CAPABILITIES* pGammaCapabilities; }` — zero caps, S_OK (or E_NOTIMPL).
3. **`pfnSetDisplayMode`** — `(DXGI_DDI_ARG_SETDISPLAYMODE*)` — `{ hDevice; hResource; SubResourceIndex; }` — fullscreen primary; S_OK no-op / E_NOTIMPL.
4. **`pfnSetResourcePriority`** — `(DXGI_DDI_ARG_SETRESOURCEPRIORITY*)` — `{ hDevice; hResource; UINT Priority; }` — S_OK no-op.
5. **`pfnQueryResourceResidency`** — `(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY*)` — `{ hDevice; CONST DXGI_DDI_HRESOURCE* pResources; DXGI_DDI_RESIDENCY* pStatus /*out*/; SIZE_T Resources; }` — fill `DXGI_DDI_RESIDENCY_FULLY_RESIDENT`(1), S_OK.
6. **`pfnRotateResourceIdentities`** — `(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES*)` — `{ hDevice; CONST DXGI_DDI_HRESOURCE* pResources; UINT Resources; }` — swap backing objects 0←1←2… (blt-model swapchains; D3D12 is flip-model only → likely unused). E_NOTIMPL.
7. **`pfnBlt`** — `(DXGI_DDI_ARG_BLT*)` — `{ hDevice; hDstResource; DstSubresource; DstLeft/Top/Right/Bottom; hSrcResource; SrcSubresource; DXGI_DDI_ARG_BLT_FLAGS Flags; DXGI_DDI_MODE_ROTATION Rotate; }` — E_NOTIMPL (see list `pfnBlt`).
8. **`pfnResolveSharedResource`** — `(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE*)` — `{ hDevice; hResource; }` — S_OK no-op.
9. **`pfnBlt1`** — `(DXGI_DDI_ARG_BLT1*)` — as Blt plus `SrcLeft/Top/Right/Bottom` — E_NOTIMPL.
10. **`pfnOfferResources`** — `(DXGI_DDI_ARG_OFFERRESOURCES*)` — `{ hDevice; const DXGI_DDI_HRESOURCE* pResources; UINT Resources; D3DDDI_OFFER_PRIORITY Priority; }` — S_OK.
11. **`pfnReclaimResources`** — `(DXGI_DDI_ARG_RECLAIMRESOURCES*)` — `{ hDevice; const DXGI_DDI_HRESOURCE* pResources; BOOL* pDiscarded /*opt out*/; UINT Resources; }` — `pDiscarded[i]=FALSE`, S_OK.
12. **`pfnGetMultiplaneOverlayCaps`** — `(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS*)` — `{ hDevice; VidPnSourceId; DXGI_DDI_MULTIPLANE_OVERLAY_CAPS MultiplaneOverlayCaps{MaxPlanes, NumCapabilityGroups}; }` — `MaxPlanes=1, NumCapabilityGroups=0` (or E_NOTIMPL).
13. **`pfnGetMultiplaneOverlayGroupCaps`** — `(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYGROUPCAPS*)` — `{ hDevice; VidPnSourceId; UINT GroupIndex; DXGI_DDI_MULTIPLANE_OVERLAY_GROUP_CAPS ...; }` — E_NOTIMPL.
14. **`pfnReserved1`** — `(void*)` — `nullptr`.
15. **`pfnPresentMultiplaneOverlay`** — `(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY*)` — `{ hDevice; pDXGIContext; VidPnSourceId; Flags; FlipInterval; UINT PresentPlaneCount; DXGI_DDI_PRESENT_MULTIPLANE_OVERLAY* pPresentPlanes; UINT Reserved; }` — E_NOTIMPL (no MPO).
16. **`pfnReserved2`** — `(void*)` — `nullptr`.
17. **`pfnPresent1`** — `(DXGI_DDI_ARG_PRESENT1*)` — `{ hDevice; CONST DXGI_DDI_ARG_PRESENTSURFACE* phSurfacesToPresent; UINT SurfacesToPresent; hDstResource; DstSubResourceIndex; pDXGIContext; Flags; FlipInterval; UINT Reserved; CONST RECT* pDirtyRects; UINT DirtyRects; UINT BackBufferMultiplicity /*out, LDA*/; }` — D3D10/11 present path; [infer] D3D12 uses list `pfnPresent`. E_NOTIMPL.
18. **`pfnCheckPresentDurationSupport`** — `(DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT*)` — `{ hDevice; VidPnSourceId; UINT DesiredPresentDuration; UINT ClosestSmallerDuration /*out*/; UINT ClosestLargerDuration /*out*/; }` — outputs 0, S_OK.
19. **`pfnTrimResidencySet`** — `(DXGI_DDI_ARG_TRIMRESIDENCYSET*)` — `{ hDevice; D3DDDI_TRIMRESIDENCYSET_FLAGS TrimFlags; UINT64 NumBytesToTrim; }` — S_OK no-op.
20. **`pfnCheckMultiplaneOverlayColorSpaceSupport`** — `(DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYCOLORSPACESUPPORT*)` — `{ hDevice; VidPnSourceId; DXGI_FORMAT Format; D3DDDI_COLOR_SPACE_TYPE ColorSpace; BOOL Supported /*out*/; }` — `Supported=FALSE`, S_OK.
21. **`pfnPresentMultiplaneOverlay1`** — `(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY1*)` — E_NOTIMPL.

---

## 2. Object model the runtime expects

### 2.0 Placement, handles, tables
- Pattern for every object type: `SIZE_T n = pfnCalcPrivateXSize(hDevice, pArgs)` → runtime allocates `n` bytes →
  `pfnCreateX(hDevice, pArgs, hX{pDrvPrivate=mem} [, hRTX])` → driver `new (hX.pDrvPrivate) Obj(...)`. Destroy =
  explicit destructor only. Handles are passed by value and compared by `pDrvPrivate`. `CastFrom(h) = static_cast<Obj*>(h.pDrvPrivate)`.
- Calc functions may be called with the same args right before Create; sizes must be deterministic for given args.
  Heap+resource is a combined call returning two sizes (`D3D12DDI_HEAP_AND_RESOURCE_SIZES`).
- Runtime handles to keep: `D3D12DDI_HRTDEVICE` (from CreateDevice; for `pfnSetErrorCb`, KM callbacks),
  `D3D12DDI_HRTCOMMANDQUEUE` (context creation), `D3D12DDI_HRTCOMMANDLIST` (`pfnSetCommandListDDITableCb`,
  `pfnSetCommandListErrorCb`), `D3D12DDI_HRTRESOURCE` (heap/resource; `hResource` of `pfnAllocateCb` if KM allocations are made),
  `D3D12DDI_HRTPIPELINESTATE`, `D3D12DDI_HRTTABLE` (from `pfnFillDdiTable` for `D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D`, indexed by `uTableNum`).
- Device-level errors from VOID DDIs: `pfnSetErrorCb(hRTDevice, hr)` (`E_OUTOFMEMORY`, `DXGI_ERROR_DEVICE_REMOVED`).
- Device callbacks struct for 0050+: `D3D12DDI_CORELAYER_DEVICECALLBACKS_0050 { pfnSetErrorCb; pfnSetCommandListErrorCb; pfnSetCommandListDDITableCb; pfnCreateContextCb; pfnCreateContextVirtualCb; pfnDestroyContextCb; pfnCreatePagingQueueCb; pfnDestroyPagingQueueCb; pfnMakeResidentCb; pfnEvictCb; pfnReclaimAllocations2Cb; pfnOfferAllocationsCb; pfnAllocateCb; pfnDeallocateCb; pfnCreateSchedulingGroupContextCb; pfnCreateSchedulingGroupContextVirtualCb; pfnCreateHwQueueCb; }`.

### 2a. Heaps and resources
`pfnCreateHeapAndResource(hDevice, pHeapDesc, hHeap, hRTResource, pResourceDesc, pClearValues, hProtSession, hResource)`
covers all API creation paths [sample comment + code]:

| API call | `pHeapDesc` | `hHeap` | `pResourceDesc` | our vkd3d call |
|---|---|---|---|---|
| `CreateCommittedResource` | non-NULL | new (construct) | non-NULL, `ReuseBufferGPUVA.Offset`=0 | `CreateCommittedResource(&props, heapFlags, &desc, state, clear, ...)`; heap object just borrows the resource |
| `CreateHeap` | non-NULL | new | NULL | `CreateHeap(&D3D12_HEAP_DESC{ByteSize, props, Alignment, flags})`; if CPU-visible and buffers allowed, also a placed whole-heap buffer at offset 0 (for `MapHeap`) |
| `CreatePlacedResource` | NULL | **existing heap** | non-NULL, `ReuseBufferGPUVA.BaseAddress.UMD.Offset` = heap offset | `CreatePlacedResource(H(hHeap)->heap, Offset, &desc, state, clear, ...)` |
| `CreateReservedResource` | NULL | NULL [infer] | non-NULL | `CreateReservedResource(&desc, state, clear, ...)` |

(The sample computes placed-buffer VA as `heap VA + ReuseBufferGPUVA.BaseAddress.UMD.Offset` and destroys the heap only
when `hHeap` is non-NULL in `pfnDestroyHeapAndResource`.)

Conversions:
- Heap properties: always `D3D12_HEAP_TYPE_CUSTOM` with `CPUPageProperty = DDI+1`, `MemoryPoolPreference = DDI+1`
  (vkd3d supports CUSTOM: memory.c ~732; `NOT_AVAILABLE`→DEVICE_LOCAL, `WRITE_COMBINE`→HOST_VISIBLE|COHERENT
  (+DEVICE_LOCAL if L1), `WRITE_BACK`→HOST_VISIBLE|CACHED). Don't map back to UPLOAD/READBACK: vkd3d then enforces
  buffer-only / GENERIC_READ / COPY_DEST rules (resource.c ~3228) that the app's CUSTOM heaps don't follow.
  What the runtime produces from API UPLOAD/READBACK/DEFAULT depends on the `D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE`
  caps we report (UMA/CacheCoherent) — keep consistent with vkd3d's `GetCustomHeapProperties` (device.c ~6895).
- Heap flags (DDI = allowed categories): `DENY_BUFFERS` if `!(Flags & D3D12DDI_HEAP_FLAG_BUFFERS)`;
  `DENY_RT_DS_TEXTURES` if `!(Flags & D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES)`; `DENY_NON_RT_DS_TEXTURES` if
  `!(Flags & D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES)`; `PRIMARY`(0x10) ~ `ALLOW_DISPLAY`; `COHERENT_SYSTEMWIDE` (cross-adapter) → `SHARED_CROSS_ADAPTER`.
  [infer] For committed resources pass `D3D12_HEAP_FLAG_NONE` (vkd3d validates deny-flags only for placed).
- Resource desc: `Dimension = ResourceType`, `Alignment = 0` (placement already decided by runtime), Width/Height/
  DepthOrArraySize/MipLevels/Format/SampleDesc/Layout copied. Flags: RENDER_TARGET(1)→`ALLOW_RENDER_TARGET`(1),
  DEPTH_STENCIL(2)→`ALLOW_DEPTH_STENCIL`(2), `_0022_UNORDERED_ACCESS`(0x80)→`ALLOW_UNORDERED_ACCESS`(4),
  CROSS_ADAPTER(4)→`ALLOW_CROSS_ADAPTER`(0x10), SIMULTANEOUS_ACCESS(8)→`ALLOW_SIMULTANEOUS_ACCESS`(0x20),
  `_0020_VIDEO_DECODE_REFERENCE_ONLY`(0x20)→0x40, `_0080_VIDEO_ENCODE_REFERENCE_ONLY`(0x1000)→0x80,
  `_0088_RAYTRACING_ACCELERATION_STRUCTURE`(0x2000)→0x100, and `DENY_SHADER_RESOURCE`(8) iff `DEPTH_STENCIL && !SHADER_RESOURCE(0x10)`.
  `pRowMajorLayout` (ROW_MAJOR textures): no API field; vkd3d computes its own pitch — verify match or reject.
- GPU VA: after creation the runtime calls `pfnCheckResourceVirtualAddress(hResource)` → return
  `ID3D12Resource::GetGPUVirtualAddress()`. For placed buffers vkd3d returns heap VA + offset already.
- `pfnMapHeap(hHeap, &p)`: committed → `resource->Map(0, NULL, &p)` (buffers; textures only if CPU-visible);
  `CreateHeap` heap → `Map` of the whole-heap placed buffer (shares the heap's `VkDeviceMemory` mapping, so pointer + offset
  addresses every placed resource). Make it idempotent; `pfnUnmapHeap` no-op. The runtime adds
  `pfnCheckSubresourceInfo().Offset` (+ the resource's heap offset) itself [infer].
- Residency/KM: none of these need KM allocations unless we want sharing (`pfnOpenHeapAndResource`,
  `pfnCheckResourceAllocationHandle`) or the KM present path (`pfnPresent`). `pfnMakeResident`/`pfnEvict` → S_OK.

### 2b. Descriptor heaps
- **Driver defines everything**: descriptor size (`pfnGetDescriptorSizeInBytes`), CPU start (`pfnGetCPUDescriptorHandleForHeapStart`,
  `SIZE_T ptr`), GPU start (`pfnGetGPUDescriptorHandleForHeapStart`, `UINT64 ptr`). The app computes
  `start + i*size` and the runtime hands those values back in `DestDescriptor`, `CopyDescriptors`, `OMSetRenderTargets`,
  `Clear*View`, `Set*RootDescriptorTable`. The sample uses the descriptor's CPU memory address as handle and
  placement-news descriptors at `DestDescriptor.ptr`.
- For vkd3d: return vkd3d's own values → every handle passes through unchanged (vkd3d CPU handles are opaque
  pointers/encodings; fine as long as nothing else interprets them).
- `CopyDescriptors` args: dest range starts + sizes (NULL sizes = all ranges of 1), src range starts + sizes, one heap
  type — exactly `ID3D12Device::CopyDescriptors`. `CopyDescriptorsSimple(n, dst, src, type)` likewise.
- Heap flags: DDI `CPU_VISIBLE`(1) / `SHADER_VISIBLE`(2) vs API `SHADER_VISIBLE`(1) — convert.

### 2c. Command pools / recorders / lists (0040 model)
- **Pool** (`pfnCreateCommandPool`, `PoolFlags` only) = backing memory = `ID3D12CommandAllocator`. Has no list type →
  hold up to 4 lazily-created allocators (DIRECT/COMPUTE/COPY/BUNDLE). `pfnResetCommandPool` = `ID3D12CommandAllocator::Reset`
  on all of them (= app `allocator->Reset()`; runtime has already validated GPU completion).
- **Recorder** (`pfnCreateCommandRecorder`, `QueueFlags`) = a "pen" that writes into whichever pool it targets;
  `pfnCommandRecorderSetCommandPoolAsTarget(recorder, pool)` re-targets it. The sample's recorder is just `{QueueFlags, pool*}`.
  [infer] The runtime pairs one recorder with each API allocator (so it can swap the pool behind an allocator), but the
  driver must only rely on: at `Reset` time, `recorder->pool` is the memory to record into.
- **List** (`pfnCreateCommandList`, `Type`+`QueueFlags`, no allocator) → `ID3D12Device4::CreateCommandList1` (created
  closed). Must call `pfnSetCommandListDDITableCb`.
- **`pfnResetCommandList(list, {hDrvCommandRecorder, ID, CommandListFlags})`** → `list->Reset(H(recorder)->pool->allocator(type), NULL)`.
  Note: `ID3D12GraphicsCommandList::Reset(allocator, initialPSO)` — DDI never passes the PSO; runtime issues
  `pfnSetPipelineState` separately [infer]. vkd3d rejects mismatched allocator/list types, hence per-type allocators.
- **`pfnCloseCommandList(list)`** → `list->Close()`; failure → `pfnSetCommandListErrorCb`.
- Sample behaviour for comparison: Reset releases previous buffers to the old pool, acquires from `recorder->GetCommandPool()`;
  Close moves the current buffer to the "filled" list; ExecuteCommandLists submits the filled buffers.

### 2d. Queues and fences
- `pfnCreateCommandQueue` → vkd3d queue **plus** a KM context created with `pfnCreateContextCb(hRTCommandQueue, ...)`
  (sample). The runtime implements `ID3D12Fence` entirely on KM monitored fences and implements
  `ID3D12CommandQueue::Signal/Wait` by `D3DKMTSignalSynchronizationObjectFromGpu*/WaitFromGpu` **on that context** (that is
  why the context creation callback takes the RT queue handle). `pfnCreateFence` merely tells the driver the fence's
  GPU VAs (`D3D12DDI_FENCE.FenceValue/FenceMonitoredValue`); `pfnSignalFence`/`pfnWaitForFence` are per-operation
  notifications returning `PhysicalAdapterMask` (sample: "only needed for MultiGPU/LDA").
- `pfnExecuteCommandLists(hQueue, Count, CONST D3D12DDI_HCOMMANDLIST*)` receives our list handles only — no fences, no
  allocators.
- **Consequence for a vkd3d-based UMD**: if vkd3d submits through its own path (Vulkan ICD → host), a runtime
  `queue->Signal(fence, v)` right after `ExecuteCommandLists` is executed by the KM scheduler immediately (empty
  context) → app sees completion before the GPU work is done; and `queue->Wait` only blocks the KM context, not
  vkd3d's submissions. Options:
  1. Correct but slow: in `pfnExecuteCommandLists`, `queue->Signal(internalFence, ++n)` and CPU-wait
     (`internalFence->SetEventOnCompletion` / `GetCompletedValue`) before returning; in `pfnWaitForFence` nothing extra
     is needed because KM waits stall the runtime's later submissions only — **but** they don't stall our next vkd3d
     submit, so cross-queue waits break unless we also CPU-wait. Use only for bring-up.
  2. Proper: after each vkd3d submission, submit a small KM command buffer on the queue's KM context
     (`pfnRenderCb`/`SubmitCommandCb` via KM callbacks) whose DMA-fence completion the KMD signals only when the host
     reports the corresponding Vulkan work done; then runtime signals are naturally ordered after our work. For waits,
     the KMD must also gate subsequent host submissions of this queue on the KM context's waits (i.e. route vkd3d
     queue submissions through the KM context).
  3. Hook `pfnSignalFence` to flush/CPU-wait before the runtime signals [infer: requires the runtime to call it
     before issuing the KM signal — verify order].

---

## 3. Root signatures

- `D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 { D3D12DDI_ROOT_SIGNATURE_VERSION Version; union { CONST D3D12DDI_ROOT_SIGNATURE_0013* pRootSignature_1_1; }; UINT NodeMask; }`
  — **a deserialized desc, not a blob**. Version is `D3D12DDI_ROOT_SIGNATURE_VERSION_1_1`(2) (runtime upconverts 1.0
  apps); `_1_2`(3) exists in the enum but the union has no 1.2 member at this DDI level.
- `D3D12DDI_ROOT_SIGNATURE_0013 { UINT NumParameters; CONST D3D12DDI_ROOT_PARAMETER_0013* pRootParameters; UINT NumStaticSamplers; CONST D3D12DDI_STATIC_SAMPLER* pStaticSamplers; D3D12DDI_ROOT_SIGNATURE_FLAGS Flags; }`
  ≡ `D3D12_ROOT_SIGNATURE_DESC1`;
  `D3D12DDI_ROOT_PARAMETER_0013 { ParameterType; union { D3D12DDI_ROOT_DESCRIPTOR_TABLE_0013 DescriptorTable{NumDescriptorRanges, pDescriptorRanges}; D3D12DDI_ROOT_CONSTANTS Constants{ShaderRegister, RegisterSpace, Num32BitValues}; D3D12DDI_ROOT_DESCRIPTOR_0013 Descriptor{ShaderRegister, RegisterSpace, Flags}; }; ShaderVisibility; }` ≡ `D3D12_ROOT_PARAMETER1`;
  `D3D12DDI_DESCRIPTOR_RANGE_0013 { RangeType; NumDescriptors; BaseShaderRegister; RegisterSpace; Flags; OffsetInDescriptorsFromTableStart; }` ≡ `D3D12_DESCRIPTOR_RANGE1`;
  `D3D12DDI_STATIC_SAMPLER` ≡ `D3D12_STATIC_SAMPLER_DESC`. All enums identical (0.1). Field order and types match, so a
  `reinterpret_cast` is valid (add `static_assert`s on `sizeof`/`offsetof` to be safe).
- Recipe:
  ```c
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC v;
  v.Version  = D3D_ROOT_SIGNATURE_VERSION_1_1;
  v.Desc_1_1 = *(const D3D12_ROOT_SIGNATURE_DESC1 *)args->pRootSignature_1_1;
  ID3DBlob *blob, *err;
  hr = vkd3d_serialize_versioned_root_signature(&v, &blob, &err);   /* include/vkd3d.h:202 */
  hr = device->CreateRootSignature(args->NodeMask, blob->GetBufferPointer(), blob->GetBufferSize(),
                                   IID_ID3D12RootSignature, (void **)&obj->rs);
  ```
  (`D3D12SerializeVersionedRootSignature` from vkd3d-proton's d3d12.dll is equivalent.) The DDI desc's pointers are
  only valid during the call — serialize immediately.
- Root signatures also arrive for embedded RTS0 parts and as `hRootSignature` in `D3D12DDIARG_CREATE_SHADER_0026`,
  `D3D12DDIARG_CREATE_PIPELINE_STATE_0033` and `D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001` — always our handle.

## 4. Pipeline states and shaders

### 4.1 `D3D12DDIARG_CREATE_PIPELINE_STATE_0033`
```
{ D3D12DDI_HSHADER hComputeShader, hVertexShader, hPixelShader, hDomainShader, hHullShader, hGeometryShader;
  D3D12DDI_HROOTSIGNATURE hRootSignature; D3D12DDI_HBLENDSTATE hBlendState; UINT SampleMask;
  D3D12DDI_HRASTERIZERSTATE hRasterizerState; D3D12DDI_HDEPTHSTENCILSTATE hDepthStencilState;
  D3D12DDI_HELEMENTLAYOUT hElementLayout; D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue;
  D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType; UINT NumRenderTargets; DXGI_FORMAT RTVFormats[8];
  DXGI_FORMAT DSVFormat; DXGI_SAMPLE_DESC SampleDesc; UINT NodeMask; D3D12DDI_LIBRARY_REFERENCE_0010 LibraryReference;
  D3D12DDI_VIEW_INSTANCING_DESC ViewInstancingDesc; }
```
- Everything is by **handle to previously created DDI objects** (shaders, blend/rasterizer/DS state, element layout,
  root signature). [infer] The runtime creates these from the API desc (D3D12 API has no state objects) and may
  dedupe/cache them across PSOs; never assume one-to-one lifetime. No `StreamOutput` field: SO lives in the GS object
  created by `pfnCreateGeometryShaderWithStreamOutput`. No `CachedPSO`, no `Flags`.
- Compute iff `hComputeShader.pDrvPrivate != NULL` →
  `CreateComputePipelineState(&D3D12_COMPUTE_PIPELINE_STATE_DESC{ rs, CS bytecode, NodeMask, {NULL,0}, D3D12_PIPELINE_STATE_FLAG_NONE })`.
- Graphics → `D3D12_GRAPHICS_PIPELINE_STATE_DESC` (or, better, a pipeline state stream via
  `ID3D12Device2::CreatePipelineState` to carry `D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1` (=21, for
  `DepthBoundsTestEnable`) and `_VIEW_INSTANCING` (=22)):
  - `pRootSignature = H(hRootSignature)->rs`
  - `VS/PS/DS/HS/GS = { container, size }` of each non-NULL shader object (synthesized container, 4.2)
  - `StreamOutput` = GS object's converted SO decl (`pSODeclaration`, `NumEntries`, `pBufferStrides=BufferStridesInBytes`, `NumStrides`, `RasterizedStream`); else zero
  - `BlendState` (stored `D3D12_BLEND_DESC`), `SampleMask`, `RasterizerState`, `DepthStencilState` (DESC1 in stream / drop
    DepthBoundsTestEnable for the plain desc), `InputLayout = { elements, n }` (synthetic names), `IBStripCutValue`,
    `PrimitiveTopologyType`, `NumRenderTargets`, `RTVFormats`, `DSVFormat`, `SampleDesc`, `NodeMask`: copy (enums identical).
  - `ViewInstancingDesc { UINT ViewInstanceCount; const D3D12DDI_VIEW_INSTANCE_LOCATION* pViewInstanceLocations{ViewportArrayIndex, RenderTargetArrayIndex}; D3D12DDI_VIEW_INSTANCING_FLAGS Flags; }` ≡ `D3D12_VIEW_INSTANCING_DESC`.
  - `LibraryReference { D3D12DDI_HPIPELINELIBRARY hLibrary; UINT PipelineIndex; }`: if non-NULL the runtime is
    loading from a library; with an empty driver library just compile normally [infer].
- A NULL state handle (e.g. `hDepthStencilState`) → [infer] use API defaults (`CD3DX12_*_DESC(D3D12_DEFAULT)`).

### 4.2 Shader bytecode: `D3D12DDIARG_CREATE_SHADER_0026`
- `pShaderCode` is `CONST UINT*` annotated `_In_reads_(pShaderCode[1])`: dword[0] = version token
  (`D3D10_SB_TOKENIZED_PROGRAM_VERSION`: bits 0-3 minor, 4-7 major, 16-31 program type), dword[1] = length in DWORDs.
  This is the **SHDR/SHEX token stream** for SM4-5.1, and [infer] for SM6 the **DXIL program part**
  (`DxilProgramHeader { ProgramVersion (same bit layout, major=6); SizeInUint32; DxilBitcodeHeader{ 'DXIL' magic, DxilVersion, BitcodeOffset, BitcodeSize } }`)
  — **not a DXBC container**. Detect: `major = (pShaderCode[0] >> 4) & 0xF` → `>=6` DXIL. Defensive: if
  `pShaderCode[0] == 'DXBC'` (0x43425844) it is already a container; log it. **Verify on the real runtime first**
  (dump the first 8 dwords for an SM5 and an SM6 PSO).
- Signatures: `IOSignatures.Standard` = `D3D12DDIARG_STAGE_IO_SIGNATURES { D3D12DDIARG_SIGNATURE_ENTRY_0012* pInputSignature; UINT NumInputSignatureEntries; D3D12DDIARG_SIGNATURE_ENTRY_0012* pOutputSignature; UINT NumOutputSignatureEntries; }`
  (+ `pPatchConstantSignature/NumPatchConstantSignatureEntries` in `D3D12DDIARG_TESSELLATION_IO_SIGNATURES` for HS/DS);
  `D3D12DDIARG_SIGNATURE_ENTRY_0012 { D3D10_SB_NAME SystemValue; UINT Register; BYTE Mask /*xyzw = bits 0..3*/; BYTE Stream; D3D10_SB_REGISTER_COMPONENT_TYPE RegisterComponentType; D3D11_SB_OPERAND_MIN_PRECISION MinPrecision; }`
  — **no semantic names/indices**, union members `pInputSignatureDeprecated` (old entry type) alias the same pointer.
- vkd3d-proton needs a DXBC container (`parse_dxbc` rejects anything without `DXBC` tag; checksum is ignored —
  dxbc.c ~118/124) and reads ISGN/ISG1/OSGN/OSG1/OSG5/PCSG/PSG1 for: VS input-layout matching (by name+index → register,
  state.c ~4816/5410), inter-stage validation (by name+index), SO declarations (by name+index). Both DXBC and DXIL go
  through `dxil_spv_parse_dxil_blob` (dxil.c ~1310). Build per shader:
  `DXBC` header (magic, 16-byte checksum (zeros), version 1, total size, chunk count, chunk offsets) + `ISGN`
  (or `ISG1` if any MinPrecision) + `OSGN`/`OSG5` (GS with streams)/`OSG1` + `PCSG`/`PSG1` (HS out / DS in) + `SHEX`
  (token stream; SHDR for SM4.x) **or** `DXIL` (program part).
- Synthetic names (must be identical on both sides of each link):
  - `SystemValue == D3D10_SB_NAME_UNDEFINED` → name `"WBREG"`, SemanticIndex = `Register` (VS inputs: `"WBATTR"`, matching 1.1 #5);
    PS outputs → `"SV_Target"`, index = `Register`.
  - Sysvals → canonical names (`SV_Position`, `SV_ClipDistance`, `SV_CullDistance`, `SV_RenderTargetArrayIndex`,
    `SV_ViewportArrayIndex`, `SV_VertexID`, `SV_PrimitiveID`, `SV_InstanceID`, `SV_IsFrontFace`, `SV_SampleIndex`,
    `SV_TessFactor`, `SV_InsideTessFactor`). `D3D_NAME` value: 0-10 identical; tess factors must be remapped:
    `D3D11_SB_NAME_FINAL_QUAD_{U,V}_EQ_{0,1}_EDGE_TESSFACTOR` (11-14) → `D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR` (11), index 0-3;
    `_FINAL_QUAD_{U,V}_INSIDE_TESSFACTOR` (15-16) → 12, index 0-1; `_FINAL_TRI_{U,V,W}_EQ_0_EDGE_TESSFACTOR` (17-19) → 13,
    index 0-2; `_FINAL_TRI_INSIDE_TESSFACTOR` (20) → 14; `_FINAL_LINE_DETAIL_TESSFACTOR` (21) → 15;
    `_FINAL_LINE_DENSITY_TESSFACTOR` (22) → 16.
  - Same register → same name across stages works because SM5 links by register and dxil-spirv assigns DXIL
    varying locations by start row; vkd3d links by semantic only for mesh→PS (state.c ~2653).
  - Risks to test: PS `oDepth`/`oMask`/`SV_Coverage` and other non-register outputs may be absent from the DDI
    signature — scan `dcl_output oDepth*` / DXIL metadata and add `SV_Depth*`/`SV_Coverage` (register 0xFFFFFFFF)
    entries if dxil-spirv's DXBC frontend needs them; packed entries sharing one register with different masks
    need distinct names → use `"WBREG"` + index `Register*4 + firstComponent` (and the same rule in SO decls).
- `hRootSignature` in the shader args is the root signature the PSO will use (driver can pre-compile); `ShaderCodeHash`
  is a stable 16-byte key for caches; `LibraryReference` as for PSOs.
