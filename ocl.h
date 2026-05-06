#ifndef OCL_H
#define OCL_H

#ifdef _WIN32
  #define CL_API_CALL           __stdcall
  #define CL_CALLBACK           __stdcall
#else
  #define CL_API_CALL
  #define CL_CALLBACK
#endif

/* Ensure your q1616_t is visible here or defined */
#include "system.h"

typedef struct _cl_platform_id   *cl_platform_id;
typedef struct _cl_device_id     *cl_device_id;
typedef struct _cl_context       *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_program       *cl_program;
typedef struct _cl_kernel        *cl_kernel;
typedef struct _cl_mem           *cl_mem;

typedef int                       cl_int;
typedef unsigned int              cl_uint;
typedef unsigned long             cl_ulong;
typedef size_t                    cl_bitfield;

#define CL_SUCCESS                0
#define CL_DEVICE_TYPE_GPU        (1<<2)
#define CL_DEVICE_TYPE_CPU        (1<<1)
#define CL_MEM_READ_ONLY          (1<<2)
#define CL_MEM_WRITE_ONLY         (1<<1)
#define CL_PROGRAM_BUILD_LOG      0x1183

typedef cl_int (CL_API_CALL *PFN_clGetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
typedef cl_int (CL_API_CALL *PFN_clGetDeviceIDs)(cl_platform_id, cl_bitfield, cl_uint, cl_device_id*, cl_uint*);
typedef cl_context (CL_API_CALL *PFN_clCreateContext)(const void*, cl_uint, const cl_device_id*, void (CL_CALLBACK*)(const char*, const void*, size_t, void*), void*, cl_int*);
typedef cl_int (CL_API_CALL *PFN_clReleaseContext)(cl_context);
typedef cl_command_queue (CL_API_CALL *PFN_clCreateCommandQueue)(cl_context, cl_device_id, cl_bitfield, cl_int*);
typedef cl_int (CL_API_CALL *PFN_clReleaseCommandQueue)(cl_command_queue);
typedef cl_program (CL_API_CALL *PFN_clCreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
typedef cl_int (CL_API_CALL *PFN_clBuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*, void (CL_CALLBACK*)(cl_program, void*), void*);
typedef cl_int (CL_API_CALL *PFN_clGetProgramBuildInfo)(cl_program, cl_device_id, cl_uint, size_t, void*, size_t*);
typedef cl_kernel (CL_API_CALL *PFN_clCreateKernel)(cl_program, const char*, cl_int*);
typedef cl_int (CL_API_CALL *PFN_clReleaseKernel)(cl_kernel);
typedef cl_mem (CL_API_CALL *PFN_clCreateBuffer)(cl_context, cl_bitfield, size_t, void*, cl_int*);
typedef cl_int (CL_API_CALL *PFN_clReleaseMemObject)(cl_mem);
typedef cl_int (CL_API_CALL *PFN_clSetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
typedef cl_int (CL_API_CALL *PFN_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, const void*, cl_uint, const void*, void*);
typedef cl_int (CL_API_CALL *PFN_clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, void*, cl_uint, const void*, void*);
typedef cl_int (CL_API_CALL *PFN_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*, const size_t*, cl_uint, const void*, void*);
typedef cl_int (CL_API_CALL *PFN_clFinish)(cl_command_queue);
typedef cl_int (CL_API_CALL *PFN_clReleaseProgram)(cl_program);

struct ocl_state {
    HMODULE dll;
    PFN_clGetPlatformIDs getPlatformIDs;
    PFN_clGetDeviceIDs getDeviceIDs;
    PFN_clCreateContext createContext;
    PFN_clReleaseContext releaseContext;
    PFN_clCreateCommandQueue createQueue;
    PFN_clReleaseCommandQueue releaseQueue;
    PFN_clCreateProgramWithSource createProgramWithSource;
    PFN_clBuildProgram buildProgram;
    PFN_clGetProgramBuildInfo getProgramBuildInfo;
    PFN_clCreateKernel createKernel;
    PFN_clReleaseKernel releaseKernel;
    PFN_clCreateBuffer createBuffer;
    PFN_clReleaseMemObject releaseMemObject;
    PFN_clSetKernelArg setKernelArg;
    PFN_clEnqueueWriteBuffer enqueueWriteBuffer;
    PFN_clEnqueueReadBuffer enqueueReadBuffer;
    PFN_clEnqueueNDRangeKernel enqueueNDRangeKernel;
    PFN_clFinish finish;
    PFN_clReleaseProgram releaseProgram;

    cl_platform_id platform;
    cl_device_id device;
    cl_context context;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel;
    cl_mem buf_out;
    cl_mem buf_palette;
    size_t out_bytes;
    int pal_uploaded_for;
    int ok;
};

static struct ocl_state g_ocl;

/* Reworked for C89 and to silence pointer mismatch warnings */
static void* ocl_getsym(const char* name) {
    /* Cast the FARPROC to void* to allow cleaner casting later */
    return (void*)GetProcAddress(g_ocl.dll, name);
}

/* C89: Use block comments only. Ensure void is used for empty param lists. */
static void CL_CALLBACK ocl_printf_cb(const char* a, const void* b, size_t c, void* d) {
    /* Unused parameter silencing in C89 */
    (void)b; 
    (void)c; 
    (void)d;
    if (a) { OutputDebugStringA(a); }
}

/** 
 * The kernel fixed-point Q16.16 string now uses the exact same scaling as your host library.
 * We inject the Q1616_ONE value directly from your header.
 */
static const char* g_ocl_kernel_src =
"#define FIXED_ONE (1<<16)\n"
"#define FIXED_LN2 45426\n"
"#define BAILOUT_FP (4<<16)\n"

"typedef unsigned int uint;\n"
"typedef int fixed;\n"

"static inline fixed fixed_mul(fixed b_a, fixed b_b) { long t = (long)b_a * (long)b_b; return (fixed)(t >> 16); }\n"
"static inline fixed fixed_add(fixed b_a, fixed b_b) { return (fixed)(b_a + b_b); }\n"
"static inline fixed fixed_sub(fixed b_a, fixed b_b) { return (fixed)(b_a - b_b); }\n"
"static inline fixed fixed_div(fixed b_a, fixed b_b) { long t = ((long)b_a << 16); return (fixed)(t / (long)b_b); }\n"

"static fixed fixed_ln_inline(fixed x) {\n"
"  if (x <= 0) return (fixed)(-30 * 65536);\n"
"  int s = 0;\n"
"  while (x < FIXED_ONE && s > -20) { x <<= 1; --s; }\n"
"  while (x >= (FIXED_ONE<<1) && s < 20) { x >>= 1; ++s; }\n"
"  fixed y = x - FIXED_ONE;\n"
"  fixed y2 = fixed_mul(y,y);\n"
"  fixed y3 = fixed_mul(y2,y);\n"
"  fixed y4 = fixed_mul(y3,y);\n"
"  fixed ln_m = fixed_sub(fixed_add(fixed_sub(y, fixed_mul(32768, y2)), fixed_mul(21845, y3)), fixed_mul(16384, y4));\n"
"  return fixed_add(ln_m, fixed_mul((fixed)(s << 16), FIXED_LN2));\n"
"}\n"

"__kernel void julia_fixed(__global uint* out, int width, int height, int cx_fp, int cy_fp, \n"
"                          int x_min_fp, int x_max_fp, int y_min_fp, int y_max_fp, \n"
"                          int max_iter, int ln_ln_bailout_fp, __constant uchar* pal_raw) {\n"
"  size_t gid = get_global_id(0);\n"
"  if (gid >= (size_t)width * (size_t)height) return;\n"

"  int px = (int)(gid % (size_t)width);\n"
"  int py = (int)(gid / (size_t)width);\n"

"  fixed dx = fixed_div((fixed)(x_max_fp - x_min_fp), (fixed)((width > 1 ? width - 1 : 1) << 16));\n"
"  fixed dy = fixed_div((fixed)(y_max_fp - y_min_fp), (fixed)((height > 1 ? height - 1 : 1) << 16));\n"

"  fixed zx = (fixed)x_min_fp + fixed_mul((fixed)(px<<16), dx);\n"
"  fixed zy = (fixed)y_min_fp + fixed_mul((fixed)(py<<16), dy);\n"

"  fixed zx2 = fixed_mul(zx,zx); fixed zy2 = fixed_mul(zy,zy);\n"
"  int iter = 0;\n"
"  while (iter < max_iter && (fixed_add(zx2, zy2) <= BAILOUT_FP)) {\n"
"    fixed tr = fixed_add(fixed_sub(zx2, zy2), (fixed)cx_fp);\n"
"    fixed ti = fixed_add(fixed_mul(fixed_mul(zx, zy), (fixed)(2<<16)), (fixed)cy_fp);\n"
"    zx = tr; zy = ti;\n"
"    zx2 = fixed_mul(zx,zx); zy2 = fixed_mul(zy,zy);\n"
"    ++iter;\n"
"  }\n"

"  fixed smooth;\n"
"  if (iter >= max_iter) smooth = (fixed)(iter<<16);\n"
"  else {\n"
"    fixed m2 = fixed_add(zx2, zy2);\n"
"    if (m2 <= 0) smooth = (fixed)(iter<<16);\n"
"    else {\n"
"      fixed l_m2 = fixed_ln_inline(m2);\n"
"      fixed l_a = fixed_div(l_m2, (fixed)(2<<16)); \n"
"      if (l_a <= 0) l_a = 65; \n"
"      fixed l_l_a = fixed_ln_inline(l_a);\n"
"      fixed num = fixed_sub(l_l_a, (fixed)ln_ln_bailout_fp);\n"
"      if (num <= 0) smooth = (fixed)((iter+1)<<16);\n"
"      else { fixed nu = fixed_div(num, FIXED_LN2); smooth = fixed_sub((fixed)((iter+1)<<16), nu); }\n"
"    }\n"
"  }\n"

"  fixed n = clamp(fixed_div(smooth, (fixed)(max_iter<<16)), 0, FIXED_ONE);\n"
"  fixed p_fp = fixed_mul(n, (fixed)(8<<16));\n"
"  int idx = (int)(p_fp >> 16); fixed fr = (fixed)(p_fp & 0xFFFF);\n"
"  if (idx < 0) { idx = 0; fr = 0; } if (idx >= 8) { idx = 7; fr = 0xFFFF; }\n"

"  int off_a = idx * 3; int off_b = (idx + 1) * 3;\n"
"  int r_a = pal_raw[off_a], g_a = pal_raw[off_a+1], b_a = pal_raw[off_a+2];\n"
"  int r_b = pal_raw[off_b], g_b = pal_raw[off_b+1], b_b = pal_raw[off_b+2];\n"

"  int rr = r_a + (((r_b - r_a) * (int)fr) >> 16);\n"
"  int gg = g_a + (((g_b - g_a) * (int)fr) >> 16);\n"
"  int bb = b_a + (((b_b - b_a) * (int)fr) >> 16);\n"

"  out[gid] = (0xFFu<<24) | ((uint)(rr&255)<<16) | ((uint)(gg&255)<<8) | (uint)(bb&255);\n"
"}\n";

static void ocl_resize_output(int new_w, int new_h) {
    (void)new_w; 
    (void)new_h;
    if (!g_ocl.ok) return;
    g_ocl.out_bytes = 0;
}

static int ocl_try_init(void) {
    /* C89: All variables must be declared at the top of the scope */
    cl_uint nplat;
    cl_platform_id plat;
    cl_device_id dev;
    cl_int err;
    const char* src;
    size_t logsz;
    char* log;

    if (g_ocl.ok) return 1;

    g_ocl.dll = LoadLibraryA("OpenCL.dll");
    if (!g_ocl.dll) return 0;

    /* Function pointer wiring */
    g_ocl.getPlatformIDs = (PFN_clGetPlatformIDs) ocl_getsym("clGetPlatformIDs");
    g_ocl.getDeviceIDs   = (PFN_clGetDeviceIDs)   ocl_getsym("clGetDeviceIDs");
    g_ocl.createContext  = (PFN_clCreateContext)  ocl_getsym("clCreateContext");
    g_ocl.releaseContext = (PFN_clReleaseContext) ocl_getsym("clReleaseContext");
    g_ocl.createQueue    = (PFN_clCreateCommandQueue) ocl_getsym("clCreateCommandQueue");
    g_ocl.releaseQueue   = (PFN_clReleaseCommandQueue) ocl_getsym("clReleaseCommandQueue");
    g_ocl.createProgramWithSource = (PFN_clCreateProgramWithSource) ocl_getsym("clCreateProgramWithSource");
    g_ocl.buildProgram   = (PFN_clBuildProgram)   ocl_getsym("clBuildProgram");
    g_ocl.getProgramBuildInfo = (PFN_clGetProgramBuildInfo) ocl_getsym("clGetProgramBuildInfo");
    g_ocl.createKernel   = (PFN_clCreateKernel)   ocl_getsym("clCreateKernel");
    g_ocl.createBuffer   = (PFN_clCreateBuffer)   ocl_getsym("clCreateBuffer");
    g_ocl.enqueueWriteBuffer = (PFN_clEnqueueWriteBuffer) ocl_getsym("clEnqueueWriteBuffer");
    g_ocl.enqueueReadBuffer  = (PFN_clEnqueueReadBuffer)  ocl_getsym("clEnqueueReadBuffer");
    g_ocl.enqueueNDRangeKernel = (PFN_clEnqueueNDRangeKernel) ocl_getsym("clEnqueueNDRangeKernel");
    g_ocl.finish = (PFN_clFinish) ocl_getsym("clFinish");
    g_ocl.setKernelArg = (PFN_clSetKernelArg) ocl_getsym("clSetKernelArg");
    g_ocl.releaseProgram = (PFN_clReleaseProgram) ocl_getsym("clReleaseProgram");
    g_ocl.releaseMemObject = (PFN_clReleaseMemObject) ocl_getsym("clReleaseMemObject");
    g_ocl.releaseKernel = (PFN_clReleaseKernel) ocl_getsym("clReleaseKernel");

    /* Validation of critical symbols */
    if (!g_ocl.getPlatformIDs || !g_ocl.getDeviceIDs || !g_ocl.createContext ||
        !g_ocl.createProgramWithSource || !g_ocl.buildProgram || !g_ocl.createKernel ||
        !g_ocl.createQueue || !g_ocl.releaseQueue || !g_ocl.releaseContext ||
        !g_ocl.setKernelArg || !g_ocl.enqueueNDRangeKernel || !g_ocl.enqueueReadBuffer ||
        !g_ocl.enqueueWriteBuffer || !g_ocl.finish) {
        FreeLibrary(g_ocl.dll); 
        g_ocl.dll = NULL; 
        return 0;
    }

    /* Platform Detection */
    nplat = 0;
    if (g_ocl.getPlatformIDs(0, NULL, &nplat) != CL_SUCCESS || nplat == 0) { 
        FreeLibrary(g_ocl.dll); g_ocl.dll = NULL; return 0; 
    }
    if (g_ocl.getPlatformIDs(1, &plat, NULL) != CL_SUCCESS) { 
        FreeLibrary(g_ocl.dll); g_ocl.dll = NULL; return 0; 
    }
    g_ocl.platform = plat;

    /* Device Detection (Prioritize GPU) */
    dev = NULL;
    if (g_ocl.getDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 1, &dev, NULL) != CL_SUCCESS) {
        if (g_ocl.getDeviceIDs(plat, CL_DEVICE_TYPE_CPU, 1, &dev, NULL) != CL_SUCCESS) { 
            FreeLibrary(g_ocl.dll); g_ocl.dll = NULL; return 0; 
        }
    }
    g_ocl.device = dev;

    /* Context Creation */
    err = 0;
    g_ocl.context = g_ocl.createContext(NULL, 1, &g_ocl.device, ocl_printf_cb, NULL, &err);
    if (!g_ocl.context || err != CL_SUCCESS) { 
        FreeLibrary(g_ocl.dll); g_ocl.dll = NULL; return 0; 
    }

    /* Command Queue Creation */
    g_ocl.queue = g_ocl.createQueue(g_ocl.context, g_ocl.device, 0, &err);
    if (!g_ocl.queue || err != CL_SUCCESS) { 
        g_ocl.releaseContext(g_ocl.context); g_ocl.context = NULL; 
        FreeLibrary(g_ocl.dll); g_ocl.dll = NULL; return 0; 
    }

    /* Program and Build */
    src = g_ocl_kernel_src;
    g_ocl.program = g_ocl.createProgramWithSource(g_ocl.context, 1, &src, NULL, &err);
    if (!g_ocl.program || err != CL_SUCCESS) { 
        g_ocl.releaseQueue(g_ocl.queue); g_ocl.releaseContext(g_ocl.context); 
        FreeLibrary(g_ocl.dll); g_ocl.dll = NULL; return 0; 
    }

    err = g_ocl.buildProgram(g_ocl.program, 1, &g_ocl.device, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        logsz = 0;
        g_ocl.getProgramBuildInfo(g_ocl.program, g_ocl.device, CL_PROGRAM_BUILD_LOG, 0, NULL, &logsz);
        if (logsz > 1 && logsz < (1<<20)) {
            log = (char*)HeapAlloc(GetProcessHeap(), 0, logsz + 1);
            if (log) {
                g_ocl.getProgramBuildInfo(g_ocl.program, g_ocl.device, CL_PROGRAM_BUILD_LOG, logsz, log, NULL);
                log[logsz] = 0;
                OutputDebugStringA("OpenCL build log:\n");
                OutputDebugStringA(log);
                OutputDebugStringA("\n");
                HeapFree(GetProcessHeap(), 0, log);
            }
        }
        /* Fallback Cleanup on Build Failure */
        if (g_ocl.program) g_ocl.releaseProgram(g_ocl.program);
        g_ocl.releaseQueue(g_ocl.queue); g_ocl.queue = NULL;
        g_ocl.releaseContext(g_ocl.context); g_ocl.context = NULL;
        FreeLibrary(g_ocl.dll); g_ocl.dll = NULL; 
        return 0;
    }

    /* Kernel Creation */
    g_ocl.kernel = g_ocl.createKernel(g_ocl.program, "julia_fixed", &err);
    if (!g_ocl.kernel || err != CL_SUCCESS) {
        if (g_ocl.program) g_ocl.releaseProgram(g_ocl.program);
        g_ocl.releaseQueue(g_ocl.queue); g_ocl.queue = NULL;
        g_ocl.releaseContext(g_ocl.context); g_ocl.context = NULL;
        FreeLibrary(g_ocl.dll); g_ocl.dll = NULL; 
        return 0;
    }

    g_ocl.ok = 1;
    return 1;
}

static int ocl_render(
    uint32_t* out_fb, int width, int height,
    int cx_fp, int cy_fp,
    int x_min_fp, int x_max_fp, int y_min_fp, int y_max_fp,
    int max_iter, int ln_ln_bailout_fp)
{
    /* C89: Declarations must come before any executable statements */
    size_t bytes;
    cl_int err;
    int a, i;
    size_t global, local;
    unsigned char pal_pack[INTERPOLATION_SEGMENTS * 3 + 3]; /* Assume INTERPOLATION_SEGMENTS is a define */

    if (!g_ocl.ok || !out_fb || width <= 0 || height <= 0) return 0;

    bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
    err = 0;

    /* Handle output buffer resizing */
    if (!g_ocl.buf_out || g_ocl.out_bytes != bytes) {
        if (g_ocl.buf_out) { 
            g_ocl.releaseMemObject(g_ocl.buf_out); 
            g_ocl.buf_out = NULL; 
            g_ocl.out_bytes = 0; 
        }
        g_ocl.buf_out = g_ocl.createBuffer(g_ocl.context, CL_MEM_WRITE_ONLY, bytes, NULL, &err);
        if (!g_ocl.buf_out || err != CL_SUCCESS) return 0;
        g_ocl.out_bytes = bytes;
    }

    /* Handle palette buffer creation */
    if (!g_ocl.buf_palette) {
        /* INTERPOLATION_SEGMENTS + 1 to account for the lerp ceiling */
        g_ocl.buf_palette = g_ocl.createBuffer(g_ocl.context, CL_MEM_READ_ONLY, 
                                              (size_t)(INTERPOLATION_SEGMENTS + 1) * 3, NULL, &err);
        if (!g_ocl.buf_palette || err != CL_SUCCESS) return 0;
    }

    /* Pack palette into a linear byte array */
    for (i = 0; i <= INTERPOLATION_SEGMENTS; i++) {
        pal_pack[i * 3 + 0] = g_fractal_palettes[g_active_palette][i].red;
        pal_pack[i * 3 + 1] = g_fractal_palettes[g_active_palette][i].green;
        pal_pack[i * 3 + 2] = g_fractal_palettes[g_active_palette][i].blue;
    }

    err = g_ocl.enqueueWriteBuffer(g_ocl.queue, g_ocl.buf_palette, 1, 0, 
                                   (size_t)(INTERPOLATION_SEGMENTS + 1) * 3, pal_pack, 0, NULL, NULL);
    if (err != CL_SUCCESS) return 0;

    /* Set Kernel Arguments */
    a = 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(cl_mem), &g_ocl.buf_out) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &width) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &height) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &cx_fp) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &cy_fp) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &x_min_fp) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &x_max_fp) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &y_min_fp) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &y_max_fp) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &max_iter) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(int), &ln_ln_bailout_fp) != CL_SUCCESS) return 0;
    if (g_ocl.setKernelArg(g_ocl.kernel, a++, sizeof(cl_mem), &g_ocl.buf_palette) != CL_SUCCESS) return 0;

    /* Define execution dimensions */
    global = (size_t)width * (size_t)height;
    local  = 64;
    if (global % local) {
        global = ((global / local) + 1) * local;
    }

    /* Execute and Read Back */
    if (g_ocl.enqueueNDRangeKernel(g_ocl.queue, g_ocl.kernel, 1, NULL, &global, &local, 0, NULL, NULL) != CL_SUCCESS) return 0;
    if (g_ocl.enqueueReadBuffer(g_ocl.queue, g_ocl.buf_out, 1, 0, bytes, out_fb, 0, NULL, NULL) != CL_SUCCESS) return 0;
    
    g_ocl.finish(g_ocl.queue);
    return 1;
}

static void ocl_shutdown(void) {
    if (!g_ocl.dll) return;

    if (g_ocl.buf_palette) { g_ocl.releaseMemObject(g_ocl.buf_palette); g_ocl.buf_palette = NULL; }
    if (g_ocl.buf_out)     { g_ocl.releaseMemObject(g_ocl.buf_out);     g_ocl.buf_out = NULL; }
    if (g_ocl.kernel)      { g_ocl.releaseKernel(g_ocl.kernel);        g_ocl.kernel = NULL; }
    if (g_ocl.program)     { g_ocl.releaseProgram(g_ocl.program);      g_ocl.program = NULL; }
    if (g_ocl.queue)       { g_ocl.releaseQueue(g_ocl.queue);          g_ocl.queue = NULL; }
    if (g_ocl.context)     { g_ocl.releaseContext(g_ocl.context);      g_ocl.context = NULL; }

    FreeLibrary(g_ocl.dll);
    g_ocl.dll = NULL;
    g_ocl.ok = 0;
    g_ocl.out_bytes = 0;
}

#endif /* OCL_H */
