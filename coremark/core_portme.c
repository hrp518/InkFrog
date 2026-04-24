/*
 * core_portme.c - XR872 CoreMark Port Implementation
 *
 * Uses XR872 SDK OS_GetTicks() for timing (1ms tick)
 * Uses standard malloc/free for memory
 */

#include "coremark.h"
#include "kernel/os/os.h"

/* Default number of contexts (must be 1 for single-threaded) */
ee_u32 default_num_contexts = 1;

/* Volatile seeds for SEED_VOLATILE method */
volatile ee_s32 seed1_volatile = 0;
volatile ee_s32 seed2_volatile = 0;
volatile ee_s32 seed3_volatile = 0;
volatile ee_s32 seed4_volatile = 0;   /* iterations: 0 = auto-detect */
volatile ee_s32 seed5_volatile = 0;   /* execs: 0 = all algorithms */

/* Timing variables */
static CORE_TICKS start_time_val;
static CORE_TICKS stop_time_val;

/* CPU frequency for time calculation - XR872 runs at 384MHz */
#define CPU_FREQ_HZ 384000000UL

/* Global to store CoreMark score for UI display */
volatile float g_coremark_score = 0.0f;
volatile ee_u32 g_coremark_iterations = 0;
volatile float g_coremark_time_secs = 0.0f;

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    if (p) {
        p->portable_id = 1;
    }
    printf("[CoreMark] Portable init done\r\n");
}

void portable_fini(core_portable *p)
{
    (void)p;
    printf("[CoreMark] Portable fini done\r\n");
}

void start_time(void)
{
    start_time_val = (CORE_TICKS)OS_GetTicks();
}

void stop_time(void)
{
    stop_time_val = (CORE_TICKS)OS_GetTicks();
}

CORE_TICKS get_time(void)
{
    return stop_time_val - start_time_val;
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    /* OS_GetTicks() returns milliseconds */
    float secs = (float)ticks / 1000.0f;
    g_coremark_time_secs = secs;  /* Store for UI display */
    #if HAS_FLOAT
    return secs;
    #else
    return (ee_u32)secs;
    #endif
}

void *portable_malloc(ee_size_t size)
{
    return malloc(size);
}

void portable_free(void *p)
{
    if (p) free(p);
}

/* Function to get CoreMark score for UI display */
float coremark_get_score(void)
{
    if (g_coremark_time_secs > 0 && g_coremark_iterations > 0) {
        return (float)g_coremark_iterations / g_coremark_time_secs;
    }
    return g_coremark_score;
}

/* Function to update CoreMark score from benchmark results */
void coremark_set_results(ee_u32 iterations, float time_secs)
{
    g_coremark_iterations = iterations;
    g_coremark_time_secs = time_secs;
    if (time_secs > 0) {
        g_coremark_score = (float)iterations / time_secs;
        printf("[CoreMark] Score updated: %.2f iterations/sec\r\n", g_coremark_score);
    }
}

