#!/usr/bin/env python3
"""
边检机器人控制板 WebSocket 模拟服务端（V4.1）

相比 V4.0 的修改：
  1. 【新增】预设指令中增加了云台控制测试指令（cmd=101/102/103/105）
  2. 鉴权通过后自动推送的预设指令现在涵盖：音箱、警灯、云台、LED
"""

import asyncio
import json
import time
import ssl
import os
import websockets
from datetime import datetime

# ============================================================
#  配置
# ============================================================
HOST = "0.0.0.0"
PORT = 8989
AUTO_SEND_PRESETS = True

# WSS 证书（自签名，内网使用）
SSL_CERT = os.path.join(os.path.dirname(__file__), "cert.pem")
SSL_KEY  = os.path.join(os.path.dirname(__file__), "key.pem")

# ============================================================
#  全局状态
# ============================================================
connected_clients = set()
token_map = {}
client_device_status = {}
client_heartbeat = {}
client_audio_count = {}   # 跟踪每个客户端的音频文件数量


def now_str():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def now_iso():
    return datetime.now().strftime("%Y-%m-%dT%H:%M:%SZ")


def now_timestamp():
    return str(int(time.time()))


# ============================================================
#  客户端工具
# ============================================================
def get_client_token(ws):
    return token_map.get(ws, "")


def get_first_authed_client():
    for ws in connected_clients:
        if ws in token_map:
            return ws
    return None


async def send_cmd_to_client(ws, cmd_data):
    msg = json.dumps(cmd_data, ensure_ascii=False)
    try:
        await ws.send(msg)
        cmd_id = cmd_data.get("cmd", "N/A")
        print(f"\n>>> 已发送 cmd={cmd_id}: {msg}")
    except Exception as e:
        print(f"\n>>> 发送失败: {e}")


# ============================================================
#  预设指令（鉴权通过后自动推送，使用实际 token）
# ============================================================
def build_presets(token):
    return [
        # {
        #     "cmd": 101,
        #     "token": "xyz123",
        #     "dir": 2,
        #     "speed": 5,
        #     "angle": 300,
        #     "stop": 0
        # },
        # {
        #     "cmd": 302,
        #     "token": token,
        #     "downUrl": "http://192.168.1.100/music",
        #     "audioData": [
        #         {"index": 0, "audioName": "障碍物",     "url": "/障碍物.wav"},
        #         {"index": 1, "audioName": "牵引棒下降", "url": "/牵引棒下降.wav"},
        #         {"index": 2, "audioName": "牵引棒上升", "url": "/牵引棒上升.wav"},
        #         {"index": 3, "audioName": "故障已清除", "url": "/故障已清除.wav"},
        #         {"index": 4, "audioName": "嘟嘟报警",   "url": "/嘟嘟报警.wav"},
        #         {"index": 5, "audioName": "交通管制",   "url": "/交通管制.wav"},
        #         {"index": 6, "audioName": "充电",       "url": "/充电.wav"},
        #     ]
        # },
        # # ---- 原有：音箱停止 ----
        # {
        #     "cmd": 301,
        #     "token": token,
        #     "stop": 1,
        #     "volume": 80
        # },
        # # ---- 原有：音箱顺序播放 ----
        # {
        #     "cmd": 301,
        #     "token": token,
        #     "play": 1,
        #     "playType": 2,
        #     "playData": [
        #         {"playIndex": 0, "audioIndex": 0},
        #         {"playIndex": 1, "audioIndex": 1},
        #         {"playIndex": 2, "audioIndex": 2},
        #         {"playIndex": 3, "audioIndex": 3},
        #     ]
        # },
    ]


async def send_presets_after_auth(ws, token):
    """鉴权成功后，间隔2秒逐条推送预设指令"""
    presets = build_presets(token)
    for i, preset in enumerate(presets):
        await asyncio.sleep(2)
        if ws not in connected_clients:
            break
        msg = json.dumps(preset, ensure_ascii=False)
        try:
            await ws.send(msg)
            print(f"\n>>> 预设[{i + 1}/{len(presets)}]: {msg}")
        except Exception:
            print(f"\n>>> 预设[{i + 1}] 推送失败")
            break
    print(f"\n--- 预设发送完毕 ---\n")


# ============================================================
#  认证处理（客户端发来的 cmd=11）
# ============================================================
async def handle_auth(ws, msg):
    device_mac = msg.get("deviceMac", "")
    api_key = msg.get("apiKey", "")
    device_time = msg.get("deviceTimes", "")
    remote = ws.remote_address

    print(f"\n<<< 认证请求 [{remote}]")
    print(f"    MAC: {device_mac}")
    api_display = api_key[:20] + "..." if len(api_key) > 20 else api_key
    print(f"    API Key: {api_display}")
    print(f"    设备时间: {device_time}")

    if device_mac and api_key:
        fake_token = f"token_{int(time.time())}"
        token_map[ws] = fake_token
        resp = {
            "cmd": 11,
            "token": fake_token,
            "result": 0,
            "serverTimes": now_str(),
        }
        print(f"    鉴权成功, 分配 token={fake_token}")
    else:
        resp = {
            "cmd": 11,
            "token": "",
            "result": 1001,
            "serverTimes": now_str(),
        }
        print("    鉴权失败: 参数缺失")

    await ws.send(json.dumps(resp))

    if resp["result"] == 0 and AUTO_SEND_PRESETS:
        print("    2秒后开始推送预设指令...")
        asyncio.create_task(send_presets_after_auth(ws, fake_token))


# ============================================================
#  旧格式查询响应处理（cmd=601/603，仅有cmd字段无code字段）
# ============================================================
async def handle_query_response(ws, msg):
    """处理旧格式查询响应 — cmd=601, 603（无 code 字段）"""
    cmd = msg.get("cmd")
    remote = ws.remote_address

    if cmd == 601:
        result = msg.get("result", -1)
        status = "成功" if result == 0 else f"失败({result})"
        print(f"\n<<< 状态采集回复 [{remote}]: {status}")
        return

    if cmd == 603:
        main_ver = msg.get("mainVer", "?")
        print(f"\n<<< 版本查询回复 [{remote}]: mainVer={main_ver}")
        return


# ============================================================
#  异常状态上报处理（cmd=607）
# ============================================================
async def handle_exception_report(ws, msg):
    """处理客户端主动推送的异常状态报告"""
    remote = ws.remote_address
    ptz     = msg.get("ptzFailStatus", 0)
    led     = msg.get("ledFailStatus", 0)
    speaker = msg.get("speakerFailStatus", 0)
    alarm   = msg.get("alarmFailStatus", 0)

    def label(v):
        return {0: "正常", 1: "不在线", 2: "响应异常"}.get(v, f"未知({v})")

    has_issue = any(v != 0 for v in (ptz, led, speaker, alarm))
    tag = "⚠ 异常" if has_issue else "✓ 全部正常"

    print(f"\n<<< 异常状态上报 [{remote}]: {tag}")
    print(f"    云台: {label(ptz)} | LED: {label(led)} | "
          f"音箱: {label(speaker)} | 警灯: {label(alarm)}")

    # 回复确认
    ack = {"code": 0, "cmd": 607, "msg": "success"}
    await ws.send(json.dumps(ack, ensure_ascii=False))


# ============================================================
#  客户端事件处理（有 event 字段的消息 → 返回 ACK）
# ============================================================
async def handle_event(ws, msg, raw):
    event = msg.get("event", "")
    data = msg.get("data", {})
    ts = msg.get("timestamp", now_iso())
    remote = ws.remote_address

    if event == "system_heartbeat":
        client_heartbeat[ws] = {"timestamp": ts, "data": data}
        model = data.get("model", "unknown")
        fw = data.get("fw_version", "?")
        uptime = data.get("uptime", "?")
        print(f"\n<<< 心跳 [{remote}]: model={model}, fw={fw}, uptime={uptime}s")
        ack = {
            "event": "system_heartbeat_ack",
            "timestamp": now_iso(),
            "data": {"serverTimes": now_str(), "status": "ok"}
        }
        await ws.send(json.dumps(ack))
        return

    if event == "device_status":
        client_device_status[ws] = {"timestamp": ts, "data": data}
        print(f"\n<<< 设备状态 [{remote}]:")
        for dev_name, dev_info in data.items():
            if isinstance(dev_info, dict):
                online = dev_info.get("online", "?")
                state = dev_info.get("state", "N/A")
                extra = ""
                if dev_name == "alarm":
                    extra = f", mode={dev_info.get('currentMode', '?')}"
                elif dev_name == "ptz":
                    extra = (f", h={dev_info.get('horizontalAngle', '?')}"
                             f", v={dev_info.get('verticalAngle', '?')}"
                             f", moving={dev_info.get('moving', False)}"
                             f", auto={dev_info.get('autonomous', False)}")
                print(f"    {dev_name}(online={online}, state={state}{extra})")
        ack = {"event": "device_status_ack", "timestamp": now_iso(),
               "data": {"status": "received"}}
        await ws.send(json.dumps(ack))
        return

    if event == "battery":
        soc = data.get("soc", "?")
        charging = data.get("charging", False)
        print(f"\n<<< 电量 [{remote}]: soc={soc}%, charging={charging}")
        ack = {"event": "battery_ack", "timestamp": now_iso(),
               "data": {"status": "received"}}
        await ws.send(json.dumps(ack))
        return

    if event == "alarm":
        alarm_type = data.get("type", "unknown")
        source = data.get("source", "unknown")
        message = data.get("message", "")
        print(f"\n<<< 报警 [{remote}]: type={alarm_type}, source={source}, msg={message}")
        ack = {"event": "alarm_ack", "timestamp": now_iso(),
               "data": {"status": "received"}}
        await ws.send(json.dumps(ack))
        return

    if event == "error":
        device = data.get("device", "unknown")
        error = data.get("error", "unknown")
        code = data.get("code", "?")
        print(f"\n<<< 异常 [{remote}]: device={device}, code={code}, error={error}")
        ack = {"event": "error_ack", "timestamp": now_iso(),
               "data": {"status": "received"}}
        await ws.send(json.dumps(ack))
        return

    print(f"\n<<< 未知事件 [{remote}]: event={event}, raw={raw[:200]}")
    ack = {"event": f"{event}_ack", "timestamp": now_iso(),
           "data": {"status": "received", "note": "unknown event type"}}
    await ws.send(json.dumps(ack))


# ============================================================
#  消息路由入口
# ============================================================
async def handle_message(ws, raw):
    try:
        msg = json.loads(raw)
    except json.JSONDecodeError:
        print(f"\n<<< 非JSON消息: {raw[:200]}")
        return

    has_cmd   = "cmd" in msg
    has_code  = "code" in msg
    has_event = "event" in msg
    remote    = ws.remote_address

    # ---- 1. Event messages FROM the board (heartbeat, device_status, etc.) ----
    if has_event:
        await handle_event(ws, msg, raw)
        return

    # ---- 2. Messages with "cmd" field ----
    if has_cmd:
        cmd = msg.get("cmd")

        # 2a. Board-originated requests (no "code" field)
        if cmd == 11:
            await handle_auth(ws, msg)
            return

        if cmd == 607:
            await handle_exception_report(ws, msg)
            return

        # 2b. Board RESPONSES to commands — have BOTH "cmd" AND "code"
        if has_code:
            code = msg.get("code", -1)
            msg_text = msg.get("msg", "")
            tag = "成功" if code == 0 else f"失败(code={code})"

            # Group by cmd code range for categorized logging
            if 100 <= cmd <= 199:
                print(f"\n<<< 云台响应 [{remote}]: cmd={cmd}, {tag} - {msg_text}")
                # cmd=103/106 返回 groupData，展示预置位详情
                if cmd == 103 and code == 0:
                    gn  = msg.get("groupNum", "?")
                    gd  = msg.get("groupData", [])
                    tot = msg.get("total", len(gd))
                    sc  = msg.get("successCount", "?")
                    fc  = msg.get("failCount", "?")
                    print(f"    预置位组: {gn}, 总计 {tot} 个, 成功 {sc} 个, 失败 {fc} 个")
                    for pt in gd:
                        if isinstance(pt, dict):
                            idx = pt.get("index", "?")
                            ha  = pt.get("horAngle", "?")
                            va  = pt.get("verAngle", "?")
                            ok  = pt.get("success", None)
                            tag2 = "✓" if ok else "✗ 失败"
                            print(f"    [{idx}] h={ha}° v={va}° {tag2}")
                elif cmd == 106 and code == 0:
                    gn = msg.get("groupNum", "?")
                    gd = msg.get("groupData", [])
                    print(f"    预置位组: {gn}, 共 {len(gd)} 个点")
                    for pt in gd:
                        if isinstance(pt, dict):
                            idx = pt.get("index", "?")
                            ha  = pt.get("horAngle", "?")
                            va  = pt.get("verAngle", "?")
                            print(f"    [{idx}] h={ha}° v={va}°")
            elif 200 <= cmd <= 299:
                print(f"\n<<< LED响应 [{remote}]: cmd={cmd}, {tag} - {msg_text}")
            elif 300 <= cmd <= 399:
                print(f"\n<<< 音箱响应 [{remote}]: cmd={cmd}, {tag} - {msg_text}")
                if cmd == 303:
                    audio_data = msg.get("audioData", [])
                    for item in audio_data:
                        if isinstance(item, dict):
                            print(f"    [{item.get('index')}] {item.get('audioName')}")
            elif 400 <= cmd <= 499:
                print(f"\n<<< 警灯响应 [{remote}]: cmd={cmd}, {tag} - {msg_text}")
            elif 600 <= cmd <= 699:
                print(f"\n<<< 系统管理响应 [{remote}]: cmd={cmd}, {tag} - {msg_text}")
            else:
                print(f"\n<<< 命令响应 [{remote}]: cmd={cmd}, code={code}, {tag} - {msg_text}")
            return

        # 2c. Old-format query responses (cmd present, NO "code" field)
        #     Currently: cmd=601 {cmd, result, ...}, cmd=603 {cmd, mainVer}
        if cmd in (601, 603):
            await handle_query_response(ws, msg)
            return

        print(f"\n<<< 未预期的 cmd={cmd} [{remote}]: "
              f"{json.dumps(msg, ensure_ascii=False)}")
        return

    # ---- 3. Legacy fallback: responses with "code" but no "cmd" ----
    if has_code:
        code = msg.get("code", -1)
        msg_text = msg.get("msg", "")
        if code == 0:
            print(f"\n<<< 命令执行成功 [{remote}]: {msg_text}")
        else:
            print(f"\n<<< 命令执行失败 [{remote}]: code={code}, {msg_text}")
        return

    print(f"\n<<< 无法识别的消息: {json.dumps(msg, ensure_ascii=False)}")


# ============================================================
#  WebSocket 连接处理
# ============================================================
async def on_connection(ws):
    connected_clients.add(ws)
    remote = ws.remote_address
    print(f"\n{'=' * 55}")
    print(f"[+] 客户端已连接: {remote}")
    print(f"{'=' * 55}")

    try:
        async for raw in ws:
            await handle_message(ws, raw)
    except websockets.ConnectionClosed:
        pass
    finally:
        connected_clients.discard(ws)
        token_map.pop(ws, None)
        client_device_status.pop(ws, None)
        client_heartbeat.pop(ws, None)
        client_audio_count.pop(ws, None)
        print(f"\n[-] 客户端已断开: {remote}")
        print(f"    当前在线: {len(connected_clients)}")


# ============================================================
#  控制台交互菜单
# ============================================================
async def console_menu():
    await asyncio.sleep(1)
    print("\n  按回车打开指令菜单...")

    while True:
        try:
            await asyncio.get_event_loop().run_in_executor(None, input, "")
        except (EOFError, KeyboardInterrupt):
            break

        ws = get_first_authed_client()
        if not ws:
            print("\n  没有已认证的客户端，请等待客户端连接并完成鉴权。")
            continue

        token = get_client_token(ws)

        print("\n" + "=" * 55)
        print("  服务端主动下发指令")
        print(f"  当前客户端: {ws.remote_address}  token={token}")
        print("=" * 55)
        print("  --- 系统管理 (cmd=601~606) ---")
        print("  [1]  状态采集 (cmd=601)")
        print("  [2]  校时服务 (cmd=602)")
        print("  [3]  版本查询 (cmd=603)")
        print("  [4]  版本更新 (cmd=604)")
        print("  [5]  服务器配置 (cmd=605)")
        print("  [6]  心跳检测 (cmd=606)")
        print("  --- 设备控制 ---")
        print("  [7]  云台移动/停止 (cmd=101)")
        print("  [8]  云台归位 (cmd=102)")
        print("  [9]  云台预置位 (cmd=103)")
        print("  [10] 云台巡航 (cmd=104)")
        print("  [11] 云台扫描 (cmd=105)")
        print("  [12] LED显示 (cmd=201)")
        print("  [13] LED开关 (cmd=202)")
        print("  [14] 音箱播放控制 (cmd=301)")
        print("  [15] 音箱设置列表 (cmd=302)")
        print("  [16] 音箱获取列表 (cmd=303)")
        print("  [17] 警灯控制 (cmd=401)")
        print("  [18] 氛围灯控制")
        print("  --- 工具 ---")
        print("  [19] 自定义JSON推送")
        print("  [20] 查看在线客户端")
        print("  [21] 查看客户端上报数据")
        print("  [22] 云台预置位查询 (cmd=106)")
        print("  [0]  返回")
        print("=" * 55)

        try:
            choice = await asyncio.get_event_loop().run_in_executor(
                None, lambda: input("  选择: ").strip()
            )
        except (EOFError, KeyboardInterrupt):
            break

        if choice == "1":
            await send_cmd_to_client(ws, {"cmd": 601, "token": token})

        elif choice == "2":
            t = now_str()
            user_input = input(f"  服务器时间 (默认 {t}): ").strip()
            t = user_input if user_input else t
            await send_cmd_to_client(ws, {
                "cmd": 602, "token": token, "serverTimes": t
            })

        elif choice == "3":
            await send_cmd_to_client(ws, {"cmd": 603, "token": token})

        elif choice == "4":
            ver = int(input("  新版本号: ").strip() or "3")
            url = input("  固件URL: ").strip() or "http://192.168.1.100/firmware/v3.bin"
            await send_cmd_to_client(ws, {
                "cmd": 604, "token": token, "mainVer": ver, "downUrl": url
            })

        elif choice == "5":
            url = input("  WebSocket地址: ").strip() or "wss://main-server.example.com/ws"
            await send_cmd_to_client(ws, {
                "cmd": 605, "token": token, "serverUrl": url
            })

        elif choice == "6":
            await send_cmd_to_client(ws, {
                "cmd": 606, "token": token, "timestamp": now_timestamp()
            })

        elif choice == "7":
            print("  模式: 0=绝对定位, 1=左, 2=右, 3=上, 4=下")
            d = int(input("  方向/模式 (默认1): ").strip() or "1")
            if d < 0 or d > 4:
                print("  dir 无效，应为 0(绝对定位)/1(左)/2(右)/3(上)/4(下)")
                continue
            if d == 0:
                # 绝对定位模式
                print("  轴: 1=水平, 2=垂直")
                axis = int(input("  轴 (默认1): ").strip() or "1")
                if axis not in (1, 2):
                    print("  axis 无效，应为 1(水平) 或 2(垂直)")
                    continue
                a_range = "0~350" if axis == 1 else "-32~23"
                a = int(input(f"  绝对目标角度 {a_range} (默认90): ").strip() or "90")
                await send_cmd_to_client(ws, {
                    "cmd": 101, "token": token,
                    "dir": 0, "angle": a, "axis": axis, "stop": 0
                })
            else:
                s = int(input("  速度 0~10 (默认5): ").strip() or "5")
                a = int(input("  相对偏移角度 (默认30, 0=持续转动): ").strip() or "30")
                await send_cmd_to_client(ws, {
                    "cmd": 101, "token": token,
                    "dir": d, "speed": s, "angle": a, "stop": 0
                })

        elif choice == "8":
            h = int(input("  水平角度 0~300 (默认90): ").strip() or "90")
            v = int(input("  垂直角度 -32~23 (默认0): ").strip() or "0")
            s = int(input("  速度 0~10 (默认5): ").strip() or "5")
            await send_cmd_to_client(ws, {
                "cmd": 102, "token": token,
                "horizontalAngle": h, "verticalAngle": v, "speed": s
            })

        elif choice == "9":
            g = int(input("  组号 1/2 (默认1): ").strip() or "1")
            if g not in (1, 2):
                print("  组号无效，应为 1 或 2")
                continue
            n = int(input("  预置位数量 1~16 (默认2): ").strip() or "2")
            if n < 1 or n > 16:
                print("  数量无效，应为 1~16")
                continue
            group_data = []
            print(f"  请输入 {n} 个预置位的参数:")
            for i in range(n):
                print(f"  --- 预置位 [{i}] ---")
                # 索引：循环直到输入合法
                while True:
                    idx = int(input(f"    索引 0~15 (默认{i}): ").strip() or str(i))
                    if 0 <= idx <= 15:
                        break
                    print(f"    ✗ 索引 {idx} 超出范围 0~15，请重新输入")
                # 水平角度：循环直到输入合法
                while True:
                    ha = int(input(f"    水平角度 0~350 (默认{45*i}): ").strip() or str(45 * i))
                    if 0 <= ha <= 350:
                        break
                    print(f"    ✗ 水平角度 {ha} 超出范围 0~350，请重新输入")
                # 垂直角度：循环直到输入合法
                while True:
                    va = int(input(f"    垂直角度 -32~23 (默认0): ").strip() or "0")
                    if -32 <= va <= 23:
                        break
                    print(f"    ✗ 垂直角度 {va} 超出范围 -32~23，请重新输入")
                group_data.append({"index": idx, "horAngle": ha, "verAngle": va})
            await send_cmd_to_client(ws, {
                "cmd": 103, "token": token,
                "groupNum": g, "groupData": group_data
            })

        elif choice == "10":
            g = int(input("  预置位组号 1/2 (默认1): ").strip() or "1")
            if g not in (1, 2):
                print("  组号无效，应为 1 或 2")
                continue
            print("  停留时间: 0=5秒, 1=10秒, 2=15秒")
            dw = int(input("  停留时间 (默认0): ").strip() or "0")
            if dw not in (0, 1, 2):
                print("  停留时间无效，应为 0(5秒)/1(10秒)/2(15秒)")
                continue
            s = int(input("  速度 0~10 (默认5): ").strip() or "5")
            await send_cmd_to_client(ws, {
                "cmd": 104, "token": token,
                "groupNum": g, "dwell": dw, "speed": s
            })

        elif choice == "11":
            print("  水平扫描 (Pelco-D 协议仅支持水平)")
            s = int(input("  速度 0~10 (默认5): ").strip() or "5")
            await send_cmd_to_client(ws, {
                "cmd": 105, "token": token, "scanDir": 1, "speed": s
            })

        elif choice == "12":
            n = int(input("  文字条数 (默认1): ").strip() or "1")
            text_data = []
            for i in range(n):
                default_text = ["请出示证件", "欢迎光临", "谢谢合作"][i] if i < 3 else f"文字{i+1}"
                txt = input(f"  文字[{i}] (默认'{default_text}'): ").strip() or default_text
                text_data.append({"textindex": i, "text": txt})
            lv = int(input("  亮度 0~100 (默认50): ").strip() or "50")
            print("  播放模式: 0=不指定, 1=重复播放, 2=定时轮换")
            st = int(input("  showType (默认0): ").strip() or "0")
            swt = 5
            if st == 2:
                swt = int(input("  切换时间/秒 (默认5): ").strip() or "5")
            print("  滚动样式: 0=不指定, 1=左右滚动, 2=上下滚动")
            ds = int(input("  displayStyle (默认1): ").strip() or "1")
            cmd_201 = {
                "cmd": 201, "token": token,
                "textData": text_data,
                "lightVal": lv,
                "showType": st,
                "switchTime": swt,
                "displayStyle": ds
            }
            await send_cmd_to_client(ws, cmd_201)

        elif choice == "13":
            s = int(input("  0=关闭, 1=开启 (默认1): ").strip() or "1")
            await send_cmd_to_client(ws, {
                "cmd": 202, "token": token, "switch": s
            })

        elif choice == "14":
            # ---- 音量（所有操作通用，始终处理） ----
            vol_input = input("  音量 0~100 (默认80, 回车跳过): ").strip()
            vol = 80
            if vol_input:
                vol = int(vol_input)
                if vol < 0:
                    vol = 0
                elif vol > 100:
                    vol = 100
                if vol != int(vol_input):
                    print(f"  音量已调整至有效范围: {vol}")

            # ---- 辅助: 验证音频索引 ----
            max_audio = client_audio_count.get(ws, 7)

            def ask_audio_index(prompt="音频索引", default="0"):
                ai = int(input(f"  {prompt} 0~{max_audio - 1} (默认{default}): ").strip() or default)
                if ai < 0 or ai >= max_audio:
                    print(f"  ✗ 错误: 音频索引 {ai} 超出范围 0~{max_audio - 1}，操作已取消")
                    return None
                return ai

            print("  操作: 1=播放, 2=暂停, 3=停止")
            op = input("  操作 (默认1): ").strip() or "1"

            if op == "2":
                await send_cmd_to_client(ws, {
                    "cmd": 301, "token": token,
                    "pause": 1, "volume": vol
                })

            elif op == "3":
                await send_cmd_to_client(ws, {
                    "cmd": 301, "token": token,
                    "stop": 1, "volume": vol
                })

            else:
                print("  播放模式: 1=循环, 2=顺序, 3=定时, 4=插播")
                pt = int(input("  模式 (默认1): ").strip() or "1")
                cmd_data = {
                    "cmd": 301, "token": token,
                    "play": 1, "playType": pt, "volume": vol
                }
                if pt == 2:
                    # 顺序播放：需要 playData 数组 [{playIndex, audioIndex}, ...]
                    n = int(input("  播放序列长度 (默认2): ").strip() or "2")
                    pd = []
                    cancelled = False
                    for i in range(n):
                        ai = ask_audio_index(f"audioIndex[{i}]", str(i))
                        if ai is None:
                            cancelled = True
                            break   # 索引无效，取消操作
                        pd.append({"playIndex": i, "audioIndex": ai})
                    if cancelled:
                        continue
                    cmd_data["playData"] = pd

                elif pt == 3:
                    ai = ask_audio_index()
                    if ai is None:
                        continue
                    cmd_data["audioIndex"] = ai
                    cmd_data["playData"] = input(
                        "  定时时间 (YYYY-MM-DD HH:mm:ss): "
                    ).strip() or now_str()

                else:
                    # playType=1 循环, playType=4 插播/单次
                    ai = ask_audio_index()
                    if ai is None:
                        continue
                    cmd_data["audioIndex"] = ai

                await send_cmd_to_client(ws, cmd_data)

        elif choice == "15":
            url = input("  下载URL前缀 (默认 http://192.168.1.100/music): ").strip()
            url = url or "http://192.168.1.100/music"
            default_data = [
                {"index": 0, "audioName": "障碍物",     "url": "/障碍物.wav"},
                {"index": 1, "audioName": "牵引棒下降", "url": "/牵引棒下降.wav"},
                {"index": 2, "audioName": "牵引棒上升", "url": "/牵引棒上升.wav"},
                {"index": 3, "audioName": "故障已清除", "url": "/故障已清除.wav"},
                {"index": 4, "audioName": "嘟嘟报警",   "url": "/嘟嘟报警.wav"},
                {"index": 5, "audioName": "交通管制",   "url": "/交通管制.wav"},
                {"index": 6, "audioName": "充电",       "url": "/充电.wav"},
            ]
            client_audio_count[ws] = len(default_data)
            await send_cmd_to_client(ws, {
                "cmd": 302, "token": token, "downUrl": url,
                "audioData": default_data
            })

        elif choice == "16":
            await send_cmd_to_client(ws, {"cmd": 303, "token": token})

        elif choice == "17":
            state = int(input("  0=关闭, 1=开启 (默认1): ").strip() or "1")
            cmd_data = {"cmd": 401, "token": token, "state": state}
            if state == 1:
                lt = int(input("  灯光模式 0~14 (默认7): ").strip() or "7")
                ot = int(input("  0=正常开启, 1=定时 (默认0): ").strip() or "0")
                cmd_data["lightType"] = lt
                cmd_data["openType"] = ot
                if ot == 1:
                    cmd_data["openData"] = input(
                        "  定时时间 (YYYY-MM-DD HH:mm:ss): "
                    ).strip()
            await send_cmd_to_client(ws, cmd_data)

        elif choice == "18":
            state = input("  on/off (默认on): ").strip() or "on"
            cmd_data = {"state": state}
            if state == "on":
                t = input("  模式 steady/breath/flash/gradient (默认breath): ").strip()
                t = t or "breath"
                cmd_data["type"] = t
            await send_cmd_to_client(ws, cmd_data)

        elif choice == "19":
            custom = input("  输入完整JSON: ").strip()
            try:
                data = json.loads(custom)
                if "token" not in data and "event" not in data:
                    data["token"] = token
                await send_cmd_to_client(ws, data)
            except json.JSONDecodeError:
                print("  JSON格式错误")

        elif choice == "20":
            print(f"\n  在线客户端: {len(connected_clients)}")
            for i, c in enumerate(connected_clients):
                t = token_map.get(c, "未认证")
                print(f"    [{i}] {c.remote_address}  token={t}")

        elif choice == "21":
            if not client_device_status and not client_heartbeat:
                print("  暂无客户端上报数据")
                continue
            for c in connected_clients:
                r = c.remote_address
                hb = client_heartbeat.get(c)
                ds = client_device_status.get(c)
                print(f"\n  客户端 {r}:")
                if hb:
                    d = hb["data"]
                    print(f"    心跳: model={d.get('model', '?')}, "
                          f"fw={d.get('fw_version', '?')}, "
                          f"uptime={d.get('uptime', '?')}s")
                else:
                    print(f"    心跳: 无数据")
                if ds:
                    print(f"    设备状态 (ts={ds['timestamp']}):")
                    for dn, di in ds["data"].items():
                        if isinstance(di, dict):
                            print(f"      {dn}: online={di.get('online', '?')}, "
                                  f"state={di.get('state', 'N/A')}")
                else:
                    print(f"    设备状态: 无数据")

        elif choice == "22":
            g = int(input("  组号 1/2 (默认1): ").strip() or "1")
            if g not in (1, 2):
                print("  组号无效，应为 1 或 2")
                continue
            await send_cmd_to_client(ws, {
                "cmd": 106, "token": token, "groupNum": g
            })

        elif choice == "0":
            continue

        else:
            print("  无效选择")


# ============================================================
#  启动
# ============================================================
async def main():
    use_tls = os.path.exists(SSL_CERT) and os.path.exists(SSL_KEY)
    proto = "wss" if use_tls else "ws"

    print(f"\n{'=' * 55}")
    print(f"  边检机器人 WebSocket 模拟服务端 V4.1")
    print(f"  监听: {proto}://{HOST}:{PORT}/ws")
    print(f"  客户端连接: {proto}://127.0.0.1:{PORT}/ws")
    print(f"  TLS: {'已启用' if use_tls else '未启用（证书缺失）'}")
    print(f"  预设指令: {'开启' if AUTO_SEND_PRESETS else '关闭'}")
    print(f"{'=' * 55}")
    print(f"  等待客户端连接...")

    if use_tls:
        ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_context.load_cert_chain(SSL_CERT, SSL_KEY)
    else:
        ssl_context = None
        print("  [警告] 未找到 cert.pem/key.pem，回退到 ws:// 模式")
        print("  生成证书: openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj '/CN=localhost'")

    async with websockets.serve(on_connection, HOST, PORT, ssl=ssl_context):
        await console_menu()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n服务端已关闭。")