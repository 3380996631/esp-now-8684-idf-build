#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 一个 UART 口的收发封装。
 *
 * 本组件只知道自己管着哪个 uart_port_t、引脚在哪、波特率多少。
 * 它不知道 ESP-NOW、不知道分帧、不知道自己是发送端还是网关，
 * 引脚以 int 传入，不认识任何 GPIO 宏。
 *
 * 单实例：一块板子只用得上一个业务串口，不需要多实例支持。
 */

typedef struct {
    uart_port_t port;
    int         tx_pin;       /* -1 = UART_PIN_NO_CHANGE（保持 IOMUX 默认） */
    int         rx_pin;       /* -1 = UART_PIN_NO_CHANGE */
    uint32_t    baud;
    int         rx_buf_size;  /* 0 => 只写模式：不建接收任务，回调被忽略 */
    int         task_prio;    /* 只写模式下无意义 */
} uart_link_cfg_t;

/* 在 UART 接收任务上下文里触发。调用方不得在其中长时间阻塞。 */
typedef void (*uart_link_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

esp_err_t uart_link_init(const uart_link_cfg_t *cfg, uart_link_rx_cb_t cb, void *ctx);

/* 阻塞写。返回写入字节数。 */
int uart_link_write(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
