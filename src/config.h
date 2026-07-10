/**
 * @file    config.h
 * @brief   全局配置常量
 *
 *          所有可配置的参数集中在此文件，方便后续修改。
 *          硬件接口、网络参数、设备参数等均在此定义。
 */

#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  网络配置                                                                  */
/* ======================================================================== */

/** 中心服务器 WebSocket 地址（控制板主动连接中心服务器） */
#define WS_SERVER_URL           "wss://127.0.0.1:8989/ws"

/** WebSocket 鉴权 Token */
#define WS_AUTH_TOKEN           "your-api-key-here"

/** WebSocket 重连间隔（秒） */
#define WS_RECONNECT_INTERVAL   5

/** WebSocket PING 间隔（秒） */
#define WS_PING_INTERVAL        30

/* ======================================================================== */
/*  设备认证配置（cmd=11）                                                      */
/* ======================================================================== */

/** 设备 MAC 地址（唯一标识） */
#define DEVICE_MAC              "00:11:22:33:44:55"

/** API Key（RSA 加密后发送） */
#define DEVICE_API_KEY          "your-rsa-encrypted-api-key"

/* ======================================================================== */
/*  硬件接口配置                                                              */
/* ======================================================================== */

/* --- 云台（串口）--- */
#define PTZ_UART_DEV            "/dev/ttyS8"        /**< 串口设备节点    */
#define PTZ_UART_BAUDRATE       2400              /**< 波特率          */

/* --- 警灯（GPIO）--- */
#define ALARM_GPIO_CHIP         "/dev/gpiochip0"    /**< GPIO 芯片       */
#define ALARM_GPIO_PIN          17                  /**< GPIO 引脚编号   */

/* --- LED 字幕屏（串口/网口，预留）--- */
#define LED_UART_DEV            "/dev/ttyS2"
#define LED_UART_BAUDRATE       115200
#define LED_NET_IP              "192.168.88.199"      /**< 若走网口        */
#define LED_NET_PORT            5000

/* --- 音响（USB）--- */
#define SPEAKER_USB_DEV         "/dev/snd/"         /**< ALSA 音频设备   */

/* --- 氛围灯（预留）--- */
#define MOOD_UART_DEV           "/dev/ttyS3"
#define MOOD_UART_BAUDRATE      115200

/* ======================================================================== */
/*  系统参数                                                                  */
/* ======================================================================== */

/** 环形缓冲区容量（消息条数） */
#define RING_BUFFER_CAPACITY    64

/** 心跳发送间隔（秒） */
#define HEARTBEAT_INTERVAL      10

/** 主循环轮询间隔（微秒） */
#define MAIN_LOOP_SLEEP_US      10000

/** 单条消息最大长度（字节） */
#define MAX_MSG_LENGTH          4096

/** JSON 解析最大嵌套深度 */
#define JSON_MAX_DEPTH          8

/* ======================================================================== */
/*  设备控制板信息                                                            */
/* ======================================================================== */

#define CTRL_BOARD_MODEL        "RobotController-V1.0"
#define CTRL_BOARD_FW_VERSION   "2"

/* ======================================================================== */
/*  API 错误码（V2.6）                                                        */
/* ======================================================================== */

#define ERR_SUCCESS             0       /**< 成功                           */
#define ERR_PARAM               1001    /**< 参数错误                       */
#define ERR_DEVICE_OFFLINE      1002    /**< 设备离线                       */
#define ERR_NOT_SUPPORTED       1003    /**< 设备不支持该操作                */
#define ERR_TIMEOUT             1004    /**< 操作超时                       */
#define ERR_INVALID_TOKEN       2001    /**< Token 无效                     */
#define ERR_TOKEN_EXPIRED       2002    /**< Token 过期                     */
#define ERR_SERVER_INTERNAL     3001    /**< 服务器内部错误                 */
#define ERR_DATABASE            3002    /**< 数据库错误                     */
#define ERR_DEVICE_COMM         3003    /**< 设备通信失败                   */

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
