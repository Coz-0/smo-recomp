/* perf_machine.h - the machine's numbers for the performance overlay (perf_machine.c). -1: not given by the platform. */
#pragma once
#include <stddef.h>

typedef struct {
    float cpu, cpu_game, cpu_temp;           /* percent of all logical processors (the machine, this process), degrees C */
    float ram_game, ram_used, ram_total;     /* MB */
    float gpu, gpu_temp, gpu_w, gpu_mhz, vram_used, vram_total, gtt_used, gtt_total;      /* percent, C, W, MHz, MB */
} PerfMachine;
void perf_machine_sample(PerfMachine *m);   /* the CPU use is since the last call */
void perf_machine_cpu_name(char *out, size_t n);
