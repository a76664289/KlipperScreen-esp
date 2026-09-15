# ESP32-C3 Wi-Fi 启动后 USB/画面停滞排障进度

更新时间：2026-09-16

## 用户复现

- 板型：`esp32c3-st7789-320_240-ec11`，原生 USB Serial/JTAG 控制台。
- 开机 Logo 播放结束后，日志固定停在：

  ```text
  I (...) wifi:mode : sta (...)
  I (...) wifi:enable tsf
  ```

- 随后画面看似完全卡死，USB CDC 串口写入超时；看不到任务/中断看门狗复位。
- 板子需要手动拉 GPIO9 进入下载模式，烧录结束后也需要手动 RESET 进入应用。
- Kimi 曾怀疑无 PCNT 芯片上的编码器 GPIO 中断风暴，已把 C3 后端改为 10 ms `esp_timer` 轮询，但症状完全不变。

## 当前设备状态

- 用户最后说明板子已手动进入下载模式，未运行应用。
- 当前枚举端口为 `COM68`，VID:PID=`303A:1001`，MAC/USB 序列为 `44:1B:F6:0B:8D:F8`。
- `COM68` 仍被至少一个 `umeko-serial-mcp` 实例占用；即使用户在一个会话关闭过串口，`esptool chip_id` 仍返回 `PermissionError(13)`。
- **尚未烧录任何诊断固件，也没有修改本问题相关源码。** 明天继续前，先确保所有 Kimi/Codex 会话都执行 `close_port(COM68)`。

## 已确认的执行位置

项目调用链：

```text
app_main
  -> boot_anim_play
  -> ui_app_create
     -> titlebar_init
        -> bsp_wifi_init
           -> ensure_init
              -> esp_wifi_init
              -> esp_wifi_set_mode
              -> esp_wifi_set_ps(WIFI_PS_NONE)
              -> esp_wifi_start      <-- 日志停在其内部/返回边界
```

`bsp_wifi_connect()` 自己的第一条 `bsp_wifi: connect ...` 日志没有出现，因此停止点早于自动连接配置，位于 `esp_wifi_start()` 或其刚返回后的控制台输出路径。

编码器的 `timer-poll` 日志在约 750 ms 出现，而 Wi-Fi 在约 4.5 s 才启动。10 ms 轮询已经陪同整个 2.5 s 开机动画正常运行，因此目前没有证据支持“PCNT/编码器直接导致固定点死机”。

## 高价值发现：与 Espressif 已知问题高度吻合

Espressif 官方 issue 的复现与本机几乎逐字一致：ESP32-C3 开启 Wi-Fi 时，原生 USB Serial/JTAG 在 `wifi:enable tsf` 附近失效，USB 串口/JTAG 均无法继续使用；外接 UART 证明程序本身仍在运行：

- https://github.com/espressif/esp-idf/issues/8046
- https://github.com/espressif/arduino-esp32/issues/6264

Arduino-ESP32 的官方修复 PR：

- https://github.com/espressif/arduino-esp32/pull/6287
- 修复提交：`c8f6698050c3499fbe9089320f57f06adf49a574`

该补丁在 `esp_wifi_init()` 成功后调用：

```c
#if CONFIG_IDF_TARGET_ESP32C3
extern "C" void phy_bbpll_en_usb(bool en);
phy_bbpll_en_usb(true);
#endif
```

作用是 Wi-Fi/PHY 切换 BBPLL 时强制保持 USB CDC+JTAG 时钟开启。

本机 ESP-IDF 5.5.5 已存在此内部接口和调用：

- `C:/esp/v5.5.5/esp-idf/components/esp_phy/include/esp_private/phy.h`
- `C:/esp/v5.5.5/esp-idf/components/esp_phy/src/phy_init.c` 约 943 行已有 `phy_bbpll_en_usb(true)`。

因此下一步不能直接盲目重复旧补丁，需要先确认：

1. 当前芯片 revision；官方文档说明内置 USB JTAG 可靠调试要求 C3 revision v0.3 或更新。
2. IDF 5.5.5 中现有 `phy_bbpll_en_usb(true)` 的编译条件和执行时机是否覆盖当前 sdkconfig/芯片 revision。
3. USB 失效是否只是控制台/JTAG假死，还是高优先级 Wi-Fi 日志阻塞后进一步饿死单核 LVGL。

官方资料：

- https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32c3/api-guides/jtag-debugging/index.html
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/usb-serial-jtag-console.html

## 已尝试但未成功

- 直接用 C3 内置 USB JTAG 附加冻结现场：OpenOCD 报 `LIBUSB_ERROR_NOT_FOUND` / 无法识别目标。
- Windows 中 JTAG 接口确实枚举为 `USB JTAG/serial debug unit`，MI_02 使用 Microsoft WinUSB；失败可能与芯片处于异常/下载状态或 Windows 驱动安装方式有关。
- 不应在未确认前重装驱动或烧 eFuse。

## 内存与看门狗旁证

- 当前 C3 链接结果：静态 DRAM 使用约 146008 B，报告剩余约 175288 B；尚不能代表 UI、双 DMA buffer 和 Wi-Fi 初始化后的动态空闲堆。
- 固件 app 分区仅余约 0x9a20（1%），但这不会解释固定停在 Wi-Fi 启动处。
- sdkconfig 已启用 300 ms Interrupt WDT 和 5 s Task WDT，但 `CONFIG_ESP_TASK_WDT_PANIC` 未启用。若 USB 时钟/控制台先失效，即使任务 WDT报警也可能无法看到；“没有日志复位”不能证明 WDT 没触发。

## 明天建议的最短实验顺序

严格一次只改一个变量：

1. 关闭所有串口占用，保持下载模式，先运行：

   ```powershell
   C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe -m esptool --chip esp32c3 -p COM68 chip_id
   ```

   记录芯片 revision。

2. 第一版诊断固件只增加阶段标记和堆日志：`esp_wifi_init` 前后、`esp_wifi_start` 前后；所有返回值都记录/检查。不要同时改编码器、Wi-Fi buffer 或任务优先级。

3. 为避免“USB 已坏导致后续日志不可见”，同时在画面/背光上提供 Wi-Fi start 返回后的可见标记，或临时把应用控制台改到 UART0（GPIO20 TX / GPIO21 RX）并用外接 USB-UART 采集。

4. 如果 UART/画面证明应用继续运行，则根因是原生 USB Serial/JTAG 与 Wi-Fi/BBPLL；检查并显式调用 IDF 私有 `phy_bbpll_en_usb(true)` 的正确时机，或将 C3 的正式运行日志/CLI改走 UART0。

5. 如果 UART 也证明 CPU 真停在 `esp_wifi_start()`，再测动态堆，并依次试验：
   - 暂停 LVGL 刷新后启动 Wi-Fi（排除 SPI DMA/单核调度交互）；
   - 完全不初始化编码器（最终排除定时器）；
   - 收缩 C3 Wi-Fi buffer 数量（验证内存压力）。

6. happy path 一旦证明立即停止，不同时叠加多个“修复”。

## 工作区注意事项

- 工作区原本已有大量未提交修改，属于用户/Kimi；继续时不要覆盖或回滚。
- 本次排障开始时，C3 编码器的 10 ms timer-poll 改动已经存在于工作区。
- Wi-Fi 代码当前仍会明文和十六进制输出保存的密码；这是独立安全问题，排障输出中不得复制密码，正式合并前应删除这些日志。

