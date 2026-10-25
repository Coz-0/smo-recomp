/* hostrt.c's test for do-nothing handlers (SMO_STRICT), for AArch64 machine code: the shapes clang gives a handler that
 * only stores constants into the guest context (x0 = Ctx *) and returns. Checked against handlers compiled by Apple clang
 * at -O1 and -O2 in tests/macos/test_audit_a64.c. */
#ifndef RECOMP_AUDIT_A64_H
#define RECOMP_AUDIT_A64_H
#include <stdint.h>
#include <string.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

/* A constant pool is memory the process cannot write; a global variable is not one, whatever its value now. */
static inline int a64_read_only(const void *p, size_t n) {
#ifdef __APPLE__
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)p;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS) return 0;
    return a <= (uintptr_t)p && (uintptr_t)p + n <= a + sz && (info.protection & VM_PROT_READ) && !(info.protection & VM_PROT_WRITE);
#else
    (void)p; (void)n;
    return 0;
#endif
}

/* The value of an ORR/AND/EOR (immediate) bitmask: DecodeBitMasks of the Arm architecture manual, imms/immr form. */
static inline int a64_bitmask(uint32_t n, uint32_t immr, uint32_t imms, int sf, uint64_t *out) {
    uint32_t combined = (n << 6) | (~imms & 0x3f);
    int len = 31 - __builtin_clz(combined ? combined : 1);
    if (len < 1 || (!sf && n)) return 0;
    uint32_t size = 1u << len, levels = size - 1, s = imms & levels, r = immr & levels;
    if (s == levels) return 0;
    uint64_t welem = (s + 1 == 64) ? ~0ull : ((1ull << (s + 1)) - 1);
    if (r) welem = ((welem >> r) | (welem << (size - r))) & (size == 64 ? ~0ull : ((1ull << size) - 1));
    uint64_t v = 0;
    for (uint32_t i = 0; i < 64; i += size) v |= welem << i;
    *out = sf ? v : (uint32_t)v;
    return 1;
}

/* 0 = real code; 1 = empty body; 2 = only stores constants (x0 := *x0 when *sets_x0) */
static inline int audit_trivial_a64(const void *fn, long long *x0, int *sets_x0) {
    const uint8_t *p = (const uint8_t *)fn;
    uint64_t reg[32];
    uint32_t known = 1u << 31;                /* register 31 of a store is xzr: zero */
    uint64_t vval[32][2];
    uint32_t vknown = 0;                      /* vector registers with a known value (movi #0, or a constant-pool load) */
    uint32_t addr = 0;                        /* registers holding an address made by adrp (+ add): a constant pool */
    int stores = 0;
    reg[31] = 0;
    *x0 = 0; *sets_x0 = 0;
    if (!p) return 0;
    for (int step = 0; step < 16; step++) {
        uint32_t i;
        memcpy(&i, p, 4);
        uint32_t rd = i & 31, rn = (i >> 5) & 31, rt2 = (i >> 10) & 31;
        int sf = (int)(i >> 31);
        if (i == 0xd65f03c0u) return stores ? 2 : 1;                                   /* ret */
        if ((i & 0xfc000000u) == 0x14000000u) {                                          /* b (tail jump, thunk) */
            int32_t off = (int32_t)(i << 6) >> 6;
            p += (intptr_t)off * 4;
            continue;
        }
        if ((i & 0x7f800000u) == 0x52800000u || (i & 0x7f800000u) == 0x12800000u) {      /* movz / movn */
            if (rd == 0) return 0;                                                       /* x0 must stay the context */
            uint64_t v = (uint64_t)((i >> 5) & 0xffff) << (((i >> 21) & 3) * 16);
            if ((i & 0x7f800000u) == 0x12800000u) v = ~v;
            reg[rd] = sf ? v : (uint32_t)v; known |= 1u << rd;
            p += 4; continue;
        }
        if ((i & 0x7f800000u) == 0x72800000u && (known >> rd & 1)) {                    /* movk */
            uint32_t sh = ((i >> 21) & 3) * 16;
            uint64_t v = (reg[rd] & ~(0xffffull << sh)) | ((uint64_t)((i >> 5) & 0xffff) << sh);
            reg[rd] = sf ? v : (uint32_t)v;
            p += 4; continue;
        }
        if ((i & 0x7f800000u) == 0x32000000u && rn == 31 && rd) {                       /* orr rd, xzr, #bitmask (mov) */
            uint64_t v;
            if (!a64_bitmask((i >> 22) & 1, (i >> 16) & 63, (i >> 10) & 63, sf, &v)) return 0;
            reg[rd] = v; known |= 1u << rd;
            p += 4; continue;
        }
        if ((i & 0xffffffe0u) == 0x6f00e400u) { vval[rd][0] = vval[rd][1] = 0; vknown |= 1u << rd; p += 4; continue; }   /* movi vd.2d, #0 */
        if ((i & 0x9f000000u) == 0x90000000u) {                                          /* adrp */
            if (rd == 0) return 0;
            int64_t imm = (int64_t)(((uint64_t)((i >> 5) & 0x7ffff) << 2 | ((i >> 29) & 3)) << 43) >> 31;
            reg[rd] = ((uint64_t)(uintptr_t)p & ~0xfffull) + (uint64_t)imm; addr |= 1u << rd; known &= ~(1u << rd);
            p += 4; continue;
        }
        if ((i & 0xff800000u) == 0x91000000u && (addr >> rn & 1) && rd) {               /* add xd, xn, #imm (adrp's low bits) */
            reg[rd] = reg[rn] + ((uint64_t)((i >> 10) & 0xfff) << ((i >> 22) & 1 ? 12 : 0)); addr |= 1u << rd;
            p += 4; continue;
        }
        /* ldr q / d from a constant pool (read-only memory). Only vector loads are taken: clang keeps a constant for a
         * general register in the instructions, so an adrp-relative load into one is a variable. */
        if (((i & 0xffc00000u) == 0x3dc00000u || (i & 0xffc00000u) == 0xfd400000u) && (addr >> rn & 1)) {
            int q = (i & 0xffc00000u) == 0x3dc00000u;
            const uint8_t *src = (const uint8_t *)(uintptr_t)(reg[rn] + (((i >> 10) & 0xfff) << (q ? 4 : 3)));
            if (!a64_read_only(src, q ? 16 : 8)) return 0;
            vval[rd][1] = 0;
            memcpy(vval[rd], src, q ? 16 : 8);
            vknown |= 1u << rd;
            p += 4; continue;
        }
        if ((i & 0xbfc00000u) == 0xb9000000u && rn == 0) {                              /* str xt/wt, [x0, #imm] */
            int is64 = (int)((i >> 30) & 1);
            uint32_t off = ((i >> 10) & 0xfff) << (is64 ? 3 : 2);
            if (!(known >> rd & 1)) return 0;
            if (off == 0) { *x0 = is64 ? (long long)reg[rd] : (long long)(uint32_t)reg[rd]; *sets_x0 = 1; }
            stores++; p += 4; continue;
        }
        if ((i & 0xffc00000u) == 0xa9000000u && rn == 0) {                              /* stp xt, xt2, [x0, #imm] */
            int32_t off = ((int32_t)(i << 10) >> 25) * 8;
            if (!(known >> rd & 1) || !(known >> rt2 & 1)) return 0;
            if (off == 0) { *x0 = (long long)reg[rd]; *sets_x0 = 1; }
            stores++; p += 4; continue;
        }
        if (((i & 0xffc00000u) == 0x3d800000u || (i & 0xffc00000u) == 0xfd000000u) && rn == 0) {   /* str q / d, [x0, #imm] */
            if (!(vknown >> rd & 1)) return 0;
            if (((i >> 10) & 0xfff) == 0) { *x0 = (long long)vval[rd][0]; *sets_x0 = 1; }
            stores++; p += 4; continue;
        }
        if (((i & 0xffc00000u) == 0xad000000u || (i & 0xffc00000u) == 0x6d000000u) && rn == 0) {   /* stp q / d, [x0, #imm] */
            if (!(vknown >> rd & 1) || !(vknown >> rt2 & 1)) return 0;
            if (((i >> 15) & 0x7f) == 0) { *x0 = (long long)vval[rd][0]; *sets_x0 = 1; }
            stores++; p += 4; continue;
        }
        return 0;
    }
    return 0;
}
#endif
