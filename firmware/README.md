# SparkBot LiveKit 常驻语音终端

这是面向 SparkBot（ESP32-S3-WROOM-1-N16R8）的基础固件。开机后后台完成
WiFi/Token/LiveKit 预热连接（warm standby），屏幕显示绿色 ✓ 表示就绪；
听到 **Jarvis / 贾维斯 / 嘉维斯** 后进入对话（此时才消耗 Token），再听到
同一唤醒词则请求 Agent 有序待机并回到就绪态；其他本地停止短语可随时打断
当前 Gemini 播报。待命期间不上传麦克风音频，不占用模型上下文或 Token。

当前版本只处理音频，不采集、发布或订阅视频。

## 项目来源与硬件

本固件基于乐鑫小铁匠开源的 **ESP-SparkBot** 开发（在原语音终端基础上改为
LiveKit 常驻语音终端）：

- 硬件主页（嘉立创开源硬件平台）：<https://oshwhub.com/esp-college/esp-sparkbot>
- 官方软件仓库：<https://gitee.com/esp-friends/esp_sparkbot>

板卡为 ESP32-S3-WROOM-1-N16R8，外设映射见下方“硬件映射”表；若你的板卡
批次或硬件定义不同，只需修改 `main/board_audio.c` 和 `main/board_ui.c`
顶部宏。

## 已实现

- ES8311 单麦克风采集和扬声器播放，16 kHz / 16 bit / mono
- ESP-SR WakeNet9 `Jarvis` 本地唤醒/休眠同词切换
- 本地停止短语控制播报打断（Gemini 侧兜底识别）
- LiveKit ESP32 SDK 音频双向通话（Opus 上行、音频下行）
- 本地唤醒成功后通过 LiveKit 数据消息请求语音 Agent 主动简短回应
- 短期 JWT 获取，不把 LiveKit API Secret 烧进设备
- 唤醒、连接完成、休眠和错误的不同提示音
- 麦克风增益固定为最高灵敏度档（42 dB / 阈值 0.52），触摸调节已取消
- 240×240 ST7789 状态色和图形提示
- BOOT 键手动切换唤醒/待命
- 16 MB Flash 双 OTA 分区和独立 ESP-SR 模型分区

## 交互状态

| 状态 | LiveKit | 本地识别 | 屏幕 | 声音 |
|---|---|---|---|---|
| 待命 | 断开 | Jarvis | 深色横线 | 进入待命降调 |
| 连接中 | 正在连接 | 暂停 | 橙色点阵 | 唤醒单音 |
| 对话中 | 音频双向 | 休息吧 / hold on | 绿色波形 / 处理点阵 / 回答对号 | 就绪双音与 Agent 回应 |
| 错误 | 断开并回待命 | 随后恢复 Jarvis | 红色 | 低音 |

## 收音灵敏度

触摸调节已取消（触摸硬件曾出现初始化超时，且误触会把灵敏度调低）。
灵敏度固定为最高档 **6/6：对话 42 dB / 阈值 0.52**；待机唤醒仍使用
44 dB / 0.58 的独立参数，防止待机误唤醒。

| 档位 | ES8311 输入增益 | WakeNet 阈值 | 适用情况 |
|---:|---:|---:|---|
| 1 | 24 dB | 0.68 | 很安静、希望降低误唤醒 |
| 2 | 29 dB | 0.65 | 安静环境 |
| 3 | 34 dB | 0.62 | 常规近距离交谈 |
| 4 | 39 dB | 0.59 | 距离较远或声音较轻 |
| 5 | 40 dB | 0.56 | 远距离交谈 |
| 6（固定） | 42 dB | 0.52 | 固定档位：收音最强，适合对话 ASR；待机时不会使用此档 |

待机时 WakeNet 固定使用 44 dB 输入增益、阈值不低于 0.58：待机只需要听到
“Jarvis”这一个词，而 42 dB 热增益和 0.52 低阈值是为对话语音识别准备的，
直接喂给旁路 DSP 的待机麦克风会把环境人声/噪声误判成唤醒。进入对话后才会
使用固定档位，因此灵敏度只增强语音识别，不再放大待机误唤醒。

扬声器音量保留 NVS 中上次保存的值（当前默认 80%，如需修改只能改代码或
配置，触摸调节已停用）。

## 硬件映射

| 功能 | GPIO |
|---|---:|
| ES8311 I2C SDA / SCL | 4 / 5 |
| I2S MCLK / BCLK / WS | 45 / 39 / 41 |
| I2S DOUT / DIN | 42 / 40 |
| LCD MOSI / CLK / CS / DC / BL | 47 / 21 / 44 / 43 / 46 |
| 侧面电容触摸（已停用） | 2 / 3 |
| BOOT 键 | 0 |

以上映射依据当前 SparkBot 硬件资料建立；若你的板卡批次或原固件定义不同，只需修改 `main/board_audio.c` 和 `main/board_ui.c` 顶部宏。

## 构建环境

已验证组合：

- ESP-IDF 5.5.3
- `livekit/livekit` 0.3.10
- `espressif/esp-sr` 2.1.5（由 LiveKit 的 `esp_capture` 依赖约束）

> 说明：`managed_components/` 目录由组件管理器在构建时自动下载并已被
> gitignore。av_render 的 AEC 参考回调补丁已整体 vendor 到
> `components/tempotian__av_render/`（与 esp_codec_dev 相同方式），构建时
> 优先使用本地组件，不会被组件管理器覆盖。

### 安装 ESP-IDF

本项目在 **ESP-IDF 5.5.3** 上验证。首次使用请先安装（Linux/macOS 示例）：

```bash
git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32s3
source export.sh
```

说明：

- 只安装 `esp32s3` 目标即可，`install.sh` 首次会下载工具链，耗时几分钟到几十分钟；
- 每次打开新终端后都要重新 `source ~/esp/esp-idf/export.sh`；
- Windows 用户请使用 ESP-IDF 官方安装器或 PowerShell 环境，参考
  <https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.3/esp32s3/get-started/>；
- 其他 ESP-IDF 版本不一定能编译通过，本项目只验证 5.5.3。
- 若报 `ESP-IDF Python virtual environment ... not found`：说明当前终端
  `python3` 版本与安装时不一致（常见于 conda 环境）。先指定已存在的环境再
  source，例如：

  ```bash
  export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.14_env"
  source ~/esp/esp-idf/export.sh
  ```

### 构建与配置

进入固件目录（monorepo 中为 `firmware/`，独立克隆时为 `sparkbot-livekit-firmware/`），
然后：

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py menuconfig
```

在 `SparkBot Jarvis terminal` 中设置。与根目录 `.env` 的关系如下：

| menuconfig 项 | 默认值 | 说明 |
|---|---|---|
| Wi-Fi SSID / 密码 | 空 | **必须自己填**（固件无法从 `.env` 读取） |
| Token 地址 | `http://<your-lan-ip>:8082/livekit/token` | 端口已与 `.env` 的 `TOKEN_SERVER_PORT=8082` 对齐；**IP 必须自己填** |
| 设备密钥 | 空 | **必须自己填**：填根目录 `.env` 的 `TOKEN_SERVICE_KEY` |
| 房间名 | 空 | **必须自己填**：填根目录 `.env` 的 `LIVEKIT_ROOM` |
| Agent 名 | `memo-agent` | 已与 `.env` 的 `LIVEKIT_AGENT_NAME` 对齐，一般不用改 |
| 音量 / 麦克风增益 / 唤醒阈值 | 50 / 35 / 0.60 | 固件本地参数，`.env` 无对应项 |

规则：`.env` 里有具体默认值的项（端口、Agent 名）固件默认值已对齐；`.env`
里只有占位符的项（IP、密钥、房间名、WiFi）固件无法推断，必须由你填写。

### 语音模型分区（自动构建）

本地唤醒模型随 ESP-SR 组件（`espressif/esp-sr` 2.1.5，由
`main/idf_component.yml` 声明）在首次 `idf.py build` 时由组件管理器自动下载。
构建完成后模型分区镜像位于 `build/srmodels/srmodels.bin`；`idf.py flash`
会把应用、启动引导和模型分区一起烧写（模型分区地址 `0xa20000`）。

若用 esptool 手动烧写，务必额外写入模型分区，否则本地唤醒不可用：

```bash
esptool.py -p /dev/ttyACM0 write_flash 0xa20000 build/srmodels/srmodels.bin
```

随后构建和烧录：

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### 烧录常见问题

- **`Could not open port /dev/ttyACM0`**：当前用户不在 `dialout` 组。执行
  `sudo usermod -aG dialout $USER` 后重新登录，或临时 `sudo chmod 666 /dev/ttyACM0`；
- **`Resource temporarily unavailable`（port busy）**：串口被其他程序占用
  （比如残留的 `idf.py monitor`）。先退出占用程序，或执行
  `fuser -k /dev/ttyACM0` 再重试；
- **`ESP-IDF Python virtual environment ... not found`**：见上文“安装 ESP-IDF”
  小节的说明（conda 导致 python 版本不一致时设置 `IDF_PYTHON_ENV_PATH`）。

## 启动短期 Token 服务

项目附带的 `tools/token_server.py` 只使用 Python 标准库。它签发 10 分钟有效、限定房间的参与者 Token，并可通过 RoomConfig 显式派发你的语音 Agent。

```bash
cd firmware          # monorepo 克隆用 firmware/；独立克隆用 sparkbot-livekit-firmware/
python3 tools/token_server.py
```

统一环境变量在**仓库根目录 `.env`**（复制根目录 `.env.example` 生成），
Token 服务与 Python Agent 都从这一个文件读取。填入与你现有
`agent_hermes.py` 相同的 LiveKit API Key 和 Secret；同时设置
`LIVEKIT_PUBLIC_URL=ws://<你的电脑局域网IP>:7880`（替换为运行 LiveKit 的主机
局域网 IP）。Token 服务优先向 ESP32 返回 `LIVEKIT_PUBLIC_URL`；该地址不能是
`localhost` 或 `127.0.0.1`。`.env` 和 `sdkconfig` 已被 Git 忽略。

脚本启动时会**自动读取仓库根目录的 `.env`**，无需手动 `source`；与旧的
`set -a; source .env` 行为一致（`.env` 会覆盖已导出的同名环境变量）。

验证服务（identity 必须以 `sparkbot-` 开头）：

```bash
curl -X POST http://127.0.0.1:8082/livekit/token \
  -H 'Content-Type: application/json' \
  -H 'X-Device-Key: replace-with-a-long-random-device-key' \
  -d '{"identity":"sparkbot-test","room":"<你的房间名>","agent_name":"memo-agent"}'
```

## 当前基础版边界

- 对话中说 `Jarvis` 是设备端本地待机命令，不进入 Gemini 上下文；排空完成后断开或静音，回到低开销 WakeNet 待命。
- `hold on` 是活动状态下的设备端停止控制词；它会停止当前 Gemini 思考、流式播报和扬声器输出，但不取消已经启动的 Hermes/Codex 工具任务。麦克风灵敏度已固定（42 dB / 0.52）。
- 本地唤醒连接完成后，ESP32 会发送可靠的 `sparkbot.wake` 数据消息。`src/agent_hermes.py` 已处理该消息并要求语音模型先自然回应一句；修改后需重启该 Python Agent。
- 基础版是单麦、无回声消除路径。设备播放声音时说“休息吧”的识别率取决于音量和声学结构；后续可加入播放参考通道做 AEC。
- 状态界面目前使用大面积纯色，优先保证无需读字就能判断状态。
- OTA 分区已经预留，但本版尚未实现远程升级下载流程。
