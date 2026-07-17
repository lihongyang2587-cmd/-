# Gimbal Controller

Ubuntu 22.04 下的云台控制 C 项目，基于需求表和串口指令表实现。

## 功能

- 水平/垂直点动控制
- 停止控制
- 速度设置
- 预置位设置、调用、删除
- 巡航停留时间设置
- 启动巡航
- 限位扫描起点设置
- 启动限位扫描 / 最大角度扫描
- 看守位开关、归位时间设置
- 云台自检开关
- 辅助开关
- 水平/垂直角度查询
- 水平/垂直角度设置

## 协议说明

项目按 `FF 地址 命令1 命令2 数据1 数据2 校验` 组织 7 字节串口帧，校验为地址到数据2的低 8 位和。

默认串口参数：

- 波特率 `2400`
- 数据位 `8`
- 停止位 `1`
- 校验位 `none`

如果你的设备参数不同，可通过命令行参数覆盖。

## 构建

```bash
sudo apt update
sudo apt install -y build-essential cmake

cd code
cmake -S . -B build
cmake --build build
```

## 使用

```bash
./build/gimbal_cli --device /dev/ttyUSB0 status
./build/gimbal_cli --device /dev/ttyUSB0 move up --speed 32 --duration-ms 500
./build/gimbal_cli --device /dev/ttyUSB0 angle-set-pan 180
./build/gimbal_cli --device /dev/ttyUSB0 preset-set 1
./build/gimbal_cli --device /dev/ttyUSB0 preset-call 1
./build/gimbal_cli --device /dev/ttyUSB0 scan-start
```

更多命令：

```bash
./build/gimbal_cli --help
```

## 角度换算

- 水平角度：协议使用 `0.8` 度为一个单位，300 度对应约 `375`。
- 垂直角度：协议使用约 `0.352` 度为一个单位，80 度对应约 `227`。

由于原始表格没有给出完整设备返回格式和极值边界说明，项目按表格中的样例值实现换算。若实机存在偏差，可只调整 [src/gimbal.c](D:\traework\云台\code\src\gimbal.c) 中的换算常量。
