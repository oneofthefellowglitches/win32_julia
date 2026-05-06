#ifndef SYSTEM_H
#define SYSTEM_H

/* -------------------------------------------------------------------------
   1. TYPE DEFINITIONS
   ------------------------------------------------------------------------- */
typedef signed int         q1616_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef signed int         int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        s8;
typedef short              s16;
typedef signed int         s32;
typedef long long          s64;
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
#if 1
#include <stddef.h>
#endif
/* -------------------------------------------------------------------------
   2. PLATFORM DETECTION
   ------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(_WIN64)
    #define SYSTEM_WINDOWS 1
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #define SYSTEM_LINUX 1
#endif

/* -------------------------------------------------------------------------
   3. SIMD & INTRINSICS
   ------------------------------------------------------------------------- */
#if defined(_MSC_VER)
    #include <intrin.h>
    #include <immintrin.h>
#elif defined(__GNUC__) || defined(__clang__)
    #include <x86intrin.h>
    #include <cpuid.h>
#endif

/* -------------------------------------------------------------------------
   4. SYSTEM ABSTRACTION MACROS
   ------------------------------------------------------------------------- */
#if defined(_MSC_VER)
    #define SYSTEM_FORCEINLINE __forceinline
    #define SYSTEM_INLINE      static __inline

    #define SYSTEM_ALIGN16_BEG __declspec(align(16))
    #define SYSTEM_ALIGN16_END 

    #define SYSTEM_ALIGN32_BEG __declspec(align(32))
    #define SYSTEM_ALIGN32_END 

    #define SYSTEM_ALIGN64_BEG __declspec(align(64))
    #define SYSTEM_ALIGN64_END 

    #ifndef __clang__
        #pragma comment(lib, "kernel32.lib")
        #pragma comment(lib, "user32.lib")
        #pragma comment(lib, "gdi32.lib")
        #pragma function(memset, memcpy, memmove, memcmp, strlen)
    #endif
    
    #if !defined(_FLTUSED_DEFINED)
    #define _FLTUSED_DEFINED
    /* need to be initialized (int _fltused = 0;) in main.c */
        extern int _fltused;
    #endif

#elif defined(__GNUC__) || defined(__clang__)
    #define SYSTEM_FORCEINLINE __attribute__((always_inline)) static __inline__
    #define SYSTEM_INLINE      static __inline__

    #define SYSTEM_ALIGN16_BEG
    #define SYSTEM_ALIGN16_END __attribute__((aligned(16)))

    #define SYSTEM_ALIGN32_BEG
    #define SYSTEM_ALIGN32_END __attribute__((aligned(32)))

    #define SYSTEM_ALIGN64_BEG __attribute__((aligned(64)))
    #define SYSTEM_ALIGN64_END 
#else
    #define SYSTEM_FORCEINLINE static
    #define SYSTEM_INLINE      static

    #define SYSTEM_ALIGN16_BEG
    #define SYSTEM_ALIGN16_END

    #define SYSTEM_ALIGN32_BEG
    #define SYSTEM_ALIGN32_END

    #define SYSTEM_ALIGN64_BEG
    #define SYSTEM_ALIGN64_END
#endif

/* -------------------------------------------------------------------------
   5. FEATURE DETECTION
   ------------------------------------------------------------------------- */
#if defined(__AVX2__) || defined(_M_AVX2)
    #define SYSTEM_HAS_AVX2 1
#else
    #define SYSTEM_HAS_AVX2 0
    #pragma message("SYSTEM: AVX2 hardware acceleration disabled in build.")
#endif

/**
 * @brief Runtime check for SSE2 support.
 */
SYSTEM_INLINE int system_supports_sse2(void) {
    int cpu_info[4];
#if defined(_MSC_VER)
    __cpuid(cpu_info, 1);
#elif defined(__GNUC__) || defined(__clang__)
    __cpuid(cpu_info, 1);
#else
    return 0;
#endif
    /* CPUID.01H:EDX.SSE2[bit 26] */
    return (cpu_info[3] & (1 << 26)) != 0;
}

/**
 * @brief Runtime check for SSE4.1 support.
 */
SYSTEM_INLINE int system_supports_sse41(void) {
    int cpu_info[4];
#if defined(_MSC_VER)
    __cpuid(cpu_info, 1);
#elif defined(__GNUC__) || defined(__clang__)
    __cpuid(cpu_info, 1);
#else
    return 0;
#endif
    /* CPUID.01H:ECX.SSE4_1[bit 19] */
    return (cpu_info[2] & (1 << 19)) != 0;
}

/**
 * @brief Runtime check for AVX2 support.
 */
SYSTEM_INLINE int system_supports_avx2(void) {
    int cpu_info[4];
#if defined(_MSC_VER)
    __cpuid(cpu_info, 7); 
#elif defined(__GNUC__) || defined(__clang__)
    __cpuid_count(7, 0, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#else
    return 0;
#endif
    return (cpu_info[1] & (1 << 5)) != 0;
}

/**
 * @brief Runtime check for AVX-512 support (safe).
 */
SYSTEM_INLINE int system_supports_avx512(void) {
    int cpu_info[4];

#if defined(_MSC_VER)
    __cpuid(cpu_info, 1);

    /* OSXSAVE + AVX must be present first */
    if (!(cpu_info[2] & (1 << 27))) return 0; /* OSXSAVE */
    if (!(cpu_info[2] & (1 << 28))) return 0; /* AVX */

    /* Check XCR0: XMM | YMM | ZMM | Hi-ZMM | Opmask */
    unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0xE6) != 0xE6) return 0;

    __cpuidex(cpu_info, 7, 0);

#elif defined(__GNUC__) || defined(__clang__)
    unsigned int eax, ebx, ecx, edx;

    __cpuid(1, eax, ebx, ecx, edx);
    if (!(ecx & (1 << 27))) return 0; /* OSXSAVE */
    if (!(ecx & (1 << 28))) return 0; /* AVX */

    unsigned long long xcr0 = __builtin_ia32_xgetbv(0);
    if ((xcr0 & 0xE6) != 0xE6) return 0;

    __cpuid_count(7, 0, eax, ebx, ecx, edx);

#else
    return 0;
#endif

    /* AVX-512 Foundation */
    return (cpu_info[1] & (1 << 16)) != 0;
}

/**
 * @brief Quick&Dirty Runtime check for AVX-512 support.
 */
SYSTEM_INLINE int system_supports_avx512_fast(void) {
    int cpu_info[4];
    __cpuidex(cpu_info, 7, 0);

    if (!(cpu_info[1] & (1 << 16)))
        return 0; /* AVX-512F */

    unsigned long long xcr0 = _xgetbv(0);
    return ((xcr0 & 0xE6) == 0xE6);
}

/* -------------------------------------------------------------------------
   Detect Vulkan support and version
   Header-only, no allocations, no surfaces
   ------------------------------------------------------------------------- */
#if VK_USE_FLAG
#if defined(_WIN32)
    #define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>

typedef struct system_vulkan_info_t {
    int      supported;      /* 0 = no Vulkan loader or no GPU */
    uint32_t loader_version; /* VK_VERSION packed */
    uint32_t gpu_version;    /* max API version supported by GPU */
} system_vulkan_info_t;

static SYSTEM_INLINE system_vulkan_info_t system_supports_vulkan(void) {
    system_vulkan_info_t info = {0};

    /* ---- Step 1: Loader presence & version ---- */
    PFN_vkEnumerateInstanceVersion pfnEnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");

    if (!pfnEnumerateInstanceVersion)
        return info;

    if (pfnEnumerateInstanceVersion(&info.loader_version) != VK_SUCCESS)
        return info;

    /* ---- Step 2: Create minimal instance ---- */
    VkApplicationInfo app = {0};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion         = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici = {0};
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
        return info;

    /* ---- Step 3: Enumerate physical devices ---- */
    uint32_t gpu_count = 0;
    if (vkEnumeratePhysicalDevices(instance, &gpu_count, NULL) != VK_SUCCESS ||
        gpu_count == 0) {
        vkDestroyInstance(instance, NULL);
        return info;
    }

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(instance, &gpu_count, &gpu);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);

    info.gpu_version = props.apiVersion;
    info.supported   = 1;

    vkDestroyInstance(instance, NULL);
    return info;
}

static SYSTEM_INLINE int system_supports_vulkan_1_1(void) {
    system_vulkan_info_t v = system_supports_vulkan();
    return v.supported && v.gpu_version >= VK_API_VERSION_1_1;
}

static SYSTEM_INLINE int system_supports_vulkan_1_2(void) {
    system_vulkan_info_t v = system_supports_vulkan();
    return v.supported && v.gpu_version >= VK_API_VERSION_1_2;
}

static SYSTEM_INLINE int system_supports_vulkan_1_3(void) {
    system_vulkan_info_t v = system_supports_vulkan();
    return v.supported && v.gpu_version >= VK_API_VERSION_1_3;
}
#endif

/* -------------------------------------------------------------------------
   6. ENTRY POINT ABSTRACTION
   ------------------------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
    #define SYSTEM_ENTRY_VISIBILITY __attribute__((externally_visible))
    #if defined(__i386__) || defined(__i686__)
        #define SYSTEM_ENTRY_ALIGN  __attribute__((force_align_arg_pointer))
    #else
        #define SYSTEM_ENTRY_ALIGN
    #endif
#else
    #define SYSTEM_ENTRY_VISIBILITY
    #define SYSTEM_ENTRY_ALIGN
#endif

#define SYSTEM_ENTRY_ATTRIBUTES SYSTEM_ENTRY_VISIBILITY SYSTEM_ENTRY_ALIGN

#endif /* SYSTEM_H */