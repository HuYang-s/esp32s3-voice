# tts_only (ESP-IDF v5.4)

最小可编译的 ESP-IDF v5.4 工程骨架，目标芯片为 `esp32s3`。

## 先决条件
- 已安装并 `export` 的 ESP-IDF v5.4 环境（`idf.py --version` 可查看）
- USB 串口权限已配置

## 构建与烧录
```bash
cd tts_only
idf.py set-target esp32s3
idf.py build
# 端口示例按实际修改
idf.py -p /dev/ttyACM0 flash monitor
```

如需使用 UART 而非 USB Serial/JTAG，请修改 `sdkconfig.defaults` 中的控制台相关配置，然后重新 `idf.py build`。