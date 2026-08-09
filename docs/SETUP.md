# 安装与使用指南

本文面向第一次接触 Persona Voice 的人。整个系统由三部分组成：

1. **电脑端服务**：LiveKit 服务器 + Token 服务 + Agent（必须）；
2. **手机 App**（可选，但推荐）：唤醒、后台、锁屏对话；
3. **SparkBot 固件**（可选）：需要 ESP-SparkBot 硬件板。

只想快速体验的人可以只跑“电脑服务 + 手机 App”，不需要固件和硬件。

---

## 0. 准备清单

**电脑（推荐 Linux）**

- Python 3.10～3.14 + `uv`
- 安装 `uv`（未安装时）：`curl -LsSf https://astral.sh/uv/install.sh | sh`
- Docker（跑 LiveKit 服务器）或改用 LiveKit Cloud
- ESP-IDF 5.5.3（只有烧固件时需要）
- Android SDK（只有构建手机 App 时需要）

**密钥**

- Google API Key（Gemini Live 用，自行到 Google AI Studio 申请）
- LiveKit API Key/Secret（自建服务器用 dev 默认值，或 LiveKit Cloud 提供）
- 可选：Hermes 后端、Codex CLI（见“可选后端”）

**网络**

- 所有设备（电脑、手机、SparkBot）处于同一局域网；
- 电脑的 7880（LiveKit）、8082（Token 服务）端口对局域网可达。

---

## 1. 电脑端服务

按顺序启动，全部就绪后设备才能工作。

### 1.1 LiveKit 服务器

自建（开发模式）：

```bash
docker run --rm --network host livekit/livekit-server:v1.13.4 --dev --bind 0.0.0.0
```

开发模式默认 API Key/Secret 为 `devkey` / `secret`。也可改用 LiveKit Cloud。
若 `7880` 端口已被占用（可先 `ss -tlnp | grep 7880` 检查），说明本机已有
LiveKit 实例，直接复用即可，不要重复启动。

### 1.2 Token 服务

先配置统一环境变量（所有电脑端服务共用一个文件）：

```bash
# 在仓库根目录执行一次
cp .env.example .env      # 填写所有 Key、房间名、设备密钥
```

Token 服务位于 `firmware/tools/token_server.py`，只依赖 Python 标准库：

```bash
cd firmware
python3 tools/token_server.py
```

脚本启动时会自动读取**仓库根目录的 `.env`**，无需手动 source。

关键变量：

| 变量                                       | 说明                                                                                                                                                   |
| ---------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| `LIVEKIT_URL`                            | Agent 本机用的 LiveKit 地址（如 `ws://127.0.0.1:7880`）                                                                                                       |
| `LIVEKIT_PUBLIC_URL`                     | 设备可访问的地址，**不能是 localhost**（如 `ws://<你的电脑局域网IP>:7880`，ip为运行`docker run --rm --network host livekit/livekit-server:v1.13.4 --dev --bind 0.0.0.0`的电脑ip） |
| `LIVEKIT_API_KEY` / `LIVEKIT_API_SECRET` | 与 LiveKit 服务器一致                                                                                                                                      |
| `LIVEKIT_ROOM`                           | 固件房间                                                                                                                                                 |
| `LIVEKIT_ANDROID_ROOM`                   | 手机 App 房间（与固件分开）                                                                                                                                     |
| `LIVEKIT_AGENT_NAME`                     | Agent 注册名（默认 `memo-agent`）                                                                                                                           |
| `TOKEN_SERVICE_KEY`                      | 设备密钥，与固件/App 端配置一致                                                                                                                                   |
| `TOKEN_SERVER_PORT`                      | 统一 `8082`（与手机 App TokenExt.kt 硬编码一致，一般不要改）                                                                                                           |

验证：

```bash
curl -X POST http://127.0.0.1:8082/livekit/token \
  -H 'Content-Type: application/json' \
  -H 'X-Device-Key: 你的设备密钥' \
  -d '{"identity":"sparkbot-test","room":"<你的固件房间名>","agent_name":"memo-agent"}'
```

> 安全提示：示例里的房间名、`test` 设备密钥都是开发默认值。公开部署或多设备
> 使用前，请改成你自己的随机设备密钥和唯一房间名。

### 1.3 Agent

```bash
cd python
uv sync
uv run python src/agent_hermes.py dev
```

首次 `uv sync` 会下载依赖，耗时几分钟。Agent 会自动读取仓库根目录 `.env`
（1.2 已配置）。看到 `registered worker` 即成功。

### 1.4 可选后端（Hermes / Codex）

当前 Agent 的工具分发**写死**为两个后端：

- **Hermes**（`HERMES_BASE_URL`、`HERMES_API_KEY`）：执行本机任务并把结果
  发送到自己的频道（如 Telegram）；
- **Codex CLI**（`CODEX_COMMAND`、`CODEX_WORKSPACE`）：执行代码开发、修改、
  审查类任务。

两个都不装时，普通对话功能照常可用，只是“让小马做…… / 让小 C 做……”
的工具调用会失败。按需安装其一或全部，并在仓库根目录 `.env` 中配置。

---

## 2. 手机 App

### 2.1 配置连接

连接参数硬编码在 `android/app/src/main/java/io/livekit/android/example/voiceassistant/TokenExt.kt`：

- `localTokenServerUrl`：改成你的 Token 服务地址；
- `localDeviceKey`：与 Token 服务的 `TOKEN_SERVICE_KEY` 一致；
- `localIdentity`：`sparkbot-` 开头的唯一身份；
- `localRoomName`：与仓库根目录 `.env` 的 `LIVEKIT_ANDROID_ROOM` 一致。

改完重新构建安装。端口已统一为 `8082`（Token 服务与 App 一致）。

### 2.2 构建与安装

仓库不提供预构建 APK：

```bash
cd android
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

命令行首次构建需要告诉 Gradle 你的 Android SDK 位置（`local.properties`
是机器相关的，不会随仓库分发）：

```bash
echo 'sdk.dir=/你的Android SDK路径' > local.properties
```

或者设置 `ANDROID_HOME=/你的Android SDK路径` 环境变量。用 Android Studio
打开项目时会自动生成 `local.properties`，无需手动操作。

### 2.3 权限

首次启动授予麦克风、通知权限；建议关闭电池优化；保持通知栏“贾维斯待命中”
气泡常驻。

---

## 3. SparkBot 固件（可选）

需要 ESP-SparkBot 硬件（ESP32-S3-WROOM-1-N16R8），硬件主页：
<https://oshwhub.com/esp-college/esp-sparkbot>。

### 3.1 构建

```bash
source <esp-idf>/export.sh
cd firmware
idf.py set-target esp32s3
idf.py menuconfig   # 设置 WiFi、Token 地址、设备密钥、房间、Agent 名
idf.py build
```

首次构建时 ESP-SR 组件会自动下载唤醒模型并生成
`build/srmodels/srmodels.bin`；`idf.py flash` 会连同模型分区一起烧写。

### 3.2 烧录

```bash
idf.py -p /dev/ttyXXX flash monitor ##ttyXXX换成sparkbot的usb端口
```

看到屏幕绿色 ✓ 即预热完成。

---

## 4. 使用流程

1. 启动 1.1～1.3 三个电脑服务；
2. SparkBot 上电，等待屏幕出现 ✓（warm standby）；
3. 手机打开 App，进入“待命”状态；
4. 对任一终端说 **“贾维斯 / Jarvis”** 唤醒并对话（此时才开始消耗 Token）；
5. 再说一次“贾维斯”，或对手机说“休息吧”进入待机；
6. 播报过程中说 **“hold on”** 可打断当前回复，后台任务不受影响；
7. 手机与 SparkBot 使用不同房间，可以同时在线。

---

## 5. 常见问题

- **手机“Error connecting to the session”**：Token 服务没起、端口不一致，
  或 `localDeviceKey` 与 `TOKEN_SERVICE_KEY` 不匹配。
- **固件连不上**：`LIVEKIT_PUBLIC_URL` 不能是 localhost；确认防火墙放行
  7880/8082；`SPARKBOT_TOKEN_API_KEY` 与 `TOKEN_SERVICE_KEY` 一致。
- **烧录报 `Could not open port`**：把当前用户加入 `dialout` 组并重新登录，
  或临时 `sudo chmod 666 /dev/ttyACM0`；报 `port busy` 时先退出其他串口程序
  或 `fuser -k /dev/ttyACM0`。
- **唤醒/对话没反应**：Agent 没在跑（`registered worker`）或房间名不匹配。
- **手机和固件同时在线时只有一方能说话**：两者必须使用不同房间
  （`LIVEKIT_ROOM` ≠ `LIVEKIT_ANDROID_ROOM`）。
- **没有唤醒音/播放声音小**：确认手机未静音、通知权限已给；MIUI 下保持
  通知常驻。
