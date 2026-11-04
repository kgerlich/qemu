/*
 * AMX tour: steps through the AMX features that the TCG emulation supports
 * (TILE, INT8, BF16, FP16, COMPLEX, TF32, FP8, MOVRS), one phase at a time,
 * and idles in a pause loop after each phase so that a debugger sees the tile
 * registers in a stable state.
 *
 *   amxtour          cycle through all phases, DWELL seconds each, forever
 *   amxtour N        stay in phase N forever
 *   amxtour -l       list the phases
 *
 * Globals for a debugger: phase_id, phase_name, phase_check (1 ok, 0 wrong,
 * -1 not checked).
 *
 * This is a tool for a Linux guest under qemu-system-x86_64 with an AMX CPU
 * model, not a linux-user test: it needs arch_prctl(ARCH_REQ_XCOMP_PERM),
 * which linux-user does not implement, and a compiler with the -mamx-*
 * options below (GCC 15), so it is not listed in meson.build.
 *
 *   gcc -O1 -g -static -mamx-tile -mamx-int8 -mamx-bf16 -mamx-fp16 \
 *       -mamx-complex -mamx-tf32 -mamx-fp8 -mamx-movrs amxtour.c -o amxtour
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define DWELL 4

struct tilecfg {
    uint8_t palette, start_row, rsvd[14];
    uint16_t colsb[16];
    uint8_t rows[16];
} __attribute__((packed, aligned(64)));

volatile int phase_id = -1;
volatile const char *volatile phase_name = "";
volatile int phase_check = -1;

static uint8_t A[16][64] __attribute__((aligned(64)));   /* tmm1 source */
static uint8_t B[16][64] __attribute__((aligned(64)));   /* tmm2 source */
static uint8_t fill[4][1024] __attribute__((aligned(64))); /* tmm4..7 markers */
static uint8_t C[1024] __attribute__((aligned(64)));     /* tmm0 read back */

static uint16_t f2bf(float f) { uint32_t u; memcpy(&u, &f, 4); return u >> 16; } /* exact inputs */
static uint16_t f2h(float f) { _Float16 h = (_Float16)f; uint16_t u; memcpy(&u, &h, 2); return u; }

static void setcfg(const int rows[8], const int colsb[8])
{
    struct tilecfg c = { .palette = 1 };
    for (int i = 0; i < 8; i++) { c.colsb[i] = colsb[i]; c.rows[i] = rows[i]; }
    _tile_loadconfig(&c);
}

/* 8 tiles of 16 rows x 64 bytes; tmm4..7 hold the marker bytes 0x44..0x77. */
static void cfg_full(void)
{
    static const int r[8] = { 16, 16, 16, 16, 16, 16, 16, 16 }, c[8] = { 64, 64, 64, 64, 64, 64, 64, 64 };
    setcfg(r, c);
    _tile_zero(0);
    _tile_zero(3);
    _tile_loadd(1, A, 64);
    _tile_loadd(2, B, 64);
    _tile_loadd(4, fill[0], 64);
    _tile_loadd(5, fill[1], 64);
    _tile_loadd(6, fill[2], 64);
    _tile_loadd(7, fill[3], 64);
}

static void read_c(void) { _tile_stored(0, C, 64); }

/* Each phase leaves its result in tmm0 and returns 1 / 0 / -1 (checked ok / wrong / not checked). */

static int p_int8(int phase)
{
    /* A[m][k] = 0x80+m, B[k/4][n*4+j] = 0x80+n: the four signedness variants give four results. */
    for (int m = 0; m < 16; m++) memset(A[m], 0x80 + m, 64);
    for (int k = 0; k < 16; k++) for (int n = 0; n < 16; n++) memset(&B[k][n * 4], 0x80 + n, 4);
    cfg_full();
    int as = 0, bs = 0;
    switch (phase) {
    case 0: _tile_dpbssd(0, 1, 2); as = 1; bs = 1; break;
    case 1: _tile_dpbsud(0, 1, 2); as = 1; bs = 0; break;
    case 2: _tile_dpbusd(0, 1, 2); as = 0; bs = 1; break;
    case 3: _tile_dpbuud(0, 1, 2); as = 0; bs = 0; break;
    }
    read_c();
    int32_t *c = (int32_t *)C;
    for (int m = 0; m < 16; m++) for (int n = 0; n < 16; n++) {
        int av = as ? (int8_t)(0x80 + m) : 0x80 + m, bv = bs ? (int8_t)(0x80 + n) : 0x80 + n;
        if (c[m * 16 + n] != 64 * av * bv) return 0;
    }
    return 1;
}

/* BF16 and FP16: A = m+1, B = n+1, K = 32 -> C = 32*(m+1)*(n+1) in fp32. */
static int p_half(int fp16)
{
    uint16_t *a = (uint16_t *)A, *b = (uint16_t *)B;
    uint16_t (*cv)(float) = fp16 ? f2h : f2bf;
    for (int m = 0; m < 16; m++) for (int k = 0; k < 32; k++) a[m * 32 + k] = cv(m + 1);
    for (int k = 0; k < 16; k++) for (int n = 0; n < 16; n++) b[k * 32 + n * 2] = b[k * 32 + n * 2 + 1] = cv(n + 1);
    cfg_full();
    if (fp16) _tile_dpfp16ps(0, 1, 2); else _tile_dpbf16ps(0, 1, 2);
    read_c();
    float *c = (float *)C;
    for (int m = 0; m < 16; m++) for (int n = 0; n < 16; n++)
        if (c[m * 16 + n] != 32.0f * (m + 1) * (n + 1)) return 0;
    return 1;
}

/* AMX-COMPLEX: pairs of fp16 (real, imag). A = (m+1) + 1i, B = (n+1) + 1i. */
static int p_complex(int imag)
{
    uint16_t *a = (uint16_t *)A, *b = (uint16_t *)B;
    for (int m = 0; m < 16; m++) for (int k = 0; k < 16; k++) { a[m * 32 + k * 2] = f2h(m + 1); a[m * 32 + k * 2 + 1] = f2h(1); }
    for (int k = 0; k < 16; k++) for (int n = 0; n < 16; n++) { b[k * 32 + n * 2] = f2h(n + 1); b[k * 32 + n * 2 + 1] = f2h(1); }
    cfg_full();
    if (imag) _tile_cmmimfp16ps(0, 1, 2); else _tile_cmmrlfp16ps(0, 1, 2);
    read_c();
    return -1;
}

/* AMX-TF32: fp32 elements, A = m+1, B = n+1, K = 16 -> C = 16*(m+1)*(n+1). */
static int p_tf32(void)
{
    float *a = (float *)A, *b = (float *)B;
    for (int m = 0; m < 16; m++) for (int k = 0; k < 16; k++) a[m * 16 + k] = m + 1;
    for (int k = 0; k < 16; k++) for (int n = 0; n < 16; n++) b[k * 16 + n] = n + 1;
    cfg_full();
    _tile_mmultf32ps(0, 1, 2);
    read_c();
    float *c = (float *)C;
    for (int m = 0; m < 16; m++) for (int n = 0; n < 16; n++)
        if (c[m * 16 + n] != 16.0f * (m + 1) * (n + 1)) return 0;
    return 1;
}

/* AMX-FP8: A = 0x38, B = 0x3c. As BF8 (E5M2) 0x38 = 0.5 and 0x3c = 1.0; as HF8 (E4M3) 0x38 = 1.0 and 0x3c = 1.5. */
static int p_fp8(int v)
{
    memset(A, 0x38, sizeof A);
    memset(B, 0x3c, sizeof B);
    cfg_full();
    switch (v) {
    case 0: _tile_dpbf8ps(0, 1, 2); break;
    case 1: _tile_dphf8ps(0, 1, 2); break;
    case 2: _tile_dphbf8ps(0, 1, 2); break;
    case 3: _tile_dpbhf8ps(0, 1, 2); break;
    }
    read_c();
    return -1;
}

/* AMX-MOVRS: tile loads with the read-shared hint. tmm3 <- DRS, tmm4 <- DRST1; both from A. */
static int p_movrs(void)
{
    for (int r = 0; r < 16; r++) for (int c = 0; c < 64; c++) A[r][c] = (r << 4) | (c & 15);
    cfg_full();
    _tile_loaddrs(3, A, 64);
    _tile_loaddrst1(4, A, 64);
    read_c();
    _tile_stored(3, C, 64);
    return memcmp(C, A, sizeof C) == 0 ? 1 : 0;
}

/* Eight tiles of different shapes, each filled with (row << 4 | col). */
static int p_shapes(void)
{
    static const int rows[8] = { 16, 8, 4, 2, 1, 16, 3, 16 };
    static const int colsb[8] = { 64, 32, 16, 8, 4, 4, 40, 64 };
    static uint8_t src[1024] __attribute__((aligned(64)));
    for (int r = 0; r < 16; r++) for (int c = 0; c < 64; c++) src[r * 64 + c] = (r << 4) | (c & 15);
    setcfg(rows, colsb);
    _tile_loadd(0, src, 64);
    _tile_loadd(1, src, 64);
    _tile_loadd(2, src, 64);
    _tile_loadd(3, src, 64);
    _tile_loadd(4, src, 64);
    _tile_loadd(5, src, 64);
    _tile_loadd(6, src, 64);
    _tile_loadd(7, src, 64);
    return -1;
}

/* TILERELEASE: palette 0 and no config, all tiles back in the INIT state. */
static int p_release(void)
{
    cfg_full();
    _tile_release();
    return -1;
}

static const struct { const char *name; } names[] = {
    { "INT8 TDPBSSD (A signed, B signed)" },
    { "INT8 TDPBSUD (A signed, B unsigned)" },
    { "INT8 TDPBUSD (A unsigned, B signed)" },
    { "INT8 TDPBUUD (A unsigned, B unsigned)" },
    { "BF16 TDPBF16PS" },
    { "FP16 TDPFP16PS" },
    { "COMPLEX TCMMRLFP16PS (real part)" },
    { "COMPLEX TCMMIMFP16PS (imaginary part)" },
    { "TF32 TMMULTF32PS" },
    { "FP8 TDPBF8PS (bf8 x bf8)" },
    { "FP8 TDPHF8PS (hf8 x hf8)" },
    { "FP8 TDPHBF8PS (hf8 x bf8)" },
    { "FP8 TDPBHF8PS (bf8 x hf8)" },
    { "MOVRS TILELOADDRS / TILELOADDRST1" },
    { "tile shapes: tmm0..7 with different rows x colsb" },
    { "TILERELEASE: palette 0, all tiles INIT" },
};
#define NPHASE ((int)(sizeof names / sizeof names[0]))

static int run_phase(int p)
{
    if (p < 4) return p_int8(p);
    switch (p) {
    case 4: return p_half(0);
    case 5: return p_half(1);
    case 6: return p_complex(0);
    case 7: return p_complex(1);
    case 8: return p_tf32();
    case 9: case 10: case 11: case 12: return p_fp8(p - 9);
    case 13: return p_movrs();
    case 14: return p_shapes();
    default: return p_release();
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-l")) {
        for (int i = 0; i < NPHASE; i++) printf("%2d  %s\n", i, names[i].name);
        return 0;
    }
    int hold = argc > 1 ? atoi(argv[1]) : -1;
    if (hold >= NPHASE) { fprintf(stderr, "phase 0..%d\n", NPHASE - 1); return 1; }
    if (syscall(SYS_arch_prctl, 0x1023, 18)) { perror("arch_prctl(XCOMP_PERM, XTILEDATA)"); return 1; }
    for (int i = 0; i < 4; i++) memset(fill[i], 0x44 + 0x11 * i, 1024);

    for (int i = hold >= 0 ? hold : 0;; i = (i + 1) % NPHASE) {
        int p = hold >= 0 ? hold : i;
        phase_id = p;
        phase_name = names[p].name;
        phase_check = run_phase(p);
        printf("phase %2d  %-52s %s\n", p, names[p].name,
               phase_check < 0 ? "(not checked)" : phase_check ? "check ok" : "CHECK FAILED");
        fflush(stdout);
        struct timespec t0, t;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        do {
            _mm_pause();
            clock_gettime(CLOCK_MONOTONIC, &t);
        } while (hold >= 0 || t.tv_sec - t0.tv_sec < DWELL);
    }
}
