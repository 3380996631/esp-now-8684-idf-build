#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESP-NOW 收发封装：一个对端、一个信道、不透明字节包。
 *
 * 本组件只知道自己要跟哪个 MAC 说话、在哪个信道上。
 * 它不知道 UART、不知道分帧、不知道自己是发送端还是网关。
 */

typedef struct {
    uint8_t peer_mac[6];   /* 对端那块板 */
    uint8_t channel;       /* 1..13，两端必须相同 */
} espnow_link_cfg_t;

typedef void (*espnow_link_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

/* 起 NVS/netif/event/wifi/esp_now 并加对端。两个角色调的是同一个函数。 */
esp_err_t espnow_link_init(const espnow_link_cfg_t *cfg);

/* 只接受来自 cfg->peer_mac 的包，其余丢弃。在 WiFi 任务上下文触发。 */
void espnow_link_set_rx_cb(espnow_link_rx_cb_t cb, void *ctx);

/* 单个 ESP-NOW 包能装多少字节（不含任何自有包头）。 */
size_t espnow_link_mtu(void);

/* 阻塞到发送回调返回。len <= mtu()。 */
esp_err_t espnow_link_send_sync(const uint8_t *data, size_t len);

/* 本机 STA MAC，打印出来好填进对端的 BRIDGE_PEER_MAC。 */
void espnow_link_get_mac(uint8_t out[6]);

#ifdef __cplusplus
}
#endif
