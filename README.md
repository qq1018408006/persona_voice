<h1 align="center">Persona Voice</h1>

<p align="center">
  <strong>多终端实时语音助手，打造懂你的专属助手。</strong><br>
  Multi-terminal real-time voice assistant · Pluggable agents · Personal profiles
</p>

<p align="center">
  <a href="docs/SETUP.md">快速开始</a> ·
  <a href="#功能总览--features">功能总览</a> ·
  <a href="#架构--architecture">系统架构</a> ·
  <a href="#文档--docs">项目文档</a>
</p>

<p align="center">
  <img src="docs/assets/persona-voice-poster.png" alt="Persona Voice 项目海报：多终端实时语音助手、可插拔 Agent、个人画像与系统架构" width="100%">
</p>

## 定位 / Vision

Persona Voice 是一个个人实时交互语音助手系统，围绕三点设计：

1. **多终端**：同一套个人语音服务可运行在手机 App、桌面和嵌入式语音终端上，
   终端只负责采集语音、播放回复与本地唤醒。
2. **任意后端 Agent**：前端交互 Agent（Gemini Live）负责实时语音交互与任务
   分发；真正执行任务的是可替换的后端 Agent（Hermes、Codex 或自定义实现），
   前端与后端解耦。
3. **个人画像**：导入个人任务画像信息，让助手理解你的习惯、偏好与工作流，
   从“通用语音助手”变成“懂你的专属助手”。

## 功能总览 / Features

从多终端接入、实时语音交互到后端任务执行，一张图了解 Persona Voice 的功能与扩展方向。

<p align="center">
  <img src="docs/assets/persona-voice-features.png" alt="Persona Voice 功能总览：多终端接入、实时语音交互、可插拔 Agent、个人画像、快速部署和家居场景扩展" width="760">
</p>

## 架构 / Architecture

```text
用户语音
   │
   ▼
终端（Android App / SparkBot 固件）
   │ LiveKit 实时音视频
   ▼
前端交互 Agent（Gemini Live）
   │  实时语音交互 + 任务分发
   ▼
后端 Agent（Hermes / Codex / 自定义）
   │  真实执行任务、调用本地工具
   ▼
结果 → 前端 Agent 播报 → 终端播放
```

## 快速开始 / Quick Start

完整安装与使用指南见 [docs/SETUP.md](docs/SETUP.md)，按顺序：

1. 在仓库根目录复制并填写统一环境变量：`cp .env.example .env`；
2. 启动电脑端服务：LiveKit 服务器 → Token 服务 → Agent（三者都读根目录 `.env`）；
3. 构建并安装手机 App（或烧录 SparkBot 固件）；
4. 说“贾维斯”唤醒，说“休息吧 / hold on”待机与打断。

也可以直接一键启动三个电脑端服务：`./start.sh`（支持 `stop` / `restart` /
`status`，日志在 `logs/`）。

当前后端 Agent **写死为 Hermes 与 Codex**：只装其一或都不装时，普通对话
仍可用，工具调用不可用。详见 SETUP 的“可选后端”一节。

## 子项目 / Components

本仓库使用 `git subtree` 管理三个独立历史：

| 目录          | 项目                           | 当前快照      | 上游                                                                                                  |
| ----------- | ---------------------------- | --------- | --------------------------------------------------------------------------------------------------- |
| `android/`  | 手机 App（唤醒/后台/锁屏对话）           | `9cad814` | [livekit-examples/agent-starter-android](https://github.com/livekit-examples/agent-starter-android) |
| `python/`   | Agent 服务（Gemini Live + 后端分发） | `2a8ecc6` | [livekit-examples/agent-starter-python](https://github.com/livekit-examples/agent-starter-python)   |
| `firmware/` | SparkBot 嵌入式终端固件             | `a29538c` | 自研                                                                                                  |

依赖：ESP-IDF 使用提交 `2c211b23670`（第三方依赖，不纳入本仓库）。

## 文档 / Docs

| 文档 | 内容 |
| --- | --- |
| [安装与使用指南](docs/SETUP.md) | 环境配置、服务启动、终端部署与常见问题 |
| [Android App](android/README.md) | 手机端构建与运行说明 |
| [Python Agent](python/README.md) | 实时语音服务与后端 Agent 接入 |
| [SparkBot 固件](firmware/README.md) | 嵌入式终端构建与烧录 |
| [Home Agent Bridge 实施方案](docs/HOME_AGENT_BRIDGE_IMPLEMENTATION.md) | 家居场景联动与主动播报的设计规划 |

## 子树更新 / Subtree updates

```bash
# 拉取某个子项目的新提交（以 python 为例）
git subtree pull --prefix=python <agent-starter-python 路径> <分支名>
```

## 快照标签 / Snapshots

- Android：`snapshot/bargein-lockscreen-20260804`
- Python：`snapshot/bargein-lockscreen-20260804`
- 固件：`snapshot/audio-clear-42-6-12-20260804`
