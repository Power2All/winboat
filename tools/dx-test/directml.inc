// Machine learning on the GPU: DirectML, the route of Topaz Video AI, ONNX Runtime and Windows ML on GPUs without
// CUDA. It runs on Direct3D 12 (compute shaders, 16-bit math where the GPU has it). DirectML.dll comes with Windows 10
// 1903 and later; MinGW-w64 has no DirectML.h, so the part of its API this test uses is declared here.

enum DML_TENSOR_DATA_TYPE { DML_TENSOR_DATA_TYPE_UNKNOWN, DML_TENSOR_DATA_TYPE_FLOAT32, DML_TENSOR_DATA_TYPE_FLOAT16 };
enum DML_TENSOR_TYPE { DML_TENSOR_TYPE_INVALID, DML_TENSOR_TYPE_BUFFER };
enum DML_TENSOR_FLAGS { DML_TENSOR_FLAG_NONE = 0 };
enum DML_OPERATOR_TYPE { DML_OPERATOR_CONVOLUTION = 53, DML_OPERATOR_GEMM = 54 };
enum DML_CONVOLUTION_MODE { DML_CONVOLUTION_MODE_CONVOLUTION, DML_CONVOLUTION_MODE_CROSS_CORRELATION };
enum DML_CONVOLUTION_DIRECTION { DML_CONVOLUTION_DIRECTION_FORWARD, DML_CONVOLUTION_DIRECTION_BACKWARD };
enum DML_MATRIX_TRANSFORM { DML_MATRIX_TRANSFORM_NONE, DML_MATRIX_TRANSFORM_TRANSPOSE };
enum DML_EXECUTION_FLAGS { DML_EXECUTION_FLAG_NONE = 0, DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION = 1 };
enum DML_CREATE_DEVICE_FLAGS { DML_CREATE_DEVICE_FLAG_NONE = 0 };
enum DML_FEATURE { DML_FEATURE_TENSOR_DATA_TYPE_SUPPORT, DML_FEATURE_FEATURE_LEVELS };
enum DML_FEATURE_LEVEL {
   DML_FEATURE_LEVEL_1_0 = 0x1000, DML_FEATURE_LEVEL_2_0 = 0x2000, DML_FEATURE_LEVEL_2_1 = 0x2100,
   DML_FEATURE_LEVEL_3_0 = 0x3000, DML_FEATURE_LEVEL_3_1 = 0x3100, DML_FEATURE_LEVEL_4_0 = 0x4000,
   DML_FEATURE_LEVEL_4_1 = 0x4100, DML_FEATURE_LEVEL_5_0 = 0x5000, DML_FEATURE_LEVEL_5_1 = 0x5100,
   DML_FEATURE_LEVEL_5_2 = 0x5200, DML_FEATURE_LEVEL_6_0 = 0x6000, DML_FEATURE_LEVEL_6_1 = 0x6100,
   DML_FEATURE_LEVEL_6_2 = 0x6200,
};
enum DML_BINDING_TYPE { DML_BINDING_TYPE_NONE, DML_BINDING_TYPE_BUFFER };

struct DML_BUFFER_TENSOR_DESC {
   DML_TENSOR_DATA_TYPE DataType;
   DML_TENSOR_FLAGS Flags;
   UINT DimensionCount;
   const UINT *Sizes;
   const UINT *Strides;
   UINT64 TotalTensorSizeInBytes;
   UINT GuaranteedBaseOffsetAlignment;
};
struct DML_TENSOR_DESC { DML_TENSOR_TYPE Type; const void *Desc; };
struct DML_OPERATOR_DESC { DML_OPERATOR_TYPE Type; const void *Desc; };
struct DML_GEMM_OPERATOR_DESC {
   const DML_TENSOR_DESC *ATensor, *BTensor, *CTensor, *OutputTensor;
   DML_MATRIX_TRANSFORM TransA, TransB;
   FLOAT Alpha, Beta;
   const DML_OPERATOR_DESC *FusedActivation;
};
struct DML_CONVOLUTION_OPERATOR_DESC {
   const DML_TENSOR_DESC *InputTensor, *FilterTensor, *BiasTensor, *OutputTensor;
   DML_CONVOLUTION_MODE Mode;
   DML_CONVOLUTION_DIRECTION Direction;
   UINT DimensionCount;
   const UINT *Strides, *Dilations, *StartPadding, *EndPadding, *OutputPadding;
   UINT GroupCount;
   const DML_OPERATOR_DESC *FusedActivation;
};
struct DML_FEATURE_QUERY_TENSOR_DATA_TYPE_SUPPORT { DML_TENSOR_DATA_TYPE DataType; };
struct DML_FEATURE_DATA_TENSOR_DATA_TYPE_SUPPORT { BOOL IsSupported; };
struct DML_FEATURE_QUERY_FEATURE_LEVELS { UINT RequestedFeatureLevelCount; const DML_FEATURE_LEVEL *RequestedFeatureLevels; };
struct DML_FEATURE_DATA_FEATURE_LEVELS { DML_FEATURE_LEVEL MaxSupportedFeatureLevel; };
struct DML_BINDING_PROPERTIES { UINT RequiredDescriptorCount; UINT64 TemporaryResourceSize, PersistentResourceSize; };
struct DML_BINDING_DESC { DML_BINDING_TYPE Type; const void *Desc; };
struct DML_BUFFER_BINDING { ID3D12Resource *Buffer; UINT64 Offset, SizeInBytes; };

struct IDMLDispatchable;
struct DML_BINDING_TABLE_DESC {
   IDMLDispatchable *Dispatchable;
   D3D12_CPU_DESCRIPTOR_HANDLE CPUDescriptorHandle;
   D3D12_GPU_DESCRIPTOR_HANDLE GPUDescriptorHandle;
   UINT SizeInDescriptors;
};

struct IDMLObject : IUnknown {
   virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) = 0;
   virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) = 0;
   virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, IUnknown *) = 0;
   virtual HRESULT STDMETHODCALLTYPE SetName(PCWSTR) = 0;
};
struct IDMLDeviceChild : IDMLObject { virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) = 0; };
struct IDMLPageable : IDMLDeviceChild {};
struct IDMLOperator : IDMLDeviceChild {};
struct IDMLDispatchable : IDMLPageable {
   // returns a struct: MSVC's COM ABI passes the result's address after this (as MinGW-w64 declares such methods
   // with WIDL_EXPLICIT_AGGREGATE_RETURNS)
   virtual DML_BINDING_PROPERTIES *STDMETHODCALLTYPE GetBindingProperties(DML_BINDING_PROPERTIES *ret) = 0;
};
struct IDMLCompiledOperator : IDMLDispatchable {};
struct IDMLOperatorInitializer : IDMLDispatchable {
   virtual HRESULT STDMETHODCALLTYPE Reset(UINT, IDMLCompiledOperator *const *) = 0;
};
struct IDMLBindingTable : IDMLDeviceChild {
   virtual void STDMETHODCALLTYPE BindInputs(UINT, const DML_BINDING_DESC *) = 0;
   virtual void STDMETHODCALLTYPE BindOutputs(UINT, const DML_BINDING_DESC *) = 0;
   virtual void STDMETHODCALLTYPE BindTemporaryResource(const DML_BINDING_DESC *) = 0;
   virtual void STDMETHODCALLTYPE BindPersistentResource(const DML_BINDING_DESC *) = 0;
   virtual HRESULT STDMETHODCALLTYPE Reset(const DML_BINDING_TABLE_DESC *) = 0;
};
struct IDMLCommandRecorder : IDMLDeviceChild {
   virtual void STDMETHODCALLTYPE RecordDispatch(ID3D12CommandList *, IDMLDispatchable *, IDMLBindingTable *) = 0;
};
struct IDMLDevice : IDMLObject {
   virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DML_FEATURE, UINT, const void *, UINT, void *) = 0;
   virtual HRESULT STDMETHODCALLTYPE CreateOperator(const DML_OPERATOR_DESC *, REFIID, void **) = 0;
   virtual HRESULT STDMETHODCALLTYPE CompileOperator(IDMLOperator *, DML_EXECUTION_FLAGS, REFIID, void **) = 0;
   virtual HRESULT STDMETHODCALLTYPE CreateOperatorInitializer(UINT, IDMLCompiledOperator *const *, REFIID, void **) = 0;
   virtual HRESULT STDMETHODCALLTYPE CreateCommandRecorder(REFIID, void **) = 0;
   virtual HRESULT STDMETHODCALLTYPE CreateBindingTable(const DML_BINDING_TABLE_DESC *, REFIID, void **) = 0;
   virtual HRESULT STDMETHODCALLTYPE Evict(UINT, IDMLPageable *const *) = 0;
   virtual HRESULT STDMETHODCALLTYPE MakeResident(UINT, IDMLPageable *const *) = 0;
   virtual HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() = 0;
   virtual HRESULT STDMETHODCALLTYPE GetParentDevice(REFIID, void **) = 0;
};
__CRT_UUID_DECL(IDMLDevice, 0x6dbd6437, 0x96fd, 0x423f, 0xa9, 0x8c, 0xae, 0x5e, 0x7c, 0x2a, 0x57, 0x3f)
__CRT_UUID_DECL(IDMLOperator, 0x26caae7a, 0x3081, 0x4633, 0x95, 0x81, 0x22, 0x6f, 0xbe, 0x57, 0x69, 0x5d)
__CRT_UUID_DECL(IDMLCompiledOperator, 0x6b15e56a, 0xbf5c, 0x4902, 0x92, 0xd8, 0xda, 0x3a, 0x65, 0x0a, 0xfe, 0xa4)
__CRT_UUID_DECL(IDMLOperatorInitializer, 0x427c1113, 0x435c, 0x469c, 0x86, 0x76, 0x4d, 0x5d, 0xd0, 0x72, 0xf8, 0x13)
__CRT_UUID_DECL(IDMLBindingTable, 0x29c687dc, 0xde74, 0x4e3b, 0xab, 0x00, 0x11, 0x68, 0xf2, 0xfc, 0x3c, 0xfc)
__CRT_UUID_DECL(IDMLCommandRecorder, 0xe6857a76, 0x2e3e, 0x4fdd, 0xbf, 0xf4, 0x5d, 0x2b, 0xa1, 0x0f, 0xb4, 0x53)

typedef HRESULT(WINAPI *PFN_DMLCreateDevice)(ID3D12Device *, DML_CREATE_DEVICE_FLAGS, REFIID, void **);

static float fromHalf(uint16_t h)
{
   uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
   uint32_t x = exp == 0 ? sign : exp == 31 ? sign | 0x7f800000u | (mant << 13) : sign | ((exp - 15 + 127) << 23) | (mant << 13);
   float f;
   memcpy(&f, &x, 4);
   return f;
}

// A 4-D buffer tensor (DirectML's sizes are NCHW; the buffer size is rounded up to 4 bytes)
struct DmlTensor {
   UINT sizes[4];
   DML_BUFFER_TENSOR_DESC buffer = {};
   DML_TENSOR_DESC desc = {};
   DmlTensor(DML_TENSOR_DATA_TYPE type, UINT n, UINT c, UINT h, UINT w)
   {
      sizes[0] = n, sizes[1] = c, sizes[2] = h, sizes[3] = w;
      buffer.DataType = type;
      buffer.DimensionCount = 4;
      buffer.Sizes = sizes;
      buffer.TotalTensorSizeInBytes = ((UINT64)n * c * h * w * (type == DML_TENSOR_DATA_TYPE_FLOAT16 ? 2 : 4) + 3) & ~3ull;
      desc.Type = DML_TENSOR_TYPE_BUFFER;
      desc.Desc = &buffer;
   }
};

// Runs one operator: compile, initialize, bind the inputs (uploaded), dispatch, read the output back
static bool runDml(D12 &d, IDMLDevice *dml, const DML_OPERATOR_DESC &opDesc, DML_EXECUTION_FLAGS flags,
                   const std::vector<std::vector<uint8_t>> &inputs, size_t outputSize, std::vector<uint8_t> &output,
                   std::string &detail)
{
   IDMLOperator *op = nullptr;
   IDMLCompiledOperator *compiled = nullptr;
   IDMLOperatorInitializer *init = nullptr;
   IDMLCommandRecorder *recorder = nullptr;
   IDMLBindingTable *table = nullptr;
   ID3D12DescriptorHeap *heap = nullptr;
   std::vector<ID3D12Resource *> buffers;
   HRESULT h = dml->CreateOperator(&opDesc, __uuidof(IDMLOperator), (void **)&op);
   if (SUCCEEDED(h)) h = dml->CompileOperator(op, flags, __uuidof(IDMLCompiledOperator), (void **)&compiled);
   if (SUCCEEDED(h)) h = dml->CreateOperatorInitializer(1, &compiled, __uuidof(IDMLOperatorInitializer), (void **)&init);
   if (SUCCEEDED(h)) h = dml->CreateCommandRecorder(__uuidof(IDMLCommandRecorder), (void **)&recorder);
   bool ok = false;
   if (FAILED(h)) {
      detail = "operator: " + hrs(h);
   } else {
      DML_BINDING_PROPERTIES ip, ep;
      init->GetBindingProperties(&ip);
      compiled->GetBindingProperties(&ep);
      UINT descriptors = std::max(ip.RequiredDescriptorCount, ep.RequiredDescriptorCount);
      D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, std::max(descriptors, 1u),
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
      d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&heap);
      auto uav = [&](UINT64 size) {
         ID3D12Resource *r = buffer12(d, std::max<UINT64>(size, 4), D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
         buffers.push_back(r);
         return r;
      };
      UINT64 tempSize = std::max(ip.TemporaryResourceSize, ep.TemporaryResourceSize);
      ID3D12Resource *temp = tempSize ? uav(tempSize) : nullptr;
      ID3D12Resource *persistent = ep.PersistentResourceSize ? uav(ep.PersistentResourceSize) : nullptr;
      DML_BUFFER_BINDING tempBinding = {temp, 0, tempSize}, persistentBinding = {persistent, 0, ep.PersistentResourceSize};
      DML_BINDING_DESC tempDesc = {DML_BINDING_TYPE_BUFFER, &tempBinding}, persistentDesc = {DML_BINDING_TYPE_BUFFER, &persistentBinding};

      // inputs: uploaded into UAV buffers
      std::vector<DML_BUFFER_BINDING> inBindings;
      for (auto &data : inputs) {
         ID3D12Resource *upload = buffer12(d, data.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
         buffers.push_back(upload);
         void *p = nullptr;
         if (upload && SUCCEEDED(upload->Map(0, nullptr, &p))) {
            memcpy(p, data.data(), data.size());
            upload->Unmap(0, nullptr);
         }
         ID3D12Resource *in = uav(data.size());
         barrier12(d.list, in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
         d.list->CopyBufferRegion(in, 0, upload, 0, data.size());
         barrier12(d.list, in, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
         inBindings.push_back({in, 0, data.size()});
      }
      ID3D12Resource *out = uav(outputSize);
      ID3D12Resource *readback = buffer12(d, outputSize, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      buffers.push_back(readback);

      // initialize (persistent data such as reordered weights), then execute
      DML_BINDING_TABLE_DESC td = {init, heap->GetCPUDescriptorHandleForHeapStart(),
                                   heap->GetGPUDescriptorHandleForHeapStart(), descriptors};
      h = dml->CreateBindingTable(&td, __uuidof(IDMLBindingTable), (void **)&table);
      if (SUCCEEDED(h)) {
         if (ip.TemporaryResourceSize) table->BindTemporaryResource(&tempDesc);
         if (persistent) table->BindOutputs(1, &persistentDesc);
         d.list->SetDescriptorHeaps(1, &heap);
         recorder->RecordDispatch(d.list, init, table);
         D3D12_RESOURCE_BARRIER uavBarrier = {D3D12_RESOURCE_BARRIER_TYPE_UAV};
         d.list->ResourceBarrier(1, &uavBarrier);
         td.Dispatchable = compiled;
         h = table->Reset(&td);
      }
      if (SUCCEEDED(h)) {
         std::vector<DML_BINDING_DESC> inDescs;
         for (auto &b : inBindings) inDescs.push_back({DML_BINDING_TYPE_BUFFER, &b});
         // GEMM's optional C and convolution's bias are bound as none
         while (inDescs.size() < 3) inDescs.push_back({DML_BINDING_TYPE_NONE, nullptr});
         DML_BUFFER_BINDING outBinding = {out, 0, outputSize};
         DML_BINDING_DESC outDesc = {DML_BINDING_TYPE_BUFFER, &outBinding};
         table->BindInputs((UINT)inDescs.size(), inDescs.data());
         table->BindOutputs(1, &outDesc);
         if (ep.TemporaryResourceSize) table->BindTemporaryResource(&tempDesc);
         if (persistent) table->BindPersistentResource(&persistentDesc);
         recorder->RecordDispatch(d.list, compiled, table);
         barrier12(d.list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
         d.list->CopyBufferRegion(readback, 0, out, 0, outputSize);
         ok = submit12(d);
         void *p = nullptr;
         if (ok && SUCCEEDED(readback->Map(0, nullptr, &p))) {
            output.assign((const uint8_t *)p, (const uint8_t *)p + outputSize);
            readback->Unmap(0, nullptr);
         } else {
            ok = false;
            detail = "the GPU work failed or timed out";
         }
      } else {
         detail = "binding table: " + hrs(h);
      }
   }
   for (auto *b : buffers) release(b);
   release(heap);
   release(table);
   release(recorder);
   release(init);
   release(compiled);
   release(op);
   return ok;
}

// The largest difference from the CPU's result, relative to the largest value
static float maxError(const std::vector<float> &gpu, const std::vector<float> &cpu)
{
   float err = 0, scale = 1e-6f;
   for (size_t i = 0; i < cpu.size(); i++) {
      err = std::max(err, std::abs(gpu[i] - cpu[i]));
      scale = std::max(scale, std::abs(cpu[i]));
   }
   return err / scale;
}

static void groupDirectML()
{
   const char *TESTS[] = {"DirectML device on the GPU", "DirectML feature level", "Matrix multiplication (GEMM, FP32)",
                          "Convolution (FP32)", "Half-precision (FP16) tensors", "Matrix multiplication (GEMM, FP16)"};
   HMODULE lib = LoadLibraryA("DirectML.dll");
   auto createDml = lib ? (PFN_DMLCreateDevice)GetProcAddress(lib, "DMLCreateDevice") : nullptr;
   if (createDml || windows10OrLater())
      check(createDml != nullptr, "DirectML runtime (DirectML.dll)", createDml ? "" : "not installed (Windows 10 1903 or later has it)");
   else
      optional(false, "DirectML runtime (DirectML.dll)", "Windows 10 1903 or later has it");
   typedef HRESULT(WINAPI * PFN_Create)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
   HMODULE lib12 = LoadLibraryA("d3d12.dll");
   auto create12 = lib12 ? (PFN_Create)GetProcAddress(lib12, "D3D12CreateDevice") : nullptr;
   if (!createDml || !create12) {
      for (auto t : TESTS) report(Status::Skip, t, createDml ? "no Direct3D 12" : "no DirectML");
      return;
   }

   // The GPU's Direct3D 12 device (DirectML has no other way onto the GPU)
   IDXGIFactory4 *factory = nullptr;
   CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&factory);
   IDXGIAdapter1 *adapter = nullptr;
   if (factory) {
      if (g_warp) {
         factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), (void **)&adapter);
      } else {
         IDXGIAdapter1 *a;
         for (UINT i = 0; !adapter && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            a->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) a->Release();
            else adapter = a;
         }
      }
   }
   D12 d;
   HRESULT h = adapter ? create12(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&d.dev) : E_FAIL;
   release(adapter);
   release(factory);
   IDMLDevice *dml = nullptr;
   if (SUCCEEDED(h)) h = createDml(d.dev, DML_CREATE_DEVICE_FLAG_NONE, __uuidof(IDMLDevice), (void **)&dml);
   if (FAILED(h)) {
      // without Direct3D 12 the GPU has no DirectML either (programs fall back to the CPU)
      report(Status::Warn, TESTS[0], d.dev ? "DirectML refused the Direct3D 12 device (" + hrs(h) + ")"
                                           : "not supported: the GPU has no Direct3D 12 (" + hrs(h) + ")");
      for (size_t i = 1; i < ARRAYSIZE(TESTS); i++) report(Status::Skip, TESTS[i], "no DirectML device");
      release(d.dev);
      return;
   }
   check(true, TESTS[0]);

   const DML_FEATURE_LEVEL levels[] = {DML_FEATURE_LEVEL_1_0, DML_FEATURE_LEVEL_2_0, DML_FEATURE_LEVEL_3_0,
                                       DML_FEATURE_LEVEL_4_0, DML_FEATURE_LEVEL_5_0, DML_FEATURE_LEVEL_6_0,
                                       DML_FEATURE_LEVEL_6_1, DML_FEATURE_LEVEL_6_2};
   DML_FEATURE_QUERY_FEATURE_LEVELS lq = {ARRAYSIZE(levels), levels};
   DML_FEATURE_DATA_FEATURE_LEVELS ld = {};
   if (SUCCEEDED(dml->CheckFeatureSupport(DML_FEATURE_FEATURE_LEVELS, sizeof lq, &lq, sizeof ld, &ld)))
      report(Status::Info, TESTS[1], fmt("%x.%x", ld.MaxSupportedFeatureLevel >> 12, (ld.MaxSupportedFeatureLevel >> 8) & 15));
   else
      report(Status::Info, TESTS[1], "1.0 (older DirectML)");

   D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
   d.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&d.queue);
   d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&d.alloc);
   if (d.alloc) d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&d.list);
   d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&d.fence);
   d.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

   srand(1234);
   auto randoms = [](size_t n) {
      std::vector<float> v(n);
      for (auto &x : v) x = (float)(rand() % 2001 - 1000) / 1000.0f;
      return v;
   };
   auto bytes = [](const std::vector<float> &v) {
      return std::vector<uint8_t>((const uint8_t *)v.data(), (const uint8_t *)(v.data() + v.size()));
   };
   auto halves = [](const std::vector<float> &v) {
      std::vector<uint8_t> b(v.size() * 2);
      for (size_t i = 0; i < v.size(); i++) {
         uint16_t hv = toHalf(v[i]);
         memcpy(&b[i * 2], &hv, 2);
      }
      return b;
   };

   // GEMM: [M x K] * [K x N]
   const UINT M = 64, N = 64, K = 64;
   std::vector<float> A = randoms(M * K), B = randoms(K * N), C(M * N, 0.0f);
   for (UINT i = 0; i < M; i++)
      for (UINT j = 0; j < N; j++)
         for (UINT k = 0; k < K; k++) C[i * N + j] += A[i * K + k] * B[k * N + j];
   auto gemm = [&](DML_TENSOR_DATA_TYPE type, const char *name) {
      if (!startTest(name)) return;
      DmlTensor a(type, 1, 1, M, K), b(type, 1, 1, K, N), o(type, 1, 1, M, N);
      DML_GEMM_OPERATOR_DESC g = {&a.desc, &b.desc, nullptr, &o.desc, DML_MATRIX_TRANSFORM_NONE, DML_MATRIX_TRANSFORM_NONE, 1.0f, 0.0f, nullptr};
      DML_OPERATOR_DESC od = {DML_OPERATOR_GEMM, &g};
      bool fp16 = type == DML_TENSOR_DATA_TYPE_FLOAT16;
      std::vector<uint8_t> out;
      std::string detail;
      bool ok = runDml(d, dml, od, fp16 ? DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION : DML_EXECUTION_FLAG_NONE,
                       {fp16 ? halves(A) : bytes(A), fp16 ? halves(B) : bytes(B)}, (size_t)o.buffer.TotalTensorSizeInBytes, out, detail);
      if (ok) {
         std::vector<float> gpu(M * N);
         for (size_t i = 0; i < gpu.size(); i++) {
            if (fp16) {
               uint16_t hv;
               memcpy(&hv, &out[i * 2], 2);
               gpu[i] = fromHalf(hv);
            } else {
               memcpy(&gpu[i], &out[i * 4], 4);
            }
         }
         float err = maxError(gpu, C);
         ok = err < (fp16 ? 0.02f : 1e-4f);
         detail = fmt("%ux%u by %ux%u, largest difference from the CPU %.2g", M, K, K, N, err);
      }
      check(ok, name, detail);
   };
   gemm(DML_TENSOR_DATA_TYPE_FLOAT32, TESTS[2]);

   // Convolution: 1x4x16x16 input, 8 filters of 4x3x3, no padding: 1x8x14x14
   if (startTest(TESTS[3])) {
      const UINT IC = 4, OC = 8, H = 16, W = 16, KS = 3, OH = H - KS + 1, OW = W - KS + 1;
      std::vector<float> in = randoms(IC * H * W), f = randoms(OC * IC * KS * KS), ref(OC * OH * OW, 0.0f);
      for (UINT oc = 0; oc < OC; oc++)
         for (UINT y = 0; y < OH; y++)
            for (UINT x = 0; x < OW; x++) {
               float s = 0;
               for (UINT ic = 0; ic < IC; ic++)
                  for (UINT ky = 0; ky < KS; ky++)
                     for (UINT kx = 0; kx < KS; kx++)
                        s += in[(ic * H + y + ky) * W + x + kx] * f[((oc * IC + ic) * KS + ky) * KS + kx];
               ref[(oc * OH + y) * OW + x] = s;
            }
      DmlTensor ti(DML_TENSOR_DATA_TYPE_FLOAT32, 1, IC, H, W), tf(DML_TENSOR_DATA_TYPE_FLOAT32, OC, IC, KS, KS),
         to(DML_TENSOR_DATA_TYPE_FLOAT32, 1, OC, OH, OW);
      const UINT ones[2] = {1, 1}, zeros[2] = {0, 0};
      DML_CONVOLUTION_OPERATOR_DESC c = {&ti.desc, &tf.desc, nullptr, &to.desc, DML_CONVOLUTION_MODE_CROSS_CORRELATION,
                                         DML_CONVOLUTION_DIRECTION_FORWARD, 2, ones, ones, zeros, zeros, zeros, 1, nullptr};
      DML_OPERATOR_DESC od = {DML_OPERATOR_CONVOLUTION, &c};
      std::vector<uint8_t> out;
      std::string detail;
      bool ok = runDml(d, dml, od, DML_EXECUTION_FLAG_NONE, {bytes(in), bytes(f)}, (size_t)to.buffer.TotalTensorSizeInBytes, out, detail);
      if (ok) {
         std::vector<float> gpu(ref.size());
         memcpy(gpu.data(), out.data(), gpu.size() * 4);
         float err = maxError(gpu, ref);
         ok = err < 1e-4f;
         detail = fmt("1x4x16x16 with 8 filters of 3x3, largest difference from the CPU %.2g", err);
      }
      check(ok, TESTS[3], detail);
   }

   // FP16: what DirectML's models run in where the GPU has 16-bit math
   DML_FEATURE_QUERY_TENSOR_DATA_TYPE_SUPPORT tq = {DML_TENSOR_DATA_TYPE_FLOAT16};
   DML_FEATURE_DATA_TENSOR_DATA_TYPE_SUPPORT ts = {};
   dml->CheckFeatureSupport(DML_FEATURE_TENSOR_DATA_TYPE_SUPPORT, sizeof tq, &tq, sizeof ts, &ts);
   optional(ts.IsSupported, TESTS[4]);
   if (ts.IsSupported) gemm(DML_TENSOR_DATA_TYPE_FLOAT16, TESTS[5]);
   else report(Status::Skip, TESTS[5], "no FP16 tensors");

   release(dml);
   if (d.event) CloseHandle(d.event);
   release(d.fence);
   release(d.list);
   release(d.alloc);
   release(d.queue);
   release(d.dev);
}
