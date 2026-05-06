#ifndef OCL_STUB_H
#define OCL_STUB_H

/* --- OpenCL Dummy Stubs for Compilation --- */
/* Attempts to initialize OpenCL; returns FALSE to force CPU path */
static BOOL ocl_try_init(void) {
    return FALSE;
}

/* Dummy cleanup function */
static void ocl_shutdown(void) {
    /* Nothing to clean up in stub mode */
}

/* Dummy render call; in your main loop, this won't be called 
   because g_is_gpu_supported will be FALSE */
static void ocl_render(uint32_t* buffer, int width, int height, 
                                   q1616_t cx,    q1616_t cy, 
                                   q1616_t x_min, q1616_t x_max, 
                                   q1616_t y_min, q1616_t y_max, 
                                   int max_iters, q1616_t ln_ln_bailout) {
    (void)buffer; (void)width; (void)height;
    (void)cx; (void)cy; (void)x_min; (void)x_max; (void)y_min; (void)y_max; 
    (void)max_iters; (void)ln_ln_bailout;
}

#endif /* OCL_STUB_H */