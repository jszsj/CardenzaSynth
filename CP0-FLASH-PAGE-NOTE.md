# Cardenza Synth v0.2.0 — 官方固件 shim 到 CP0/Cardputer 网页烧录

日期 2026-11-17。用户要把官方最新固件刷到 "cardputer 1.0"（作者称硬件核心相同）。

## 事实
- 官方 Release v0.2.0 只发 `CardenzaSynthFirmware.bin`（**app-only**，给 203 Systems Launcher 用；
  md5 20c6ad3d，1.85MB，首字节 0xE9，0x8000 处是 "CANCEL" 文本不是分区表 0xAA50）。**不能单独整片刷**。
- `Devices/Cardenza` 目标：ESP32-S3，分区 nvs@0x9000 / phy_init@0xf000 / factory(app)@0x10000(4M)。
  官方产品页(203.io)实测规格：**ESP32-S3 · 8MB flash · 2MB PSRAM · 56键LED · 240×135 · Compatible with M5Stack Cardputer V1**。sdkconfig 默认 16MB 是作者开发板值，已改为 8MB 重编；codec 硬编码 ES8156（Cardputer 标准版是 ES8311）。作者板若同核心则一致。

## 本地构建（补齐 bootloader+分区表）
用 ESP-IDF v5.5.4（工程原标 5.3.5，5.5 可编）：
```
source ~/esp-idf-v5.5/export.sh
cd Devices/Cardenza && idf.py -B ../../build/Cardenza set-target esp32s3 && idf.py -B ../../build/Cardenza build
```
产物：bootloader.bin(22272) / partition-table.bin(3072) / CardenzaSynthFirmware.bin(1888400, md5 d21e9cc3)
IDF 自身推荐烧录参数：dio / 80m / 16MB @0x0,0x8000,0x10000。

## 烧录页
https://demo.asbox.net/cardenza-flash/ （3 分区 keep-NVS，flashMode dio / freq 80m / size keep，eraseAll=false）
webroot: /home/web/html/demo.asbox.net/cardenza-flash/{index.html, fw/*.bin}
不覆盖现有 /cardenza/（那是官方 web 模拟器）。

## 8MB 修正(2026-11-17 二次构建)
- sdkconfig/sdkconfig.defaults: FLASHSIZE 16MB→8MB（产品页实测 8MB）。
- 重编：esptool 推荐 --flash_mode dio --flash_size 8MB --flash_freq 80m @0x0/0x8000/0x10000。
- app.bin md5=7d6d32b8ea6d45a7c49b73321e00d250（1,888,400B）。页面 flashSize 改为显式 "8MB"。
- 产品页现行地址: https://203.io/products/cardenza-mini-synthsiser （旧 cardenza-synth.203.io NXDOMAIN）; 硬件仓库 203-Systems/Cardenza-Hardware 仅 Cardenza_V1.0_SCH.pdf。

## 黑屏 boot loop 修复(2026-11-17 三次构建)
- 实刷日志根因: bootloader 跑 QIO + 120MHz(作者16MB定制板 W25Q128JV 配置)。用户设备 flash 撑不住 → spi_flash 'detected chip: generic / Detected flash size >16MB' 乱码 → esp_flash_init assert(startup_funcs.c:118) → RTC_SW_CPU_RST 重启循环。
- 同时 quad_psram 'chip not connected' → 用户设备实为 **无 PSRAM**(疑似 M5 Cardputer V1.0，非 Cardenza 原机)，firmware 降级继续。
- 修复: sdkconfig.defaults 改 FLASHMODE QIO→DIO / FLASHFREQ 120M→80M / SPIRAM_SPEED 120M→80M（FLASHSIZE 已 8MB）。删 sdkconfig 从 defaults 重生成。
- 重编产物: dio/8MB/80m @0x0/0x8000/0x10000；app.bin 1,883,296B md5=d5578fa093517322e051cbdbd74744b8。页面 flashFreq=80m flashMode=dio flashSize=8MB。
- codec 仍硬编码 ES8156(仅此一种)，Cardputer V1 若是 ES8311 → 无声(非致命，log 警告继续)。
- 若 80M/DIO 仍 boot 失败 → 下一步换 ESP-IDF 5.3.5(=作者版本)，5.5.4 的 flash timing tuning/HPM 可能与老 flash 不兼容。

## 决定性修复: 对齐 M5 官方 Cardputer 配置(2026-11-17 四次构建)
- 拉 M5Cardputer-UserDemo(main/sdkconfig) 对照: M5 官方 Cardputer = QIO(y, header dio) / FLASHFREQ 80M / FLASHSIZE 8MB + DETECT / 无 SPIRAM。
- 作者 board/sdkconfig.defaults = QIO / 120M / 16MB / PSRAM2MB —— 差异就在 120M vs 80M、16MB vs 8MB、有PSRAM vs 无。**120M 是黑屏真凶**(app 用 120M 跑 flash, RDID 读乱成 >16MB → esp_flash_init assert)。第三次改 DIO 是反向错误(M5 用 QIO)。
- 引脚对照(实锤同板): 键盘 M5 output{8,9,11}/input{13,15,3,4,5,6,7} == CardenzaHAL 完全一致; SD M5 MISO39/MOSI14/CLK40/CS12 == Cardenza SD_CMD14/CLK40/DAT0 39/DAT3 12; I2S 41/42/43; 麦46; 红外44。Cardenza=M5 Cardputer 同款板,仅换 codec(ES8156)+加2MB PSRAM。
- 最终 build: FLASHMODE QIO / FLASHFREQ 80M / FLASHSIZE 8MB+DETECT / SPIRAM_SPEED 80M(SPIRAM=y 无PSRAM自动bail). app.bin 1,883,312B md5=0fa4dc4bb7e8e99ba1e836b909fe044a. esptool: dio/8MB/80m @0x0/0x8000/0x10000.
- 预期: 可启动进界面(显示+键盘+SD 正常), 音频因 ES8156 vs ES8311 无声。需插 microSD.
