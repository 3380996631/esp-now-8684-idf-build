#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 串口透传的分帧编解码。
 *
 * 本组件不依赖 ESP-IDF，只有 <stdint.h>/<stdbool.h>/<stddef.h>，
 * 可以用普通 gcc 在 PC 上编译和测试（见 test_bridge_proto.c）。
 *
 * 线上格式（5 字节包头 + 最多 245 字节载荷 = 250，正好是 ESP-NOW 的单包上限）：
 *
 *     magic(1)=0xA5 | seq(2, 小端) | len(2, 小端) | payload(len)
 *
 * 两端都是 ESP32-C2（同架构、同小端），所以直接把 packed 结构体 memcpy
 * 到线上是安全的 —— 这也正是不值得手写逐字节序列化器的原因。
 * 换异构平台（比如 PC 端直接解析）时，这段假设必须重新审视。
 */

#define BRIDGE_MAGIC       0xA5
#define BRIDGE_HDR_LEN     5
#define BRIDGE_MAX_PAYLOAD 245

/*
 * 序号回退多少算"发送端重启了"。
 *
 * 直连 ESP-NOW 不会把包乱序 8 个以上，所以小幅度回退是真正的迟到包（丢弃），
 * 大幅度回退只可能是发送端重启后序号从 0 重来。不重新对齐的话，网关会一直
 * 把新流当迟到包丢掉，哑掉的时长正好等于发送端重启前已运行的时长。
 *
 * 代价：发送端运行不足 8 个包就重启时，网关最多哑 8 个包的时间（约 5 秒）。
 */
#define BRIDGE_RESYNC_GAP  8

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint16_t seq;    /* 每包递增，uint16 自然回绕 */
    uint16_t len;    /* 本包载荷字节数，<= BRIDGE_MAX_PAYLOAD */
} bridge_hdr_t;

/*
 * 编码一个包到 out。返回整包长度（头 + 载荷），参数非法时返回 0。
 * len 超过 BRIDGE_MAX_PAYLOAD 或 out_cap 放不下都算非法。
 */
size_t bridge_encode(uint16_t seq, const uint8_t *payload, size_t len,
                     uint8_t *out, size_t out_cap);

/* 接收端重组状态。synced 为 false 时，收到的第一个包决定 expected。 */
typedef struct {
    uint16_t expected;    /* 下一个期望的 seq */
    uint32_t lost_pkts;   /* 累计检测到的丢包数 */
    bool     synced;
} bridge_reasm_t;

/*
 * 校验一个收到的包，并把载荷追加到 out。
 *
 * 返回追加的字节数；0 表示这个包被丢弃（非法 / 迟到 / 重复）。
 * 检测到序号空洞时，*lost 写回跳过的包数（无丢包时写 0）。
 *
 * 绝不重排序：迟到的包属于流的更早位置，追加会破坏顺序，所以直接丢弃。
 * 丢弃丢字节但保持流单向推进，对下游解析器更安全。
 */
size_t bridge_decode(bridge_reasm_t *st, const uint8_t *pkt, size_t pkt_len,
                     uint8_t *out, size_t out_cap, uint16_t *lost);

#ifdef __cplusplus
}
#endif
