/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - CTB (Command Transport
 * Buffer) ring protocol
 *
 * Once the guest has registered both rings' GGTT addresses via SELF_CFG
 * (abi/guc_klvs_abi.h keys, captured by alchemist_guc.c into
 * s->h2g/s->g2h) and toggled CTB on, all further host<->GuC traffic
 * moves from the mmio mailbox to this real ring-buffer protocol -
 * transcribed directly from abi/guc_communication_ctb_abi.h and
 * xe_guc_ct.c (h2g_write()/g2h_read()/dequeue_one_g2h()).
 *
 * The descriptor (struct guc_ct_buffer_desc) holds head/tail as DWORD
 * offsets into the ring: for H2G the guest (sender) owns tail and we
 * (receiver) own head; for G2H it's reversed. Each ring message is a CTB
 * header dword (FENCE/FORMAT/NUM_DWORDS) followed by an embedded HXG
 * message - the same HXG format already used by the mmio mailbox, just
 * delivered through guest memory instead of VF_SW_FLAG registers.
 *
 * We only ever send a synchronous CTB reply for HXG_TYPE_REQUEST messages
 * - HXG_TYPE_EVENT and HXG_TYPE_FAST_REQUEST are both explicitly
 * documented (guc_messages_abi.h) as not expecting one, so sending one
 * would be protocol-incorrect, not just unnecessary. FAST_REQUEST/EVENT
 * messages with a recognized action are instead handed to
 * alchemist_submit.c (context registration/scheduling - see its file
 * comment), which may itself later send an *unsolicited* G2H event (e.g.
 * SCHED_CONTEXT_MODE_DONE) via alchemist_ctb_send_sched_context_mode_done()
 * below - a real, separate wire message, not a reply to this one.
 *
 * A few actions need a real side effect beyond whatever ack/non-ack
 * their message type gets - currently just GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST
 * (see alchemist_pc.c), checked regardless of type: confirmed live that
 * xe_guc_pc_start()'s RESET event arrives as HXG_TYPE_FAST_REQUEST, not
 * TYPE_REQUEST as its own ABI doc block's TYPE field suggests in the
 * abstract - xe_guc_ct.c's h2g_write() only promotes to TYPE_REQUEST
 * when the caller passes a g2h_fence, and pc_action_reset() (like
 * submit_exec_queue()'s SCHED_CONTEXT) does not.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

static uint32_t ctb_desc_read32(AlchemistState *s, uint64_t desc_addr,
                                 uint32_t off)
{
    uint32_t val = 0;

    alchemist_ggtt_read(s, desc_addr + off, &val, sizeof(val));
    return val;
}

static void ctb_desc_write32(AlchemistState *s, uint64_t desc_addr,
                              uint32_t off, uint32_t val)
{
    alchemist_ggtt_write(s, desc_addr + off, &val, sizeof(val));
}

static void ctb_ring_write_dwords(AlchemistState *s, uint64_t ring_addr,
                                   uint32_t ring_size_dwords, uint32_t pos,
                                   const uint32_t *dwords, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t idx = (pos + i) % ring_size_dwords;

        alchemist_ggtt_write(s, ring_addr + (uint64_t)idx * 4, &dwords[i], 4);
    }
}

static bool ctb_ring_read_dwords(AlchemistState *s, uint64_t ring_addr,
                                  uint32_t ring_size_dwords, uint32_t pos,
                                  uint32_t *dwords, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t idx = (pos + i) % ring_size_dwords;

        if (!alchemist_ggtt_read(s, ring_addr + (uint64_t)idx * 4,
                                  &dwords[i], 4)) {
            return false;
        }
    }
    return true;
}

void alchemist_ctb_register(AlchemistState *s, uint16_t key, uint64_t val)
{
    switch (key) {
    case GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_KEY:
        s->h2g.desc_addr = val;
        break;
    case GUC_KLV_SELF_CFG_H2G_CTB_ADDR_KEY:
        s->h2g.ring_addr = val;
        break;
    case GUC_KLV_SELF_CFG_H2G_CTB_SIZE_KEY:
        s->h2g.ring_size_dwords = val / sizeof(uint32_t);
        break;
    case GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_KEY:
        s->g2h.desc_addr = val;
        break;
    case GUC_KLV_SELF_CFG_G2H_CTB_ADDR_KEY:
        s->g2h.ring_addr = val;
        break;
    case GUC_KLV_SELF_CFG_G2H_CTB_SIZE_KEY:
        s->g2h.ring_size_dwords = val / sizeof(uint32_t);
        break;
    default:
        break;
    }
}

static void ctb_g2h_send(AlchemistState *s, const uint32_t *msg, uint32_t n)
{
    uint32_t tail, new_tail;

    if (s->g2h.ring_size_dwords == 0) {
        return;
    }

    tail = ctb_desc_read32(s, s->g2h.desc_addr, CTB_DESC_OFF_TAIL);

    ctb_ring_write_dwords(s, s->g2h.ring_addr, s->g2h.ring_size_dwords,
                           tail, msg, n);

    new_tail = (tail + n) % s->g2h.ring_size_dwords;
    ctb_desc_write32(s, s->g2h.desc_addr, CTB_DESC_OFF_TAIL, new_tail);

    alchemist_irq_raise_guc2host(s);
}

static void alchemist_ctb_send_g2h(AlchemistState *s, uint32_t fence,
                                    uint32_t hxg_response_word)
{
    uint32_t msg[2];

    msg[0] = (fence << CTB_MSG_0_FENCE_SHIFT) |
             (CTB_FORMAT_HXG << CTB_MSG_0_FORMAT_SHIFT) |
             (1u & CTB_MSG_0_NUM_DWORDS_MASK);
    msg[1] = hxg_response_word;

    ctb_g2h_send(s, msg, 2);
}

/*
 * XE_GUC_ACTION_SCHED_CONTEXT_MODE_DONE - an unsolicited G2H HXG_TYPE_EVENT,
 * not a reply to any specific request. xe_guc_ct.c's parse_g2h_event()
 * confirms the fence field is unchecked for EVENT-type messages (only
 * used for ring-credit accounting), so 0 is fine there.
 */
void alchemist_ctb_send_sched_context_mode_done(AlchemistState *s,
                                                 uint32_t guc_id,
                                                 uint32_t runnable_state)
{
    uint32_t msg[4];
    uint32_t hxg_hdr = (HXG_ORIGIN_GUC << HXG_MSG_0_ORIGIN_SHIFT) |
                        (HXG_TYPE_EVENT << HXG_MSG_0_TYPE_SHIFT) |
                        (XE_GUC_ACTION_SCHED_CONTEXT_MODE_DONE &
                         HXG_REQUEST_MSG_0_ACTION_MASK);

    msg[0] = (0u << CTB_MSG_0_FENCE_SHIFT) |
             (CTB_FORMAT_HXG << CTB_MSG_0_FORMAT_SHIFT) |
             (3u & CTB_MSG_0_NUM_DWORDS_MASK);
    msg[1] = hxg_hdr;
    msg[2] = guc_id;
    msg[3] = runnable_state;

    ctb_g2h_send(s, msg, 4);
}

void alchemist_ctb_check_h2g(AlchemistState *s)
{
    uint32_t head, tail;

    if (s->h2g.ring_size_dwords == 0) {
        /* CTB not registered yet - this doorbell write is for the mmio
         * mailbox path (alchemist_guc.c) instead. */
        return;
    }

    head = ctb_desc_read32(s, s->h2g.desc_addr, CTB_DESC_OFF_HEAD);
    tail = ctb_desc_read32(s, s->h2g.desc_addr, CTB_DESC_OFF_TAIL);

    while (head != tail) {
        uint32_t ctb_hdr = 0, hxg_hdr = 0;
        uint32_t num_dwords, fence, full_len, msg_start;

        msg_start = head;

        if (!ctb_ring_read_dwords(s, s->h2g.ring_addr, s->h2g.ring_size_dwords,
                                   head, &ctb_hdr, 1)) {
            break;
        }

        num_dwords = ctb_hdr & CTB_MSG_0_NUM_DWORDS_MASK;
        fence = (ctb_hdr >> CTB_MSG_0_FENCE_SHIFT) & 0xFFFFu;
        full_len = 1 + num_dwords;

        if (num_dwords >= 1) {
            ctb_ring_read_dwords(s, s->h2g.ring_addr, s->h2g.ring_size_dwords,
                                  (msg_start + 1) % s->h2g.ring_size_dwords,
                                  &hxg_hdr, 1);
        }

        head = (head + full_len) % s->h2g.ring_size_dwords;
        ctb_desc_write32(s, s->h2g.desc_addr, CTB_DESC_OFF_HEAD, head);

        if (num_dwords >= 1) {
            uint32_t type = (hxg_hdr >> HXG_MSG_0_TYPE_SHIFT) & HXG_TYPE_MASK;
            uint32_t action = hxg_hdr & HXG_REQUEST_MSG_0_ACTION_MASK;
            uint32_t payload[16];
            uint32_t n = num_dwords - 1; /* dwords after the hxg header */
            bool have_payload;

            if (n > ARRAY_SIZE(payload)) {
                n = ARRAY_SIZE(payload);
            }
            have_payload = n == 0 ||
                ctb_ring_read_dwords(s, s->h2g.ring_addr,
                                      s->h2g.ring_size_dwords,
                                      (msg_start + 2) % s->h2g.ring_size_dwords,
                                      payload, n);

            /*
             * GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST's GGTT side effect
             * (see alchemist_pc.c) applies regardless of message type -
             * confirmed live that xe_guc_pc_start()'s RESET actually
             * arrives as HXG_TYPE_FAST_REQUEST, the same as
             * XE_GUC_ACTION_SCHED_CONTEXT (xe_guc_ct.c's h2g_write()
             * only promotes to HXG_TYPE_REQUEST when the caller passes a
             * g2h_fence, which pc_action_reset() - like
             * submit_exec_queue() - does not).
             */
            if (have_payload && action == GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST) {
                alchemist_pc_handle_slpc_request(s, payload, n);
            }

            if (type == HXG_TYPE_REQUEST) {
                uint32_t resp = (HXG_ORIGIN_GUC << HXG_MSG_0_ORIGIN_SHIFT) |
                                 (HXG_TYPE_RESPONSE_SUCCESS << HXG_MSG_0_TYPE_SHIFT);

                alchemist_ctb_send_g2h(s, fence, resp);
            } else if (type == HXG_TYPE_FAST_REQUEST || type == HXG_TYPE_EVENT) {
                if (have_payload) {
                    alchemist_submit_handle_action(s, action, payload, n);
                }
            }
        }

        tail = ctb_desc_read32(s, s->h2g.desc_addr, CTB_DESC_OFF_TAIL);
    }
}
