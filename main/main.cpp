/*
 * 串口透传的两端装配点。逻辑一行没有，全在三个组件里：
 *
 *   传感器 ──UART1 38400──> C2#1(发送端) ──ESP-NOW──> C2#2(网关) ──UART0/USB──> PC
 *
 * 角色在 menuconfig -> Serial bridge 里选。两块板跑的是同一份代码。
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/uart.h"

#include "bridge_proto.h"
#include "espnow_link.h"
#include "uart_link.h"

static const char *TAG = "bridge";

/* choice 里没被选中的那个符号在 sdkconfig.h 里是 #undef，不是 0，
   所以角色分支一律用 #ifdef，不能写 #if */
#ifdef CONFIG_BRIDGE_ROLE_SENDER
#define ROLE_NAME "sender"
#else
#define ROLE_NAME "gateway"
#endif

/* 编译期自检：bridge_proto 的序号/回绕逻辑，失败直接 assert 崩在这里 */
extern "C" void bridge_proto_selftest(void);

/* ---- Kconfig 宏 -> 运行时值。只在 app_main 里读这一次，
       三个组件里看不到任何 CONFIG_，这是它们能在 PC 上单独编译的原因 ---- */

static void parse_mac(const char *s, uint8_t out[6])
{
    unsigned a[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2],
               &a[3], &a[4], &a[5]) != 6) {
        ESP_LOGE(TAG, "BRIDGE_PEER_MAC 格式不对: \"%s\"", s);
        abort();
    }
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)a[i];
}

#ifdef CONFIG_BRIDGE_ROLE_SENDER

/* 传感器来了多少字节都无所谓：一律按 245 切，切块点在哪儿都不影响还原 */
static void on_sensor_data(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    static uint8_t  pkt[BRIDGE_HDR_LEN + BRIDGE_MAX_PAYLOAD];
    static uint16_t seq;

    while (len > 0) {
        size_t n  = (len > BRIDGE_MAX_PAYLOAD) ? BRIDGE_MAX_PAYLOAD : len;
        size_t pl = bridge_encode(seq++, data, n, pkt, sizeof pkt);
        if (pl) {
            esp_err_t e = espnow_link_send_sync(pkt, pl);
            if (e != ESP_OK) ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(e));
        }
        data += n;
        len  -= n;
    }
    /* ponytail: 发送期间 UART 环形缓冲(2048B, 38400 下约 0.53s)仍在填。
       射频卡住超过这个时间会静默丢字节——要顶住就得上 StreamBuffer + 独立发送任务。 */
}

#else  /* CONFIG_BRIDGE_ROLE_GATEWAY */

static bridge_reasm_t s_reasm;

static void on_air(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    static uint8_t out[BRIDGE_MAX_PAYLOAD];

    /* lost 传 NULL：网关日志在初始化后已全部关掉，丢包计数没地方显示。
       要排查丢包就在这里传个变量并 ESP_LOGW——记得先别关日志。 */
    size_t n = bridge_decode(&s_reasm, data, len, out, sizeof out, NULL);
    if (n) uart_link_write(out, n);
}

#endif

extern "C" void app_main(void)
{
    bridge_proto_selftest();

    uint8_t self[6], peer[6];
    espnow_link_get_mac(self);
    parse_mac(CONFIG_BRIDGE_PEER_MAC, peer);

    ESP_LOGI(TAG, "mode=%s self=" MACSTR " peer=" MACSTR " ch=%d",
             ROLE_NAME, MAC2STR(self), MAC2STR(peer), CONFIG_BRIDGE_CHANNEL);

    /* 两个角色共用一份 Kconfig，来回切角色时很容易忘了把 PEER_MAC 跟着换。
       填成本机 MAC 的话收发都静默失败，日志里很难一眼看出来。 */
    if (memcmp(self, peer, 6) == 0) {
        ESP_LOGE(TAG, "BRIDGE_PEER_MAC 是本机 MAC，链路一定会哑 —— 填对端那块板的 MAC");
    }

    espnow_link_cfg_t nc = {};
    memcpy(nc.peer_mac, peer, 6);
    nc.channel = CONFIG_BRIDGE_CHANNEL;
    ESP_ERROR_CHECK(espnow_link_init(&nc));

#ifdef CONFIG_BRIDGE_ROLE_SENDER

    uart_link_cfg_t uc = {};
    uc.port        = UART_NUM_1;
    uc.tx_pin      = CONFIG_BRIDGE_TX_GPIO;
    uc.rx_pin      = CONFIG_BRIDGE_RX_GPIO;
    uc.baud        = CONFIG_BRIDGE_BAUD;
    uc.rx_buf_size = 2048;
    uc.task_prio   = 10;
    ESP_ERROR_CHECK(uart_link_init(&uc, on_sensor_data, NULL));

    ESP_LOGI(TAG, "UART%d tx=%d rx=%d %d bps, MTU=%u",
             UART_NUM_1, uc.tx_pin, uc.rx_pin, (int)uc.baud,
             (unsigned)espnow_link_mtu());

#else

    /* UART0 既是 console 又是唯一通往 PC 的路。console 走 ROM 的
       esp_rom_output_tx_one_char()，不占 UART 驱动，所以装驱动不冲突。
       引脚不能动（就是 console 的默认引脚）。

       波特率跟着 console 走，不写死：装驱动会把同一个 UART 的分频器重设一遍，
       所以驱动和 console 本来就是同一个速率，写死成两个数只会在以后改 console
       时悄悄错开。sdkconfig.defaults 里把它定在 115200（IDF 对 C2+26MHz 的
       默认是 74880，见 docs/notes.md 第 2 节）。 */
    uart_link_cfg_t uc = {};
    uc.port        = UART_NUM_0;
    uc.tx_pin      = -1;
    uc.rx_pin      = -1;
    uc.baud        = CONFIG_ESP_CONSOLE_UART_BAUDRATE;
    uc.rx_buf_size = 0;            /* 只写：不建接收任务 */
    ESP_ERROR_CHECK(uart_link_init(&uc, NULL, NULL));

    ESP_LOGI(TAG, "gateway ready, piping to UART0 @115200");

    /* 日志先关，再挂接收回调：否则头几个包会和最后几行日志抢串口 */
    esp_log_level_set("*", ESP_LOG_NONE);
    espnow_link_set_rx_cb(on_air, NULL);

#endif
}
