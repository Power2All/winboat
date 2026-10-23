// OpenCL: GPU compute for programs that use it (filters in ffmpeg, image and video tools). The virtual GPU has no
// OpenCL driver of its own; Microsoft's "OpenCL, OpenGL and Vulkan Compatibility Pack" (Store) runs OpenCL on any
// Direct3D 12 GPU (OpenCLOn12). MinGW-w64 has no CL headers: the few calls used are declared here.

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
typedef cl_ulong cl_bitfield;
typedef struct _cl_platform_id *cl_platform_id;
typedef struct _cl_device_id *cl_device_id;
typedef struct _cl_context *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_mem *cl_mem;
typedef struct _cl_program *cl_program;
typedef struct _cl_kernel *cl_kernel;
typedef struct _cl_event *cl_event;
#define CL_API WINAPI
enum {
   CL_PLATFORM_VERSION = 0x0901, CL_PLATFORM_NAME = 0x0902, CL_DEVICE_TYPE_GPU = 1 << 2,
   CL_DEVICE_TYPE_ALL = (int)0xFFFFFFFF, CL_DEVICE_NAME = 0x102B, CL_DEVICE_VERSION = 0x102F,
   CL_MEM_READ_WRITE = 1, CL_MEM_COPY_HOST_PTR = 1 << 5, CL_PROGRAM_BUILD_LOG = 0x1183,
};

#define CL_FUNCS(X)                                                                                                   \
   X(clGetPlatformIDs, cl_int, (cl_uint, cl_platform_id *, cl_uint *))                                               \
   X(clGetPlatformInfo, cl_int, (cl_platform_id, cl_uint, size_t, void *, size_t *))                                 \
   X(clGetDeviceIDs, cl_int, (cl_platform_id, cl_bitfield, cl_uint, cl_device_id *, cl_uint *))                      \
   X(clGetDeviceInfo, cl_int, (cl_device_id, cl_uint, size_t, void *, size_t *))                                     \
   X(clCreateContext, cl_context, (const intptr_t *, cl_uint, const cl_device_id *, void *, void *, cl_int *))       \
   X(clCreateCommandQueue, cl_command_queue, (cl_context, cl_device_id, cl_bitfield, cl_int *))                      \
   X(clCreateBuffer, cl_mem, (cl_context, cl_bitfield, size_t, void *, cl_int *))                                    \
   X(clCreateProgramWithSource, cl_program, (cl_context, cl_uint, const char **, const size_t *, cl_int *))          \
   X(clBuildProgram, cl_int, (cl_program, cl_uint, const cl_device_id *, const char *, void *, void *))              \
   X(clGetProgramBuildInfo, cl_int, (cl_program, cl_device_id, cl_uint, size_t, void *, size_t *))                   \
   X(clCreateKernel, cl_kernel, (cl_program, const char *, cl_int *))                                                \
   X(clSetKernelArg, cl_int, (cl_kernel, cl_uint, size_t, const void *))                                             \
   X(clEnqueueNDRangeKernel, cl_int, (cl_command_queue, cl_kernel, cl_uint, const size_t *, const size_t *,          \
                                      const size_t *, cl_uint, const cl_event *, cl_event *))                        \
   X(clEnqueueReadBuffer, cl_int, (cl_command_queue, cl_mem, cl_uint, size_t, size_t, void *, cl_uint,               \
                                   const cl_event *, cl_event *))                                                    \
   X(clFinish, cl_int, (cl_command_queue))                                                                           \
   X(clReleaseKernel, cl_int, (cl_kernel))                                                                           \
   X(clReleaseProgram, cl_int, (cl_program))                                                                         \
   X(clReleaseMemObject, cl_int, (cl_mem))                                                                           \
   X(clReleaseCommandQueue, cl_int, (cl_command_queue))                                                              \
   X(clReleaseContext, cl_int, (cl_context))

struct ClT {
#define CL_DECLARE(name, ret, args) ret(CL_API *name) args = nullptr;
   CL_FUNCS(CL_DECLARE)
#undef CL_DECLARE
};

static std::string clString(ClT &cl, cl_device_id dev, cl_uint what)
{
   char s[256] = "";
   cl.clGetDeviceInfo(dev, what, sizeof s - 1, s, nullptr);
   return s;
}

static void groupOpenCL()
{
   const char *TESTS[] = {"OpenCL device on the GPU", "OpenCL kernel (compile and run)"};
   HMODULE lib = LoadLibraryA("OpenCL.dll");
   ClT cl;
   bool loaded = lib != nullptr;
#define CL_LOAD(name, ret, args) cl.name = (ret(CL_API *) args)GetProcAddress(lib, #name), loaded = loaded && cl.name;
   if (lib) {
      CL_FUNCS(CL_LOAD)
   }
#undef CL_LOAD
   report(loaded ? Status::Pass : Status::Warn, "OpenCL runtime (OpenCL.dll)", loaded ? "" : "not installed");
   if (!loaded) {
      for (auto t : TESTS) report(Status::Skip, t, "no OpenCL runtime");
      return;
   }

   // Every platform's GPU devices; the first that isn't a software one (WARP through OpenCLOn12)
   cl_uint np = 0;
   cl.clGetPlatformIDs(0, nullptr, &np);
   std::vector<cl_platform_id> platforms(np);
   if (np) cl.clGetPlatformIDs(np, platforms.data(), nullptr);
   cl_device_id dev = nullptr;
   std::string platformName, all;
   for (auto p : platforms) {
      char name[128] = "";
      cl.clGetPlatformInfo(p, CL_PLATFORM_NAME, sizeof name - 1, name, nullptr);
      cl_uint nd = 0;
      cl.clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 0, nullptr, &nd);
      std::vector<cl_device_id> devs(nd);
      if (nd) cl.clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, nd, devs.data(), nullptr);
      for (auto dv : devs) {
         std::string dn = clString(cl, dv, CL_DEVICE_NAME);
         all += std::string(all.empty() ? "" : ", ") + dn;
         bool software = dn.find("Basic Render") != std::string::npos || dn.find("WARP") != std::string::npos;
         if (!dev && software == g_warp) {
            dev = dv;
            platformName = name;
         }
      }
   }
   if (!dev) {
      report(Status::Warn, TESTS[0], np ? "no OpenCL GPU device (" + (all.empty() ? std::string("none") : all) + ")"
                                        : "no OpenCL platform: install Microsoft's OpenCL, OpenGL and Vulkan Compatibility Pack");
      report(Status::Skip, TESTS[1], "no OpenCL GPU device");
      return;
   }
   check(true, TESTS[0], clString(cl, dev, CL_DEVICE_NAME) + ", " + clString(cl, dev, CL_DEVICE_VERSION) + " (" + platformName + ")");

   if (startTest(TESTS[1])) {
      // y = a * x + y over 1M floats
      const size_t n = 1 << 20;
      std::vector<float> x(n), y(n);
      for (size_t i = 0; i < n; i++) x[i] = (float)(i % 1000), y[i] = (float)(i % 7);
      const char *src = "__kernel void saxpy(float a, __global const float *x, __global float *y)\n"
                        "{ size_t i = get_global_id(0); y[i] = a * x[i] + y[i]; }\n";
      cl_int err = 0;
      cl_context ctx = cl.clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &err);
      cl_command_queue q = ctx ? cl.clCreateCommandQueue(ctx, dev, 0, &err) : nullptr;
      cl_mem bx = ctx ? cl.clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, n * 4, x.data(), &err) : nullptr;
      cl_mem by = ctx ? cl.clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, n * 4, y.data(), &err) : nullptr;
      cl_program prog = ctx ? cl.clCreateProgramWithSource(ctx, 1, &src, nullptr, &err) : nullptr;
      std::string detail;
      bool ok = q && bx && by && prog;
      if (ok && cl.clBuildProgram(prog, 1, &dev, "", nullptr, nullptr) != 0) {
         char log[512] = "";
         cl.clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof log - 1, log, nullptr);
         detail = std::string("build failed: ") + log;
         ok = false;
      }
      cl_kernel k = ok ? cl.clCreateKernel(prog, "saxpy", &err) : nullptr;
      if (k) {
         float a = 2.0f;
         cl.clSetKernelArg(k, 0, sizeof a, &a);
         cl.clSetKernelArg(k, 1, sizeof bx, &bx);
         cl.clSetKernelArg(k, 2, sizeof by, &by);
         size_t global = n;
         std::vector<float> out(n);
         cl_int e1 = cl.clEnqueueNDRangeKernel(q, k, 1, nullptr, &global, nullptr, 0, nullptr, nullptr);
         cl_int e2 = e1 ? 0 : cl.clEnqueueReadBuffer(q, by, 1, 0, n * 4, out.data(), 0, nullptr, nullptr);
         cl_int e3 = e1 || e2 ? 0 : cl.clFinish(q);
         size_t wrong = 0;
         for (size_t i = 0; !e1 && !e2 && !e3 && i < n; i++) wrong += out[i] != 2.0f * x[i] + y[i];
         ok = !e1 && !e2 && !e3 && wrong == 0;
         detail = ok ? "saxpy over 1M floats"
                  : e1 || e2 || e3 ? fmt("saxpy: %s failed (%d)", e1 ? "clEnqueueNDRangeKernel" : e2 ? "clEnqueueReadBuffer" : "clFinish", e1 ? e1 : e2 ? e2 : e3)
                                   : fmt("saxpy: %zu wrong values", wrong);
         cl.clReleaseKernel(k);
      } else if (detail.empty()) {
         detail = fmt("setup failed (%d)", err);
         ok = false;
      }
      check(ok, TESTS[1], detail);
      if (prog) cl.clReleaseProgram(prog);
      if (bx) cl.clReleaseMemObject(bx);
      if (by) cl.clReleaseMemObject(by);
      if (q) cl.clReleaseCommandQueue(q);
      if (ctx) cl.clReleaseContext(ctx);
   }
}
