#!/usr/bin/env bash
# Persona Voice 前置服务一键启动 / 停止 / 状态
#
# 用法：
#   ./start.sh          # 启动（默认）
#   ./start.sh start
#   ./start.sh stop
#   ./start.sh restart
#   ./start.sh status
#
# 管理三个服务：LiveKit 服务器（docker）、Token 服务、Python Agent。
# 日志输出到 logs/（已被 gitignore），PID 记录在 logs/*.pid。

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$ROOT/logs"
mkdir -p "$LOG_DIR"

# 加载根目录 .env（各进程自身也会读取，这里用于端口等默认值）
set -a
if [ -f "$ROOT/.env" ]; then
    # shellcheck disable=SC1091
    . "$ROOT/.env"
fi
set +a

LIVEKIT_PORT="${LIVEKIT_PORT:-7880}"
TOKEN_PORT="${TOKEN_SERVER_PORT:-8082}"
LIVEKIT_IMAGE="${LIVEKIT_IMAGE:-livekit/livekit-server:v1.13.4}"

pid_alive() {
    [ -f "$1" ] && kill -0 "$(cat "$1" 2>/dev/null)" 2>/dev/null
}

livekit_container() {
    docker ps -q -f name=persona-livekit 2>/dev/null | head -1
}

wait_port() {
    local port="$1" seconds="$2"
    for _ in $(seq 1 "$seconds"); do
        if ss -tln 2>/dev/null | grep -q ":$port "; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_livekit() {
    if [ -n "$(livekit_container)" ]; then
        echo "[livekit] 已在运行"
        return 0
    fi
    if ss -tln 2>/dev/null | grep -q ":$LIVEKIT_PORT "; then
        echo "[livekit] 端口 $LIVEKIT_PORT 已被其他进程占用，跳过"
        return 0
    fi
    echo "[livekit] 启动 $LIVEKIT_IMAGE ..."
    if ! docker run -d --rm --network host --name persona-livekit \
        "$LIVEKIT_IMAGE" --dev --bind 0.0.0.0 \
        >"$LOG_DIR/livekit.log" 2>&1; then
        echo "[livekit] 启动失败，看 logs/livekit.log"
        return 1
    fi
    if wait_port "$LIVEKIT_PORT" 30; then
        echo "[livekit] 就绪（端口 $LIVEKIT_PORT）"
        return 0
    fi
    echo "[livekit] 启动超时，看 logs/livekit.log"
    return 1
}

start_token() {
    if pid_alive "$LOG_DIR/token.pid"; then
        echo "[token] 已在运行"
        return 0
    fi
    if ss -tln 2>/dev/null | grep -q ":$TOKEN_PORT "; then
        echo "[token] 端口 $TOKEN_PORT 已被其他进程占用，跳过"
        return 0
    fi
    echo "[token] 启动 token_server.py（端口 $TOKEN_PORT）..."
    nohup python3 "$ROOT/firmware/tools/token_server.py" \
        >"$LOG_DIR/token.log" 2>&1 &
    echo $! >"$LOG_DIR/token.pid"
    for _ in $(seq 1 15); do
        if curl -s -o /dev/null -m 1 "http://127.0.0.1:$TOKEN_PORT/" 2>/dev/null; then
            echo "[token] 就绪"
            return 0
        fi
        sleep 1
    done
    echo "[token] 启动超时，看 logs/token.log"
    return 1
}

pick_python() {
    if [ -x "$ROOT/python/.venv/bin/python" ]; then
        echo "$ROOT/python/.venv/bin/python"
    elif python3 -c 'import livekit.agents' 2>/dev/null; then
        echo "python3"
    elif command -v uv >/dev/null 2>&1; then
        echo "uv run python"
    else
        echo "python3"
    fi
}

start_agent() {
    if pid_alive "$LOG_DIR/agent.pid"; then
        echo "[agent] 已在运行"
        return 0
    fi
    PY="$(pick_python)"
    echo "[agent] 使用 $PY 启动 agent_hermes.py ..."
    (
        cd "$ROOT/python" || exit 1
        nohup $PY src/agent_hermes.py dev >"$LOG_DIR/agent.log" 2>&1 &
        echo $! >"$LOG_DIR/agent.pid"
    )
    for _ in $(seq 1 90); do
        if grep -q "registered worker" "$LOG_DIR/agent.log" 2>/dev/null; then
            echo "[agent] 就绪"
            return 0
        fi
        sleep 1
    done
    echo "[agent] 启动超时，看 logs/agent.log"
    return 1
}

stop_one() {
    local name="$1"
    if pid_alive "$LOG_DIR/$name.pid"; then
        kill "$(cat "$LOG_DIR/$name.pid")" 2>/dev/null
        rm -f "$LOG_DIR/$name.pid"
        echo "[$name] 已停止"
    fi
}

stop_all() {
    stop_one agent
    stop_one token
    if [ -n "$(livekit_container)" ]; then
        docker stop persona-livekit >/dev/null 2>&1
        echo "[livekit] 容器已停止"
    fi
}

status_all() {
    echo "livekit: $(livekit_container || echo '未运行')"
    if pid_alive "$LOG_DIR/token.pid"; then
        echo "token: 运行中 (pid $(cat "$LOG_DIR/token.pid"))"
    elif ss -tln 2>/dev/null | grep -q ":$TOKEN_PORT "; then
        echo "token: 端口 $TOKEN_PORT 被其他进程占用"
    else
        echo "token: 未运行"
    fi
    if pid_alive "$LOG_DIR/agent.pid"; then
        echo "agent: 运行中 (pid $(cat "$LOG_DIR/agent.pid"))"
    elif pgrep -f 'agent_hermes.py de[v]' >/dev/null 2>&1; then
        echo "agent: 运行中（非本脚本启动）"
    else
        echo "agent: 未运行"
    fi
}

case "${1:-start}" in
    start)
        start_livekit
        start_token
        start_agent
        ;;
    stop)
        stop_all
        ;;
    restart)
        stop_all
        sleep 2
        start_livekit
        start_token
        start_agent
        ;;
    status)
        status_all
        ;;
    *)
        echo "用法: $0 [start|stop|restart|status]"
        exit 1
        ;;
esac
