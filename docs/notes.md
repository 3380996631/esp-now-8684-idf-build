# 实测记录与操作手册

[plan.md](plan.md) 是设计文档，这份是**跑起来之后才知道的事**：硬件对应关系、
两个容易踩的坑、验证方法。全部数字都是 2026-10-02 在实物上量出来的。

---

## 1. 硬件与端口对应

| 端口 | 角色 | MAC | 传感器 |
|---|---|---|---|
| COM7 | 网关 gateway | `d8:bc:38:09:cf:b8` | — |
| COM8 | 发送端 sender | `d8:bc:38:0a:9f:84` | GNSS 模块，38400 |

因此两块板的 `BRIDGE_PEER_MAC` 是**交叉**的：

- 发送端(COM8) 填 `d8:bc:38:09:cf:b8`
- 网关(COM7) 填 `d8:bc:38:0a:9f:84`

跳线：传感器 TX → **GPIO6**（ESP 的 RX），传感器 RX → **GPIO7**。两端信道都是 1。

端口号不保证稳定，重新插拔后对不上就用这个重读（只读，不写 flash）：

```bash
python -m esptool --chip esp32c2 -p COM7 read-mac
```

---

## 2. 启动日志的波特率（曾经是 74880，已改 115200）

**这块最容易浪费半小时，而且早期这份文档给的解释是错的。**

之前量到 console 是 74880，文档里写成"ROM 按 40MHz 晶振算分频，实际 26MHz，
于是 115200 × 26/40 = 74880"。查完源码后：**74880 不是算出来的，是 IDF 给
C2 + 26MHz 的 Kconfig 默认值**，`components/esp_stdio/Kconfig:154`：

```kconfig
config ESP_CONSOLE_UART_BAUDRATE
    int
    prompt "UART console baud rate" if ESP_CONSOLE_UART_CUSTOM   # ← 注意这个 if
    default 74880 if (IDF_TARGET_ESP32C2 && XTAL_FREQ_26)
    default 115200
```

IDF 自己的测试套件里 `esp32c2_xtal26m` 板子也一律配 74880，所以这是有意为之。
IDF 侧不存在 26/40 错配：C2 上 `ESP_ROM_UART_CLK_IS_XTAL=1`，IDF 调
`_uart_ll_set_baudrate(..., CONFIG_ESP_CONSOLE_UART_BAUDRATE, esp_clk_xtal_freq())`
用的是真实的 26MHz，分频算得对。（`74880 = 115200 × 26/40` 这个数值本身没错，
但它对应的是 **ROM 自己的**固定分频——ROM 里写死 40MHz 假设，所以芯片一复位、
IDF 还没跑起来时那几行是 74880。IDF 把默认值也设成 74880 就是为了跟它对齐。）

**已经改成 115200**，写在 [sdkconfig.defaults](../sdkconfig.defaults) 里：

```
CONFIG_ESP_CONSOLE_UART_CUSTOM=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
```

两行必须一起写。原因：那个 prompt 带了 `if ESP_CONSOLE_UART_CUSTOM`，走默认的
`ESP_CONSOLE_UART_DEFAULT` 时**这个值在 menuconfig 里根本不显示**，是个隐形旋钮。

**只改 sdkconfig 不生效**——kconfig 每次重新生成都会用上面那条条件默认值盖回来
（实测连改两次、`ESP_CONSOLE_UART_CUSTOM=y` 已经在文件里了照样被打回 74880）。
`sdkconfig.defaults` 才压得住（实测生效）。

改完要**两块板都重烧**才生效。烧完之后：

- 网关那根 UART0 上速率统一了，全是 115200 —— 日志和数据一个速率，
  PC 端开 115200 就能同时看到两者，不再有"先一段乱码横幅再正常数据"。
- `idf.py monitor` / `flash` 会自己读 sdkconfig 里的波特率，跟着变成 115200。
- 唯一还会乱的是复位后 **ROM 自己打的那几行**（在 IDF 拿到控制权之前），
  预计仍是 74880 读出来是乱码。看到开头几个乱码字节属正常。

---

## 3. 换角色时必须同时换 PEER_MAC

`BRIDGE_ROLE` 和 `BRIDGE_PEER_MAC` 在同一个 Kconfig 里，切角色时**很容易只翻
前者**。填成本机 MAC 后果是：ESP-NOW 收发的 MAC 过滤会把所有包丢掉，
**没有任何报错，就是没数据**。

已经在 [main.cpp](../main/main.cpp) 加了防呆，填错会直接报：

```
E (478) bridge: BRIDGE_PEER_MAC 是本机 MAC，链路一定会哑 —— 填对端那块板的 MAC
```

看到这行就说明该改 Kconfig 了。

---

## 4. 发送端重启：曾经会哑很久

**修复前实测**：发送端跑了一段时间后只重启它，网关的恢复时间 **76.7 秒**
（积累阶段 407 B/s，重启后 0~70s 累计 0 字节）。

规律是 **哑掉的时长 ≈ 发送端重启前已运行的时长**。因为发送端每次启动 `seq`
从 0 重来，而网关的 `expected` 已经是 N，把新流全判成"迟到包"丢弃，要等
`seq` 爬回 N 才重新接受——又需要同样长的时间。**运行一小时后掉电，网关要哑一小时。**

**修复后**：**1.5 秒**（基本就是发送端自己的启动时间，重启后第一个包就被接受）。

做法是 [bridge_proto.c](../components/bridge_proto/bridge_proto.c) 里按回退幅度区分：

- 序号小幅倒退 → 真·迟到包，照旧丢弃
- 序号倒退超过 `BRIDGE_RESYNC_GAP`（8 个包）→ 判为发送端重启，重新对齐

直连 ESP-NOW 不会把包乱序 8 个以上，所以这个阈值是安全的。代价是发送端运行
不足 8 个包就重启时，网关最多哑约 5 秒。

---

## 5. 怎么验证链路是好的

传感器是 GNSS 模块，输出 NMEA，每句自带 XOR 校验和——**这是白送的端到端完整性检查**。

网关口按 115200 打开，抓 20 秒，逐句验校验和：

```bash
# 期望结果：240 句校验和全通过，0 句截断
```

正常状态下的基线数字：

| 指标 | 值 |
|---|---|
| 吞吐 | 369 B/s（38400 带宽只用了约 10%） |
| 句子速率 | 12 句/秒，RMC/VTG/GGA 各 1 Hz |
| 校验和 | 240/240 通过 |
| 截断句子 | 0 |
| ESP-NOW 包速率 | ≈1.5 包/秒（245 字节切块） |

**判定标准**：出现校验失败的句子或截断的句子（`$` 数量 > 完整解析句数），
就说明字节流被破坏了；丢包会正好切在某句中间，必然留下截断句。
各句子类型的条数应该精确相等（GSA 是 4 倍，因为有 4 个星座），少几条就是丢了包。

---

## 6. 单元测试

`bridge_proto` 的序号逻辑有 6 组用例（往返 / 丢包 / 乱序 / 回绕 / 发送端重启 / 非法包），
**开机自动跑**，失败直接 assert 崩在启动阶段。

在没有硬件的机器上用普通 gcc 跑：

```bash
gcc -DBRIDGE_PROTO_HOST_TEST -I components/bridge_proto/include \
    components/bridge_proto/bridge_proto.c \
    components/bridge_proto/test_bridge_proto.c -o /tmp/t && /tmp/t
```

注意：本机没有 host C 编译器（无 gcc/clang/MSYS2），所以这条命令跑不了，
只能靠开机自检。
