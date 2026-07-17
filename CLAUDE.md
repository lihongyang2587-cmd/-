# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**重要约定**：本项目所有代码修改**不需要编译测试**，开发环境在目标机器上，本地无交叉编译工具链。代码修改完成后直接说明变更内容即可，无需执行 `make` 验证编译。

## 项目概述

边检机器人控制板嵌入式 C 程序，运行在 Ubuntu 22.04 上。控制板通过 WebSocket 连接中心服务器，接收 JSON 格式的 cmd 指令，控制云台、LED 字幕屏、音箱、警灯、氛围灯等外设，并定时上报心跳和设备状态。

## 构建与运行

```bash
# 构建（在 src/ 目录下）
cd src && make          # 编译生成 device_app
make clean              # 清理编译产物

# 直接运行控制板程序
./device_app

# 运行模拟服务端（测试用，在项目根目录）
python3 clientv2.py     # 监听 wss://0.0.0.0:8989/ws，提供交互式控制台
```

编译依赖：`gcc`, `g++`, `libasound2-dev` (ALSA), `libpthread`。LED SDK 动态库 `libledplayer7.so` 放在 `src/thirdparty/led/` 下。

## 目录结构

```
src/
├── Makefile                     # 顶层 Makefile，-I 覆盖所有子目录
├── main.c / main.h              # 程序入口，启动流程与主循环
├── config.h                     # 全局配置常量（串口、GPIO、网络、系统参数）
│
├── core/                        # 基础数据结构
│   ├── ring_buffer.h/.c         #   线程安全环形缓冲（WS recv → 主循环）
│   ├── task_queue.h/.c          #   线程安全阻塞队列（mutex+cond，主线程→worker）
│   └── ws_send_queue.h/.c       #   WS 发送串行化队列（封装发送线程）
│
├── network/                     # WebSocket 通信层
│   ├── ws_client.h/.c           #   mongoose WebSocket 客户端，自动重连/PING
│   ├── mongoose.h/.c            #   第三方 WebSocket 库
│   ├── heartbeat.h/.c           #   心跳定时发送线程
│   └── device_auth.h/.c         #   设备认证请求构建
│
├── protocol/                    # 协议解析与消息构建
│   ├── cmd_parser.h/.c          #   JSON 解析，提取 cmd_id/token/字段
│   ├── cmd_dispatch.h/.c        #   cmd_id → handler 查表，返回设备类型
│   └── msg_builder.h/.c         #   出站 JSON 构建（心跳/状态/响应/异常上报）
│
├── devices/                     # 外设驱动模块
│   ├── ptz/                     #   云台（Pelco-D 协议，UART /dev/ttyS8）
│   │   ├── device_ptz.h/.c      #     云台命令执行入口
│   │   ├── gimbal.h/.c          #     高层角度/预置位/巡航/扫描接口
│   │   ├── pelco_d.h/.c         #     Pelco-D 7字节帧编解码
│   │   └── serial_port.h/.c     #     Linux 串口封装
│   ├── led/                     #   LED 字幕屏（灵信视觉 SDK）
│   │   ├── device_led.h         #     C 兼容头
│   │   └── device_led.cpp       #     C++ 实现，调用 libledplayer7.so
│   ├── speaker/                 #   音箱（USB ALSA 音频）
│   ├── alarm/                   #   警灯（GPIO，15 种灯光模式）
│   ├── mood/                    #   氛围灯（预留串口，无 cmd 字段）
│   └── system/                  #   系统管理（状态/校时/版本/升级/配置）
│
├── config/                      # 配置管理
│   └── config_manager.h/.c      #   7 个 JSON 配置文件，启动恢复→同步→增量更新
│
├── thirdparty/                  # 第三方库
│   ├── cjson/                   #   cJSON 轻量 JSON 解析库
│   └── led/                     #   灵信视觉 ledplayer7 SDK（.so + .h）
│
└── font/                        # 字体文件
    └── simsun.ttc
```

**include 路径说明**：Makefile 将所有子目录加入 `-I` 编译选项，源码中 `#include "xxx.h"` 使用引号格式，编译器会通过 `-I` 路径自动解析，无需写相对路径。仅 `device_led.cpp` 中有一处 `#include "ledplayer7.h"` 指向 `thirdparty/led/` 下的 SDK 头文件。

## 架构

### 启动流程（main.c:537-637）

```
1. 注册 SIGINT/SIGTERM → 2. 初始化各设备模块（串口/GPIO/ALSA）
→ 3. config_manager_init() 加载 JSON 配置文件
→ 4. config_manager_restore_device_state() 恢复设备内存状态
→ 5. config_manager_sync_device_status() 采集硬件实际状态并落盘
→ 6. 创建 ring_buffer → 7. 启动 WebSocket 客户端连接服务器
→ 8. do_authenticate() 发送 cmd=11 认证请求，等待 token
→ 9. 启动 send_queue 和 4 个 worker 线程（PTZ/LED/SPK/ALARM）
→ 10. 启动心跳定时器 → 11. 进入主循环
```

### 主循环（main_loop, main.c:430-531）

```
ring_buffer_pop() 取消息 → is_server_ack() 过滤服务端确认
→ cmd_parser_parse() 解析 JSON
→ 按 cmd_id 路由到对应设备：
  · PTZ/LED/SPK/ALARM → task_queue_push() 投递到 worker 线程（非阻塞）
  · SYSTEM (601~607)  → 主线程内联执行 cmd_dispatch_execute()
  · 氛围灯（无 cmd 字段） → 主线程内联执行
→ worker 或主线程生成响应 → ws_send_queue_enqueue() 入队发送
```

### 线程模型

| 线程 | 角色 |
|------|------|
| 主线程 | 消息接收、解析、路由、系统命令执行、氛围灯 |
| WebSocket recv 线程 (mongoose 内部) | 接收消息 → push 到 ring_buffer |
| WebSocket send 线程 (ws_send_queue) | 串行化所有 ws_client_send() 调用（mongoose 非线程安全） |
| PTZ worker | 阻塞等待 task_queue → 执行云台 Pelco-D 串口指令 |
| LED worker | 阻塞等待 task_queue → 执行 LED 字幕屏指令 |
| Speaker worker | 阻塞等待 task_queue → 执行 ALSA 音频播放指令 |
| Alarm worker | 阻塞等待 task_queue → 执行 GPIO 警灯指令 |
| Heartbeat 线程 | 定时构建心跳/状态 JSON → 入队 send_queue |

### 命令 ID 体系（cmd_parser.h:31-51）

| cmd | 功能 | 执行线程 |
|-----|------|---------|
| 11 | 设备认证 | 主线程（启动阶段） |
| 101~105 | 云台（移动/归位/预置位/巡航/扫描） | PTZ worker |
| 201~202 | LED（字幕播放/开关） | LED worker |
| 301~303 | 音箱（播放/设置列表/获取列表） | Speaker worker |
| 401 | 警灯 | Alarm worker |
| 601~607 | 系统管理（状态/校时/版本/升级/配置/心跳/异常上报） | 主线程 |
| 无 cmd | 氛围灯（state + type 字段） | 主线程 |

### 关键模块

- **cmd_parser** — JSON 解析，提取 cmd_id、token、字段。依赖 cJSON。
- **cmd_dispatch** — cmd_id → handler 函数查表，返回设备类型用于路由。
- **msg_builder** — 构建出站 JSON：心跳、设备状态、cmd 响应、异常上报、认证请求。
- **config_manager** — 7 个 JSON 配置文件（`./config/` 下：server/ptz/led/speaker/alarm/mood/system.json），启动时加载→恢复→同步，运行时增量更新。
- **ring_buffer** — 线程安全环形缓冲，WebSocket 接收线程 → 主循环。
- **task_queue** — 线程安全阻塞队列（mutex + cond），主线程 → worker 线程。
- **ws_send_queue** — 串行化发送，解决 mongoose `mg_ws_send()` 非线程安全问题。
- **ws_client** — 基于 mongoose 的 WebSocket 客户端，自动重连、PING 保活。
- **heartbeat** — 独立线程定时发送 `system_heartbeat` 和 `device_status` 事件。

### 设备模块

- **device_ptz** — 云台，Pelco-D 协议（7 字节帧：`FF addr cmd1 cmd2 data1 data2 checksum`），通过串口 (`/dev/ttyS8`, 2400 baud) 通信。V3.1 非阻塞模式。
  - **Pelco-D 关键约定**：云台有手动/自主两种模式。自主模式（自检 preset 105、扫描 preset 89/19、巡航 preset 83/84）下硬件**忽略标准 STOP 帧** (`FF 01 00 00 00 00 01`)。退出自主模式必须先发手动运动指令（如 `gimbal_move(RIGHT, 0x01)` → `FF 01 00 02 01 00 04`）抢占总线切回手动模式，再发 STOP。代码中三处停止逻辑（`hardware_force_stop`、`stop_autonomous_mode`、`device_ptz_init`）均采用 `move → usleep(50ms) → stop` 序列。
  - **角度缓存策略**：心跳和 cmd=601 状态上报只读内存缓存 `g_current_h_angle`/`g_current_v_angle`，不主动查硬件（扫描期间 RS-485 被硬件上报帧占满，查询会超时）。缓存由 cmd=102 和 cmd=101 angle>0（**V3.1 起为绝对角度**，dir=1/2 时 angle=水平绝对目标，dir=3/4 时 angle=俯仰绝对目标）更新。启动时在 `device_ptz_init` 查一次硬件角度初始化缓存。`device_ptz_query_position()` 可用于主动查询（有自主模式保护，扫描期间自动跳过），但 **cmd=601 不再调用它**（避免主线程阻塞）。
- **device_led** — LED 字幕屏，通过串口/网口，依赖灵信视觉 `libledplayer7.so` SDK。
- **device_speaker** — USB 音箱，ALSA 音频播放，支持音频文件下载和管理。
- **device_alarm** — 警灯，GPIO 控制 (`/sys/devices/platform/gpio_out/gpio_out4/value`)，15 种灯光模式通过高低电平脉冲切换。
- **device_mood** — 氛围灯，预留串口 (`/dev/ttyS3`)，无 cmd 字段，直接传 `state` + `type`。
- **device_system** — 系统管理：状态采集、NTP 校时、版本查询、固件升级、服务器配置更新。

### Pelco-D 协议注意事项

云台通信基于 Pelco-D 协议（参考 [接口对照表.md](接口对照表.md)），开发 PTZ 相关功能时必须遵守：

1. **帧格式**：7 字节固定长度 `FF addr cmd1 cmd2 data1 data2 checksum`，校验和 = addr + cmd1 + cmd2 + data1 + data2（取低 8 位）。
2. **手动/自主两种模式**：云台在自主模式（自检/扫描/巡航）下**忽略标准 STOP 帧** (`FF 01 00 00 00 00 01`)。退出自主模式必须先发手动运动指令（`gimbal_move` → `FF 01 00 02/04/08/10 …`）抢占总线切回手动模式，再发 STOP。
3. **半双工 RS-485**：同一时刻总线上只能有一方在发数据。扫描/巡航期间硬件持续发送位置上报帧（~29ms/帧），此时发查询指令（`gimbal_query_pan/tilt`）会冲突超时。**扫描期间禁止主动查询硬件角度**，只用内存缓存。
4. **停止的正确方式**：始终用 `gimbal_move(RIGHT, 0x01)` + `usleep(50000)` + `gimbal_stop()` 序列，不要单独发 STOP 帧。代码中 `hardware_force_stop()`、`stop_autonomous_mode()`、`device_ptz_init()` 三处均遵循此约定。
5. **角度范围**：水平 0~350°，俯仰 -32~+23°。`gimbal_set_pan/tilt` 内部有范围校验，超范围会返回错误。
6. **预置位编号**：第一组 1~16，第二组 33~48。扫描限位 17/18，扫描启动 19，巡航停留时间 70~72，巡航路线 83/84，全角度扫描 89。
7. **相关文件**：`device_ptz.c`（cmd 处理）、`gimbal.c`（Pelco-D 帧收发）、`pelco_d.c`（编解码）、`接口对照表.md`（CLI/C/串口指令对照）。

### 配置（config.h）

所有可配置常量集中在 `config.h`：WebSocket 地址、串口设备节点、GPIO 引脚、心跳间隔、环形缓冲容量等。修改硬件接线或服务器地址时只需改此文件。

## 测试

`clientv2.py` 是 Python 模拟服务端，支持 WSS（需 `cert.pem`/`key.pem`）或 WS 回退。连接后可交互式下发各类 cmd 指令（菜单选项 1~18），也可直接输入自定义 JSON。认证成功后可自动推送预设指令序列。

```bash
# 生成自签名证书（如需 WSS）
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj '/CN=localhost'

# 启动模拟服务端
python3 clientv2.py

# 然后运行控制板程序，控制板会连接 127.0.0.1:8989
```

## 消息协议

- **入站**（Server → Board）：`{"cmd": <int>, "token": "...", ...其他参数}`
- **出站事件**（Board → Server）：`{"event": "...", "timestamp": "...", "data": {...}}`
- **出站 cmd 响应**（Board → Server）：`{"code": 0, "cmd": <int>, "msg": "success", ...}`
- **认证**：Board 发送 `{"cmd": 11, "deviceMac": "...", "apiKey": "...", "deviceTimes": "..."}`，Server 返回 token
- **服务端 ACK**：Server 收到 event 后回复对应的 `_ack` 消息（如 `system_heartbeat_ack`），Board 主循环识别后静默跳过
