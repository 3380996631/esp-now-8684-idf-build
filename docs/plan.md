# ESP32-C2 串口透传系统（UART → ESP-NOW → UART → PC）

## Context

现状：[main/main.cpp](main/main.cpp) 是一个 57 行的单文件程序，从 UART1 读传感器数据然后打日志。能用，但所有东西挤在一起，且数据出不去这块板子。

目标拓扑：

```
传感器 ──UART1 38400──> C2#1(发送端) ──ESP-NOW──> C2#2(网关) ──UART0/USB──> PC
```

要把这个做成：**字节透明**（PC 收到的字节序列与传感器发出的完全一致）、**结构清晰**（可复用逻辑独立成组件，业务装配留在 main）、**一块代码两种角色**。

已确认的决定：ESP-NOW 无线传输 · IDF 原生 `components/` 布局 · 单工程 + Kconfig 选角色 · 网关启动时开日志、初始化完关。

## 已核实的环境事实

全部读自本机安装的 IDF v6.1 头文件，不是凭记忆：

| 事实 | 出处 |
|---|---|
| ESP-NOW 支持 C2 | 官方 `examples/wifi/espnow/README.md` 的 Supported Targets 表首行含 ESP32-C2 |
| `esp_now.h` 由 `esp_wifi` 组件提供 | `components/esp_wifi/include/esp_now.h` |
| **发送回调签名是 `(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)`** | esp_now.h:127 |
| 接收回调签名是 `(const esp_now_recv_info_t *info, const uint8_t *data, int len)` | esp_now.h:120 |
| **ESP-NOW 不需要 `esp_netif_create_default_wifi_sta()`** | 官方示例只调 `esp_netif_init()` + `esp_event_loop_create_default()`，没有 create_default_wifi_sta |
| `ESP_NOW_MAX_DATA_LEN = 250`（v1.0） | esp_now.h:35-36 |
| **console 走 ROM 函数 `esp_rom_output_tx_one_char()`，不占用 UART 驱动** | `components/esp_stdio/stdio_simple.c:14` |
| C2 无 USB 控制器 | `SOC_USB_SERIAL_JTAG_SUPPORTED` 未定义 |
| `SOC_UART_NUM = 2`，可用 GPIO 仅 0–10、18–20 | soc_caps.h |
| 晶振已配 26MHz | `CONFIG_XTAL_FREQ_26=y` |

因为 console 走 ROM 而非驱动，**在 UART0 上装 UART 驱动不冲突**——这是网关方案成立的前提。

## 目录结构

```
components/
  uart_link/        一个 UART 口：配置 + 读任务 + 回调 / 阻塞写
  espnow_link/      ESP-NOW 起来 + 一个 peer；收发不透明包
  bridge_proto/     纯分帧编解码，不依赖 IDF，可在 PC 上跑测试
main/
  main.cpp          app_main：角色分发，把组件接起来
  bridge_config.h   Kconfig 宏 → 配置结构体
  Kconfig.projbuild 角色/波特率/引脚/信道/peer MAC
```

三个组件，不多不少。每个都有独立的变更理由，且都被 main 使用：

- **`uart_link`** — 知道：一个 `uart_port_t`、引脚、波特率。**不知道**：ESP-NOW、分帧、角色、Kconfig。引脚以 int 传入，不认识 GPIO 宏。
- **`espnow_link`** — 知道：NVS/netif/event/wifi/esp_now 的启动顺序、一个 peer MAC、≤250 字节收发。**不知道**：UART、分帧、角色。
- **`bridge_proto`** — 知道：5 字节包头和序号运算。**不包含任何 IDF 头文件**，只有 `<stdint.h><stdbool.h><string.h>`。这是它能脱离硬件测试的原因。

## 组件 API

### `components/uart_link/include/uart_link.h`

```c
typedef struct {
    uart_port_t port;
    int         tx_pin;       // -1 = UART_PIN_NO_CHANGE
    int         rx_pin;       // -1 = UART_PIN_NO_CHANGE
    uint32_t    baud;
    int         rx_buf_size;  // 0 => 只写模式：不装驱动、不建任务、忽略回调
    int         task_prio;
} uart_link_cfg_t;

typedef void (*uart_link_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

esp_err_t uart_link_init(const uart_link_cfg_t *cfg, uart_link_rx_cb_t cb, void *ctx);
int       uart_link_write(const uint8_t *data, size_t len);   // 阻塞
```

回调在 UART 读任务上下文触发，**调用方不得在里面长时间阻塞**。

### `components/espnow_link/include/espnow_link.h`

```c
typedef struct {
    uint8_t peer_mac[6];      // 对端那块板
    uint8_t channel;          // 1..13，必须显式设置
} espnow_link_cfg_t;

typedef void (*espnow_link_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

esp_err_t espnow_link_init(const espnow_link_cfg_t *cfg);
void      espnow_link_set_rx_cb(espnow_link_rx_cb_t cb, void *ctx);
size_t    espnow_link_mtu(void);                                   // 返回 250
esp_err_t espnow_link_send_sync(const uint8_t *data, size_t len);  // 阻塞到发完
```

`mtu()` 返回裸值 250，**不减包头**——那是 `bridge_proto` 的事。`send_sync` 用信号量把发送回调包起来，main 不用碰回调。**不提供** deinit、统计接口、tx 完成回调，等真有调用方要再加。

### `components/bridge_proto/include/bridge_proto.h`

```c
#define BRIDGE_MAGIC       0xA5
#define BRIDGE_HDR_LEN     5
#define BRIDGE_MAX_PAYLOAD 245           // 250 - 5

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint16_t seq;    // 每包递增，回绕
    uint16_t len;    // 本包载荷字节数
} bridge_hdr_t;

size_t bridge_encode(uint16_t seq, const uint8_t *payload, size_t len,
                     uint8_t *out, size_t out_cap);

typedef struct { uint16_t expected; uint32_t lost_pkts; bool synced; } bridge_reasm_t;

// 校验并追加载荷。检测到丢包时 *lost 返回跳过的包数。返回追加的字节数（0 = 丢弃/重复/非法）。绝不重排序。
size_t bridge_decode(bridge_reasm_t *st, const uint8_t *pkt, size_t pkt_len,
                     uint8_t *out, size_t out_cap, uint16_t *lost);
```

直接用 `memcpy` 处理 packed 结构是安全的，**前提是两端都是 ESP32-C2**（同架构、同小端）。这一点要写进注释——它正是不值得手写逐字节序列化器的原因。

## 分帧设计（最关键的部分）

约束：ESP-NOW 单包硬上限 250 字节且面向包；UART 是字节流，一块 1000 字节的传感器帧可能被拆成多次 `uart_read_bytes`，也可能好几个小帧挤在一次调用里返回；ESP-NOW 单播有 MAC 层 ACK 但**默认不重传**，且不保证顺序。

**线上格式（5 字节包头 + 245 字节载荷）：**

```
magic(1)=0xA5 | seq(2, LE) | len(2, LE) | payload(len)
```

**发送端**：读到多少字节都无所谓，一律按 245 切块，每块 `seq++` 后编码发送。缓冲区大小不影响正确性——切块点永远在 245 边界上。

**网关**（`bridge_decode`，永不重排序）：

- `seq == expected` → 追加载荷，`expected++`
- `(int16_t)(seq - expected) > 0` → 中间有丢包：仍然追加（保证向前推进），`expected = seq+1`，`lost_pkts += 差值`
- 其余（迟到/重复）→ **丢弃**，不追加任何字节

对回绕的 `uint16_t` 做有符号差值，让跨越回绕点也正确，无需特判。

**明确放弃了什么：**

- **不做重传/ARQ。** 正确性保证是*如果无丢包且无乱序，输出与输入逐字节相同*。丢包只**检测**（seq 空洞记进 `lost_pkts`）不**修复**。理由：单播链路下发失败意味着对端不在范围内，重传只是往不在范围的对端继续排队；在范围内的话 ESP-NOW 自己会 ACK。**升级路径**（若实测发现范围内丢包）：发送回调报失败时重发当前包一次，`ponytail:` 标注，约 6 行。
- **不在带内插丢包标记。** 任何标记字节都会破坏字节透明性（载荷是任意字节）。丢包只通过 `lost_pkts` 计数暴露，而网关日志关了之后读不到——所以它是调试期工具，不是运行时信号。PC 端看不到空洞；要感知完整性得靠传感器协议自身的校验和。
- **乱序时丢弃而非拼接。** 迟到的包其字节属于流的更早位置，追加会破坏顺序。丢弃丢字节但保持流单向推进，对下游解析器更安全。
- **不加校验和。** ESP-NOW 在 802.11 层已有 CRC 且单播有 ACK，载荷 CRC 只能重复检出射频已经检出的问题。
- **16 位序号。** 有符号差值处理让 16 位在回绕下依然正确，省 2 字节。

## 角色配置

`main/Kconfig.projbuild`：

```
menu "Serial bridge"
choice BRIDGE_ROLE
  prompt "Role"
  default BRIDGE_ROLE_SENDER
config BRIDGE_ROLE_SENDER   bool "Sender (UART RX -> ESP-NOW)"
config BRIDGE_ROLE_GATEWAY  bool "Gateway (ESP-NOW -> UART0 TX)"
endchoice
config BRIDGE_BAUD      int    "Sensor baud"          default 38400
config BRIDGE_TX_GPIO   int    "Sensor UART TX GPIO"  default 7
config BRIDGE_RX_GPIO   int    "Sensor UART RX GPIO"  default 6
config BRIDGE_CHANNEL   int    "ESP-NOW channel"      default 1
config BRIDGE_PEER_MAC  string "Peer MAC aa:bb:cc:dd:ee:ff" default "ff:ff:ff:ff:ff:ff"
endmenu
```

保持解耦的几条规矩：

- 组件**不 include `sdkconfig.h`**、不读全局变量。`main/bridge_config.h` 在 `app_main` 里把 Kconfig 宏转成两个 cfg 结构体各一次，按指针传下去。
- **网关的引脚不进 Kconfig**：UART0/GPIO19/GPIO20 由板子固定。只有发送端的 GPIO Matrix 引脚是可选项，因为只有它真的是个选择。少一个旋钮。
- MAC 解析就一行 `sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", ...)`，不写辅助函数。
- `main.cpp` 用 `#if CONFIG_BRIDGE_ROLE_SENDER` 选配置。

## ESP-NOW 初始化顺序（v6.1 实测）

```c
nvs_flash_init();                        // 失败且是 NO_FREE_PAGES/NEW_VERSION 则 erase 后重试
esp_netif_init();
esp_event_loop_create_default();         // 注意：不需要 esp_netif_create_default_wifi_sta()
wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_init(&wc);
esp_wifi_set_storage(WIFI_STORAGE_RAM);
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_start();
esp_wifi_set_ps(WIFI_PS_NONE);           // 必须在 start 之后
esp_wifi_set_channel(cfg->channel, WIFI_SECOND_CHAN_NONE);   // 必须在 start 之后

esp_now_init();
esp_now_register_send_cb(send_cb);       // void(const esp_now_send_info_t*, esp_now_send_status_t)
esp_now_register_recv_cb(recv_cb);       // void(const esp_now_recv_info_t*, const uint8_t*, int)

esp_now_peer_info_t peer = {};
memcpy(peer.peer_addr, cfg->peer_mac, 6);
peer.channel = 0;                        // 0 = 用当前信道
peer.ifidx   = WIFI_IF_STA;
peer.encrypt = false;
esp_now_add_peer(&peer);
```

两个角色跑**完全相同**的初始化。接收回调里按 `info->src_addr` 过滤，非目标 MAC 直接丢弃。`encrypt = false` 换来零 PKM/LMK 配置——点对点台面链路合理，但要记着这是明文。

## 网关的 UART0 处理

网关 UART0 既是 console 又是唯一通往 PC 的路。做法：

1. `uart_driver_install(UART_NUM_0, 256, 1024, 0, NULL, 0)` —— RX 缓冲给 256（**不能传 0**，头文件明确要求 rx_buffer_size 大于 FIFO 长度 128，传 0 会被判参数错误），虽然永远不读。
2. `uart_param_config(UART0, 115200 8N1)` —— 与现有 console 配置一致，**不要改波特率**。
3. **不调 `uart_set_pin`** —— 引脚就是 console 的默认引脚，不需要动。
4. 启动阶段日志正常打；`espnow_link_init` 返回后立刻 `esp_log_level_set("*", ESP_LOG_NONE)`，**然后**才注册接收回调，否则头几个包会和最后几行日志抢串口。

## 验证

**主机端单元测试（不需要硬件，这是唯一有真实逻辑的地方，必须有）**

`bridge_proto` 不依赖 IDF，用普通 gcc 直接编：

```
gcc -I components/bridge_proto/include components/bridge_proto/bridge_proto.c \
    components/bridge_proto/test_bridge_proto.c -o /tmp/t && /tmp/t
```

测试用例（`assert` 即可，不引框架）：

1. 往返：随机 600 字节 → 切成 245×2 + 110 → 逐包 encode/decode → 输出与输入逐字节相同
2. 丢包：喂 seq 0,1,3,4 → 输出等于输入去掉第 2 包，`lost_pkts == 1`，且不因丢包卡死
3. 乱序：喂 seq 0,2,1,3 → 第 3 包（seq 1，迟到）被丢弃，`expected` 保持前进
4. 回绕：从 seq 65534 连发 4 包跨过 0 → 无假丢包，`lost_pkts == 0`
5. 非法包：magic 错、len 超 245、pkt_len 与 len 不符 → 返回 0 且不越界写

**硬件端到端**

1. `idf.py -p <COM> flash monitor`，确认启动日志里打印出各自的 MAC
2. 把 C2#1 的 MAC 填进 C2#2 的 `BRIDGE_PEER_MAC`，反之亦然，各自重编重烧
3. 两块板 `BRIDGE_CHANNEL` 必须相同
4. 传感器端短接发数据，或用 `RX_LOOPBACK_TEST` 自环
5. PC 端打开网关的串口（115200，**注意跳过重启时的 ROM 启动横幅**），确认收到的字节与传感器发出的一致
6. 用规则文本（如周期性发 `0123456789\n`）验证最直观——错位、丢包、乱码一眼可见

## 风险（按可能性排序）

1. **1MB app 分区够不够——用实测数字说话，别猜。** 实测各 blob 的真实代码段（`riscv32-esp-elf-size -t`，已排除调试信息）：

   | 库 | 代码量 | 是什么 |
   |---|---|---|
   | `libnet80211.a` | 228 KB | 802.11 协议栈 |
   | `libpp.a` | 90 KB | 包处理/驱动层 |
   | `libphy.a` | 23 KB | 射频校准 |
   | `libespnow.a` | **13 KB** | ESP-NOW 本体 |
   | 合计 | **~354 KB** | |

   想要的功能只占 13 KB，其余是它必须站着的地基。

   这是 `.a` 全集，链接器还能靠 `--gc-sections` 裁——两个 blob 都带 function sections（`libnet80211.a` 有 902 个 `.text.*` 分节，`libpp.a` 有 623 个），所以进 bin 的少于此数。可能被拖进来的外围（均为上限值，大部分不会被引用）：wpa_supplicant 213 KB、mbedcrypto 177 KB、lwip 165 KB。

   预期落在 **450~650 KB**（现有 152 KB + 300~500 KB），对 1 MB 分区**大概率够**。**第一步仍是先写最小 ESP-NOW 骨架编译一次实测体积**，但这是核实，不是危机预案。

   真超了，按此顺序削：

   a. **关掉用不到的认证功能。** 纯 STA 模式明文 ESP-NOW 不关联任何 AP，下面这些当前都是 `y` 却一个都用不上——它们正是把 wpa_supplicant 和 mbedcrypto 往里拖的原因，通常能省 100~200 KB：

   ```
   CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n
   CONFIG_ESP_WIFI_ENABLE_SAE_H2E=n
   CONFIG_ESP_WIFI_ENABLE_SAE_PK=n
   CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=n
   CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA=n
   CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_SOFTAP=n
   CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT=n
   CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n
   CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n
   ```

   b. `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` —— 2MB flash 下 factory 从 1M 变 1500K。
   c. 自定义 `partitions.csv`。

   注意 a 是单向的：关掉之后以后想加 WiFi 联网得挨个找回来。所以**别提前砍**，只在实测真的超了之后再动。
2. **信道不匹配 = 静默全失败。** 两块板都不连 AP，信道靠各自配置决定，不同就一个包都收不到且不报错。两端都显式 `esp_wifi_set_channel` 且值相同。
3. **电源管理丢包。** 默认 modem sleep 会带来上百毫秒的唤醒延迟并可能丢 ESP-NOW 帧。`esp_wifi_set_ps(WIFI_PS_NONE)`，且在 `esp_wifi_start()` 之后调。
4. **PC 端会先收到 ROM 启动横幅。** 应用跑起来之前的启动日志是不可避免的，PC 端要么忽略重启后的前若干字节，要么等一会儿再开始读。
5. **静默丢包。** 不重传意味着丢一个包就是一个 PC 无法察觉的空洞。清楚这一点再去排查"传感器数据损坏"。
6. **`FREERTOS_HZ=100`，`pdMS_TO_TICKS(1)` 等于 0。** 不要用 1ms 延时；所有等待按 ≥10ms 或事件驱动表达。
7. **别把引脚改到 GPIO8/9（strapping）或 18/19/20（含 UART0）。** 发送端现在用 6/7，在 0–10 的有效范围内，是对的。

## 发送端要不要加队列？

**不加。** 38400 波特率约 3.8KB/s，245 字节一包约 16 包/秒。阻塞式 `send_sync` 把读任务的节奏压得远低于上限，ESP-NOW 自身还有内部发送队列。再加一个 StreamBuffer + 独立发送任务，只是多一个任务、一次拷贝和一个故障模式。

真正的上限在别处：`send_sync` 阻塞期间，UART RX 环形缓冲（2048 字节 ≈ 38400 下的 0.53 秒余量）仍在填。射频卡住超过这个时间就会静默丢字节。检测方法是在读循环里监听 UART FIFO 溢出事件并计数——加 `ponytail:` 注释标注这个天花板和升级路径（真要超了再上 StreamBuffer + 发送任务）。
