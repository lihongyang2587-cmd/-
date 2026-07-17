#!/bin/bash
# ============================================================================
# 独立看门狗守护脚本
#
# 完全独立于 device_app 运行，由 systemd 管理生命周期。
# 每 30 秒检查主进程是否存活，死亡时自动重启。
# 支持固件升级：重启前检查 upgrade_pending.json 并替换二进制。
#
# 部署路径（按实际环境修改顶部配置变量）：
#   /usr/local/bin/device_watchdog.sh
#
# 日志：
#   /var/log/device_watchdog.log
# ============================================================================

set -u

# ============================================================================
# 配置（按实际部署路径修改）
# ============================================================================
PID_FILE="/tmp/device_app.pid"
CHECK_INTERVAL=30
APP_BIN="/root/src/device_app"
APP_WORKDIR="/root/src"
UPGRADE_FILE="config/upgrade_pending.json"   # 相对于 APP_WORKDIR
LOG_FILE="/var/log/device_watchdog.log"
LOG_MAX_LINES=2000                            # 日志行数上限，防止撑满磁盘

# ============================================================================
# 工具函数
# ============================================================================

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

log_both() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $*"
    echo "$msg"
    # 控制日志文件大小
    if [ -f "$LOG_FILE" ]; then
        local lines
        lines=$(wc -l < "$LOG_FILE" 2>/dev/null || echo 0)
        if [ "$lines" -ge "$LOG_MAX_LINES" ]; then
            tail -n $((LOG_MAX_LINES / 2)) "$LOG_FILE" > "${LOG_FILE}.tmp" 2>/dev/null
            mv "${LOG_FILE}.tmp" "$LOG_FILE" 2>/dev/null
        fi
    fi
    echo "$msg" >> "$LOG_FILE" 2>/dev/null
}

# 从简单 JSON 中提取字符串值（仅支持单层、不含嵌套对象/数组）
# 优先级：jq → python3 → grep Perl 正则
json_val() {
    local key="$1" file="$2"
    if command -v jq &>/dev/null; then
        jq -r ".${key} // empty" "$file" 2>/dev/null
    elif command -v python3 &>/dev/null; then
        python3 -c "import json; d=json.load(open('${file}')); print(d.get('${key}',''))" 2>/dev/null
    else
        grep -oP "\"${key}\"\s*:\s*\"[^\"]*\"" "$file" 2>/dev/null \
            | head -1 | grep -oP ':\s*"\K[^"]*'
    fi
}

# ============================================================================
# 重启 device_app
# ============================================================================
restart_app() {
    local upgrade_file="${APP_WORKDIR}/${UPGRADE_FILE}"

    # ---- 检查升级文件 ----
    if [ -f "$upgrade_file" ] && [ -s "$upgrade_file" ]; then
        log_both "检测到升级文件: $upgrade_file"

        local new_bin tgt_bin
        new_bin=$(json_val "newBin" "$upgrade_file")
        tgt_bin=$(json_val "targetBin" "$upgrade_file")

        if [ -n "$new_bin" ] && [ -f "$new_bin" ] && [ -n "$tgt_bin" ]; then
            log_both "固件升级: $new_bin → $tgt_bin"

            # 备份旧二进制
            if [ -f "$tgt_bin" ]; then
                cp "$tgt_bin" "${tgt_bin}.bak" 2>/dev/null
                log_both "旧二进制已备份: ${tgt_bin}.bak"
            fi

            # 替换二进制（优先 rename 原子操作）
            rm -f "$tgt_bin"
            if mv "$new_bin" "$tgt_bin" 2>/dev/null; then
                chmod 755 "$tgt_bin"
                log_both "二进制替换完成 (rename): $tgt_bin"
            else
                # 跨文件系统回退（如 /tmp tmpfs → ext4）
                cp "$new_bin" "$tgt_bin" && rm -f "$new_bin"
                chmod 755 "$tgt_bin"
                log_both "二进制替换完成 (cp+rm): $tgt_bin"
            fi
        else
            log_both "升级文件无效或 newBin 不存在，跳过升级"
        fi

        # 删除升级文件，防止重复尝试
        rm -f "$upgrade_file"
    fi

    # ---- 启动 device_app ----
    local bin_to_launch="${APP_BIN}"
    if [ ! -f "$bin_to_launch" ]; then
        log_both "错误: 二进制不存在: $bin_to_launch"
        return 1
    fi

    if [ ! -x "$bin_to_launch" ]; then
        chmod 755 "$bin_to_launch" 2>/dev/null
    fi

    cd "$APP_WORKDIR" || {
        log_both "错误: 无法进入工作目录: $APP_WORKDIR"
        return 1
    }

    nohup "$bin_to_launch" >> /var/log/device_app.log 2>&1 &
    local new_pid=$!
    echo "$new_pid" > "$PID_FILE"
    log_both "主进程已启动 (PID=$new_pid, binary=$bin_to_launch)"
    return 0
}

# ============================================================================
# 检查进程是否存活
# ============================================================================
is_alive() {
    local pid="$1"
    kill -0 "$pid" 2>/dev/null
}

# ============================================================================
# 主循环
# ============================================================================
log_both "============================================"
log_both "看门狗启动"
log_both "  监控二进制: $APP_BIN"
log_both "  工作目录:   $APP_WORKDIR"
log_both "  PID 文件:   $PID_FILE"
log_both "  检测间隔:   ${CHECK_INTERVAL}s"
log_both "============================================"

# 首次启动：若 device_app 未运行则拉起
PID=$(cat "$PID_FILE" 2>/dev/null || true)
if [ -z "$PID" ] || ! is_alive "$PID"; then
    log_both "首次检查: 主进程未运行，立即启动"
    restart_app || log_both "首次启动失败，将在下轮重试"
fi

while true; do
    sleep "$CHECK_INTERVAL"

    PID=$(cat "$PID_FILE" 2>/dev/null || true)
    if [ -z "$PID" ]; then
        # PID 文件为空或不存在，尝试启动
        log_both "PID 文件缺失，尝试启动"
        restart_app || true
        continue
    fi

    if is_alive "$PID"; then
        continue
    fi

    log_both "主进程 (PID=$PID) 已终止，准备重启"
    restart_app || log_both "重启失败，将在 ${CHECK_INTERVAL}s 后重试"
done
