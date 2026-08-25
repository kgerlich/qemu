/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - GuC PC (SLPC, Single Loop
 * Power Control - GT frequency/power management)
 *
 * xe_guc_pc_start() (xe_guc_pc.c) sends GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST
 * with SLPC_EVENT_RESET over the CTB ring - a plain HXG_TYPE_REQUEST our
 * generic CTB dispatch (alchemist_ctb.c) already ACKs correctly. But the
 * ack alone doesn't satisfy the driver: it doesn't wait on the CT
 * response at all for startup, it separately polls a driver-allocated,
 * GGTT-mapped buffer (struct slpc_shared_data, whose GGTT address is the
 * RESET event's own payload) for `header.global_state` to become
 * SLPC_GLOBAL_STATE_RUNNING - real GuC firmware writes that as a side
 * effect of processing RESET, entirely separately from the CT ack. That
 * write is what this file performs; it isn't a shortcut, it's the actual
 * mechanism the real protocol depends on.
 *
 * SLPC_EVENT_QUERY_TASK_STATE/PARAMETER_SET/PARAMETER_UNSET (same action
 * code, different EVENT_ID, sent throughout the device's life for
 * frequency sysfs and GT resets) need no GGTT side effect - the generic
 * CTB ack already covers them, since xe_guc_pc_start()'s
 * pc_adjust_freq_bounds() only reads task_state_data.freq fields it's
 * already prepared to see as zero (both operands of its ">" checks are 0
 * without a real RP0/RPn frequency table modeled) - see
 * docs/alchemist-bringup.md for why that's a deliberate, evidence-based
 * deferral rather than an oversight.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

void alchemist_pc_handle_slpc_request(AlchemistState *s,
                                       const uint32_t *payload, uint32_t n)
{
    uint32_t event_id;
    uint64_t shared_data_addr;
    uint32_t state;

    /* payload[0] = (event_id << 8) | argc, payload[1..] = EVENT_DATA1..N -
     * abi/guc_actions_slpc_abi.h. */
    if (n < 1) {
        return;
    }

    event_id = (payload[0] >> HOST2GUC_PC_SLPC_REQUEST_EVENT_ID_SHIFT) &
               HOST2GUC_PC_SLPC_REQUEST_EVENT_ID_MASK;

    if (event_id != SLPC_EVENT_RESET) {
        return;
    }
    if (n < 2) {
        return;
    }

    shared_data_addr = payload[1]; /* EVENT_DATA1 = xe_bo_ggtt_addr(pc->bo) */
    state = SLPC_GLOBAL_STATE_RUNNING;
    alchemist_ggtt_write(s, shared_data_addr + SLPC_SHARED_DATA_GLOBAL_STATE_OFF,
                          &state, sizeof(state));
}
