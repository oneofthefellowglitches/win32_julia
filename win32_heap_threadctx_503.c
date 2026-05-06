/* win32_heap_threadctx_503.c */
#define CRT_IMPLEMENTATION
#include "crt.h"
#include "q1616.h"
#define PALETTE_IMPLEMENTATION
#include "palette.h"

/**GDI
== CPU rasterization
== Immediate mode
== State-heavy, fragile
== Single-threaded by design
== No real backbuffer unless you build one
✅ Dead simple
✅ Zero setup
✅ Perfect for:
❌ Flicker unless double-buffered
❌ No vsync control
❌ DPI / scaling weirdness
❌ Slow at scale
❌ Undefined behavior if GDI objects are mishandled
❌ Not compatible with Vulkan/modern GPU paths
Window ownership
Backbuffer presentation
HUD drawing (one to keep)
*/

/**DXGI
DXGI:
owns backbuffers (GPU textures)
handles vsync
handles tearing
handles fullscreen
handles resizing
is required for D3D11 / D3D12 / Vulkan-on-Windows
✅ Tear-free
✅ VSync control
✅ Flip model (modern)
✅ Zero flicker
✅ GPU-native
✅ Plays perfectly with Vulkan
✅ No GDI object lifetime madness
*/

/**
 * win32_window
   +-- input (raw, msg loop)
   +-- renderer backend
         |
         +-- CPU scalar
         +-- CPU SSE2 / SSE4 / AVX2
         +-- CPU AVX512
         +-- Vulkan compute
         +-- Vulkan fragment
         +-- DXGI swapchain (single)
*/

/**
[ CPU Threads + SIMD ]
[ g_pixel_buffer ]
[ CPU upload texture ]
[ swapchain backbuffer ]
[ OS compositor ]
[ monitor ]
*/

#define OCL_USE_FLAG

typedef struct {
    /* 1. Thread Control & Sync (16 bytes on 32-bit, 24-32 on 64-bit) */
    HANDLE  signal_start_event;  /* 4 or 8 bytes *//* Auto-reset: triggers work start */   
    BOOL    is_exit_pending;     /* 4 bytes *//* Termination flag */

    /* 2. Fractal Parameters (20 bytes) *//* Julia set constants */    
    q1616_t seed_cx;             /* 4 bytes */
    q1616_t seed_cy;             /* 4 bytes */

    q1616_t bailout;             /* 4 bytes *//* Squared escape radius */
    q1616_t ln_ln_bailout;       /* 4 bytes *//* Precomputed for smooth coloring */
    int     max_iterations;      /* 4 bytes */

    /* 3. Viewport (16 bytes) *//* Coordinate space: viewport boundaries */    
    q1616_t view_x_min;          /* 4 bytes */
    q1616_t view_x_max;          /* 4 bytes */
    q1616_t view_y_min;          /* 4 bytes */
    q1616_t view_y_max;          /* 4 bytes */

    /* 4. Manual Padding 
     * Current total (approx): 52 to 60 bytes.
     * We pad to 64 bytes to ensure the array elements don't overlap cache lines. 
     * The size below is a safe buffer; the compiler's sizeof() will tell the truth. */
    char    cache_padding[24];
} thread_context_t;


/* generalize to GPU+CPU modes */
typedef enum render_cpu_mode_t {
    RENDER_CPU_SCALAR = 0,
    RENDER_CPU_SSE2   = 1,
    RENDER_CPU_SSE41  = 2,
    RENDER_CPU_AVX2   = 3,
    RENDER_CPU_AVX512 = 4,
    RENDER_CPU_COUNT
} render_cpu_mode_t;

volatile render_cpu_mode_t      g_render_cpu_mode       = RENDER_CPU_AVX2;

int                             _fltused                = 0;
#define MAX_THREAD_COUNT 64
static size_t                   g_aligned_stride        = 0;
static int                      g_active_thread_count   = 0;
static HANDLE                   g_thread_handles[MAX_THREAD_COUNT];
static void*                    g_thread_data_raw       = NULL;
static thread_context_t*        g_thread_data           = NULL;

static volatile LONG            g_next_row_index        = 0;    /* Atomic: Next row to be processed */
static volatile LONG            g_rows_completed        = 0;    /* Atomic: Total rows finished this frame */
static HANDLE                   g_frame_complete_event  = NULL; /* Manual-reset: all threads done */
static HANDLE                   g_process_heap          = NULL;
static uint32_t*                g_pixel_buffer          = NULL; /* PixelBuffer (0xRRGGBB) */
static int                      g_window_width          = 1420;
static int                      g_window_height         = 800;
static int                      g_active_palette        = 1;

static HWND                     g_window_handle         = NULL;
static BOOL                     g_is_fullscreen_mode    = FALSE;
static BOOL                     g_is_ocl_supported      = FALSE;
static BOOL                     g_is_ocl_accelerated    = FALSE;
static BOOL                     g_is_cpu_scalar         = TRUE;
static BOOL                     g_is_cpu_sse2           = FALSE;
static BOOL                     g_is_cpu_sse41          = FALSE;
static BOOL                     g_is_cpu_avx2           = FALSE;
static BOOL                     g_is_cpu_avx512f        = FALSE;
#ifdef VK_USE_FLAG
static BOOL                     g_is_vulkan_supported   = FALSE;
static BOOL                     g_is_vulkan_accelerated = FALSE;
static uint32_t                 g_vulkan_version_major  = 0;
static uint32_t                 g_vulkan_version_minor  = 0;
static uint32_t                 g_vulkan_version_patch  = 0;
#endif
/* Window placement for toggling fullscreen */
static WINDOWPLACEMENT          g_prev_window_pos       =
{ sizeof(WINDOWPLACEMENT), 0, 0, {0, 0}, {0, 0}, {0, 0, 0, 0} };

static volatile BOOL            g_is_paused             = FALSE;
static volatile BOOL            g_is_rendering          = FALSE;
static volatile BOOL            g_is_help_visible       = TRUE;

static LARGE_INTEGER            g_timer_frequency       = { {0, 0} };
static LARGE_INTEGER            g_time_frame_start      = { {0, 0} };
static LARGE_INTEGER            g_time_title_update     = { {0, 0} };
static uint32_t                 g_frames_accumulated    = 0;
static uint32_t                 g_last_measured_fps     = 0;

#ifdef OCL_USE_FLAG
#include "ocl.h" /* deps: palette.h, g_active_palette */
#else
#include "ocl_stub.h"
#endif

static SYSTEM_FORCEINLINE int render_mode_is_supported(render_cpu_mode_t mode) {
    switch (mode) {
    case RENDER_CPU_SCALAR: return g_is_cpu_scalar;
    case RENDER_CPU_SSE2:   return g_is_cpu_sse2;
    case RENDER_CPU_SSE41:  return g_is_cpu_sse41;
    case RENDER_CPU_AVX2:   return g_is_cpu_avx2;
    case RENDER_CPU_AVX512: return g_is_cpu_avx512f;
    default: return 0;
    }
}

static SYSTEM_FORCEINLINE void render_mode_cycle_next(void) {
    int i;

    for (i = 1; i <= RENDER_CPU_COUNT; ++i) {
        render_cpu_mode_t next =
            (render_cpu_mode_t)((g_render_cpu_mode + i) % RENDER_CPU_COUNT);

        if (render_mode_is_supported(next)) {
            g_render_cpu_mode = next;
            break;
        }
    }
}

static SYSTEM_FORCEINLINE void win32_telemetry_init(void) {
    QueryPerformanceFrequency(&g_timer_frequency);
    QueryPerformanceCounter(&g_time_frame_start);
    g_time_title_update = g_time_frame_start;
    g_frames_accumulated = 0;
    g_last_measured_fps = 0;
}

static void win32_window_status_update(HWND window_handle, uint32_t fps);

static SYSTEM_FORCEINLINE void win32_telemetry_update(HWND window_handle) {
    LARGE_INTEGER current_tick;
    LONGLONG ticks_since_last_update;
    uint32_t current_fps;
    uint64_t total_weighted_frames;

    QueryPerformanceCounter(&current_tick);
    /* Increment the count of finished frames */
    g_frames_accumulated++;

    /* Calculate the duration since the last window title refresh */
    ticks_since_last_update = current_tick.QuadPart - g_time_title_update.QuadPart;

    /* Update the UI twice per second (if 500ms has passed) */
    if (ticks_since_last_update >= (g_timer_frequency.QuadPart / 2)) {
        current_fps = 0;

        if (ticks_since_last_update > 0) {
            /* FPS = (Frames * TicksPerSecond) / TicksElapsed */
            total_weighted_frames = (uint64_t)g_frames_accumulated * (uint64_t)g_timer_frequency.QuadPart;
            /* Perform division with rounding to nearest integer: (N + D/2) / D */
            current_fps = (uint32_t)((total_weighted_frames + (ticks_since_last_update / 2)) / 
                                    (uint64_t)ticks_since_last_update);
        }

        /* Store for internal telemetry and update the window bar */
        g_last_measured_fps = current_fps;
        win32_window_status_update(window_handle, current_fps);

        /* Reset counters for the next interval */
        g_frames_accumulated = 0;
        g_time_title_update = current_tick;
    }

    /* Track the absolute time of the last frame processed */
    g_time_frame_start = current_tick;
}

static const char* cpu_mode_to_str(render_cpu_mode_t mode) {
    switch (mode) {
    case RENDER_CPU_SCALAR: return "SCALAR ";
    case RENDER_CPU_SSE2:   return "SSE2 ";
    case RENDER_CPU_SSE41:  return "SSE41 ";
    case RENDER_CPU_AVX2:   return "AVX2 ";
    case RENDER_CPU_AVX512: return "AVX512 ";
    default:                return "SCALAR ";
    }
}

static void win32_window_status_update(HWND window_handle, uint32_t fps) {
    char title_buffer[128];
    char *dst;
    char *num_src;
    const char* mode_prefix;
    const char* suffix = " FPS";

    mode_prefix = g_is_ocl_accelerated
                ? "OpenCL "
                : cpu_mode_to_str(g_render_cpu_mode);

    dst = title_buffer;

    /* Append prefix */
    while (*mode_prefix) *dst++ = *mode_prefix++;
    
    /* Append FPS number */
    num_src = crt_u32_to_str_rev(title_buffer + sizeof(title_buffer), fps);
    while (*num_src) *dst++ = *num_src++;
    
    /* Append suffix */
    while (*suffix) *dst++ = *suffix++;
    
    *dst = '\0';
    SetWindowTextA(window_handle, title_buffer);
}

/* -------------------- OverlayHUD Render -------------------- */
static void win32_gdi_hud_render(HDC device_context) {
    HFONT hFontInterface, hOldFont;
    HPEN hBorderPen, hOldPen;
    HBRUSH hStatusBrush, hOldBrush;    
    RECT rectBadge;

    char fps_string[48];
    const char* fps_prefix = "FPS: ";
    const char* gpu_suffix = "  [GPU]";
    const char* gpu_toggle_help = "G - toggle GPU (OpenCL)";
    static const char* help_lines[] = {
        "H    - toggle help",
        "P    - pause..resume",
        "1..9 - switch palette",
        "F5   - switch render mode",
        "F11  - fullscreen",
        "F12  - take screenshot (.bmp)",
        "ESC  - exit fullscreen"
    };

    char *str_cursor, *num_ptr;
    const char *src_ptr;

    int badge_x, badge_y, badge_w, badge_h;
    int help_x, help_y, line_spacing, i;
    int margin_padding = 8;
    int badge_spacing  = 4;

    /* --- GDI State Setup --- */
    SetBkMode(device_context, TRANSPARENT);
    SetTextColor(device_context, RGB(255, 255, 255));
    hFontInterface = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    hOldFont = (HFONT)SelectObject(device_context, hFontInterface);

    /* --- Render FPS Counter --- */
    str_cursor = fps_string;
    src_ptr = fps_prefix;
    while (*src_ptr) *str_cursor++ = *src_ptr++;
    
    num_ptr = crt_u32_to_str_rev(fps_string + sizeof(fps_string), g_last_measured_fps);
    while (*num_ptr) *str_cursor++ = *num_ptr++;
    
    if (g_is_ocl_accelerated) {
        src_ptr = gpu_suffix;
        while (*src_ptr) *str_cursor++ = *src_ptr++;
    }
    *str_cursor = '\0';
    
    TextOutA(device_context, margin_padding, margin_padding, 
             fps_string, crt_str_len(fps_string));


    /* --- Stack of CPU/GPU Badges --- */
    badge_w = 100; 
    badge_h = 20;
    badge_x = g_window_width - badge_w - margin_padding;
    badge_y = margin_padding;

    /* Helper macro to reduce repetition */
    #define DRAW_BADGE(label_str, is_supported, is_active) do { \
        rectBadge.left   = badge_x; \
        rectBadge.top    = badge_y; \
        rectBadge.right  = badge_x + badge_w; \
        rectBadge.bottom = badge_y + badge_h; \
        \
        if (!(is_supported)) { \
            hStatusBrush = CreateSolidBrush(RGB(160,0,0)); \
        } else if ((is_active)) { \
            hStatusBrush = CreateSolidBrush(RGB(0,160,0)); \
        } else { \
            hStatusBrush = CreateSolidBrush(RGB(100,100,100)); \
        } \
        \
        FillRect(device_context, &rectBadge, hStatusBrush); \
        hBorderPen = CreatePen(PS_SOLID, 1, RGB(0,0,0)); \
        hOldPen    = (HPEN)SelectObject(device_context, hBorderPen); \
        hOldBrush  = (HBRUSH)SelectObject(device_context, GetStockObject(NULL_BRUSH)); \
        RoundRect(device_context, rectBadge.left, rectBadge.top, rectBadge.right, rectBadge.bottom, 8, 8); \
        TextOutA(device_context, rectBadge.left + 8, rectBadge.top + ((badge_h - 14)/2), label_str, crt_str_len(label_str)); \
        SelectObject(device_context, hOldBrush); \
        SelectObject(device_context, hOldPen); \
        DeleteObject(hStatusBrush); \
        DeleteObject(hBorderPen); \
        badge_y += badge_h + badge_spacing; \
    } while(0)

    /* --- GPU/OpenCL Badge --- */
    DRAW_BADGE(
        !g_is_ocl_supported ? "OpenCL: N/A" : (g_is_ocl_accelerated ? "OpenCL: ON" : "OpenCL: OFF"),
        g_is_ocl_supported,
        g_is_ocl_accelerated
    );

    /* --- SCALAR Badge (Default) --- */
    DRAW_BADGE(
        !g_is_cpu_scalar ? "SCALAR: N/A" : (g_render_cpu_mode == RENDER_CPU_SCALAR ? "SCALAR: ON" : "SCALAR: OFF"),
        g_is_cpu_scalar,
        g_render_cpu_mode == RENDER_CPU_SCALAR
    );

    /* --- SSE2 Badge --- */
    DRAW_BADGE(
        !g_is_cpu_sse2 ? "SSE2: N/A" : (g_render_cpu_mode == RENDER_CPU_SSE2 ? "SSE2: ON" : "SSE2: OFF"),
        g_is_cpu_sse2,
        g_render_cpu_mode == RENDER_CPU_SSE2
    );

    /* --- SSE4.1 Badge --- */
    DRAW_BADGE(
        !g_is_cpu_sse41 ? "SSE4.1: N/A" : (g_render_cpu_mode == RENDER_CPU_SSE41 ? "SSE4.1: ON" : "SSE4.1: OFF"),
        g_is_cpu_sse41,
        g_render_cpu_mode == RENDER_CPU_SSE41
    );

    /* --- AVX2 Badge --- */
    DRAW_BADGE(
        !g_is_cpu_avx2 ? "AVX2: N/A" : (g_render_cpu_mode == RENDER_CPU_AVX2 ? "AVX2: ON" : "AVX2: OFF"),
        g_is_cpu_avx2,
        g_render_cpu_mode == RENDER_CPU_AVX2
    );

    /* --- AVX512 Badge --- */
    DRAW_BADGE(
        !g_is_cpu_avx512f ? "AVX512: N/A" : (g_render_cpu_mode == RENDER_CPU_AVX512 ? "AVX512: ON" : "AVX512: OFF"),
        g_is_cpu_avx512f,
        g_render_cpu_mode == RENDER_CPU_AVX512
    );

    #if 0
    /* --- Stack of Badges --- */
    badge_w = 100; badge_h = 20;
    badge_x = g_window_width - badge_w - margin_padding;
    badge_y = margin_padding;

    /* --- GPU/OpenCL Badge --- */
    rectBadge.left   = badge_x;
    rectBadge.top    = badge_y;
    rectBadge.right  = badge_x + badge_w;
    rectBadge.bottom = badge_y + badge_h;

    const char* gpu_label;
    if (!g_is_ocl_supported) {
        hStatusBrush = CreateSolidBrush(RGB(160,0,0)); gpu_label = "OpenCL: N/A";
    } else if (g_is_ocl_accelerated) {
        hStatusBrush = CreateSolidBrush(RGB(0,160,0)); gpu_label = "OpenCL: ON";
    } else {
        hStatusBrush = CreateSolidBrush(RGB(100,100,100)); gpu_label = "OpenCL: OFF";
    }

    FillRect(device_context, &rectBadge, hStatusBrush);
    hBorderPen = CreatePen(PS_SOLID, 1, RGB(0,0,0));
    hOldPen    = (HPEN)SelectObject(device_context, hBorderPen);
    hOldBrush  = (HBRUSH)SelectObject(device_context, GetStockObject(NULL_BRUSH));
    RoundRect(device_context, rectBadge.left, rectBadge.top, rectBadge.right, rectBadge.bottom, 8, 8);
    TextOutA(device_context, rectBadge.left + 8, rectBadge.top + ((badge_h - 14)/2), gpu_label, crt_str_len(gpu_label));
    SelectObject(device_context, hOldBrush);
    SelectObject(device_context, hOldPen);
    DeleteObject(hStatusBrush);
    DeleteObject(hBorderPen);

    /* --- AVX2 Badge --- */
    badge_y += badge_h + badge_spacing;
    rectBadge.top    = badge_y;
    rectBadge.bottom = badge_y + badge_h;

    const char* avx2_label;
    if (!g_is_cpu_avx2) {
        hStatusBrush = CreateSolidBrush(RGB(160,0,0)); avx2_label = "AVX2: N/A";
    } else if (g_render_cpu_mode == RENDER_CPU_AVX2) {
        hStatusBrush = CreateSolidBrush(RGB(0,160,0)); avx2_label = "AVX2: ON";
    } else {
        hStatusBrush = CreateSolidBrush(RGB(100,100,100)); avx2_label = "AVX2: OFF";
    }

    FillRect(device_context, &rectBadge, hStatusBrush);
    hBorderPen = CreatePen(PS_SOLID, 1, RGB(0,0,0));
    hOldPen    = (HPEN)SelectObject(device_context, hBorderPen);
    hOldBrush  = (HBRUSH)SelectObject(device_context, GetStockObject(NULL_BRUSH));
    RoundRect(device_context, rectBadge.left, rectBadge.top, rectBadge.right, rectBadge.bottom, 8, 8);
    TextOutA(device_context, rectBadge.left + 8, rectBadge.top + ((badge_h - 14)/2), avx2_label, crt_str_len(avx2_label));
    SelectObject(device_context, hOldBrush);
    SelectObject(device_context, hOldPen);
    DeleteObject(hStatusBrush);
    DeleteObject(hBorderPen);

    /* --- AVX512 Badge --- */
    badge_y += badge_h + badge_spacing;
    rectBadge.top    = badge_y;
    rectBadge.bottom = badge_y + badge_h;

    const char* avx512_label;
    if (!g_is_cpu_avx512f) {
        hStatusBrush = CreateSolidBrush(RGB(160,0,0)); avx512_label = "AVX512: N/A";
    } else if (g_render_cpu_mode == RENDER_CPU_AVX512) {
        hStatusBrush = CreateSolidBrush(RGB(0,160,0)); avx512_label = "AVX512: ON";
    } else {
        hStatusBrush = CreateSolidBrush(RGB(100,100,100)); avx512_label = "AVX512: OFF";
    }

    FillRect(device_context, &rectBadge, hStatusBrush);
    hBorderPen = CreatePen(PS_SOLID, 1, RGB(0,0,0));
    hOldPen    = (HPEN)SelectObject(device_context, hBorderPen);
    hOldBrush  = (HBRUSH)SelectObject(device_context, GetStockObject(NULL_BRUSH));
    RoundRect(device_context, rectBadge.left, rectBadge.top, rectBadge.right, rectBadge.bottom, 8, 8);
    TextOutA(device_context, rectBadge.left + 8, rectBadge.top + ((badge_h - 14)/2), avx512_label, crt_str_len(avx512_label));
    SelectObject(device_context, hOldBrush);
    SelectObject(device_context, hOldPen);
    DeleteObject(hStatusBrush);
    DeleteObject(hBorderPen);
    #endif

    /* --- Render Keyboard Shortcuts Help --- */
    if (g_is_help_visible) {
        help_x = 8; 
        help_y = 28; 
        line_spacing = 18;

        /* Step 1: Draw Drop Shadows (Black) */
        SetTextColor(device_context, RGB(0,0,0));
        for (i = 0; i < 7; ++i)
            TextOutA(device_context, help_x+1, help_y + (i*line_spacing)+1, help_lines[i], crt_str_len(help_lines[i]));
        if (g_is_ocl_supported)
            TextOutA(device_context, help_x+1, help_y + (7*line_spacing)+1, gpu_toggle_help, crt_str_len(gpu_toggle_help));

        /* Step 2: Draw Foreground Text (White) */
        SetTextColor(device_context, RGB(255,255,255));
        for (i = 0; i < 7; ++i)
            TextOutA(device_context, help_x, help_y + (i*line_spacing), help_lines[i], crt_str_len(help_lines[i]));
        if (g_is_ocl_supported)
            TextOutA(device_context, help_x, help_y + (7*line_spacing), gpu_toggle_help, crt_str_len(gpu_toggle_help));
    }

    /* --- Final State Restore --- */
    SelectObject(device_context, hOldFont);
}

/* -------------------- PixelBuffer Management -------------------- */
static void win32_pixel_buffer_heapalloc(int target_width, int target_height) {
    /* Release previous allocation from the process heap */
    if (g_pixel_buffer) {
        HeapFree(g_process_heap, 0, g_pixel_buffer);
        g_pixel_buffer = NULL;
    }

    /* Clamp dimensions to ensure at least 1x1 allocation.
       Update global dimensions */
    g_window_width  = (target_width  > 0) ? target_width  : 1;
    g_window_height = (target_height > 0) ? target_height : 1;

    /* Allocate zeroed memory for the new resolution. 
       We use 4 bytes (32-bit) per pixel for XRGB format. */
    g_pixel_buffer = (uint32_t*)HeapAlloc(
        g_process_heap, 
        HEAP_ZERO_MEMORY, 
        (SIZE_T)g_window_width * (SIZE_T)g_window_height * sizeof(uint32_t)
    );
}

/**
 * Presents the internal backbuffer to the window and renders the UI overlay.
 * Standard Win32 GDI implementation for a high-performance bit-blitting.
 */
static void win32_pixel_buffer_blit(HWND main_window_handle) {
    HDC device_context;
    BITMAPINFO bitmap_metadata;

    /* --- Validation --- */
    /* Ensure the backbuffer exists and has valid dimensions before blitting */
    if (!g_pixel_buffer || g_window_width <= 0 || g_window_height <= 0) {
        return;
    }

    device_context = GetDC(main_window_handle);
    if (!device_context) {
        return;
    }

    /* --- Metadata Setup --- */
    /* Initialize the DIB (Device-Independent Bitmap) header */
    memset(&bitmap_metadata, 0, sizeof(bitmap_metadata));
    bitmap_metadata.bmiHeader.biSize        = sizeof(bitmap_metadata.bmiHeader);
    bitmap_metadata.bmiHeader.biWidth       = g_window_width;
    
    /* Using a negative height creates a "Top-Down" bitmap where (0,0) is top-left.
       Standard Windows BMPs are usually "Bottom-Up" (positive height). */
    bitmap_metadata.bmiHeader.biHeight      = -g_window_height; 
    
    bitmap_metadata.bmiHeader.biPlanes      = 1;
    bitmap_metadata.bmiHeader.biBitCount    = 32; /* 8 bits per channel (ARGB/XRGB) */
    bitmap_metadata.bmiHeader.biCompression = BI_RGB;

    /* --- Blit Operation --- */
    /* StretchDIBits transfers our raw pixel array directly to the window's DC.
       We use a 1:1 scale (source and dest rects are identical). */
    StretchDIBits(
        device_context,
        0, 0, g_window_width, g_window_height, /* Destination coordinates */
        0, 0, g_window_width, g_window_height, /* Source coordinates      */
        g_pixel_buffer, 
        &bitmap_metadata, 
        DIB_RGB_COLORS, 
        SRCCOPY
    );

    /* --- Post-Blit UI --- */
    /* Draw the HUD/Help text on top of the newly presented frame */
    win32_gdi_hud_render(device_context);
    
    /* GDI Cleanup: DCs are a limited system resource */
    ReleaseDC(main_window_handle, device_context);
}

static void win32_fullscreen_mode_toggle(HWND hwnd) {
    LONG_PTR style;
    HMONITOR hMon;
    MONITORINFO mi;
    
    mi.cbSize = sizeof(mi);
    style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    if (!g_is_fullscreen_mode) {
        GetWindowPlacement(hwnd, &g_prev_window_pos);
        hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(hMon, &mi)) return;
        SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                    mi.rcMonitor.right - mi.rcMonitor.left,
                    mi.rcMonitor.bottom - mi.rcMonitor.top,
                    SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_is_fullscreen_mode = TRUE;
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_prev_window_pos);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                    SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_is_fullscreen_mode = FALSE;
    }
}

static void win32_screenshot_save(const char* filename) {
    if (!g_pixel_buffer) return;
    HANDLE hFile = CreateFileA(filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    DWORD headersSize = (DWORD)(sizeof(bfh) + sizeof(bih));
    DWORD imageSize = (DWORD)((SIZE_T)g_window_width * (SIZE_T)g_window_height * 4);

    memset(&bfh, 0, sizeof(bfh));
    memset(&bih, 0, sizeof(bih));

    bfh.bfType = 0x4D42; /* "BM" */
    bfh.bfSize = headersSize + imageSize;
    bfh.bfOffBits = headersSize;

    bih.biSize = (DWORD)sizeof(bih);
    bih.biWidth = g_window_width;
    bih.biHeight = -g_window_height; /* top-left origin */
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imageSize;

    DWORD written;
    WriteFile(hFile, &bfh, sizeof(bfh), &written, NULL);
    WriteFile(hFile, &bih, sizeof(bih), &written, NULL);
    WriteFile(hFile, g_pixel_buffer, imageSize, &written, NULL);
    CloseHandle(hFile);
}

static void win32_screenshot_save_next(void) {
    static int shot_index = 1;
    char filename[64];
    char *p;
    const char *base = "screenshot";
    const char *ext = ".bmp";
    int i, idx;

    while (shot_index <= 999) {
        p = filename + sizeof(filename);
        *--p = '\0';
        
        for (i = 4; i >= 0; --i) *--p = ext[i];
        
        idx = shot_index;
        for (i = 0; i < 3; ++i) { 
            *--p = (char)('0' + (idx % 10)); 
            idx /= 10; 
        }
        
        for (i = 9; i >= 0; --i) *--p = base[i];

        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
            win32_screenshot_save(p);
            ++shot_index;
            return;
        }
        ++shot_index;
    }
}

#if 0
static DWORD WINAPI win32_thread_proc_render(LPVOID thread_param) {
    thread_context_t* ctx;

    uint32_t* ptr_pixel_buffer;
    uint32_t* ptr_pixel_row;

    int canvas_width;
    int canvas_height;
    int div_x, div_y;
    int px, py;
    int max_iterations;

    q1616_t x_step;
    q1616_t y_step;
    q1616_t x_base_re;
    q1616_t y_im;

    q1616_t inv_ln2;
    q1616_t view_left_re;
    q1616_t view_top_im;

    q1616_t seed_re;
    q1616_t seed_im;
    q1616_t escape_radius_sq;
    q1616_t ln_ln_bailout;

    const rgb_color_t* active_palette;

    inv_ln2 = Q1616_FROM_FLOAT(1.442695f); /* 1/ln(2) */

    ctx = (thread_context_t*)thread_param;
    if (!ctx) return 1;

    for (;;) {
        /* Wait for frame start */
        WaitForSingleObject(ctx->signal_start_event, INFINITE);
        if (ctx->is_exit_pending) break;

        canvas_width     = g_window_width;
        canvas_height    = g_window_height;
        ptr_pixel_buffer = g_pixel_buffer;

        /* Invalid buffer: still contribute to completion */
        if (!ptr_pixel_buffer || canvas_width <= 0 || canvas_height <= 0) {
            if (InterlockedIncrement(&g_rows_completed) == canvas_height)
                SetEvent(g_frame_complete_event);
            continue;
        }

        div_x = (canvas_width  - 1) > 0 ? (canvas_width  - 1) : 1;
        div_y = (canvas_height - 1) > 0 ? (canvas_height - 1) : 1;

        x_step = q1616_div(q1616_sub(ctx->view_x_max, ctx->view_x_min),Q1616_FROM_INT(div_x));
        y_step = q1616_div(q1616_sub(ctx->view_y_max, ctx->view_y_min),Q1616_FROM_INT(div_y));

        seed_re          = ctx->seed_cx;
        seed_im          = ctx->seed_cy;
        max_iterations   = ctx->max_iterations;
        escape_radius_sq = ctx->bailout;
        ln_ln_bailout    = ctx->ln_ln_bailout;

        view_left_re     = ctx->view_x_min;
        view_top_im      = ctx->view_y_min;

        active_palette   = g_fractal_palettes[g_active_palette];

        /* Work-stealing loop */
        while ((py = InterlockedIncrement(&g_next_row_index) - 1) < canvas_height) {
            y_im = q1616_add(view_top_im,q1616_mul(Q1616_FROM_INT(py), y_step));

            ptr_pixel_row = ptr_pixel_buffer + (SIZE_T)py * (SIZE_T)canvas_width;
            x_base_re = view_left_re;

            for (px = 0; px < canvas_width; ++px) {
                q1616_t z_re;
                q1616_t z_im;
                q1616_t z_re2;
                q1616_t z_im2;
                q1616_t smooth_val;

                int iter = 0;

                z_re = x_base_re;
                z_im = y_im;

                z_re2 = q1616_mul(z_re, z_re);
                z_im2 = q1616_mul(z_im, z_im);

                while (iter < max_iterations && q1616_add(z_re2, z_im2) <= escape_radius_sq) {
                    q1616_t zrzi = q1616_mul(z_re, z_im);

                    z_im = q1616_add(zrzi << 1, seed_im);
                    z_re = q1616_add(q1616_sub(z_re2, z_im2),seed_re);

                    z_re2 = q1616_mul(z_re, z_re);
                    z_im2 = q1616_mul(z_im, z_im);

                    ++iter;
                }

                /* Smooth iteration value */
                if (iter >= max_iterations) {
                    smooth_val = Q1616_FROM_INT(iter);
                } else {
                    q1616_t mag2 = q1616_add(z_re2, z_im2);

                    if (mag2 <= 0) {
                        smooth_val = Q1616_FROM_INT(iter);
                    } else {
                        q1616_t ln_abs = q1616_ln_fast(mag2) >> 1;
                        if (ln_abs <= 0) ln_abs = 1;

                        q1616_t ln_ln_abs = q1616_ln_fast(ln_abs);
                        q1616_t nu = q1616_mul(q1616_sub(ln_ln_abs, ln_ln_bailout),inv_ln2);
                        smooth_val = q1616_sub(Q1616_FROM_INT(iter + 1),nu);
                    }
                }

                /* Palette mapping */
                q1616_t norm_t;
                q1616_t lerp_pos;
                q1616_t fraction;
                int pal_idx;

                norm_t = q1616_div(smooth_val,Q1616_FROM_INT(max_iterations));

                if (norm_t < 0) norm_t = 0;
                if (norm_t > Q1616_ONE_VAL) norm_t = Q1616_ONE_VAL;

                lerp_pos = q1616_mul(norm_t,Q1616_FROM_INT(INTERPOLATION_SEGMENTS - 1));

                pal_idx = (int)(lerp_pos >> 16);
                fraction = lerp_pos & 0xFFFF;

                rgb_color_t c1 = active_palette[pal_idx];
                rgb_color_t c2 = active_palette[pal_idx + 1];

                uint8_t r = (uint8_t)(c1.red   + (((c2.red   - c1.red)   * fraction) >> 16));
                uint8_t g = (uint8_t)(c1.green + (((c2.green - c1.green) * fraction) >> 16));
                uint8_t b = (uint8_t)(c1.blue  + (((c2.blue  - c1.blue)  * fraction) >> 16));

                ptr_pixel_row[px] = (uint32_t)((r << 16) | (g << 8) | b);

                x_base_re = q1616_add(x_base_re, x_step);
            }
            if (InterlockedIncrement(&g_rows_completed) == canvas_height)
                SetEvent(g_frame_complete_event);
        }
    }
    return 0;
}

static DWORD WINAPI win32_thread_proc_render_avx2(LPVOID thread_param) {
    thread_context_t* ctx;

    uint32_t* ptr_pixel_buffer;
    uint32_t* ptr_pixel_row;

    int canvas_width;
    int canvas_height;
    int div_x, div_y;
    int px, py;
    
    q1616_t inv_max_iterations;

    q1616_t x_step;
    q1616_t y_step;
    q1616_t x_base_re;
    q1616_t y_im;

    __m256i seed_re_v;
    __m256i seed_im_v;
    __m256i escape_radius_sq_v;
    __m256i x_step_offsets_v;

    const rgb_color_t* active_palette;

    ctx = (thread_context_t*)thread_param;
    if (!ctx) return 1;

    for (;;) {
        /* Wait for frame start */
        WaitForSingleObject(ctx->signal_start_event, INFINITE);
        if (ctx->is_exit_pending) break;

        canvas_width     = g_window_width;
        canvas_height    = g_window_height;
        ptr_pixel_buffer = g_pixel_buffer;

        /* Invalid buffer: still contribute to completion */
        if (!ptr_pixel_buffer || canvas_width <= 0 || canvas_height <= 0) {
            if (InterlockedIncrement(&g_rows_completed) == canvas_height)
                SetEvent(g_frame_complete_event);
            continue;
        }

        div_x = (canvas_width  - 1) > 0 ? (canvas_width  - 1) : 1;
        div_y = (canvas_height - 1) > 0 ? (canvas_height - 1) : 1;

        x_step = q1616_div(q1616_sub(ctx->view_x_max, ctx->view_x_min),Q1616_FROM_INT(div_x));
        y_step = q1616_div(q1616_sub(ctx->view_y_max, ctx->view_y_min),Q1616_FROM_INT(div_y));

        seed_re_v = _mm256_set1_epi32(ctx->seed_cx);
        seed_im_v = _mm256_set1_epi32(ctx->seed_cy);

        escape_radius_sq_v = _mm256_set1_epi32(ctx->bailout);
        inv_max_iterations = q1616_div(Q1616_ONE_VAL, Q1616_FROM_INT(ctx->max_iterations));

        x_step_offsets_v = _mm256_setr_epi32(
            0,
            x_step,
            x_step * 2,
            x_step * 3,
            x_step * 4,
            x_step * 5,
            x_step * 6,
            x_step * 7
        );

        active_palette = g_fractal_palettes[g_active_palette];

        /* Work-stealing loop */
        for (;;) {
            py = InterlockedExchangeAdd(&g_next_row_index, 1);
            if (py >= canvas_height) break;

            ptr_pixel_row =  ptr_pixel_buffer + (SIZE_T)py * (SIZE_T)canvas_width;
            y_im = q1616_add(ctx->view_y_min, q1616_mul(Q1616_FROM_INT(py), y_step));

            x_base_re = ctx->view_x_min;
            px = 0;

            /* SIMD horizontal loop */
            for (; px <= canvas_width - 8; px += 8) {
                __m256i z_re_v;
                __m256i z_im_v;
                __m256i iter_v;
                __m256i active_mask_v;

                __m256i final_z_re_v;
                __m256i final_z_im_v;

                SYSTEM_ALIGN32_BEG int iter_lane[8]; SYSTEM_ALIGN32_END
                SYSTEM_ALIGN32_BEG int z_re_lane[8]; SYSTEM_ALIGN32_END
                SYSTEM_ALIGN32_BEG int z_im_lane[8]; SYSTEM_ALIGN32_END

                int i, lane;

                z_re_v        = _mm256_add_epi32(_mm256_set1_epi32(x_base_re), x_step_offsets_v);
                z_im_v        = _mm256_set1_epi32(y_im);
                iter_v        = _mm256_setzero_si256();
                active_mask_v = _mm256_set1_epi32(-1);

                final_z_re_v  = _mm256_setzero_si256();
                final_z_im_v  = _mm256_setzero_si256();

                for (i = 0; i < ctx->max_iterations; ++i) {
                    __m256i z_re2_v;
                    __m256i z_im2_v;
                    __m256i mag2_v;
                    __m256i escaped_v;
                    __m256i just_escaped_v;
                    __m256i zrzi_v;

                    z_re2_v = q1616_mul8(z_re_v, z_re_v);
                    z_im2_v = q1616_mul8(z_im_v, z_im_v);
                    mag2_v  = _mm256_add_epi32(z_re2_v, z_im2_v);

                    escaped_v      = _mm256_cmpgt_epi32(mag2_v, escape_radius_sq_v);
                    just_escaped_v = _mm256_and_si256(escaped_v, active_mask_v);

                    if (!_mm256_testz_si256(just_escaped_v, just_escaped_v)) {
                        final_z_re_v = _mm256_blendv_epi8(final_z_re_v,z_re_v,just_escaped_v);
                        final_z_im_v = _mm256_blendv_epi8(final_z_im_v,z_im_v,just_escaped_v);
                    }

                    active_mask_v = _mm256_andnot_si256(escaped_v, active_mask_v);

                    if (_mm256_testz_si256(active_mask_v, active_mask_v)) break;

                    /* iter += 1 for active lanes */
                    /* iter_v = _mm256_sub_epi32(iter_v, active_mask_v); */
                    iter_v = _mm256_add_epi32(iter_v, _mm256_and_si256(active_mask_v, _mm256_set1_epi32(1)));
                    zrzi_v = q1616_mul8(z_re_v, z_im_v);
                    z_im_v = _mm256_add_epi32(_mm256_slli_epi32(zrzi_v, 1),seed_im_v);
                    z_re_v = _mm256_add_epi32(_mm256_sub_epi32(z_re2_v, z_im2_v),seed_re_v);
                }

                final_z_re_v = _mm256_blendv_epi8(final_z_re_v,z_re_v,active_mask_v);
                final_z_im_v = _mm256_blendv_epi8(final_z_im_v,z_im_v,active_mask_v);

                _mm256_store_si256((__m256i*)iter_lane, iter_v);
                _mm256_store_si256((__m256i*)z_re_lane, final_z_re_v);
                _mm256_store_si256((__m256i*)z_im_lane, final_z_im_v);

                for (lane = 0; lane < 8; ++lane) {
                    q1616_t smooth_val;
                    q1616_t z_re_s;
                    q1616_t z_im_s;
                    int iter_s;

                    iter_s = iter_lane[lane];

                    if (iter_s >= ctx->max_iterations) {
                        smooth_val = Q1616_FROM_INT(iter_s);
                    } else {
                        q1616_t m2;

                        z_re_s = (q1616_t)z_re_lane[lane];
                        z_im_s = (q1616_t)z_im_lane[lane];

                        m2 = q1616_add(q1616_mul(z_re_s, z_re_s),q1616_mul(z_im_s, z_im_s));
                        if (m2 <= 0) m2 = Q1616_ONE_VAL;

                        q1616_t ln_m2  = q1616_ln_fast(m2);
                        q1616_t ln_abs = ln_m2 >> 1;
                        q1616_t nu = q1616_div(q1616_sub(q1616_ln_fast(ln_abs),ctx->ln_ln_bailout),
                                     Q1616_FROM_FLOAT(0.693147f));
                        
                        smooth_val = q1616_sub(Q1616_FROM_INT(iter_s + 1),nu);
                    }

                    /* Palette mapping */
                    q1616_t norm_t;
                    q1616_t lerp_pos;
                    q1616_t fraction;
                    int pal_idx;

                    norm_t = q1616_mul(smooth_val, inv_max_iterations);
                    if (norm_t < 0) norm_t = 0;
                    if (norm_t > Q1616_ONE_VAL) norm_t = Q1616_ONE_VAL;

                    lerp_pos = q1616_mul(norm_t,Q1616_FROM_INT(INTERPOLATION_SEGMENTS - 1));

                    pal_idx = (int)(lerp_pos >> 16);
                    fraction = lerp_pos & 0xFFFF;

                    rgb_color_t c1 = active_palette[pal_idx];
                    rgb_color_t c2 = active_palette[pal_idx + 1];

                    uint8_t r = (uint8_t)(c1.red   + (((c2.red   - c1.red)   * fraction) >> 16));
                    uint8_t g = (uint8_t)(c1.green + (((c2.green - c1.green) * fraction) >> 16));
                    uint8_t b = (uint8_t)(c1.blue  + (((c2.blue  - c1.blue)  * fraction) >> 16));

                    ptr_pixel_row[px + lane] = (uint32_t)((r << 16) | (g << 8) | b);
                }
                x_base_re += x_step * 8;
            }

            /* Scalar tail (bit-identical) */
            for (; px < canvas_width; ++px) {
                q1616_t z_re_s;
                q1616_t z_im_s;
                q1616_t smooth_val;
                int iter_s;

                z_re_s = x_base_re;
                z_im_s = y_im;
                iter_s = 0;

                for (; iter_s < ctx->max_iterations; ++iter_s) {
                    q1616_t z_re2 = q1616_mul(z_re_s, z_re_s);
                    q1616_t z_im2 = q1616_mul(z_im_s, z_im_s);

                    if (q1616_add(z_re2, z_im2) > ctx->bailout)
                        break;

                    q1616_t zrzi = q1616_mul(z_re_s, z_im_s);
                    z_im_s = q1616_add(zrzi << 1, ctx->seed_cy);
                    z_re_s = q1616_add(q1616_sub(z_re2, z_im2),ctx->seed_cx);
                }

                if (iter_s >= ctx->max_iterations) {
                    smooth_val = Q1616_FROM_INT(iter_s);
                } else {
                    q1616_t m2 = q1616_add(q1616_mul(z_re_s, z_re_s),q1616_mul(z_im_s, z_im_s));
                    if (m2 <= 0) m2 = Q1616_ONE_VAL;

                    q1616_t ln_m2  = q1616_ln_fast(m2);
                    q1616_t ln_abs = ln_m2 >> 1;
                    q1616_t nu = q1616_div(q1616_sub(q1616_ln_fast(ln_abs),ctx->ln_ln_bailout),
                                 Q1616_FROM_FLOAT(0.693147f));

                    smooth_val = q1616_sub(Q1616_FROM_INT(iter_s + 1),nu);
                }

                /* Palette mapping */
                q1616_t norm_t;
                q1616_t lerp_pos;
                q1616_t fraction;
                int pal_idx;

                norm_t = q1616_mul(smooth_val, inv_max_iterations);
                if (norm_t < 0) norm_t = 0;
                if (norm_t > Q1616_ONE_VAL) norm_t = Q1616_ONE_VAL;

                lerp_pos = q1616_mul(norm_t,Q1616_FROM_INT(INTERPOLATION_SEGMENTS - 1));
                    
                pal_idx = (int)(lerp_pos >> 16);
                fraction = lerp_pos & 0xFFFF;

                rgb_color_t c1 = active_palette[pal_idx];
                rgb_color_t c2 = active_palette[pal_idx + 1];

                uint8_t r = (uint8_t)(c1.red   + (((c2.red   - c1.red)   * fraction) >> 16));
                uint8_t g = (uint8_t)(c1.green + (((c2.green - c1.green) * fraction) >> 16));
                uint8_t b = (uint8_t)(c1.blue  + (((c2.blue  - c1.blue)  * fraction) >> 16));

                ptr_pixel_row[px] = (uint32_t)((r << 16) | (g << 8) | b);

                x_base_re += x_step;
            }
            if (InterlockedIncrement(&g_rows_completed) == canvas_height)
                SetEvent(g_frame_complete_event);
        }
    }
    return 0;
}
#endif

#if 1
static void render_row_scalar(thread_context_t* ctx, int py);
static void render_row_sse2  (thread_context_t* ctx, int py);
static void render_row_sse41 (thread_context_t* ctx, int py);
static void render_row_avx2  (thread_context_t* ctx, int py);
static void render_row_avx512(thread_context_t* ctx, int py);

static DWORD WINAPI win32_thread_proc_render_generalized(LPVOID thread_param) {
    thread_context_t* ctx = (thread_context_t*)thread_param;

    for (;;) {
        WaitForSingleObject(ctx->signal_start_event, INFINITE);
        if (ctx->is_exit_pending) break;

        /* work-stealing loop */
        for (;;) {
            int py = InterlockedExchangeAdd(&g_next_row_index, 1);
            if (py >= g_window_height) break;

            #if 1
            if (g_is_cpu_avx512f && g_render_cpu_mode == RENDER_CPU_AVX512) {
                render_row_avx512(ctx, py);
            } else if (g_is_cpu_avx2 && g_render_cpu_mode == RENDER_CPU_AVX2) {
                render_row_avx2(ctx, py);
            } else if (g_is_cpu_sse41 && g_render_cpu_mode == RENDER_CPU_SSE41) {
                render_row_sse41(ctx, py);
            } else if (g_is_cpu_sse2 && g_render_cpu_mode == RENDER_CPU_SSE2) {
                render_row_sse2(ctx, py);
            } else if (g_is_cpu_scalar && g_render_cpu_mode == RENDER_CPU_SCALAR) {
                render_row_scalar(ctx, py);
            }
            #endif

            if (InterlockedIncrement(&g_rows_completed) == g_window_height)
                SetEvent(g_frame_complete_event);
        }
    }
    return 0;
}

static void render_row_scalar(thread_context_t* ctx, int py) {
    uint32_t* ptr_pixel_row;
    int canvas_width;
    int canvas_height;
    int div_x, div_y;
    int px;

    q1616_t x_step;
    q1616_t y_step;
    q1616_t x_base_re;
    q1616_t y_im;
    q1616_t inv_max_iterations;

    const rgb_color_t* active_palette;

    canvas_width  = g_window_width;
    canvas_height = g_window_height;
    ptr_pixel_row = g_pixel_buffer + (SIZE_T)py * (SIZE_T)canvas_width;

    /* --- coordinate step setup --- */
    div_x = (canvas_width  - 1) > 0 ? (canvas_width  - 1) : 1;
    div_y = (canvas_height - 1) > 0 ? (canvas_height - 1) : 1;

    x_step = q1616_div(
        q1616_sub(ctx->view_x_max, ctx->view_x_min),
        Q1616_FROM_INT(div_x)
    );

    y_step = q1616_div(
        q1616_sub(ctx->view_y_max, ctx->view_y_min),
        Q1616_FROM_INT(div_y)
    );

    y_im = q1616_add(
        ctx->view_y_min,
        q1616_mul(Q1616_FROM_INT(py), y_step)
    );

    x_base_re = ctx->view_x_min;

    inv_max_iterations =
        q1616_div(Q1616_ONE_VAL, Q1616_FROM_INT(ctx->max_iterations));

    active_palette = g_fractal_palettes[g_active_palette];

    /* --- horizontal pixel loop --- */
    for (px = 0; px < canvas_width; ++px) {
        q1616_t z_re;
        q1616_t z_im;
        q1616_t smooth_val;
        int iter;

        z_re = x_base_re;
        z_im = y_im;

        /* --- iteration loop --- */
        for (iter = 0; iter < ctx->max_iterations; ++iter) {
            q1616_t z_re2 = q1616_mul(z_re, z_re);
            q1616_t z_im2 = q1616_mul(z_im, z_im);

            if (q1616_add(z_re2, z_im2) > ctx->bailout)
                break;

            q1616_t zrzi = q1616_mul(z_re, z_im);
            z_im = q1616_add(zrzi << 1, ctx->seed_cy);
            z_re = q1616_add(q1616_sub(z_re2, z_im2), ctx->seed_cx);
        }

        /* --- smooth iteration value (matches SIMD path) --- */
        if (iter >= ctx->max_iterations) {
            smooth_val = Q1616_FROM_INT(iter);
        } else {
            q1616_t m2 = q1616_add(
                q1616_mul(z_re, z_re),
                q1616_mul(z_im, z_im)
            );

            if (m2 <= 0)
                m2 = Q1616_ONE_VAL;

            q1616_t ln_m2  = q1616_ln_fast(m2);
            q1616_t ln_abs = ln_m2 >> 1;

            q1616_t nu =
                q1616_div(
                    q1616_sub(q1616_ln_fast(ln_abs), ctx->ln_ln_bailout),
                    Q1616_FROM_FLOAT(0.693147f) /* ln(2) */
                );

            smooth_val =
                q1616_sub(Q1616_FROM_INT(iter + 1), nu);
        }

        /* --- palette mapping --- */
        {
            q1616_t norm_t;
            q1616_t lerp_pos;
            q1616_t fraction;
            int pal_idx;

            norm_t = q1616_mul(smooth_val, inv_max_iterations);
            if (norm_t < 0) norm_t = 0;
            if (norm_t > Q1616_ONE_VAL) norm_t = Q1616_ONE_VAL;

            lerp_pos =
                q1616_mul(
                    norm_t,
                    Q1616_FROM_INT(INTERPOLATION_SEGMENTS - 1)
                );

            pal_idx  = (int)(lerp_pos >> 16);
            fraction = lerp_pos & 0xFFFF;

            rgb_color_t c1 = active_palette[pal_idx];
            rgb_color_t c2 = active_palette[pal_idx + 1];

            uint8_t r =
                (uint8_t)(c1.red +
                (((c2.red - c1.red) * fraction) >> 16));

            uint8_t g =
                (uint8_t)(c1.green +
                (((c2.green - c1.green) * fraction) >> 16));

            uint8_t b =
                (uint8_t)(c1.blue +
                (((c2.blue - c1.blue) * fraction) >> 16));

            ptr_pixel_row[px] =
                (uint32_t)((r << 16) | (g << 8) | b);
        }

        x_base_re = q1616_add(x_base_re, x_step);
    }
}

static void render_row_sse2(thread_context_t* ctx, int py) {
    uint32_t* ptr_pixel_row;
    int canvas_width;
    int canvas_height;
    int div_x, div_y;
    int px;

    q1616_t x_step;
    q1616_t y_step;
    q1616_t x_base_re;
    q1616_t y_im;
    q1616_t inv_max_iterations;

    __m128i seed_re_v;
    __m128i seed_im_v;
    __m128i escape_radius_sq_v;
    __m128i x_step_offsets_v;

    const rgb_color_t* active_palette;

    canvas_width  = g_window_width;
    canvas_height = g_window_height;
    ptr_pixel_row = g_pixel_buffer + (SIZE_T)py * (SIZE_T)canvas_width;

    /* --- coordinate step setup --- */
    div_x = (canvas_width  - 1) > 0 ? (canvas_width  - 1) : 1;
    div_y = (canvas_height - 1) > 0 ? (canvas_height - 1) : 1;

    x_step = q1616_div(
        q1616_sub(ctx->view_x_max, ctx->view_x_min),
        Q1616_FROM_INT(div_x)
    );

    y_step = q1616_div(
        q1616_sub(ctx->view_y_max, ctx->view_y_min),
        Q1616_FROM_INT(div_y)
    );

    y_im = q1616_add(
        ctx->view_y_min,
        q1616_mul(Q1616_FROM_INT(py), y_step)
    );

    x_base_re = ctx->view_x_min;

    inv_max_iterations =
        q1616_div(Q1616_ONE_VAL, Q1616_FROM_INT(ctx->max_iterations));

    seed_re_v          = _mm_set1_epi32(ctx->seed_cx);
    seed_im_v          = _mm_set1_epi32(ctx->seed_cy);
    escape_radius_sq_v = _mm_set1_epi32(ctx->bailout);

    x_step_offsets_v = _mm_setr_epi32(
        0,
        x_step,
        x_step * 2,
        x_step * 3
    );

    active_palette = g_fractal_palettes[g_active_palette];

    /* --- SIMD horizontal loop (4 pixels) --- */
    for (px = 0; px <= canvas_width - 4; px += 4) {
        __m128i z_re_v;
        __m128i z_im_v;
        __m128i iter_v;
        __m128i active_mask_v;
        __m128i final_z_re_v;
        __m128i final_z_im_v;

        SYSTEM_ALIGN16_BEG int iter_lane[4]; SYSTEM_ALIGN16_END
        SYSTEM_ALIGN16_BEG int z_re_lane[4]; SYSTEM_ALIGN16_END
        SYSTEM_ALIGN16_BEG int z_im_lane[4]; SYSTEM_ALIGN16_END

        int i, lane;

        z_re_v = _mm_add_epi32(
                    _mm_set1_epi32(x_base_re),
                    x_step_offsets_v
                 );
        z_im_v = _mm_set1_epi32(y_im);

        iter_v        = _mm_setzero_si128();
        active_mask_v = _mm_set1_epi32(-1);
        final_z_re_v  = _mm_setzero_si128();
        final_z_im_v  = _mm_setzero_si128();

        /* --- iteration loop --- */
        for (i = 0; i < ctx->max_iterations; ++i) {
            __m128i z_re2_v;
            __m128i z_im2_v;
            __m128i mag2_v;
            __m128i escaped_v;
            __m128i just_escaped_v;
            __m128i zrzi_v;

            z_re2_v = q1616_mul4(z_re_v, z_re_v);
            z_im2_v = q1616_mul4(z_im_v, z_im_v);
            mag2_v  = _mm_add_epi32(z_re2_v, z_im2_v);

            escaped_v      = _mm_cmpgt_epi32(mag2_v, escape_radius_sq_v);
            just_escaped_v = _mm_and_si128(escaped_v, active_mask_v);

            /* capture z exactly once */
            final_z_re_v = _mm_or_si128(
                final_z_re_v,
                _mm_and_si128(just_escaped_v, z_re_v)
            );
            final_z_im_v = _mm_or_si128(
                final_z_im_v,
                _mm_and_si128(just_escaped_v, z_im_v)
            );

            active_mask_v = _mm_andnot_si128(escaped_v, active_mask_v);

            if (_mm_movemask_epi8(active_mask_v) == 0)
                break;

            /* iter += 1 for active lanes */
            iter_v = _mm_sub_epi32(iter_v, active_mask_v);

            zrzi_v = q1616_mul4(z_re_v, z_im_v);
            z_im_v = _mm_add_epi32(_mm_slli_epi32(zrzi_v, 1), seed_im_v);
            z_re_v = _mm_add_epi32(_mm_sub_epi32(z_re2_v, z_im2_v), seed_re_v);
        }

        /* lanes that never escaped */
        final_z_re_v = _mm_or_si128(
            final_z_re_v,
            _mm_and_si128(active_mask_v, z_re_v)
        );
        final_z_im_v = _mm_or_si128(
            final_z_im_v,
            _mm_and_si128(active_mask_v, z_im_v)
        );

        _mm_store_si128((__m128i*)iter_lane, iter_v);
        _mm_store_si128((__m128i*)z_re_lane, final_z_re_v);
        _mm_store_si128((__m128i*)z_im_lane, final_z_im_v);

        /* --- scalar post-pass (smoothing + palette) --- */
        for (lane = 0; lane < 4; ++lane) {
            q1616_t smooth_val;
            q1616_t z_re_s;
            q1616_t z_im_s;
            int iter_s;

            iter_s = iter_lane[lane];

            if (iter_s >= ctx->max_iterations) {
                smooth_val = Q1616_FROM_INT(iter_s);
            } else {
                q1616_t m2;

                z_re_s = (q1616_t)z_re_lane[lane];
                z_im_s = (q1616_t)z_im_lane[lane];

                m2 = q1616_add(
                        q1616_mul(z_re_s, z_re_s),
                        q1616_mul(z_im_s, z_im_s)
                     );
                if (m2 <= 0)
                    m2 = Q1616_ONE_VAL;

                q1616_t ln_m2  = q1616_ln_fast(m2);
                q1616_t ln_abs = ln_m2 >> 1;
                q1616_t nu =
                    q1616_div(
                        q1616_sub(
                            q1616_ln_fast(ln_abs),
                            ctx->ln_ln_bailout
                        ),
                        Q1616_FROM_FLOAT(0.693147f)
                    );

                smooth_val =
                    q1616_sub(Q1616_FROM_INT(iter_s + 1), nu);
            }

            /* palette mapping */
            {
                q1616_t norm_t;
                q1616_t lerp_pos;
                q1616_t fraction;
                int pal_idx;

                norm_t = q1616_mul(smooth_val, inv_max_iterations);
                if (norm_t < 0) norm_t = 0;
                if (norm_t > Q1616_ONE_VAL) norm_t = Q1616_ONE_VAL;

                lerp_pos =
                    q1616_mul(
                        norm_t,
                        Q1616_FROM_INT(INTERPOLATION_SEGMENTS - 1)
                    );

                pal_idx  = (int)(lerp_pos >> 16);
                fraction = lerp_pos & 0xFFFF;

                rgb_color_t c1 = active_palette[pal_idx];
                rgb_color_t c2 = active_palette[pal_idx + 1];

                uint8_t r =
                    (uint8_t)(c1.red +
                    (((c2.red - c1.red) * fraction) >> 16));
                uint8_t g =
                    (uint8_t)(c1.green +
                    (((c2.green - c1.green) * fraction) >> 16));
                uint8_t b =
                    (uint8_t)(c1.blue +
                    (((c2.blue - c1.blue) * fraction) >> 16));

                ptr_pixel_row[px + lane] =
                    (uint32_t)((r << 16) | (g << 8) | b);
            }
        }
        x_base_re += x_step * 4;
    }

    /* --- scalar tail --- */
    for (; px < canvas_width; ++px) {
        render_row_scalar(ctx, py);
        break;
    }
}


static void render_row_sse41(thread_context_t* ctx, int py) {
    uint32_t* ptr_pixel_row;
    int canvas_width;
    int canvas_height;
    int div_x, div_y;
    int px;

    q1616_t x_step;
    q1616_t y_step;
    q1616_t x_base_re;
    q1616_t y_im;
    q1616_t inv_max_iterations;

    __m128i seed_re_v;
    __m128i seed_im_v;
    __m128i escape_radius_sq_v;
    __m128i x_step_offsets_v;

    const rgb_color_t* active_palette;

    canvas_width  = g_window_width;
    canvas_height = g_window_height;
    ptr_pixel_row = g_pixel_buffer + (SIZE_T)py * (SIZE_T)canvas_width;

    /* --- coordinate step setup --- */
    div_x = (canvas_width  - 1) > 0 ? (canvas_width  - 1) : 1;
    div_y = (canvas_height - 1) > 0 ? (canvas_height - 1) : 1;

    x_step = q1616_div(
        q1616_sub(ctx->view_x_max, ctx->view_x_min),
        Q1616_FROM_INT(div_x)
    );

    y_step = q1616_div(
        q1616_sub(ctx->view_y_max, ctx->view_y_min),
        Q1616_FROM_INT(div_y)
    );

    y_im = q1616_add(
        ctx->view_y_min,
        q1616_mul(Q1616_FROM_INT(py), y_step)
    );

    x_base_re = ctx->view_x_min;
    inv_max_iterations = q1616_div(Q1616_ONE_VAL, Q1616_FROM_INT(ctx->max_iterations));

    /* --- broadcast constants into SIMD lanes --- */
    seed_re_v          = _mm_set1_epi32(ctx->seed_cx);
    seed_im_v          = _mm_set1_epi32(ctx->seed_cy);
    escape_radius_sq_v = _mm_set1_epi32(ctx->bailout);

    x_step_offsets_v = _mm_setr_epi32(
        0,
        x_step,
        x_step * 2,
        x_step * 3
    );

    active_palette = g_fractal_palettes[g_active_palette];

    /* --- SIMD horizontal loop (4 pixels) --- */
    for (px = 0; px <= canvas_width - 4; px += 4) {
        __m128i z_re_v;
        __m128i z_im_v;
        __m128i iter_v;
        __m128i active_mask_v;
        __m128i final_z_re_v;
        __m128i final_z_im_v;

        SYSTEM_ALIGN16_BEG int iter_lane[4]; SYSTEM_ALIGN16_END
        SYSTEM_ALIGN16_BEG int z_re_lane[4]; SYSTEM_ALIGN16_END
        SYSTEM_ALIGN16_BEG int z_im_lane[4]; SYSTEM_ALIGN16_END

        int i, lane;

        z_re_v = _mm_add_epi32(_mm_set1_epi32(x_base_re), x_step_offsets_v);
        z_im_v = _mm_set1_epi32(y_im);

        iter_v        = _mm_setzero_si128();
        active_mask_v = _mm_set1_epi32(-1);
        final_z_re_v  = _mm_setzero_si128();
        final_z_im_v  = _mm_setzero_si128();

        /* --- iteration loop --- */
        for (i = 0; i < ctx->max_iterations; ++i) {
            __m128i z_re2_v = q1616_mul4(z_re_v, z_re_v);
            __m128i z_im2_v = q1616_mul4(z_im_v, z_im_v);
            __m128i mag2_v  = _mm_add_epi32(z_re2_v, z_im2_v);

            /* SSE4.1 _mm_cmpgt_epi32 + _mm_testz_si128 for active lanes */
            __m128i escaped_v      = _mm_cmpgt_epi32(mag2_v, escape_radius_sq_v);
            __m128i just_escaped_v = _mm_and_si128(escaped_v, active_mask_v);

            final_z_re_v = _mm_or_si128(final_z_re_v, _mm_and_si128(just_escaped_v, z_re_v));
            final_z_im_v = _mm_or_si128(final_z_im_v, _mm_and_si128(just_escaped_v, z_im_v));

            active_mask_v = _mm_andnot_si128(escaped_v, active_mask_v);

            if (_mm_testz_si128(active_mask_v, active_mask_v)) break; /* all escaped */

            /* increment iterations for active lanes */
            iter_v = _mm_sub_epi32(iter_v, active_mask_v);

            __m128i zrzi_v = q1616_mul4(z_re_v, z_im_v);
            z_im_v = _mm_add_epi32(_mm_slli_epi32(zrzi_v, 1), seed_im_v);
            z_re_v = _mm_add_epi32(_mm_sub_epi32(z_re2_v, z_im2_v), seed_re_v);
        }

        /* lanes that never escaped */
        final_z_re_v = _mm_or_si128(final_z_re_v, _mm_and_si128(active_mask_v, z_re_v));
        final_z_im_v = _mm_or_si128(final_z_im_v, _mm_and_si128(active_mask_v, z_im_v));

        _mm_store_si128((__m128i*)iter_lane, iter_v);
        _mm_store_si128((__m128i*)z_re_lane, final_z_re_v);
        _mm_store_si128((__m128i*)z_im_lane, final_z_im_v);

        /* --- scalar post-pass (smoothing + palette) --- */
        for (lane = 0; lane < 4; ++lane) {
            q1616_t smooth_val;
            q1616_t z_re_s = (q1616_t)z_re_lane[lane];
            q1616_t z_im_s = (q1616_t)z_im_lane[lane];
            int iter_s = iter_lane[lane];

            if (iter_s >= ctx->max_iterations) {
                smooth_val = Q1616_FROM_INT(iter_s);
            } else {
                q1616_t m2 = q1616_add(q1616_mul(z_re_s, z_re_s), q1616_mul(z_im_s, z_im_s));
                if (m2 <= 0) m2 = Q1616_ONE_VAL;

                q1616_t ln_m2  = q1616_ln_fast(m2);
                q1616_t ln_abs = ln_m2 >> 1;
                q1616_t nu = q1616_div(q1616_sub(q1616_ln_fast(ln_abs), ctx->ln_ln_bailout), Q1616_FROM_FLOAT(0.693147f));
                smooth_val = q1616_sub(Q1616_FROM_INT(iter_s + 1), nu);
            }

            /* palette mapping */
            q1616_t norm_t = q1616_mul(smooth_val, inv_max_iterations);
            if (norm_t < 0) norm_t = 0;
            if (norm_t > Q1616_ONE_VAL) norm_t = Q1616_ONE_VAL;

            q1616_t lerp_pos = q1616_mul(norm_t, Q1616_FROM_INT(INTERPOLATION_SEGMENTS - 1));
            int pal_idx  = (int)(lerp_pos >> 16);
            q1616_t fraction = lerp_pos & 0xFFFF;

            rgb_color_t c1 = active_palette[pal_idx];
            rgb_color_t c2 = active_palette[pal_idx + 1];

            uint8_t r = (uint8_t)(c1.red   + (((c2.red   - c1.red)   * fraction) >> 16));
            uint8_t g = (uint8_t)(c1.green + (((c2.green - c1.green) * fraction) >> 16));
            uint8_t b = (uint8_t)(c1.blue  + (((c2.blue  - c1.blue)  * fraction) >> 16));

            ptr_pixel_row[px + lane] = (uint32_t)((r << 16) | (g << 8) | b);
        }
        x_base_re += x_step * 4;
    }

    /* --- scalar tail for remaining pixels --- */
    for (; px < canvas_width; ++px) {
        render_row_scalar(ctx, py);
    }
}

static void render_row_avx2(thread_context_t* ctx, int py) {
    uint32_t* ptr_pixel_row;
    int canvas_width;
    int canvas_height;
    int div_x, div_y;
    int px;

    q1616_t x_step;
    q1616_t y_step;
    q1616_t x_base_re;
    q1616_t y_im;
    q1616_t inv_max_iterations;

    __m256i seed_re_v;
    __m256i seed_im_v;
    __m256i escape_radius_sq_v;
    __m256i x_step_offsets_v;

    const rgb_color_t* active_palette;

    canvas_width   = g_window_width;
    canvas_height  = g_window_height;
    ptr_pixel_row  = g_pixel_buffer + (SIZE_T)py * (SIZE_T)canvas_width;

    /* --- coordinate step setup --- */
    div_x = (canvas_width  - 1) > 0 ? (canvas_width  - 1) : 1;
    div_y = (canvas_height - 1) > 0 ? (canvas_height - 1) : 1;

    x_step = q1616_div(
        q1616_sub(ctx->view_x_max, ctx->view_x_min),
        Q1616_FROM_INT(div_x)
    );

    y_step = q1616_div(
        q1616_sub(ctx->view_y_max, ctx->view_y_min),
        Q1616_FROM_INT(div_y)
    );

    y_im = q1616_add(
        ctx->view_y_min,
        q1616_mul(Q1616_FROM_INT(py), y_step)
    );

    x_base_re = ctx->view_x_min;

    inv_max_iterations =
        q1616_div(Q1616_ONE_VAL, Q1616_FROM_INT(ctx->max_iterations));

    seed_re_v          = _mm256_set1_epi32(ctx->seed_cx);
    seed_im_v          = _mm256_set1_epi32(ctx->seed_cy);
    escape_radius_sq_v = _mm256_set1_epi32(ctx->bailout);

    x_step_offsets_v = _mm256_setr_epi32(
        0,
        x_step,
        x_step * 2,
        x_step * 3,
        x_step * 4,
        x_step * 5,
        x_step * 6,
        x_step * 7
    );

    active_palette = g_fractal_palettes[g_active_palette];

    /* --- SIMD horizontal loop --- */
    for (px = 0; px <= canvas_width - 8; px += 8) {
        __m256i z_re_v;
        __m256i z_im_v;
        __m256i iter_v;
        __m256i active_mask_v;
        __m256i final_z_re_v;
        __m256i final_z_im_v;

        SYSTEM_ALIGN32_BEG int iter_lane[8]; SYSTEM_ALIGN32_END
        SYSTEM_ALIGN32_BEG int z_re_lane[8]; SYSTEM_ALIGN32_END
        SYSTEM_ALIGN32_BEG int z_im_lane[8]; SYSTEM_ALIGN32_END

        int i, lane;

        z_re_v        = _mm256_add_epi32(
                            _mm256_set1_epi32(x_base_re),
                            x_step_offsets_v
                        );
        z_im_v        = _mm256_set1_epi32(y_im);
        
        #if 1
        /* --- AVX2 ITERATION LOOP --- */
        iter_v        = _mm256_setzero_si256();
        active_mask_v = _mm256_set1_epi32(-1);

        final_z_re_v  = _mm256_setzero_si256();
        final_z_im_v  = _mm256_setzero_si256();

        for (i = 0; i < ctx->max_iterations; ++i) {
            __m256i z_re2_v;
            __m256i z_im2_v;
            __m256i mag2_v;
            __m256i escaped_v;
            __m256i just_escaped_v;
            __m256i zrzi_v;

            /* z^2 */
            z_re2_v = q1616_mul8(z_re_v, z_re_v);
            z_im2_v = q1616_mul8(z_im_v, z_im_v);
            mag2_v  = _mm256_add_epi32(z_re2_v, z_im2_v);

            /* escape test */
            escaped_v      = _mm256_cmpgt_epi32(mag2_v, escape_radius_sq_v);
            just_escaped_v = _mm256_and_si256(escaped_v, active_mask_v);

            /* capture final z exactly once */
            if (!_mm256_testz_si256(just_escaped_v, just_escaped_v)) {
                final_z_re_v = _mm256_or_si256(
                    final_z_re_v,
                    _mm256_and_si256(just_escaped_v, z_re_v)
                );
                final_z_im_v = _mm256_or_si256(
                    final_z_im_v,
                    _mm256_and_si256(just_escaped_v, z_im_v)
                );
            }

            /* deactivate escaped lanes */
            active_mask_v = _mm256_andnot_si256(escaped_v, active_mask_v);

            /* stop if all lanes dead */
            if (_mm256_testz_si256(active_mask_v, active_mask_v))
                break;

            /* iter += 1 for active lanes (active = -1) */
            iter_v = _mm256_sub_epi32(iter_v, active_mask_v);

            /* z = z^2 + c */
            zrzi_v = q1616_mul8(z_re_v, z_im_v);
            z_im_v = _mm256_add_epi32(_mm256_slli_epi32(zrzi_v, 1), seed_im_v);
            z_re_v = _mm256_add_epi32(_mm256_sub_epi32(z_re2_v, z_im2_v), seed_re_v);
        }

        /* capture z for lanes that never escaped */
        final_z_re_v = _mm256_or_si256(
            final_z_re_v,
            _mm256_and_si256(active_mask_v, z_re_v)
        );
        final_z_im_v = _mm256_or_si256(
            final_z_im_v,
            _mm256_and_si256(active_mask_v, z_im_v)
        );

        /* store results */
        _mm256_store_si256((__m256i*)iter_lane, iter_v);
        _mm256_store_si256((__m256i*)z_re_lane, final_z_re_v);
        _mm256_store_si256((__m256i*)z_im_lane, final_z_im_v);
        #endif

        for (lane = 0; lane < 8; ++lane) {
            q1616_t smooth_val;
            q1616_t z_re_s;
            q1616_t z_im_s;
            int iter_s;

            iter_s = iter_lane[lane];

            if (iter_s >= ctx->max_iterations) {
                smooth_val = Q1616_FROM_INT(iter_s);
            } else {
                q1616_t m2;

                z_re_s = (q1616_t)z_re_lane[lane];
                z_im_s = (q1616_t)z_im_lane[lane];

                m2 = q1616_add(
                        q1616_mul(z_re_s, z_re_s),
                        q1616_mul(z_im_s, z_im_s)
                     );
                if (m2 <= 0)
                    m2 = Q1616_ONE_VAL;

                {
                    q1616_t ln_m2  = q1616_ln_fast(m2);
                    q1616_t ln_abs = ln_m2 >> 1;
                    q1616_t nu =
                        q1616_div(
                            q1616_sub(
                                q1616_ln_fast(ln_abs),
                                ctx->ln_ln_bailout
                            ),
                            Q1616_FROM_FLOAT(0.693147f)
                        );

                    smooth_val =
                        q1616_sub(Q1616_FROM_INT(iter_s + 1), nu);
                }
            }

            /* palette mapping */
            {
                q1616_t norm_t;
                q1616_t lerp_pos;
                q1616_t fraction;
                int pal_idx;

                norm_t = q1616_mul(smooth_val, inv_max_iterations);
                if (norm_t < 0) norm_t = 0;
                if (norm_t > Q1616_ONE_VAL) norm_t = Q1616_ONE_VAL;

                lerp_pos =
                    q1616_mul(
                        norm_t,
                        Q1616_FROM_INT(INTERPOLATION_SEGMENTS - 1)
                    );

                pal_idx  = (int)(lerp_pos >> 16);
                fraction = lerp_pos & 0xFFFF;

                rgb_color_t c1 = active_palette[pal_idx];
                rgb_color_t c2 = active_palette[pal_idx + 1];

                uint8_t r =
                    (uint8_t)(c1.red +
                    (((c2.red - c1.red) * fraction) >> 16));
                uint8_t g =
                    (uint8_t)(c1.green +
                    (((c2.green - c1.green) * fraction) >> 16));
                uint8_t b =
                    (uint8_t)(c1.blue +
                    (((c2.blue - c1.blue) * fraction) >> 16));

                ptr_pixel_row[px + lane] =
                    (uint32_t)((r << 16) | (g << 8) | b);
            }
        }
        x_base_re += x_step * 8;
    }

    /* --- scalar tail (bit-identical) --- */
    for (; px < canvas_width; ++px) {
        q1616_t z_re_s;
        q1616_t z_im_s;
        q1616_t smooth_val;
        int iter_s;

        z_re_s = x_base_re;
        z_im_s = y_im;

        for (iter_s = 0; iter_s < ctx->max_iterations; ++iter_s) {
            q1616_t z_re2 = q1616_mul(z_re_s, z_re_s);
            q1616_t z_im2 = q1616_mul(z_im_s, z_im_s);

            if (q1616_add(z_re2, z_im2) > ctx->bailout)
                break;

            {
                q1616_t zrzi = q1616_mul(z_re_s, z_im_s);
                z_im_s = q1616_add(zrzi << 1, ctx->seed_cy);
                z_re_s = q1616_add(
                            q1616_sub(z_re2, z_im2),
                            ctx->seed_cx
                         );
            }
        }

        if (iter_s >= ctx->max_iterations) {
            smooth_val = Q1616_FROM_INT(iter_s);
        } else {
            q1616_t m2 =
                q1616_add(
                    q1616_mul(z_re_s, z_re_s),
                    q1616_mul(z_im_s, z_im_s)
                );
            if (m2 <= 0)
                m2 = Q1616_ONE_VAL;

            {
                q1616_t ln_m2  = q1616_ln_fast(m2);
                q1616_t ln_abs = ln_m2 >> 1;
                q1616_t nu =
                    q1616_div(
                        q1616_sub(
                            q1616_ln_fast(ln_abs),
                            ctx->ln_ln_bailout
                        ),
                        Q1616_FROM_FLOAT(0.693147f)
                    );

                smooth_val =
                    q1616_sub(Q1616_FROM_INT(iter_s + 1), nu);
            }
        }

        /* palette mapping */
        {
            q1616_t norm_t;
            q1616_t lerp_pos;
            q1616_t fraction;
            int pal_idx;

            norm_t = q1616_mul(smooth_val, inv_max_iterations);
            if (norm_t < 0) norm_t = 0;
            if (norm_t > Q1616_ONE_VAL) norm_t = Q1616_ONE_VAL;

            lerp_pos =
                q1616_mul(
                    norm_t,
                    Q1616_FROM_INT(INTERPOLATION_SEGMENTS - 1)
                );

            pal_idx  = (int)(lerp_pos >> 16);
            fraction = lerp_pos & 0xFFFF;

            rgb_color_t c1 = active_palette[pal_idx];
            rgb_color_t c2 = active_palette[pal_idx + 1];

            uint8_t r =
                (uint8_t)(c1.red +
                (((c2.red - c1.red) * fraction) >> 16));
            uint8_t g =
                (uint8_t)(c1.green +
                (((c2.green - c1.green) * fraction) >> 16));
            uint8_t b =
                (uint8_t)(c1.blue +
                (((c2.blue - c1.blue) * fraction) >> 16));

            ptr_pixel_row[px] =
                (uint32_t)((r << 16) | (g << 8) | b);
        }
        x_base_re = q1616_add(x_base_re, x_step);
    }
}

        #if 0
        iter_v        = _mm256_setzero_si256();
        active_mask_v = _mm256_set1_epi32(-1);
        final_z_re_v  = _mm256_setzero_si256();
        final_z_im_v  = _mm256_setzero_si256();

        for (i = 0; i < ctx->max_iterations; ++i) {
            __m256i z_re2_v;
            __m256i z_im2_v;
            __m256i mag2_v;
            __m256i escaped_v;
            __m256i just_escaped_v;
            __m256i zrzi_v;

            z_re2_v = q1616_mul8(z_re_v, z_re_v);
            z_im2_v = q1616_mul8(z_im_v, z_im_v);
            mag2_v  = _mm256_add_epi32(z_re2_v, z_im2_v);

            escaped_v      = _mm256_cmpgt_epi32(mag2_v, escape_radius_sq_v);
            just_escaped_v = _mm256_and_si256(escaped_v, active_mask_v);

            if (!_mm256_testz_si256(just_escaped_v, just_escaped_v)) {
                final_z_re_v =
                    _mm256_blendv_epi8(final_z_re_v, z_re_v, just_escaped_v);
                final_z_im_v =
                    _mm256_blendv_epi8(final_z_im_v, z_im_v, just_escaped_v);
            }

            active_mask_v =
                _mm256_andnot_si256(escaped_v, active_mask_v);

            if (_mm256_testz_si256(active_mask_v, active_mask_v))
                break;

            /* iter += 1 for active lanes */
            iter_v = _mm256_sub_epi32(iter_v, active_mask_v);

            zrzi_v = q1616_mul8(z_re_v, z_im_v);
            z_im_v = _mm256_add_epi32(
                        _mm256_slli_epi32(zrzi_v, 1),
                        seed_im_v
                     );
            z_re_v = _mm256_add_epi32(
                        _mm256_sub_epi32(z_re2_v, z_im2_v),
                        seed_re_v
                     );
        }

        final_z_re_v = _mm256_blendv_epi8(final_z_re_v, z_re_v, active_mask_v);
        final_z_im_v = _mm256_blendv_epi8(final_z_im_v, z_im_v, active_mask_v);

        _mm256_store_si256((__m256i*)iter_lane, iter_v);
        _mm256_store_si256((__m256i*)z_re_lane, final_z_re_v);
        _mm256_store_si256((__m256i*)z_im_lane, final_z_im_v);
        #endif

static void render_row_avx512(thread_context_t* ctx, int py) {
    #ifdef AVX512_USE_FLAG
    uint32_t* ptr_pixel_row;
    int canvas_width;
    int canvas_height;
    int div_x, div_y;
    int px;

    q1616_t x_step;
    q1616_t y_step;
    q1616_t x_base_re;
    q1616_t y_im;
    q1616_t inv_max_iterations;

    __m512i seed_re_v;
    __m512i seed_im_v;
    __m512i escape_radius_sq_v;
    __m512i x_step_offsets_v;

    const rgb_color_t* active_palette;

    canvas_width   = g_window_width;
    canvas_height  = g_window_height;
    ptr_pixel_row  = g_pixel_buffer + (SIZE_T)py * (SIZE_T)canvas_width;

    /* --- coordinate step setup --- */
    div_x = (canvas_width  - 1) > 0 ? (canvas_width  - 1) : 1;
    div_y = (canvas_height - 1) > 0 ? (canvas_height - 1) : 1;

    x_step = q1616_div(
        q1616_sub(ctx->view_x_max, ctx->view_x_min),
        Q1616_FROM_INT(div_x)
    );

    y_step = q1616_div(
        q1616_sub(ctx->view_y_max, ctx->view_y_min),
        Q1616_FROM_INT(div_y)
    );

    y_im = q1616_add(
        ctx->view_y_min,
        q1616_mul(Q1616_FROM_INT(py), y_step)
    );

    x_base_re = ctx->view_x_min;

    inv_max_iterations =
        q1616_div(Q1616_ONE_VAL, Q1616_FROM_INT(ctx->max_iterations));

    seed_re_v          = _mm512_set1_epi32(ctx->seed_cx);
    seed_im_v          = _mm512_set1_epi32(ctx->seed_cy);
    escape_radius_sq_v = _mm512_set1_epi32(ctx->bailout);

    x_step_offsets_v = _mm512_setr_epi32(
        0,
        x_step,
        x_step * 2,
        x_step * 3,
        x_step * 4,
        x_step * 5,
        x_step * 6,
        x_step * 7,
        x_step * 8,
        x_step * 9,
        x_step * 10,
        x_step * 11,
        x_step * 12,
        x_step * 13,
        x_step * 14,
        x_step * 15
    );

    active_palette = g_fractal_palettes[g_active_palette];

    /* --- SIMD horizontal loop (16 pixels) --- */
    for (px = 0; px <= canvas_width - 16; px += 16) {
        __m512i z_re_v;
        __m512i z_im_v;
        __m512i iter_v;
        __mmask16 active_mask;
        __m512i final_z_re_v;
        __m512i final_z_im_v;

        SYSTEM_ALIGN64_BEG int iter_lane[16]; SYSTEM_ALIGN64_END
        SYSTEM_ALIGN64_BEG int z_re_lane[16]; SYSTEM_ALIGN64_END
        SYSTEM_ALIGN64_BEG int z_im_lane[16]; SYSTEM_ALIGN64_END

        int i, lane;

        z_re_v =
            _mm512_add_epi32(
                _mm512_set1_epi32(x_base_re),
                x_step_offsets_v
            );
        z_im_v = _mm512_set1_epi32(y_im);

        iter_v        = _mm512_setzero_epi32();
        active_mask   = 0xFFFF;

        final_z_re_v  = _mm512_setzero_epi32();
        final_z_im_v  = _mm512_setzero_epi32();

        for (i = 0; i < ctx->max_iterations; ++i) {
            __m512i z_re2_v;
            __m512i z_im2_v;
            __m512i mag2_v;
            __mmask16 escaped_mask;
            __mmask16 just_escaped;
            __m512i zrzi_v;

            z_re2_v = q1616_mul16(z_re_v, z_re_v);
            z_im2_v = q1616_mul16(z_im_v, z_im_v);
            mag2_v  = _mm512_add_epi32(z_re2_v, z_im2_v);

            escaped_mask =
                _mm512_cmpgt_epi32_mask(mag2_v, escape_radius_sq_v);

            just_escaped = escaped_mask & active_mask;

            if (just_escaped) {
                final_z_re_v =
                    _mm512_mask_mov_epi32(
                        final_z_re_v,
                        just_escaped,
                        z_re_v
                    );
                final_z_im_v =
                    _mm512_mask_mov_epi32(
                        final_z_im_v,
                        just_escaped,
                        z_im_v
                    );
            }

            active_mask &= ~escaped_mask;
            if (!active_mask)
                break;

            iter_v =
                _mm512_mask_add_epi32(
                    iter_v,
                    active_mask,
                    iter_v,
                    _mm512_set1_epi32(1)
                );

            zrzi_v = q1616_mul16(z_re_v, z_im_v);
            z_im_v =
                _mm512_add_epi32(
                    _mm512_slli_epi32(zrzi_v, 1),
                    seed_im_v
                );
            z_re_v =
                _mm512_add_epi32(
                    _mm512_sub_epi32(z_re2_v, z_im2_v),
                    seed_re_v
                );
        }

        /* capture lanes that never escaped */
        final_z_re_v =
            _mm512_mask_mov_epi32(
                final_z_re_v,
                active_mask,
                z_re_v
            );
        final_z_im_v =
            _mm512_mask_mov_epi32(
                final_z_im_v,
                active_mask,
                z_im_v
            );

        _mm512_store_epi32(iter_lane, iter_v);
        _mm512_store_epi32(z_re_lane, final_z_re_v);
        _mm512_store_epi32(z_im_lane, final_z_im_v);

        /* --- scalar finalize per lane --- */
        for (lane = 0; lane < 16; ++lane) {
            q1616_t smooth_val;
            q1616_t z_re_s;
            q1616_t z_im_s;
            int iter_s;

            iter_s = iter_lane[lane];

            if (iter_s >= ctx->max_iterations) {
                smooth_val = Q1616_FROM_INT(iter_s);
            } else {
                q1616_t m2;

                z_re_s = (q1616_t)z_re_lane[lane];
                z_im_s = (q1616_t)z_im_lane[lane];

                m2 = q1616_add(
                        q1616_mul(z_re_s, z_re_s),
                        q1616_mul(z_im_s, z_im_s)
                     );
                if (m2 <= 0)
                    m2 = Q1616_ONE_VAL;

                {
                    q1616_t ln_m2  = q1616_ln_fast(m2);
                    q1616_t ln_abs = ln_m2 >> 1;
                    q1616_t nu =
                        q1616_div(
                            q1616_sub(
                                q1616_ln_fast(ln_abs),
                                ctx->ln_ln_bailout
                            ),
                            Q1616_FROM_FLOAT(0.693147f)
                        );

                    smooth_val =
                        q1616_sub(Q1616_FROM_INT(iter_s + 1), nu);
                }
            }

            /* palette mapping */
            {
                q1616_t norm_t;
                q1616_t lerp_pos;
                q1616_t fraction;
                int pal_idx;

                norm_t = q1616_mul(smooth_val, inv_max_iterations);
                if (norm_t < 0) norm_t = 0;
                if (norm_t > Q1616_ONE_VAL) norm_t = Q1616_ONE_VAL;

                lerp_pos =
                    q1616_mul(
                        norm_t,
                        Q1616_FROM_INT(INTERPOLATION_SEGMENTS - 1)
                    );

                pal_idx  = (int)(lerp_pos >> 16);
                fraction = lerp_pos & 0xFFFF;

                rgb_color_t c1 = active_palette[pal_idx];
                rgb_color_t c2 = active_palette[pal_idx + 1];

                uint8_t r =
                    (uint8_t)(c1.red +
                    (((c2.red - c1.red) * fraction) >> 16));
                uint8_t g =
                    (uint8_t)(c1.green +
                    (((c2.green - c1.green) * fraction) >> 16));
                uint8_t b =
                    (uint8_t)(c1.blue +
                    (((c2.blue - c1.blue) * fraction) >> 16));

                ptr_pixel_row[px + lane] =
                    (uint32_t)((r << 16) | (g << 8) | b);
            }
        }

        x_base_re = q1616_add(x_base_re, x_step * 16);
    }

    /* --- scalar tail (unchanged, bit-identical) --- */
    for (; px < canvas_width; ++px)
        render_row_scalar(ctx, py);

    #endif /* AVX512_USE_FLAG */
}
#endif /* followup */

static void win32_thread_pool_init(int n_threads) {
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    int i;

    if (n_threads <= 0) n_threads = (int)sysinfo.dwNumberOfProcessors;
    g_active_thread_count = n_threads;

    g_aligned_stride = (sizeof(thread_context_t) + 63) & ~(size_t)63;
    g_thread_data_raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (g_aligned_stride * n_threads) + 64);
    g_thread_data = (thread_context_t*)(((UINT_PTR)g_thread_data_raw + 63) & ~(UINT_PTR)63);
    
    g_frame_complete_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    for (i = 0; i < n_threads; ++i) {
        thread_context_t* ctx = (thread_context_t*)((char*)g_thread_data + (i * g_aligned_stride));
        ctx->signal_start_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        #if 1
        g_thread_handles[i] = CreateThread(NULL, 0, win32_thread_proc_render_generalized, 
            ctx, 0, NULL);
        #endif
        #if 0
        g_thread_handles[i] = CreateThread(NULL, 0, 
            system_supports_avx2() ? win32_thread_proc_render_avx2 : win32_thread_proc_render, 
            ctx, 0, NULL);
        #endif
        #if 0
        g_thread_handles[i] = CreateThread(NULL, 0, 
            system_supports_avx2() ? win32_thread_proc_render : win32_thread_proc_render_avx2, 
            ctx, 0, NULL);
        #endif
    }
}

static void win32_thread_pool_shutdown(void) {
    int i;

    if (g_active_thread_count <= 0) return;

    /* 1. Signal all threads to wake up and exit */
    for (i = 0; i < g_active_thread_count; ++i) {
        thread_context_t* ctx = (thread_context_t*)((char*)g_thread_data + (i * g_aligned_stride));
        ctx->is_exit_pending = TRUE;
        SetEvent(ctx->signal_start_event);
    }

    /* 2. Block until all worker threads return */
    WaitForMultipleObjects((DWORD)g_active_thread_count, g_thread_handles, TRUE, INFINITE);

    /* 3. Cleanup Windows handles */
    for (i = 0; i < g_active_thread_count; ++i) {
        thread_context_t* ctx = (thread_context_t*)((char*)g_thread_data + (i * g_aligned_stride));
        if (g_thread_handles[i]) CloseHandle(g_thread_handles[i]);
        if (ctx->signal_start_event) CloseHandle(ctx->signal_start_event);
    }

    if (g_frame_complete_event) CloseHandle(g_frame_complete_event);

    /* 4. Free the HEAP memory (Use the RAW pointer) */
    if (g_thread_data_raw) {
        HeapFree(GetProcessHeap(), 0, g_thread_data_raw);
        g_thread_data_raw = NULL;
        g_thread_data = NULL;
    }

    g_active_thread_count = 0;
}

static LRESULT CALLBACK win32_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PAINTSTRUCT ps;
    int total_palettes;
    int new_w, new_h;
    BOOL was_paused;
    /* total_palettes   = (int)(sizeof(g_fractal_palettes) / sizeof(g_fractal_palettes[0])); */
    total_palettes = PAL_TOTAL_COUNT;

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_PAINT:
        BeginPaint(hwnd, &ps);
        win32_pixel_buffer_blit(hwnd);
        EndPaint(hwnd, &ps);
        return 0;

    case WM_KEYDOWN:
        switch (wParam) {
        case 'P': g_is_paused = !g_is_paused; break;
        case 'H': g_is_help_visible = !g_is_help_visible; break;
        case 'G': if (g_is_ocl_supported) g_is_ocl_accelerated = !g_is_ocl_accelerated; break;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {            
            g_active_palette = ((int)(wParam - '1')) % (total_palettes > 0 ? total_palettes : 1);
        } break;
        case VK_F5:  render_mode_cycle_next(); break;
        case VK_F11: win32_fullscreen_mode_toggle(hwnd); break;
        case VK_F12: win32_screenshot_save_next(); break;
        case VK_ESCAPE: if (g_is_fullscreen_mode) win32_fullscreen_mode_toggle(hwnd); break;
        }
        return 0;

    case WM_SIZE:
        new_w = (int)LOWORD(lParam);
        new_h = (int)HIWORD(lParam);

        /* Avoid processing if the window is minimized or dimensions are invalid */
        if (new_w <= 0 || new_h <= 0) {
            return 0;
        }

        /* --- Critical Section: Prevent Concurrent Access --- */
        was_paused = g_is_paused;
        g_is_paused = TRUE;

        /* If we are mid-frame, wait for workers to park before we free their memory */
        if (g_is_rendering && g_frame_complete_event) {
            WaitForSingleObject(g_frame_complete_event, INFINITE);
        }

        /* Reallocate the backbuffer */
        win32_pixel_buffer_heapalloc(new_w, new_h);

        /* --- Re-partition Workloads --- */
        /* REMOVED */

        /* Reset the atomic row dispenser to prevent threads from grabbing old indices */
        InterlockedExchange(&g_next_row_index, 0);
        InterlockedExchange(&g_rows_completed, 0);

        /* Force a redraw */
        InvalidateRect(hwnd, NULL, FALSE);
        g_is_paused = was_paused;
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}



__declspec(noreturn) void win32_main_entry(void) {
    HINSTANCE h_instance;
    WNDCLASSW wc;
    SYSTEM_INFO sysinfo;
    MSG msg;
    
    uint32_t *row;
    uint8_t r, g;
    int running, i;
    int y, x;

    /* Animation and Math parameters */
    q1616_t bailout, ln_bailout, ln_ln_bailout;
    q1616_t radius, ux, uy, cos_d, sin_d, x_min, x_max, y_min, y_max;
    q1616_t cx_fp, cy_fp, nx, ny;
    int max_iters;

    /* g_is_cpu_scalar  = TRUE; */
    g_is_cpu_sse2    = system_supports_sse2();
    g_is_cpu_sse41   = system_supports_sse41();
    g_is_cpu_avx2    = system_supports_avx2();
    g_is_cpu_avx512f = system_supports_avx512();

    /* 1. System and Buffer Initialization */
    if (sizeof(thread_context_t) % 64 != 0) {
        /* This will alert you if the struct size isn't a multiple of a cache line */
        OutputDebugStringA("Warning: thread_context_t is not cache-aligned.\n");
    }

    #ifdef G_PALETTE_LINEAR
    palette_precompute_linear();
    #endif

    g_process_heap = GetProcessHeap();
    win32_pixel_buffer_heapalloc(g_window_width, g_window_height);
    h_instance = GetModuleHandleW(NULL);

    /* 2. Window Registration */
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = win32_window_proc;
    wc.hInstance     = h_instance;
    wc.lpszClassName = L"inlined_julia_class";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_window_handle = CreateWindowExW(
        0, wc.lpszClassName, L"Julia (fixed, inlined)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, g_window_width, g_window_height,
        NULL, NULL, h_instance, NULL
    );

    ShowWindow(g_window_handle, SW_SHOW);
    UpdateWindow(g_window_handle);

    /* 3. Thread and GPU Setup */
    win32_telemetry_init();
    GetSystemInfo(&sysinfo);
    win32_thread_pool_init((int)sysinfo.dwNumberOfProcessors);

    g_is_ocl_supported   = ocl_try_init();
    g_is_ocl_accelerated = g_is_ocl_supported;

    /* 4. Precompute Constants */
    bailout           = Q1616_FROM_FLOAT(4.0f);
    ln_bailout        = q1616_ln_fast(bailout);
    ln_ln_bailout     = q1616_ln_fast(ln_bailout);
    radius            = Q1616_FROM_FLOAT(0.7885f);
    ux                = Q1616_FROM_FLOAT(1.0f);
    uy                = Q1616_FROM_FLOAT(0.0f);
    cos_d             = Q1616_FROM_FLOAT(0.9999500004f);
    sin_d             = Q1616_FROM_FLOAT(0.00999983333f);
    x_min             = Q1616_FROM_FLOAT(-1.6f);
    x_max             = Q1616_FROM_FLOAT(1.6f);
    y_min             = Q1616_FROM_FLOAT(-0.9f);
    y_max             = Q1616_FROM_FLOAT(0.9f);
    max_iters         = 200;

    /* 5. Splash/Debug Gradient */
    for (y = 0; y < g_window_height; ++y) {
        row = g_pixel_buffer + (size_t)y * (size_t)g_window_width;
        for (x = 0; x < g_window_width; ++x) {
            r = (uint8_t)((x * 255) / (g_window_width ? g_window_width : 1));
            g = (uint8_t)((y * 255) / (g_window_height ? g_window_height : 1));
            row[x] = (uint32_t)((0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | 128u);
        }
    }
    win32_pixel_buffer_blit(g_window_handle);
    Sleep(275);

    /* 6. Main Message & Render Loop */
    running = 1;
    while (running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!running) break;
        if (g_is_paused) {
            Sleep(10);
            continue;
        }

        /* Compute rotation for Julia set constant 'c' */
        cx_fp = q1616_mul(radius, ux);
        cy_fp = q1616_mul(radius, uy);

        if (g_is_ocl_supported && g_is_ocl_accelerated) {
            ocl_render(
                g_pixel_buffer, g_window_width, g_window_height,
                cx_fp, cy_fp, x_min, x_max, y_min, y_max,
                max_iters, ln_ln_bailout
            );
            win32_pixel_buffer_blit(g_window_handle);
            win32_telemetry_update(g_window_handle);
        } else {
            /* --- PRE-RENDER RESET --- */
            g_is_rendering = TRUE;
            ResetEvent(g_frame_complete_event);
            
            /* CRITICAL: Reset both the dispenser AND the completion counter */
            InterlockedExchange(&g_next_row_index, 0);
            InterlockedExchange(&g_rows_completed, 0);

            for (i = 0; i < g_active_thread_count; ++i) {
                thread_context_t* ctx = (thread_context_t*)((char*)g_thread_data + (i * g_aligned_stride));
                
                /* Update parameters */
                ctx->seed_cx          = cx_fp;
                ctx->seed_cy          = cy_fp;
                ctx->max_iterations   = max_iters;
                ctx->bailout          = bailout;
                ctx->view_x_min       = x_min;
                ctx->view_x_max       = x_max;
                ctx->view_y_min       = y_min;
                ctx->view_y_max       = y_max;

                /* Wake up the thread */
                SetEvent(ctx->signal_start_event);
            }

            /* Wait for the very last row to be reported finished */
            WaitForSingleObject(g_frame_complete_event, INFINITE);
            
            g_is_rendering = FALSE;
            win32_pixel_buffer_blit(g_window_handle);
            win32_telemetry_update(g_window_handle);
        }

        /* Update rotation vector for next frame */
        nx = q1616_sub(q1616_mul(cos_d, ux), q1616_mul(sin_d, uy));
        ny = q1616_add(q1616_mul(sin_d, ux), q1616_mul(cos_d, uy));
        ux = nx; 
        uy = ny;

        Sleep(0); /* Yield to system */
    }

    /* 7. Cleanup */
    win32_thread_pool_shutdown();
    ocl_shutdown();

    if (g_pixel_buffer) {
        HeapFree(g_process_heap, 0, g_pixel_buffer);
        g_pixel_buffer = NULL;
    }

    ExitProcess(0);
}
