/*
 * bridge_proto 的自检 —— 这是整个工程里唯一有真实逻辑（分支 + 回绕运算）的地方，
 * 所以留了这一个检查。默认编进固件，app_main 开机调一次 bridge_proto_selftest()。
 *
 * 想在 PC 上跑（更快，不用烧板子）：
 *
 *   gcc -DBRIDGE_PROTO_HOST_TEST -I components/bridge_proto/include \
 *       components/bridge_proto/bridge_proto.c \
 *       components/bridge_proto/test_bridge_proto.c -o /tmp/t && /tmp/t
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bridge_proto.h"

static uint8_t pkt[256];
static uint8_t out[4096];

static size_t enc(uint16_t seq, const uint8_t *p, size_t n)
{
    return bridge_encode(seq, p, n, pkt, sizeof pkt);
}

/* 1. 往返：600 字节切成 245 + 245 + 110，逐字节还原 */
static void test_roundtrip(void)
{
    uint8_t src[600];
    for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 7 + 3);

    bridge_reasm_t st = {0};
    size_t got = 0;
    uint16_t seq = 100;

    for (size_t off = 0; off < sizeof src; off += BRIDGE_MAX_PAYLOAD) {
        size_t n = sizeof src - off;
        if (n > BRIDGE_MAX_PAYLOAD) n = BRIDGE_MAX_PAYLOAD;

        size_t pl = enc(seq++, src + off, n);
        assert(pl == BRIDGE_HDR_LEN + n);

        uint16_t lost;
        size_t w = bridge_decode(&st, pkt, pl, out + got, sizeof out - got, &lost);
        assert(w == n);
        assert(lost == 0);
        got += w;
    }

    assert(got == sizeof src);
    assert(memcmp(out, src, sizeof src) == 0);
    assert(st.lost_pkts == 0);
}

/* 2. 丢包：号 0,1,3,4 到达 —— 检测到一次空洞，但不卡死 */
static void test_loss(void)
{
    bridge_reasm_t st = {0};
    uint8_t buf[8];
    size_t got = 0;

    for (uint16_t s = 0; s <= 4; s++) {
        if (s == 2) continue;                 /* 模拟第 3 包丢了 */
        memset(buf, (uint8_t)s, sizeof buf);
        uint16_t lost;
        got += bridge_decode(&st, pkt, enc(s, buf, sizeof buf),
                             out + got, sizeof out - got, &lost);
    }

    assert(got == 4 * 8);
    assert(st.lost_pkts == 1);
    assert(out[0] == 0 && out[8] == 1 && out[16] == 3 && out[24] == 4);
}

/* 3. 乱序：0,2,1,3 —— 迟到的 1 被丢弃，流保持单向推进 */
static void test_reorder(void)
{
    bridge_reasm_t st = {0};
    uint8_t buf[4];
    size_t got = 0;
    const uint16_t order[] = {0, 2, 1, 3};

    for (int i = 0; i < 4; i++) {
        memset(buf, (uint8_t)order[i], sizeof buf);
        uint16_t lost;
        got += bridge_decode(&st, pkt, enc(order[i], buf, sizeof buf),
                             out + got, sizeof out - got, &lost);
    }

    assert(got == 3 * 4);
    assert(st.lost_pkts == 1);       /* seq 2 跳过了预期的 1，记一次 */
    assert(out[0] == 0 && out[4] == 2 && out[8] == 3);
}

/* 4. 回绕：65534,65535,0,1 连发 —— 不得产生假丢包 */
static void test_wrap(void)
{
    bridge_reasm_t st = {0};
    uint8_t buf[4];
    size_t got = 0;
    const uint16_t order[] = {65534, 65535, 0, 1};

    for (int i = 0; i < 4; i++) {
        memset(buf, (uint8_t)i, sizeof buf);
        uint16_t lost = 99;
        got += bridge_decode(&st, pkt, enc(order[i], buf, sizeof buf),
                             out + got, sizeof out - got, &lost);
        assert(lost == 0);
    }

    assert(got == 16);
    assert(st.lost_pkts == 0);
    assert(st.expected == 2);
}

/* 4b. 发送端重启：序号从 0 重来 —— 网关必须重新对齐，而不是一直丢 */
static void test_sender_restart(void)
{
    bridge_reasm_t st = {0};
    uint8_t buf[4];

    /* 先跑一段，把 expected 推高 */
    for (uint16_t s = 0; s < 2000; s++) {
        uint16_t lost;
        bridge_decode(&st, pkt, enc(s, buf, sizeof buf), out, sizeof out, &lost);
    }
    assert(st.expected == 2000);

    /* 发送端重启，序号回到 0：第 1 个包就必须被接受 */
    uint16_t lost = 99;
    size_t n = bridge_decode(&st, pkt, enc(0, buf, sizeof buf), out, sizeof out, &lost);
    assert(n == 4);
    assert(lost == 0);            /* 重新对齐，不是"丢了 2000 个包" */
    assert(st.expected == 1);

    /* 后续继续正常推进 */
    n = bridge_decode(&st, pkt, enc(1, buf, sizeof buf), out, sizeof out, &lost);
    assert(n == 4 && st.expected == 2);

    /* 但大幅度倒退只认作重启，小幅度倒退仍是迟到包 */
    bridge_reasm_t st2 = {0};
    uint16_t l2;
    for (uint16_t s = 0; s < 100; s++)
        bridge_decode(&st2, pkt, enc(s, buf, sizeof buf), out, sizeof out, &l2);
    assert(st2.expected == 100);
    /* 迟到 3 个包 -> 丢弃，expected 不动 */
    assert(bridge_decode(&st2, pkt, enc(97, buf, sizeof buf), out, sizeof out, &l2) == 0);
    assert(st2.expected == 100);
    /* 倒退 100 个包 -> 判为重启，接受 */
    assert(bridge_decode(&st2, pkt, enc(0, buf, sizeof buf), out, sizeof out, &l2) == 4);
    assert(st2.expected == 1);
}

/* 5. 非法输入一律拒绝，且不越界写 */
static void test_invalid(void)
{
    bridge_reasm_t st = {0};
    uint16_t lost;
    uint8_t payload[300] = {0};     /* 内容无关，但必须初始化：-Werror=maybe-uninitialized */
    size_t pl = enc(0, payload, 10);
    uint8_t bad[256];

    memcpy(bad, pkt, pl);
    bad[0] = 0x00;                                          /* magic 错 */
    assert(bridge_decode(&st, bad, pl, out, sizeof out, &lost) == 0);

    memcpy(bad, pkt, pl);
    bad[3] = 0xFF; bad[4] = 0xFF;                           /* len 超 245 */
    assert(bridge_decode(&st, bad, pl, out, sizeof out, &lost) == 0);

    assert(bridge_decode(&st, pkt, pl - 1, out, sizeof out, &lost) == 0);  /* 长度与 len 不符 */
    assert(bridge_decode(&st, pkt, 3, out, sizeof out, &lost) == 0);       /* 短于包头 */

    /* 输出缓冲比载荷小 —— 整包丢弃，不截断 */
    memcpy(bad, pkt, pl);
    assert(bridge_decode(&st, bad, pl, out, 5, &lost) == 0);

    /* 编码侧拒绝超长 */
    assert(bridge_encode(0, payload, BRIDGE_MAX_PAYLOAD + 1, pkt, sizeof pkt) == 0);
    assert(bridge_encode(0, payload, 10, pkt, 5) == 0);     /* 缓冲区放不下 */

    assert(st.lost_pkts == 0);      /* 被拒的包不应污染丢包统计 */
}

static void run_all(void)
{
    test_roundtrip();
    test_loss();
    test_reorder();
    test_wrap();
    test_sender_restart();
    test_invalid();
}

#ifdef BRIDGE_PROTO_HOST_TEST
int main(void)
{
    run_all();
    printf("bridge_proto: 6 组用例全部通过\n");
    return 0;
}
#else
void bridge_proto_selftest(void);
void bridge_proto_selftest(void)
{
    run_all();
}
#endif
