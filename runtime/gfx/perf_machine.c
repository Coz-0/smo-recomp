/* perf_machine.c - the machine's numbers for the performance overlay (perf_overlay.c): CPU use of all processors and of
 * this process, memory of the process and of the machine, the CPU's name; on Linux also the CPU temperature (hwmon:
 * k10temp, zenpower, coretemp) and the GPU's use, temperature, power, clock and memory (amdgpu sysfs). A number the
 * platform does not give is -1. Read on the overlay's own thread, twice a second while the full overlay is shown. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>                         /* PROCESS_MEMORY_COUNTERS; K32GetProcessMemoryInfo is in kernel32 */
#else
#include "win32_posix.h"
#include <dirent.h>
#endif
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#include <cpuid.h>
#endif
#include "perf_machine.h"

static ULONG64 ft64(FILETIME f) { return ((ULONG64)f.dwHighDateTime << 32) | f.dwLowDateTime; }
#ifdef __linux__
static double read_num(const char *path) {  /* the first number in a sysfs file, -1 if none */
    FILE *f = fopen(path, "r");
    double v = -1;
    if (f) { if (fscanf(f, "%lf", &v) != 1) v = -1; fclose(f); }
    return v;
}
static char g_hw_cpu[300], g_hw_gpu[300], g_drm[300];       /* the CPU's hwmon folder, the GPU's hwmon folder, its drm device */
static void find_sensors(void) {
    DIR *d = opendir("/sys/class/hwmon");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char p[300], n[64] = "";
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(n, sizeof n, f)) n[0] = 0;
        fclose(f);
        n[strcspn(n, "\n")] = 0;
        if (!g_hw_cpu[0] && (!strcmp(n, "k10temp") || !strcmp(n, "zenpower") || !strcmp(n, "coretemp"))) snprintf(g_hw_cpu, sizeof g_hw_cpu, "/sys/class/hwmon/%s", e->d_name);
        if (!g_hw_gpu[0] && !strcmp(n, "amdgpu")) snprintf(g_hw_gpu, sizeof g_hw_gpu, "/sys/class/hwmon/%s", e->d_name);
    }
    if (d) closedir(d);
    for (int i = 0; i < 8 && !g_drm[0]; i++) {
        char p[300];
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/gpu_busy_percent", i);
        if (read_num(p) >= 0) snprintf(g_drm, sizeof g_drm, "/sys/class/drm/card%d/device", i);
    }
}
#endif
void perf_machine_sample(PerfMachine *m) {
    static ULONG64 idle0, kern0, user0, pk0, pu0;
    FILETIME i, k, u, c, e, pk, pu;
    m->cpu = m->cpu_game = -1;
    if (GetSystemTimes(&i, &k, &u) && GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) {
        ULONG64 total = (ft64(k) - kern0) + (ft64(u) - user0), idle = ft64(i) - idle0, game = (ft64(pk) - pk0) + (ft64(pu) - pu0);
        if (kern0 && total) { m->cpu = (float)(100.0 * (double)(total - idle) / (double)total); m->cpu_game = (float)(100.0 * (double)game / (double)total); }
        idle0 = ft64(i); kern0 = ft64(k); user0 = ft64(u); pk0 = ft64(pk); pu0 = ft64(pu);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    m->ram_game = K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? (float)((double)pmc.WorkingSetSize / 1048576.0) : -1;
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) { m->ram_total = (float)((double)ms.ullTotalPhys / 1048576.0); m->ram_used = (float)((double)(ms.ullTotalPhys - ms.ullAvailPhys) / 1048576.0); }
#elif defined(__linux__)
    {   FILE *f = fopen("/proc/meminfo", "r");
        char l[128];
        double tot = -1, avail = -1;
        while (f && fgets(l, sizeof l, f)) { if (!strncmp(l, "MemTotal:", 9)) tot = atof(l + 9); else if (!strncmp(l, "MemAvailable:", 13)) avail = atof(l + 13); }
        if (f) fclose(f);
        if (tot > 0 && avail >= 0) { m->ram_total = (float)(tot / 1024.0); m->ram_used = (float)((tot - avail) / 1024.0); }
    }
    static int found;
    if (!found) { found = 1; find_sensors(); }
    char p[400];
    if (g_hw_cpu[0]) { snprintf(p, sizeof p, "%s/temp1_input", g_hw_cpu); double v = read_num(p); m->cpu_temp = v >= 0 ? (float)(v / 1000.0) : -1; }
    if (g_drm[0]) {
        snprintf(p, sizeof p, "%s/gpu_busy_percent", g_drm); m->gpu = (float)read_num(p);
        double v;
        snprintf(p, sizeof p, "%s/mem_info_vram_used", g_drm); v = read_num(p); m->vram_used = v >= 0 ? (float)(v / 1048576.0) : -1;
        snprintf(p, sizeof p, "%s/mem_info_vram_total", g_drm); v = read_num(p); m->vram_total = v >= 0 ? (float)(v / 1048576.0) : -1;
        snprintf(p, sizeof p, "%s/mem_info_gtt_used", g_drm); v = read_num(p); m->gtt_used = v >= 0 ? (float)(v / 1048576.0) : -1;
        snprintf(p, sizeof p, "%s/mem_info_gtt_total", g_drm); v = read_num(p); m->gtt_total = v >= 0 ? (float)(v / 1048576.0) : -1;
    }
    if (g_hw_gpu[0]) {
        double v;
        snprintf(p, sizeof p, "%s/temp1_input", g_hw_gpu); v = read_num(p); m->gpu_temp = v >= 0 ? (float)(v / 1000.0) : -1;
        snprintf(p, sizeof p, "%s/power1_average", g_hw_gpu); v = read_num(p);
        if (v < 0) { snprintf(p, sizeof p, "%s/power1_input", g_hw_gpu); v = read_num(p); }
        m->gpu_w = v >= 0 ? (float)(v / 1e6) : -1;
        snprintf(p, sizeof p, "%s/freq1_input", g_hw_gpu); v = read_num(p); m->gpu_mhz = v >= 0 ? (float)(v / 1e6) : -1;
    }
#endif
}
void perf_machine_cpu_name(char *out, size_t n) {
    out[0] = 0;
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
    unsigned r[12];
    if (!__get_cpuid(0x80000000, &r[0], &r[1], &r[2], &r[3]) || r[0] < 0x80000004) return;
    for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002 + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
    char b[49];
    memcpy(b, r, 48); b[48] = 0;
    char *s = b; while (*s == ' ') s++;
    snprintf(out, n, "%s", s);
#endif
}
