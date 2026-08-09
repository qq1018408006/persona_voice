# Home Agent Bridge 实施步骤文档

> 依据需求文档（内部）：`docs/private/livekit_hermes_haos_architecture_feasibility.md`
> 本文档是后续实现的唯一执行清单：每一阶段必须先通过“验证步骤”，再进入下一阶段。
> 每完成一个阶段：先 `git commit`，再在复盘手册追加记录。

---

## 0. 最终目标（对应需求文档全部功能）

1. **HA → LiveKit 播报**：HAOS 自动化触发固定语音播报到指定终端（早晚问候）。
2. **Hermes → LiveKit 播报**：Hermes 任务完成后通过 Bridge 主动播报。
3. **HA → Hermes 分析任务**：HAOS 触发 Hermes 查询/分析 HA 状态，结果推送到
   Channel（Telegram），可选播报摘要。
4. **多终端 / 多 Room**：逻辑目标（`sparkbot.living_room`、`phone.owner` 等），
   静态注册表起步，阶段 5 动态发现。
5. **可靠性**：幂等（command_id）、TTL、队列、优先级、重试、状态查询、完成回执。
6. **安全**：Bearer Token + scope、凭据隔离（只有 Bridge 持有 LiveKit Secret）、
   防重放、过期拒绝。
7. **工程化**：REST + MCP 双入口共用 core、Docker/Compose、HA Package/Blueprint、
   后续 HAOS Add-on / HACS。

## 1. 总体架构

```text
Home Assistant
  ├── REST ──> Bridge ──> LiveKit SendData ──> Agent(agent_hermes.py) ──> 终端播报
  └── REST ──> Bridge ──> Hermes Runs API ──> 分析 ──> Telegram / Channel

Hermes
  └── REST/MCP ──> Bridge ──> LiveKit SendData ──> Agent ──> 终端播报
```

Bridge 只做协议转换/鉴权/路由/队列/幂等/回执，不引入 LLM。

## 2. 关键设计决策（实现时不再变更）

### 2.1 代码位置

在 `persona_voice` 仓库内新建 `home_agent_bridge/` 独立子项目（自己的
`pyproject.toml`），与 agent 同仓库便于协议同步；阶段 8 分发时再评估是否拆独立
仓库。固件与 Agent 的改动仍在原目录。

### 2.2 技术栈

- Python 3.11+，FastAPI + uvicorn
- SQLite（标准库 `sqlite3`，不引入 ORM）
- `livekit-api`（已装 1.2.0，`RoomService.send_data` 已验证可用）
- `httpx` 调 Hermes Runs API
- 阶段 7 用 `fastmcp`（或 `mcp` SDK）加 MCP 入口

### 2.3 目录结构

```text
home_agent_bridge/
├── pyproject.toml
├── bridge/
│   ├── __init__.py
│   ├── app.py              # FastAPI 入口
│   ├── config.py           # 从根目录 .env 读取
│   ├── models.py           # Pydantic 模型
│   ├── storage.py          # SQLite：jobs 表 + 幂等
│   ├── targets.py          # targets.yaml 解析
│   ├── auth.py             # Bearer token + scope
│   ├── livekit_adapter.py  # RoomService.send_data / list_participants / dispatch
│   ├── hermes_adapter.py   # 复用 HermesClient 逻辑（/v1/runs）
│   └── core.py             # announce() / run_agent()：REST 与 MCP 共用
├── targets.yaml
└── tests/
```

### 2.4 协议定义（全局唯一）

**Bridge → Agent**（LiveKit SendData，topic `agent.command.announce.v1`）：

```json
{
  "schema_version": 1,
  "command_id": "wifi-morning-2026-08-05",
  "text": "你好",
  "speech_mode": "literal",
  "interrupt_mode": "queue",
  "priority": 50,
  "expires_at": "2026-08-05T09:10:00+08:00",
  "callback_url": "http://<你的电脑局域网IP>:8650/v1/jobs/{job_id}/result"
}
```

**Agent → 设备**（topic `sparkbot.announce`）：

```json
{"command_id": "wifi-morning-2026-08-05", "text": "你好"}
```

**设备 → Agent**（topic `sparkbot.announce_done`）：

```json
{"command_id": "wifi-morning-2026-08-05", "status": "completed"}
```

**Agent → Bridge**（HTTP POST callback_url）：

```json
{"command_id": "wifi-morning-2026-08-05", "status": "completed", "ts": "2026-08-05T09:00:05+08:00"}
```

**Bridge → Hermes**（复用 Runs API）：

```text
POST {HERMES_BASE_URL}/v1/runs
Authorization: Bearer {HERMES_API_KEY}
{"model": "...", "input": "任务描述", "session_id": "...", "instructions": "..."}
```

### 2.5 SQLite 表

```sql
CREATE TABLE jobs (
  job_id      TEXT PRIMARY KEY,
  command_id  TEXT UNIQUE,
  kind        TEXT NOT NULL,            -- announce | agent_run
  target      TEXT,
  status      TEXT NOT NULL DEFAULT 'accepted', -- accepted/queued/routing/sent/acknowledged/completed/failed/expired/cancelled
  priority    INTEGER NOT NULL DEFAULT 50,
  payload     TEXT NOT NULL,
  expires_at  TEXT,
  created_at  TEXT NOT NULL,
  updated_at  TEXT NOT NULL
);
```

幂等：`command_id` 唯一；重复提交返回已有 job 状态（200），不再创建。
TTL：`expires_at` 过期后直接标记 `expired` 不执行。

### 2.6 targets.yaml（v1 静态）

```yaml
targets:
  sparkbot.living_room:
    room: room1
    agent_name: memo-agent
    device_identity: <设备 participant identity，例如 sparkbot-living-room>
    capabilities: [announce]

  sparkbot.bedroom:      # 阶段 5 才要求真实存在
    room: room-bedroom
    agent_name: memo-agent
    capabilities: [announce]

  phone.owner:           # 阶段 5 才要求真实存在（需要 App 待机轻连接，另立项）
    room: room2
    agent_name: memo-agent
    capabilities: [announce, notification]
```

解析规则：v1 固定 `room + agent_name + device_identity`；阶段 5 改为
`list_participants` 按 room + agent_name 解析在线 agent participant，按
participant attributes 解析设备。

### 2.7 环境变量（加入根目录 .env / .env.example）

```text
BRIDGE_HOST=0.0.0.0
BRIDGE_PORT=8650
BRIDGE_HA_TOKEN=         # HA 调用用的 Bearer
BRIDGE_HERMES_TOKEN=     # Hermes 调用用的 Bearer
BRIDGE_LIVEKIT_URL=ws://127.0.0.1:7880
BRIDGE_LIVEKIT_API_KEY=devkey
BRIDGE_LIVEKIT_API_SECRET=secret
BRIDGE_CALLBACK_BASE_URL=http://<你的电脑局域网IP>:8650   # 给 Agent 回调用
```

### 2.8 播报文字生成（literal 策略）

Agent 收到 announce 命令后，用现有“受控实时文本注入”机制播报（与唤醒问候同款）：

```text
系统事件：必须立刻用中文语音只说“{text}”。不要解释、不要调用工具。
```

阶段 0-3 都走这条路径；若实测 Gemini 不够逐字，阶段 6 再引入 edge-tts
预生成音频。

---

## 3. 分阶段实施

> 每个阶段含：目标 / 涉及文件 / 实现要点 / 验证步骤 / 完成标准。

### 阶段 0：最小播报链路（Bridge 骨架 + Agent handler + 固件 announce 态）

**目标**：打通 `curl → Bridge → LiveKit → Agent → SparkBot 出声`，并拿到完成回执。

**涉及文件**

- 新增：`home_agent_bridge/`（pyproject、app.py、config.py、models.py、
  storage.py、targets.py、auth.py、livekit_adapter.py、core.py、targets.yaml）
- 修改：`python/src/agent_hermes.py`（announce handler）
- 修改：固件 `firmware/main/app_controller.c`、`firmware/main/livekit_terminal.c`
  （announce 状态与数据包）

**实现要点**

1. Bridge：
   - `POST /v1/announcements`：校验 token、解析 target、幂等查重、写 jobs、
     返回 202 `{job_id,status:"accepted"}`；
   - `livekit_adapter.send_announcement()`：`RoomService.send_data`，topic
     `agent.command.announce.v1`，destination 为目标 room 里 agent participant
     identity（用 `list_participants` 按 `agent_name` 前缀/属性解析）；
   - `POST /v1/jobs/{job_id}/result`：接收 Agent 回执，更新状态；
   - `GET /v1/jobs/{job_id}`：查询状态。
2. Agent（agent_hermes.py）：
   - 在现有 `data_received` 监听 topic `agent.command.announce.v1`；
   - 校验 `expires_at`；只处理 `interrupt_mode=queue` 且当前空闲/可播报的会话；
   - 向设备发 `sparkbot.announce`；
   - 监听 `sparkbot.announce_done`，完成后 `httpx` POST `callback_url`。
3. 固件：
   - 新增 `APP_STATE_ANNOUNCE`：收到 `sparkbot.announce` 且当前 STANDBY 时，
     恢复远端渲染器播放、保持麦克风静音、不进入 ACTIVE 会话；
   - 播放结束（远端音频静音 650ms 后）或 30s 超时 → 回 STANDBY 并发
     `sparkbot.announce_done`；
   - 若当前 ACTIVE：忽略并回 `status:"busy"`（阶段 2 再做排队）。

**验证步骤**

```bash
cd home_agent_bridge
uv run uvicorn bridge.app:app --host 0.0.0.0 --port 8650

curl -X POST http://127.0.0.1:8650/v1/announcements \
  -H "Authorization: Bearer $BRIDGE_HA_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"command_id":"poc-2026-08-05-001","target":"sparkbot.living_room",
       "text":"你好","speech_mode":"literal","interrupt_mode":"queue"}'
```

预期：

- Bridge 返回 202，job 状态 `accepted`；
- Agent 日志出现 `agent.command.announce.v1` 处理记录；
- 固件日志出现 `announce` 状态切换 + `sparkbot.announce_done`；
- **SparkBot 扬声器确实播报“你好”**（这是最终验收，不能只看日志）；
- `GET /v1/jobs/{job_id}` 状态变为 `completed`；
- 相同 `command_id` 再提交返回已有状态、不重复播报。

**完成标准**：上述 6 条全部满足。提交 + 手册。

---

### 阶段 1：HA 固定播报自动化（早晚问候）

**目标**：早晨手机首次连家中 Wi-Fi 播报“你好”；21:00 后在家播报“该走了”，
每日各一次。

**涉及文件**

- 新增：`homeassistant/packages/home_agent_bridge.yaml`、
  `homeassistant/blueprints/once_daily_announcement.yaml`
- Bridge：无需新增接口（复用阶段 0）

**实现要点**

1. HA `device_tracker` 检测手机 Wi-Fi 状态；自动化触发条件：
   - 早晨：`to_state: "home"` + 时间 05:00–12:00 + `input_datetime.last_morning_announce != 今天`；
   - 晚间：21:00 定时 + 手机 home；以及 21:00 后重新连 Wi-Fi；
   - 保持连接 30 秒再去重（`for: "00:00:30"`）。
2. 去重两层：HA `input_datetime` 记录最后成功日期；Bridge 用
   `command_id=wifi-morning-YYYY-MM-DD` / `wifi-evening-YYYY-MM-DD` 幂等。
3. `rest_command` 调 Bridge：

```yaml
rest_command:
  ha_announce:
    url: "http://<你的电脑局域网IP>:8650/v1/announcements"
    method: post
    content_type: application/json
    headers:
      Authorization: "Bearer !secret ha_bridge_token"
    payload: '{"command_id":"{{ command_id }}","target":"{{ target }}",
               "text":"{{ text }}","speech_mode":"literal",
               "interrupt_mode":"queue"}'
```

**验证步骤**

1. HA 开发者工具直接调用 `rest_command.ha_announce`，参数填早晨场景 → SparkBot 播报“你好”，HA 日志无错误；
2. 手动把手机 device_tracker 设为“离开”再“回家”（或直接触发自动化）→ 早晨窗口内只播一次；
3. 重复触发同一天 → 不重复播报（HA 层 + Bridge 层都验证）；
4. 把手机设回 home，等到 21:00 后（或临时改自动化时间测试）→ 播报“该走了”；
5. 21:00–24:00 内再次触发 → 不重复。

**完成标准**：早晚场景各播报一次且不重复；重启 HA 后去重记录仍有效。

---

### 阶段 2：队列、优先级、TTL、重试与状态查询

**目标**：多条播报并发时按优先级排队；设备 busy 时不丢命令；过期不执行；
失败可重试。

**涉及文件**

- `bridge/core.py`、`bridge/livekit_adapter.py`、`bridge/storage.py`
- `python/src/agent_hermes.py`（busy 回执处理）
- 固件（`busy` 回执已具备，阶段 0）

**实现要点**

1. `interrupt_mode`：v1 只实现 `queue` 与 `drop_if_busy`；`interrupt` 预留字段暂不实现（避免破坏对话打断逻辑）；
2. Bridge 维护内存队列 + SQLite 状态：`queued → routing → sent → acknowledged → completed`；
3. Agent 回 `busy` 时：`priority >= 80` 且 `interrupt_mode=queue` → 10s 后重试（最多 3 次）；否则标记 `cancelled` 或按 TTL 到期；
4. `expires_at` 过期检查在 Bridge 入队和 Agent 执行前各做一次；
5. `GET /v1/announcements/{command_id}` 与 `GET /v1/jobs/{job_id}` 支持状态查询。

**验证步骤**

1. 连续提交 3 条 announce（低优先级“你好”、中优先级“备份完成”、高优先级“报警”）→ 观察执行顺序按优先级；
2. 让设备处于 ACTIVE 对话，提交一条高优先级命令 → Agent 回 busy → Bridge 重试，对话结束后播报；
3. 提交 `expires_at` 已过期的命令 → 直接 `expired`，不播报；
4. 杀掉 Agent 进程再提交 → Bridge 标记 `failed`（阶段 5 才做 dispatch 自动恢复）；
5. 用 `GET` 接口确认每个命令的状态流转正确。

**完成标准**：优先级/排队/busy 重试/过期四类场景全部符合预期。

---

### 阶段 3：Hermes → Bridge 播报

**目标**：Hermes 完成任务后主动播报结果（备份完成、异常告警等）。

**涉及文件**

- `home_agent_bridge/`：新增 Hermes 侧 REST 工具说明文档；
- `hermes/`（若 Hermes 支持 HTTP 工具）：配置示例 `hermes/http-tools.example.yaml`
- 不新增 Bridge 接口（复用 `/v1/announcements`）

**实现要点**

1. Hermes 侧增加一个 HTTP 工具 `announce(target, text, priority=50)`，调用
   `POST {BRIDGE_URL}/v1/announcements`，使用 `BRIDGE_HERMES_TOKEN`；
2. Hermes 指令/技能里约定：任务完成后若用户要求“播报/告诉我”或任务性质需要
   主动通知，调用该工具；
3. 第一阶段不引入 MCP（阶段 7）。

**验证步骤**

1. 在 Hermes 里手动执行一个任务：备份/分析类，明确要求“完成后在音箱播报”；
2. 预期：Hermes 日志出现调用 announce 工具；SparkBot 播报结果；Bridge job
   `completed`；
3. 用非 Hermes 身份（无 `hermes:announce` scope）调用 → 401/403。

**完成标准**：Hermes 真实任务完成后能主动触发语音播报，且鉴权生效。

---

### 阶段 4：HA → Hermes 分析任务

**目标**：HAOS 触发 Hermes 分析家居状态，结果推送到 Telegram，可选播报摘要。

**涉及文件**

- `bridge/hermes_adapter.py`、`bridge/core.py`（`run_agent()`）
- `homeassistant/packages/home_agent_bridge.yaml`（新增 rest_command：agent-run）
- `hermes/profiles/home-observer.yaml`（只读 HA + 推送 + 播报，禁止控制设备）

**实现要点**

1. `POST /v1/agent-runs`：

```json
{
  "request_id": "home-report-2026-08-05-2100",
  "agent": "hermes",
  "profile": "home-observer",
  "task": "analyze_home_status",
  "parameters": {
    "reason": "晚间例行检查",
    "scope": ["doors", "windows", "lights", "air_conditioning", "sensor_batteries"],
    "delivery": {"channel": "telegram", "target": "home-status-channel"},
    "voice_summary": {"enabled": true, "target": "sparkbot.living_room"}
  }
}
```

2. Bridge 创建 `agent_run` job → 调 Hermes Runs API（`POST /v1/runs`，复用现有
   HermesClient 模式）→ 轮询结果；
3. Hermes 用现有 HA 工具查状态 → 生成摘要 → 推 Telegram；
4. 若 `voice_summary.enabled`：Hermes 在结果里调用 announce 工具（阶段 3）播报摘要；
5. Hermes 使用 `home-observer` profile：只读 HA、允许发消息/播报、禁止设备控制。

**验证步骤**

1. HA 手动触发 `agent-run`（原因：晚间检查）→ 202；
2. 预期：Hermes 日志出现分析任务；Telegram 收到报告；
3. `voice_summary.enabled=true` 时 SparkBot 播报摘要（可用一句话）；
4. 让 Hermes 尝试调用“关闭灯光”类控制工具 → 被 profile 拒绝；
5. 重复 `request_id` → Bridge 幂等返回已有结果，不重复执行。

**完成标准**：分析、推送、可选播报、权限隔离、幂等全部符合预期。

---

### 阶段 5：多目标 / 多 Room / 动态发现 / Agent 离线 dispatch

**目标**：逻辑目标扩展到多终端；Agent 离线时自动 dispatch；设备能力动态发现。

**涉及文件**

- `bridge/livekit_adapter.py`、`bridge/targets.py`、`bridge/core.py`
- `homeassistant/`（新增卧室目标自动化示例）
- 固件/App：为新终端补 participant attributes（阶段 5 至少 SparkBot 支持）

**实现要点**

1. 目标解析改为动态：
   - `RoomService.list_rooms` 确认 room 存在；
   - `RoomService.list_participants(room)` 找到 agent participant
     （identity 前缀 `agent-` 或 attributes `agent.name=memo-agent`）；
   - 设备 participant 通过 attributes 识别（`device.id`、`device.type`、`capability.announce`）；
2. Agent 不在线但设备在线：
   - `AgentDispatchService` 按 `agent_name` dispatch 到该 room（token 的
     roomConfig 已有该能力）；
   - 等 agent participant 出现（轮询 ≤15s）再 SendData；
   - 防重复：dispatch 前先检查 room 是否已有同名 agent participant。
3. 设备离线：按 `interrupt_mode` 返回 `failed`/排队至 TTL。
4. targets.yaml 增加 `sparkbot.bedroom` 示例；真实第二台终端按硬件接入后注册。

**验证步骤**

1. 停掉 Agent 进程，提交 announce → Bridge 自动 dispatch → agent 恢复 → 播报成功；
2. 连续提交两条（dispatch 尚未完成）→ 只 dispatch 一次，两条按序执行；
3. 把 target 改成 `sparkbot.bedroom`（无设备）→ 返回明确错误/排队，不误投；
4. 验证 `list_targets` 接口输出包含全部注册目标及在线状态。

**完成标准**：动态解析、离线 dispatch、防重复、错误目标四类场景符合预期。

---

### 阶段 6：安全加固 + Literal 播报质量

**目标**：多调用方权限隔离；防重放/限流/审计；若 Gemini 逐字不稳则换预生成音频。

**涉及文件**

- `bridge/auth.py`、`bridge/app.py`（限流中间件）
- 可选：`agent` 播报改 edge-tts 预生成音频方案

**实现要点**

1. Bearer Token + scope：`ha:announce`、`ha:run-hermes`、`hermes:announce`、
   `admin:*`；每个调用方独立 token（.env 配置）；
2. 防重放：`command_id` 唯一 + `expires_at`；可选 HMAC 签名（`X-Bridge-Signature`，
   阶段 6 末期评估是否需要）；
3. 限流：按 token 每分钟 N 次（HA 10/min、Hermes 30/min）；
4. 审计：`jobs` 表记录调用方 scope + 来源 IP + 结果；日志不含文本正文（避免隐私）；
5. Literal 质量：若受控文本注入出现增字/改字，改 edge-tts 生成 `text.wav`，
   Bridge 直接投递音频路径（Agent 侧新增 `announce.audio` 处理），验证逐字性。

**验证步骤**

1. 用错误 scope token 调接口 → 403；
2. 重复 command_id → 幂等；过期 → expired；
3. 高频调用 → 429；
4. 审计表能查到每次调用方与结果；
5. 如果做了 TTS：分别播报“你好”“该走了”10 次，确认逐字一致（听感验收）。

**完成标准**：权限/幂等/限流/审计四类安全场景通过；播报文字满足逐字要求。

---

### 阶段 7：MCP 入口

**目标**：Bridge 通过 MCP 暴露 `announce` / `list_announcement_targets` /
`get_announcement_status`，供 Hermes 或未来其他 Agent 复用。

**涉及文件**

- `bridge/mcp.py`（FastMCP，in-process 与 FastAPI 共用 core 函数）
- `hermes/mcp-config.example.yaml`

**实现要点**

1. FastMCP server 注册三个工具，全部调用 `core.announce()` / `targets.list()` /
   `storage.get_job()`；
2. MCP 鉴权：`BRIDGE_HERMES_TOKEN`（请求头/初始化参数）；
3. 若 Hermes 支持 MCP client：配置示例并接入；若不支持，保留 REST 工具为 v1，
   MCP 作为开放接口供其他 Agent 使用。

**验证步骤**

1. 用 MCP inspector（或任意 MCP client）调用 `announce` → SparkBot 播报；
2. `list_announcement_targets` 返回 targets.yaml 内容；
3. `get_announcement_status` 返回对应 job 状态；
4. 无 token 调用 → 拒绝。

**完成标准**：MCP 三工具可用且与 REST 状态一致。

---

### 阶段 8：分发（Docker / Compose / HA Package / Blueprint / 文档）

**目标**：别人 clone 后一条命令跑起来。

**涉及文件**

- `home_agent_bridge/Dockerfile`、`docker-compose.yml`、`.env.example`
- `README.md`（Home Agent Bridge 章节：安装、配置、测试）
- `docs/SETUP.md` 增加 Bridge 章节

**实现要点**

1. Dockerfile：python:3.13-slim + uv 安装依赖 + 启动 uvicorn；
2. compose 与现有 `start.sh` 一致：network_mode host（Linux），挂载
   `.env`、`targets.yaml`、SQLite 数据目录；
3. `.env.example` 补 Bridge 全部变量；
4. HA Package + Blueprint 模板放入仓库 `homeassistant/` 并提供安装说明。

**验证步骤**

1. 干净 clone + `cp .env.example .env` + `docker compose up -d`；
2. 按“新人安装演练”方法，从克隆目录走一遍：Bridge 起来 → curl announce →
   SparkBot 播报；
3. 重新 clone 演练时记录文档缺漏并修复（沿用 16.25 的做法）。

**完成标准**：新人按文档可独立完成 Bridge 安装与一条播报验证。

---

### 阶段 9（可选）：HAOS Add-on / HACS / 拆分独立仓库

**目标**：进一步降低 HA 用户安装成本；评估是否把 Bridge 拆为独立开源仓库。

**实现要点**

1. HAOS Add-on：同一 Docker 镜像 + `addon/config.yaml`（图形化配置
   Bridge URL/Token/LiveKit 凭据）；
2. HACS Integration：封装 `home_agent_bridge.announce` /
   `home_agent_bridge.run_agent` / `home_agent_bridge.list_targets` 三个 action；
3. 拆分决策：若社区复用价值高，把 `home_agent_bridge/` 移到独立仓库（git subtree
   或直接复制），persona_voice 保留子模块/脚本引用。

**验证步骤**

1. 在 HAOS 测试环境安装 Add-on → 配置 → 触发自动化 → 播报成功；
2. HACS 安装 → action 可用；
3. 拆分后在新仓库按阶段 8 演练通过。

**完成标准**：至少 Add-on 或 HACS 其一可用（取决于分发优先级）。

---

## 4. 贯穿性约定

1. 每阶段结束：`git commit`（提交信息带阶段号），并在复盘手册追加记录
   （现象→改动→验证→结论）；
2. 所有新接口/协议先在本文档登记，禁止实现时临时改协议；
3. 验证优先用“真实终端出声”收尾，日志只是辅助；
4. 涉及 `.env` 的新变量同步更新根 `.env.example`（按“必改/可选/默认”分组）；
5. 每阶段新增的 HA/Hermes/固件改动保持向后兼容，不影响现有唤醒/对话/待机。

## 5. 风险与兜底

| 风险 | 兜底 |
|---|---|
| 固件 announce 态渲染恢复时序不稳 | 30s 超时回待机；先在小房间长时间验证 |
| Gemini literal 播报增字/改字 | 阶段 6 换 edge-tts 预生成音频 |
| Agent busy 时播报与对话冲突 | v1 只支持 queue/drop_if_busy，不做 interrupt |
| 手机 App 待机断连无法播报 | `phone.owner` 标记为“需 App 待机轻连接”，另立项 |
| Agent dispatch 重复启动 | dispatch 前按 room+agent_name 查重 |
| Hermes MCP 支持未知 | 阶段 3 先走 REST，MCP 独立可用即可 |

## 6. 工作量参考

| 阶段 | 估计 |
|---|---|
| 0 最小链路 | 3~4 天（含固件） |
| 1 HA 自动化 | 0.5 天 |
| 2 队列/重试 | 1 天 |
| 3 Hermes 播报 | 0.5 天 |
| 4 HA→Hermes | 1 天 |
| 5 动态发现/dispatch | 1~2 天 |
| 6 安全/TTS | 1~2 天 |
| 7 MCP | 1 天 |
| 8 分发 | 1~2 天 |
| 合计 | 约 2~3 周 |
