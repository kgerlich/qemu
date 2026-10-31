/*
 *  x86 AMX tile instruction helpers
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/helper-proto.h"
#include "helper-tcg.h"
#include "access.h"

/*
 * The tile configuration is kept in env->xtilecfg in the layout of the
 * XTILECFG state component and of the LDTILECFG operand:
 *
 *   byte 0        palette id
 *   byte 1        start_row, non-zero only while a load or store is restarted
 *   bytes 16..47  colsb[16], in bytes, as 16-bit values
 *   bytes 48..63  rows[16]
 *
 * Only palette 1 exists, with 8 tiles of up to 16 rows of 64 bytes.  Tile t
 * is at env->xtiledata + t * 1024, one row every 64 bytes.
 */
#define AMX_MAX_NAMES    8
#define AMX_MAX_ROWS     16
#define AMX_MAX_COLSB    64
#define AMX_TILE_BYTES   (AMX_MAX_ROWS * AMX_MAX_COLSB)

#define AMX_XCR0 (XSTATE_XTILE_CFG_MASK | XSTATE_XTILE_DATA_MASK)

static inline unsigned tilecfg_colsb(const uint8_t *cfg, int t)
{
    return lduw_le_p(cfg + 16 + 2 * t);
}

static inline unsigned tilecfg_rows(const uint8_t *cfg, int t)
{
    return cfg[48 + t];
}

/*
 * Check a tile configuration.  Palette 0 stands for the initial state and is
 * always valid; @restore is for XRSTOR, which has to accept the start_row of
 * an interrupted instruction, unlike LDTILECFG.
 */
bool x86_tilecfg_valid(const uint8_t *cfg, bool restore)
{
    int i;

    if (cfg[0] == 0) {
        return true;
    }
    if (cfg[0] > 1) {
        return false;
    }
    if (restore ? cfg[1] >= AMX_MAX_ROWS : cfg[1] != 0) {
        return false;
    }

    /* Reserved bytes 2..15, and the colsb and rows of the unused tiles.  */
    for (i = 2; i < 16; i++) {
        if (cfg[i]) {
            return false;
        }
    }
    for (i = AMX_MAX_NAMES; i < 16; i++) {
        if (tilecfg_colsb(cfg, i) || tilecfg_rows(cfg, i)) {
            return false;
        }
    }

    for (i = 0; i < AMX_MAX_NAMES; i++) {
        unsigned colsb = tilecfg_colsb(cfg, i);
        unsigned rows = tilecfg_rows(cfg, i);

        if (colsb > AMX_MAX_COLSB || rows > AMX_MAX_ROWS ||
            (colsb == 0) != (rows == 0)) {
            return false;
        }
    }
    return true;
}

/* #UD if the guest has not enabled AMX.  */
static void amx_check(CPUX86State *env, uintptr_t ra)
{
    if (!(env->cr[4] & CR4_OSXSAVE_MASK) ||
        (env->xcr0 & AMX_XCR0) != AMX_XCR0) {
        raise_exception_ra(env, EXCP06_ILLOP, ra);
    }
}

/* #NM if XFD still disables AMX.  The SDM lists it after every #UD.  */
static void amx_check_xfd(CPUX86State *env, uintptr_t ra)
{
    if ((env->features[FEAT_XSAVE] & CPUID_XSAVE_XFD) &&
        (env->msr_xfd & XSTATE_XTILE_DATA_MASK)) {
        env->msr_xfd_err = XSTATE_XTILE_DATA_MASK;
        raise_exception_ra(env, EXCP07_PREX, ra);
    }
}

/* #UD unless @tmm names a configured tile.  */
static void amx_check_tile(CPUX86State *env, int tmm, uintptr_t ra)
{
    if (env->xtilecfg[0] == 0 || tmm >= AMX_MAX_NAMES ||
        tilecfg_rows(env->xtilecfg, tmm) == 0) {
        raise_exception_ra(env, EXCP06_ILLOP, ra);
    }
}

void HELPER(ldtilecfg)(CPUX86State *env, target_ulong a0)
{
    uintptr_t ra = GETPC();
    uint8_t cfg[64];
    X86Access ac;
    int i;

    amx_check(env, ra);

    access_prepare(&ac, env, a0, sizeof(cfg), MMU_DATA_LOAD, ra);
    for (i = 0; i < sizeof(cfg); i += 8) {
        stq_le_p(cfg + i, access_ldq(&ac, a0 + i));
    }
    if (!x86_tilecfg_valid(cfg, false)) {
        raise_exception_ra(env, EXCP0D_GPF, ra);
    }

    /* Loading a configuration, even palette 0, clears all tile data.  */
    if (cfg[0] == 0) {
        memset(env->xtilecfg, 0, sizeof(env->xtilecfg));
    } else {
        memcpy(env->xtilecfg, cfg, sizeof(env->xtilecfg));
    }
    memset(env->xtiledata, 0, sizeof(env->xtiledata));
}

void HELPER(sttilecfg)(CPUX86State *env, target_ulong a0)
{
    uintptr_t ra = GETPC();
    X86Access ac;
    int i;

    amx_check(env, ra);

    access_prepare(&ac, env, a0, sizeof(env->xtilecfg), MMU_DATA_STORE, ra);
    for (i = 0; i < sizeof(env->xtilecfg); i += 8) {
        /* The configuration is all zero when no tiles are configured.  */
        access_stq(&ac, a0 + i, ldq_le_p(env->xtilecfg + i));
    }
}

void HELPER(tilerelease)(CPUX86State *env)
{
    amx_check(env, GETPC());

    memset(env->xtilecfg, 0, sizeof(env->xtilecfg));
    memset(env->xtiledata, 0, sizeof(env->xtiledata));
}

void HELPER(tilezero)(CPUX86State *env, uint32_t tmm)
{
    uintptr_t ra = GETPC();

    amx_check(env, ra);
    amx_check_tile(env, tmm, ra);
    amx_check_xfd(env, ra);

    memset(env->xtiledata + tmm * AMX_TILE_BYTES, 0, AMX_TILE_BYTES);
    env->xtilecfg[1] = 0;
}

/*
 * Rows are transferred from tilecfg.start_row on.  That field is kept up to
 * date row by row, so a page fault in the middle leaves the restart point
 * behind for the guest to resume from.
 */
static void tile_rows_check(CPUX86State *env, int tmm, uintptr_t ra)
{
    amx_check(env, ra);
    amx_check_tile(env, tmm, ra);

    if (tilecfg_colsb(env->xtilecfg, tmm) % 4 ||
        env->xtilecfg[1] >= tilecfg_rows(env->xtilecfg, tmm)) {
        raise_exception_ra(env, EXCP06_ILLOP, ra);
    }
    amx_check_xfd(env, ra);
}

void HELPER(tileloadd)(CPUX86State *env, target_ulong base, target_ulong stride,
                       uint32_t tmm)
{
    uintptr_t ra = GETPC();
    uint8_t *tile = env->xtiledata + tmm * AMX_TILE_BYTES;
    unsigned rows, colsb, r;

    tile_rows_check(env, tmm, ra);
    rows = tilecfg_rows(env->xtilecfg, tmm);
    colsb = tilecfg_colsb(env->xtilecfg, tmm);

    for (r = env->xtilecfg[1]; r < rows; r++) {
        target_ulong addr = base + r * stride;
        uint8_t row[AMX_MAX_COLSB] = {};
        unsigned j;

        env->xtilecfg[1] = r;
        for (j = 0; j < colsb; j++) {
            row[j] = cpu_ldub_data_ra(env, addr + j, ra);
        }
        memcpy(tile + r * AMX_MAX_COLSB, row, AMX_MAX_COLSB);
    }

    /* The rows the tile does not use read as zero.  */
    memset(tile + rows * AMX_MAX_COLSB, 0, AMX_TILE_BYTES - rows * AMX_MAX_COLSB);
    env->xtilecfg[1] = 0;
}

void HELPER(tilestored)(CPUX86State *env, target_ulong base, target_ulong stride,
                        uint32_t tmm)
{
    uintptr_t ra = GETPC();
    const uint8_t *tile = env->xtiledata + tmm * AMX_TILE_BYTES;
    unsigned rows, colsb, r;

    tile_rows_check(env, tmm, ra);
    rows = tilecfg_rows(env->xtilecfg, tmm);
    colsb = tilecfg_colsb(env->xtilecfg, tmm);

    for (r = env->xtilecfg[1]; r < rows; r++) {
        target_ulong addr = base + r * stride;
        unsigned j;

        env->xtilecfg[1] = r;
        for (j = 0; j < colsb; j++) {
            cpu_stb_data_ra(env, addr + j, tile[r * AMX_MAX_COLSB + j], ra);
        }
    }
    env->xtilecfg[1] = 0;
}
