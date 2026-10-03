#include "uart_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define READ_BUF 512
#define TX_BUF   1024

/*
 * 驱动要求的 RX 环形缓冲下限（必须大于 UART_HW_FIFO_LEN=128）。
 * 网关是只写模式，但 uart_driver_install 传 rx_buffer_size=0 会被判参数错误，
 * 所以哪怕不读也得给一个合法的小缓冲。
 */
#define RX_MIN 256

static uart_port_t       s_port;
static uart_link_rx_cb_t s_cb;
static void             *s_ctx;
static uint8_t           s_buf[READ_BUF];

static void uart_rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        int n = uart_read_bytes(s_port, s_buf, READ_BUF, pdMS_TO_TICKS(100));
        if (n > 0 && s_cb) s_cb(s_buf, (size_t)n, s_ctx);
    }
}

esp_err_t uart_link_init(const uart_link_cfg_t *cfg, uart_link_rx_cb_t cb, void *ctx)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    s_port = cfg->port;
    s_cb   = cb;
    s_ctx  = ctx;

    /* C++ 里不用指定初始化器：v6.1 的 uart_config_t 字段顺序和匿名 union 会踩坑 */
    uart_config_t uc = {};
    uc.baud_rate  = (int)cfg->baud;
    uc.data_bits  = UART_DATA_8_BITS;
    uc.parity     = UART_PARITY_DISABLE;
    uc.stop_bits  = UART_STOP_BITS_1;
    uc.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(cfg->port,
                                        cfg->rx_buf_size > 0 ? cfg->rx_buf_size : RX_MIN,
                                        TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(uart_param_config(cfg->port, &uc));

    /* 两个引脚都是 -1 时不要调 uart_set_pin：让 IOMUX 默认路由保持原样 */
    if (cfg->tx_pin >= 0 || cfg->rx_pin >= 0) {
        ESP_ERROR_CHECK(uart_set_pin(cfg->port,
                                     cfg->tx_pin >= 0 ? cfg->tx_pin : UART_PIN_NO_CHANGE,
                                     cfg->rx_pin >= 0 ? cfg->rx_pin : UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }

    if (cfg->rx_buf_size > 0) {
        xTaskCreate(uart_rx_task, "uart_rx", 3072, NULL, cfg->task_prio, NULL);
    }
    return ESP_OK;
}

int uart_link_write(const uint8_t *data, size_t len)
{
    /* TX_BUF > 0，所以这里是拷贝进环形缓冲即返回，不会阻塞调用者 */
    return uart_write_bytes(s_port, (const void *)data, len);
}
