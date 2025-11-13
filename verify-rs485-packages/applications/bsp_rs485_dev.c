/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-11-03     Administrator       the first version
 */
#include "bsp_sys.h"
#include "bsp_rs485_drv.h"

#define DBG_TAG "rs485.dev"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>


/***
 * @param dev
 * @param oflag 作用：告诉驱动程序，以何种方式打开设备，此处未使用
 * @return
 */
static rt_err_t rs485_dev_open(rt_device_t dev, rt_uint16_t oflag)
{
    /* 把dev结构体指针赋值给pdev */
    rs485_dev_t *pdev = (rs485_dev_t *)dev;
    return(rs485_connect(pdev->hinst));
}

/***
 * @brief
 * @param dev
 * @return
 */
static rt_err_t rs485_dev_close(rt_device_t dev)
{
    rs485_dev_t *pdev = (rs485_dev_t *)dev;
    return(rs485_disconn(pdev->hinst));
}

/***
 * @param dev       设备句柄
 * @param pos       参数会被传入但忽略(RS485是流设备)
 * @param buffer    接收来自从机的数据
 * @param size      决定最多读取多少字节(可能返回少于size的值)
 * @return  返回实际读取的字节数(≥0)或者错误码
 */
static rt_size_t rs485_dev_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rs485_dev_t *pdev = (rs485_dev_t *)dev;
    return(rs485_recv(pdev->hinst, buffer, size));
}

/***
 * @param dev       设备句柄
 * @param pos       写入偏移量，对流式设备（如串口、RS485）通常被忽略
 * @param buffer    用户数据缓冲区，包含要发送的数据（只读）
 * @param size      要发送的字节数
 * @return 返回实际写入的字节数；成功 ≥0，失败返回负值（如 RT_ERROR）
 */
static rt_size_t rs485_dev_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    rs485_dev_t *pdev = (rs485_dev_t *)dev;
    return(rs485_send(pdev->hinst, (void *)buffer, size));
}



/**
 * @brief RS485 设备控制接口函数
 *
 * 用户通过 rt_device_control() 调用此函数，对 RS485 设备进行配置、超时设置、发送接收等操作
 *
 * @param dev   设备句柄（指向 rs485_dev_t 私有结构体）
 * @param cmd   控制命令（自定义，如 RS485_CTRL_CFG）
 * @param args  命令参数指针（根据 cmd 不同，指向不同结构体）
 *
 * @return rt_err_t  RT_EOK 成功，其他为错误码
 */
static rt_err_t rs485_dev_control(rt_device_t dev, int cmd, void *args)
{
    rt_err_t rst = RT_EINVAL;               // 默认返回“无效参数”
    rs485_dev_t *pdev = (rs485_dev_t *)dev; // 转为私有设备结构体


    switch(cmd)
    {
        /* ============================================= */
        /* 1. 配置串口参数：波特率、数据位、校验位、停止位 */
        /* ============================================= */
        case RS485_CTRL_CFG:
            if (args)   // 必须传入参数结构体
            {
                // args 指向用户传入的配置参数结构体
                rs485_dev_cfg_param_t *params = (rs485_dev_cfg_param_t *)args;
                // 调用底层 HAL 层配置函数
                rst = rs485_config( pdev->hinst,
                                    params->baudrate,   // 波特率
                                    params->databits,   // 数据位：7/8/9
                                    params->parity,     // 校验：NONE/ODD/EVEN
                                    params->stopbits);  // 停止位：1/2
            }
            // 若 args == NULL，保持 rst = RT_EINVAL
            break;


        /* ============================================= */
        /* 2. 设置接收超时时间（应答超时 + 字节间超时） */
        /* ============================================= */
        case RS485_CTRL_SET_TMO:
            if (args)
            {
                rs485_dev_tmo_param_t *params = (rs485_dev_tmo_param_t *)args;
                // 设置整帧应答超时（从发送完到开始接收的等待时间）
                rst = rs485_set_recv_tmo(pdev->hinst, params->ack_tmo_ms);
                // 一旦失败就提前退出
                if (rst != RT_EOK) break;
                // 设置字节间超时（接收过程中两个字节之间的最大间隔）
                rst = rs485_set_byte_tmo(pdev->hinst, params->byte_tmo_ms);
            }
            break;

        /* ============================================= */
        /* 3. 中断当前正在进行的接收操作 */
        /* ============================================= */
        case RS485_CTRL_BREAK_RECV:
            // 立即终止当前接收，适用于超时未响应或需要取消
            rst = rs485_break_recv(pdev->hinst);
            break;

        /* ============================================= */
        /* 4. 发送一帧数据后立即切换为接收模式（典型 Modbus 场景） */
        /* ============================================= */
        case RS485_CTRL_SEND_THEN_RECV:
            if (args)
            {
                rs485_dev_send_then_recv_param_t *params = (rs485_dev_send_then_recv_param_t *)args;

                // 一站式操作：发送 → 自动切换方向 → 接收响应
                rst = rs485_send_then_recv( pdev->hinst,
                                            params->sbuf,   // 发送缓冲区
                                            params->slen,   // 发送长度
                                            params->rbuf,   // 接收缓冲区
                                            params->rlen);  // 期望接收长度
            }
            break;
        default:
            break;
    }
    return(rst);
}



#ifdef RT_USING_DEVICE_OPS
/**
     * @brief RS485 设备的操作方法表（使用 rt_device_ops 方式注册）
     *
     * 当宏 RT_USING_DEVICE_OPS 启用时，驱动通过 .ops 字段注册回调函数
     * 比传统方式（直接赋值 dev->open/read/write...）更灵活、安全、统一
     */
static const struct rt_device_ops rs485_dev_ops = {
        .init   = RT_NULL,          // 初始化函数（可选，RS485 通常在 open 时初始化）
        .open   = rs485_dev_open,   // 打开设备
        .close  = rs485_dev_close,  // 关闭设备
        .read   = rs485_dev_read,   // 读数据（非阻塞/阻塞）
        .write  = rs485_dev_write,  // 写数据
        .control= rs485_dev_control // 控制接口（配置、发后收等）
    };
#endif




/**
 * @brief RS485 设备配置表（编译时静态配置）
 */
static const rs485_dev_cfg_t rs485_dev_cfg_table[] = RS485_DEV_CFG_TABLE;

/**
 * @brief 自动创建所有 RS485 设备
 *
 * 根据 rs485_dev_cfg_table 中的配置，动态创建多个 RS485 设备实例
 *
 * @return rt_err_t  RT_EOK 成功，-RT_ENOMEM 内存不足
 */
static int rs485_dev_create(void)
{
    // 计算设备数量
    for(int i=0; i<(sizeof(rs485_dev_cfg_table)/sizeof(rs485_dev_cfg_table[0])); i++)
    {
        // 1. 分配私有设备结构体（清零初始化）
        rs485_dev_t *dev = (rs485_dev_t *)rt_calloc(1, sizeof(rs485_dev_t));
        if (dev == RT_NULL)
        {
            rt_kprintf("rs485 alloc failed!\n");
            return(-RT_ENOMEM);// 内存不足
        }

        // 2. 获取当前设备的配置参数
        rs485_dev_cfg_t *pcfg = (rs485_dev_cfg_t *)rs485_dev_cfg_table + i;

        // 3. 创建底层 RS485 实例（HAL 层）
        // 参数：串口名、波特率、校验位、DE引脚、DE电平
        dev->hinst = rs485_create( pcfg->serial,
                                   pcfg->baudrate,
                                   pcfg->parity,
                                   pcfg->pin,
                                   pcfg->level);

        if (dev->hinst == RT_NULL)
        {
            rt_free(dev);
            rt_kprintf("rs485_create %s failed!\n", pcfg->rs485);
            return(-RT_ENOMEM);
        }

        // 4. 设置设备类型为字符设备
        dev->parent.type = RT_Device_Class_Char;

        // 5. 注册操作函数表
        #ifdef RT_USING_DEVICE_OPS
        dev->parent.ops = &rs485_dev_ops;  // 新方式（推荐）
        #else
        // 旧方式：逐个赋值（为了兼容性保留）
        dev->parent.init = NULL;
        dev->parent.open = (void *)rs485_dev_open;
        dev->parent.close = (void *)rs485_dev_close;
        dev->parent.read = (void *)rs485_dev_read;
        dev->parent.write = (void *)rs485_dev_write;
        dev->parent.control = (void *)rs485_dev_control;
        #endif

        // 6. 注册设备到系统
        rt_device_register( (rt_device_t)dev,
                            pcfg->rs485,
                            RT_DEVICE_FLAG_RDWR |   // 可读写
                            RT_DEVICE_FLAG_STREAM | // 流方式
                            RT_DEVICE_FLAG_DMA_RX | // DMA接收
                            RT_DEVICE_FLAG_DMA_TX); // DMA发送
    }

    return(RT_EOK);
}
INIT_DEVICE_EXPORT(rs485_dev_create);

















