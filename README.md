# Tencent IoT SDK —— ESP32 源码交付包

本包是即插即用的 ESP-IDF 工程：
- demo 全量源码（`iot_video_demo` + `iot_aitalk_demo`）
- 预编 `libtc_iot_sdk.a`（含 TRTC-c 与本包启用的 IoT 模块）
- 7 个 public API 头 + HAL_Audio 必要的 `hal/*.h`

demo 仅依赖 `tc_iot_sdk/include/` 下的 `tc_iot_*.h`。

## 软件版本

- **ESP-IDF**：`v6.0.1`（必须严格匹配，混版本会因 PSA 加密曲线裁剪导致 TRTC ECDH 失败）
- **支持芯片**：`ESP32-P4`、`ESP32-S3`（**单包仅 1 个 target**，由 sdkconfig 文件名决定）

ESP-IDF 安装：

- [ESP32-P4 快速入门](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.0.1/esp32p4/get-started/index.html)
- [ESP32-S3 快速入门](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.0.1/esp32s3/get-started/index.html)
- [ESP-IDF GitHub Release v6.0.1](https://github.com/espressif/esp-idf/releases/tag/v6.0.1)

## 目录结构

```
.
├── CMakeLists.txt
├── sdkconfig.defaults
├── sdkconfig.<target>.v6.0.1                    # 当前包仅含一个 target 的默认配置
├── partitions.csv
├── spiffs_data/                                 # SPIFFS 镜像（aitalk_send_audio.wav 等）
├── main/                                        # 工程入口（console + Wi-Fi + demo 命令注册）
└── components/
    ├── cmd_iot_demo/                            # demo 全量源码（video + aitalk）+ HAL_Audio_esp.c
    ├── tc_iot_sdk/
    │   ├── include/                             # 7 个 public API 头 + hal/ 子集
    │   ├── lib/libtc_iot_sdk.a                  # 自包含静态库（含 TRTC-c + IoT + 本包启用模块）
    │   ├── module_config.cmake                  # 本包 .a 实际启用的模块
    │   └── CMakeLists.txt                       # 把 .a 注册为 IDF prebuilt 组件
    ├── cmd_nvs/  cmd_system/  cmd_wifi/         # 调试 console 子命令
    └── ...
```

## 编译、烧录与运行

确认 IDF 版本：

```shell
idf.py --version
```

必须为：

```shell
ESP-IDF v6.0.1
```

拷默认 sdkconfig（`<target>` 为 `esp32s3` 或 `esp32p4`，与本包内 `sdkconfig.<target>.v6.0.1` 文件名一致）：

```shell
cp sdkconfig.<target>.v6.0.1 sdkconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

烧录成功后串口先连 Wi-Fi：

```shell
join <ssid> <password>
```

联网后即可使用 `components/cmd_iot_demo/cmd_iot_demo.c` 中注册的命令。

## 设备三元组 / Wi-Fi

编译前修改 `components/cmd_iot_demo/iot_video_demo/iot_demo_config.h`：

- `IOT_PRODUCT_ID` / `IOT_DEVICE_ID` / `IOT_DEVICE_SECRET`：从 IoT 控制台获取
- `IOT_DEFAULT_CALLEE`：默认呼叫对端 ID
- `IOT_WIFI_SSID` / `IOT_WIFI_PASSWORD`：Wi-Fi 凭据（也可串口 `join` 命令运行时输入）

## IoT 视频通话命令

### 启动 IoT 视频 Demo

```shell
iot_start
```

启动后使用工程配置中的 `IOT_PRODUCT_ID` / `IOT_DEVICE_ID` / `IOT_DEVICE_SECRET` 与默认空区域（国内站）初始化 IoT SDK，并在后台运行音视频服务。

命令参数（兼容外部输入；实际启动用工程配置）：

```shell
iot_start -p <product_id> -d <device_id> -s <device_secret>
```

### 停止 IoT 视频 Demo

```shell
iot_stop
```

### 主动呼叫

```shell
iot_call -u <user_id>            # 默认音视频，清晰度 SD
iot_call -u <user_id> -c 1       # 仅音频
```

参数：

- `-u <user_id>`：目标用户 ID，必填
- `-c <1=audio>`：媒体类型，可选；`1` 表示仅音频，未传时为音视频

### 接听来电

```shell
iot_accept -u <user_id>
```

### 拒绝来电

```shell
iot_reject -u <user_id>
```

### 挂断通话

```shell
iot_hangup -u <user_id>
```

## AI 对话命令

### 启动 AI 对话 Demo

```shell
iot_aitalk
```

启动后使用工程配置中的设备三元组、默认空区域（国内站）与默认 `IOT_AITALK_BOT_ID` 连接 AI 对话服务。

指定参数启动：

```shell
iot_aitalk -p <product_id> -d <device_id> -s <device_secret> -b <bot_id>
```

### 停止 AI 对话 Demo

```shell
iot_aitalk_stop
```

## 模块裁剪

`components/tc_iot_sdk/module_config.cmake` 记录本预编 `.a` 实际启用的模块（`BUILD_IOT_AV` / `BUILD_IOT_CLOUD_STORAGE` / `BUILD_IOT_AITALK`）。

如启用集合与你的 demo 调用不一致（如本包 `BUILD_IOT_AITALK=0` 但你需要 AI 对话），请联系 SDK 提供方按需重新出包。

## 说明

- ESP32 仅支持 C 版 TRTC（`.a` 已含 TRTC-c）。
- 默认 sdkconfig 启用 PSIRAM、SPIFFS、esp_audio_codec 等，避免随意 `make menuconfig` 改动导致链接错误。
- 单包仅 1 target；同时需要 `esp32s3` 与 `esp32p4` 请获取两份独立交付包。

## SDK 体积（firmware，esp32s3 / trtc=c / p4_av_cs）

| .text | .rodata | .data | .bss | Flash |
|---:|---:|---:|---:|---:|
| 311,336 | 38,430 | 208 | 3,092 | **349,974** |
