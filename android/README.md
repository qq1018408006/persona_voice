<img src="./.github/assets/app-icon.png" alt="Voice Assistant App Icon" width="100" height="100">

# Android Agent Starter

This is a starter template for [LiveKit Agents](https://docs.livekit.io/agents/overview/) that provides a simple voice interface using the [LiveKit Android SDK](https://github.com/livekit/client-sdk-android).

This template is free for you to use or modify as you see fit.

## 本仓库定制版（SparkBot 手机端）

本仓库是 SparkBot 项目的手机客户端定制版，与上游模板的差异：

- 本地唤醒词“贾维斯”、休眠词“休息吧”、后台常驻通知与锁屏对话；
- 默认使用局域网自建 Token 服务（不走 LiveKit Cloud sandbox）；
- 播报使用通话声道并做增益补偿，锁屏/后台也能出声。

### 构建与安装

仓库不提供预构建 APK，请从源码自行构建：

```bash
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

需要 Android SDK（compileSdk 36）；首次构建会自动下载 Gradle 与依赖。
命令行构建还需要 `android/local.properties` 指向你的 Android SDK：

```bash
echo 'sdk.dir=/你的Android SDK路径' > local.properties
```

（用 Android Studio 打开项目会自动生成这个文件；它已被 gitignore，不会提交。）

### 连接配置（TokenExt.kt）

连接参数目前硬编码在
`app/src/main/java/io/livekit/android/example/voiceassistant/TokenExt.kt`，
修改后需重新构建：

| 常量 | 含义 | 默认值 |
|---|---|---|
| `localTokenServerUrl` | 局域网 Token 服务地址（改成你自己的） | `http://<你的电脑局域网IP>:8082/livekit/token` |
| `localDeviceKey` | 请求头 `X-Device-Key`（对应 Token 服务 `TOKEN_SERVICE_KEY`） | `test` |
| `localIdentity` | 参与者身份（必须以 `sparkbot-` 开头） | `sparkbot-android-002` |
| `localRoomName` | 手机专用房间（与固件房间分开；改成你自己 `.env` 的 `LIVEKIT_ANDROID_ROOM`） | `sparkbot-android-room` |
| `sandboxID` | LiveKit Cloud sandbox（留空则走本地 Token 服务） | 空 |

> 注意：Token 服务与 App 已统一使用 `8082`（根目录 `.env` 的
> `TOKEN_SERVER_PORT=8082`），不要改端口，否则 App 连不上。

> 安全提示：上表是代码里的**开发默认值**（`test` 设备密钥、示例房间名）。
> 公开部署或多设备使用前，请改成你自己的随机设备密钥和房间名，避免与他人
> 冲突或被人冒用 Token 服务。

### 手机权限与 MIUI 注意

- 首次启动需授予麦克风、通知权限；建议在系统设置中关闭电池优化，避免后台被杀；
- 通知栏常驻“贾维斯待命中”气泡用于确认监听状态，不要手动划掉；
- 锁屏唤醒依赖系统识别服务，个别机型（MIUI）在息屏后会切换到端侧识别，
  这是预期行为。

## Getting started

The easiest way to get this app running is with a [token server](https://docs.livekit.io/frontends/authentication/tokens/sandbox-token-server/) and the [LiveKit CLI](https://docs.livekit.io/home/cli/cli-setup/).

First, enable the token server from your project's **Options** on the [Settings](https://cloud.livekit.io/projects/p_/settings/project) page in LiveKit Cloud and copy the `sandboxID`.

Then, run the following command to automatically clone this template and connect it to LiveKit Cloud:

```bash
lk app create --template agent-starter-android --sandbox <token_server_sandbox_id>
```

Build and run the app in Android Studio.

You'll also need an agent to speak with. Try our starter agent for [Python](https://github.com/livekit-examples/agent-starter-python), [Node.js](https://github.com/livekit-examples/agent-starter-node), or [create your own from scratch](https://docs.livekit.io/agents/start/voice-ai/).

> [!NOTE]
> To setup without the LiveKit CLI, clone the repository and edit the `TokenExt.kt` file to add either a `sandboxID` (from your project's **Options** on the [Settings](https://cloud.livekit.io/projects/p_/settings/project) page), or a [manually generated](#token-generation) URL and token.

## Token generation

In a production environment, you will be responsible for developing a solution to [generate tokens for your users](https://docs.livekit.io/home/server/generating-tokens/) which is integrated with your authentication solution. You should disable the token server and modify `TokenExt.kt` to use your own token server.

## Contributing

This template is open source and we welcome contributions! Please open a PR or issue through GitHub, and don't forget to join us in the [LiveKit Community Slack](https://livekit.io/join-slack)!
