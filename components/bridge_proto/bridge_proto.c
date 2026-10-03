#include "bridge_proto.h"
#include <string.h>

size_t bridge_encode(uint16_t seq, const uint8_t *payload, size_t len,
                     uint8_t *out, size_t out_cap)
{
    if (len > BRIDGE_MAX_PAYLOAD || out_cap < BRIDGE_HDR_LEN + len) return 0;
    if (len && !payload) return 0;

    bridge_hdr_t h = {
        .magic = BRIDGE_MAGIC,
        .seq   = seq,
        .len   = (uint16_t)len,
    };
    memcpy(out, &h, BRIDGE_HDR_LEN);
    if (len) memcpy(out + BRIDGE_HDR_LEN, payload, len);
    return BRIDGE_HDR_LEN + len;
}

/*
 * 回绕安全的有符号差值：把 seq 相对 expected 的距离映射到 [-32768, 32767]。
 * 先按 uint16 算出无符号差（有定义的模运算），再手工解释符号位，
 * 避免把超范围的整数强转成 int16_t 那种实现定义行为。
 */
static int32_t seq_delta(uint16_t seq, uint16_t expected)
{
    uint16_t d = (uint16_t)(seq - expected);
    return (d <= 0x7FFFu) ? (int32_t)d : (int32_t)d - 0x10000;
}

size_t bridge_decode(bridge_reasm_t *st, const uint8_t *pkt, size_t pkt_len,
                     uint8_t *out, size_t out_cap, uint16_t *lost)
{
    if (lost) *lost = 0;
    if (!st || !pkt || pkt_len < BRIDGE_HDR_LEN) return 0;

    bridge_hdr_t h;
    memcpy(&h, pkt, BRIDGE_HDR_LEN);

    if (h.magic != BRIDGE_MAGIC)                     return 0;
    if (h.len > BRIDGE_MAX_PAYLOAD)                  return 0;
    if (pkt_len != BRIDGE_HDR_LEN + (size_t)h.len)   return 0;

    /* 第一个包决定基准，这样网关可以从流的任意位置起步 */
    if (!st->synced) {
        st->synced   = true;
        st->expected = h.seq;
    }

    int32_t d = seq_delta(h.seq, st->expected);

    if (d < 0) {
        if (d > -(int32_t)BRIDGE_RESYNC_GAP) return 0;   /* 真·迟到或重复，丢弃 */

        /* 倒退太多，这不是迟到，是发送端重启、序号从 0 重来了。
           重新对齐，否则网关会一直把新流当迟到包丢掉，
           哑掉的时长正好等于发送端重启前已经运行的时长。 */
        st->expected = h.seq;
        d = 0;
    }

    if (d > 0) {                                /* 中间有丢包，记录但仍向前推进 */
        if (lost) *lost = (uint16_t)d;
        st->lost_pkts += (uint32_t)d;
    }

    if (h.len > out_cap) return 0;              /* 输出放不下，整包丢弃 */

    memcpy(out, pkt + BRIDGE_HDR_LEN, h.len);
    st->expected = (uint16_t)(h.seq + 1);
    return h.len;
}
