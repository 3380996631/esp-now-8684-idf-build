#include "espnow_link.h"
#include <string.h>
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * ESP-NOW 的发送回调在 WiFi 任务上下文里跑（不是 ISR），
 * 所以这里用普通 Give/Take 即可，不需要 FromISR 变体。
 */
static SemaphoreHandle_t     s_tx_done;
static volatile esp_now_send_status_t s_tx_status;

static espnow_link_cfg_t     s_cfg;
static espnow_link_rx_cb_t   s_rx_cb;
static void                 *s_rx_ctx;

static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    s_tx_status = status;
    xSemaphoreGive(s_tx_done);
}

/* ff:ff:.. 是 ESP-NOW 的广播地址，也当"填不出 MAC 时先跑通"的哨兵值 */
static bool mac_is_wildcard(const uint8_t m[6])
{
    const uint8_t ff[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t z[6]  = {0};
    return memcmp(m, ff, 6) == 0 || memcmp(m, z, 6) == 0;
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0) return;
    /* 单播 + peer 过滤：不是对端来的直接丢，省得别人的包混进串口流 */
    if (!mac_is_wildcard(s_cfg.peer_mac) &&
        memcmp(info->src_addr, s_cfg.peer_mac, 6) != 0) return;
    if (s_rx_cb) s_rx_cb(data, (size_t)len, s_rx_ctx);
}

esp_err_t espnow_link_init(const espnow_link_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;

    s_tx_done = xSemaphoreCreateBinary();
    if (!s_tx_done) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 不需要 esp_netif_create_default_wifi_sta()：官方 ESP-NOW 示例也没调，
       链接层直接用 WiFi 接口，没走 IP 栈 */
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 这两条都必须在 esp_wifi_start() 之后。
       PS_NONE：默认 modem sleep 会带来上百毫秒唤醒延迟并丢 ESP-NOW 帧 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(cfg->channel, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, cfg->peer_mac, 6);
    peer.channel = 0;              /* 0 = 跟随当前信道 */
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;          /* 明文：省掉整套 PKM/LMK 配置 */
    return esp_now_add_peer(&peer);
}

void espnow_link_set_rx_cb(espnow_link_rx_cb_t cb, void *ctx)
{
    s_rx_cb  = cb;
    s_rx_ctx = ctx;
}

size_t espnow_link_mtu(void)
{
    return ESP_NOW_MAX_DATA_LEN;   /* 250，减包头是 bridge_proto 的事 */
}

esp_err_t espnow_link_send_sync(const uint8_t *data, size_t len)
{
    if (len == 0 || len > ESP_NOW_MAX_DATA_LEN) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_tx_done, 0);              /* 清掉上一轮可能残留的信号 */
    esp_err_t err = esp_now_send(s_cfg.peer_mac, data, len);
    if (err != ESP_OK) return err;

    /* ponytail: 500ms 是等 ACK 的上限。真超时说明对端不在范围内，
       这里只报错不重传——重传等于往一个收不到的对端继续排队。
       实测发现范围内丢包再考虑失败重发一次。 */
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    return (s_tx_status == ESP_NOW_SEND_SUCCESS) ? ESP_OK : ESP_FAIL;
}

void espnow_link_get_mac(uint8_t out[6])
{
    esp_read_mac(out, ESP_MAC_WIFI_STA);
}
