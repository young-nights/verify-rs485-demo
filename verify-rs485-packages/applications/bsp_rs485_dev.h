/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-11-03     Administrator       the first version
 */
#ifndef APPLICATIONS_MACBSP_INC_BSP_RS485_DEV_H_
#define APPLICATIONS_MACBSP_INC_BSP_RS485_DEV_H_

#include "bsp_sys.h"

#ifdef __cplusplus
extern "C"
{
#endif

//rs485-device-name, serial-device-name, baudrate, parity(0~2), control-pin, send-level(0~1)
#define RS485_DEV_CFG_TABLE {                     \
    {"rs485-1", "uart3",    9600, 0,  79,  1}, \
}

#define RS485_CTRL_CFG              0
#define RS485_CTRL_SET_TMO          1
#define RS485_CTRL_BREAK_RECV       2
#define RS485_CTRL_SEND_THEN_RECV   3


/**
 * @brief RS485 设备配置表项结构体
 *
 * 该结构体用于 **静态配置** 系统中所有的 RS485 设备实例。
 * 通过宏 `RS485_DEV_CFG_TABLE` 展开为数组，驱动在初始化时遍历创建多个设备。
 *
 * @note 所有成员均为 **const**，存放在 Flash（.rodata）中，节省 RAM。
 */
typedef struct {
    /**
     * @brief RS485 设备在系统中的名字（注册到 /dev 下的设备名）
     * @example "rs485_1", "rs485_modbus", "rs485_sensor"
     * @note 必须唯一，不能重复
     */
    const char *rs485;

    /**
     * @brief 底层串口设备名（UART 设备）
     * @example "uart1", "uart2", "uart3"
     * @note 必须是系统中已注册的 UART 设备
     */
    const char *serial;

    /**
     * @brief 默认通信波特率（bits per second）
     * @example 9600, 19200, 38400, 57600, 115200
     * @note 可在运行时通过 control 接口修改
     */
    int baudrate;

    /**
     * @brief 校验位配置
     * @note 常用宏定义（需自行定义）：
     *       PARITY_NONE = 0,   // 无校验
     *       PARITY_ODD  = 1,   // 奇校验
     *       PARITY_EVEN = 2    // 偶校验
     */
    int parity;

    /**
     * @brief RS485 方向控制引脚（DE/RE 引脚）
     * @example 45, 67, 128
     * @note 对应 MCU 的 GPIO 引脚编号（BSP 层定义）
     */
    int pin;

    /**
     * @brief 发送方向控制电平
     * @note
     *       level = 1 : 高电平发送（DE = 1 → 发送）
     *       level = 0 : 低电平发送（DE = 0 → 发送）
     * @warning 必须与硬件电路匹配！
     */
    int level;
} rs485_dev_cfg_t;


/**
 * 配置 UART 底层参数（波特率、数据位、校验位、停止位）,不涉及引脚方向
 */
typedef struct {
    rt_uint32_t baudrate;     // 波特率，如 9600, 115200
    rt_uint8_t  databits;     // 数据位：5~8
    rt_uint8_t  parity;       // 校验：NONE, ODD, EVEN
    rt_uint8_t  stopbits;     // 停止位：1, 2
} rs485_dev_cfg_param_t;




/**
 * @brief RS485 接收超时控制参数
 *
 * 用于通过 `rt_device_control(dev, RS485_CTRL_SET_TMO, &param)` 设置接收超时。
 * 适用于 Modbus、自定义协议等需要精确超时控制的场景。
 *
 * @note 单位：毫秒 (ms)
 */
typedef struct {
    /**
     * @brief 应答超时（Acknowledgment Timeout）
     *
     * 从 **发送完成** 到 **开始接收第一个字节** 的最大等待时间。
     *
     * @example
     *   - 发送 Modbus 请求后，等待从站响应
     *   - 典型值：100 ~ 1000 ms
     *
     * @default 500 ms（推荐）
     */
    int ack_tmo_ms;

    /**
     * @brief 字节间超时（Inter-byte Timeout）
     *
     * 接收过程中，**两个字节之间的最大间隔时间**。
     * 超过此时间认为一帧结束。
     *
     * @example
     *   - Modbus 帧间 3.5 字符时间 ≈ 3.5 * 11 / baudrate 秒
     *   - 9600bps 下约 4ms
     *
     * @default 10 ~ 50 ms（根据波特率调整）
     */
    int byte_tmo_ms;
} rs485_dev_tmo_param_t;





/**
 * @brief RS485 “发送后接收”操作参数结构体
 *
 * 用于 `RS485_CTRL_SEND_THEN_RECV` 命令，实现：
 *   1. 发送一帧数据
 *   2. 自动切换为接收模式
 *   3. 等待响应帧（带超时）
 *
 * 典型应用：Modbus 主站发送读/写命令并等待响应
 */
typedef struct {
    /**
     * @brief 发送缓冲区指针
     * @note 必须是有效内存，不能是局部变量（生命周期要覆盖整个操作）
     */
    void *sbuf;

    /**
     * @brief 发送数据长度（字节）
     * @note 最大值受底层驱动限制（如 256）
     */
    int slen;

    /**
     * @brief 接收缓冲区指针
     * @note 必须足够大，避免溢出
     */
    void *rbuf;

    /**
     * @brief 接收缓冲区大小（最大可接收字节数）
     * @note 实际接收长度通常由底层返回（或通过 rbuf[0] 长度字段）
     */
    int rlen;
} rs485_dev_send_then_recv_param_t;





/**
 * @brief RS485 设备私有数据结构
 *
 * 继承 rt_device，扩展 RS485 专有字段。
 * 每个 RS485 设备实例对应一个该结构体。
 */
typedef struct {
    /**
     * @brief 继承 RT-Thread 通用设备结构体
     *
     * 必须是 **第一个成员**，支持类型强转：
     *     rs485_dev_t *pdev = (rs485_dev_t *)dev;
     *
     * 包含：
     *   - type, flag, open_flag
     *   - init, open, close, read, write, control
     *   - user_data, rx_indicate, tx_complete
     */
    struct rt_device parent;

    /**
     * @brief 底层 RS485 实例句柄（HAL 层）
     *
     * 由 `rs485_create()` 返回，包含：
     *   - UART 句柄
     *   - DE 引脚控制
     *   - DMA/中断配置
     *   - 接收缓冲环形队列
     *
     * @warning 不能直接访问，应通过 HAL 接口
     */
    rs485_inst_t *hinst;
} rs485_dev_t;

#ifdef __cplusplus
}
#endif

#endif /* APPLICATIONS_MACBSP_INC_BSP_RS485_DEV_H_ */
