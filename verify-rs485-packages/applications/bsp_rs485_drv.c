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


#define DBG_TAG "rs485"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define RS485_EVT_RX_IND    (1<<0)  // Receive Indicate - 接收表明
#define RS485_EVT_RX_BREAK  (1<<1)  // 用于标记总线"断裂"



struct rs485_inst
{
    rt_device_t serial;     // 串口设备句柄
    rt_mutex_t lock;        // 互斥锁句柄
    rt_event_t evt;         // 事件句柄
    rt_uint8_t status;      // 连接状态
    rt_uint8_t level;       // RE/DE 控制引脚的电平状态
    rt_int16_t pin;         // RE/DE 引脚序号（-1：未使用）
    rt_int32_t timeout;     // 接收超时时间
    rt_int32_t byte_tmo;    // 接收字节的时间区间
#ifdef RS485_USING_DMA_TX
    rt_int32_t tx_dly_ms;
    struct rt_completion tx_comp;//send completion
#endif
};


#ifdef RS485_USING_DMA_TX
static rt_err_t rs485_send_comp_hook(rt_device_t dev, void *buffer)
{
    rs485_inst_t *hinst = (rs485_inst_t *)(dev->user_data);
    rt_completion_done(&(hinst->tx_comp));
    return(RT_EOK);
}
#endif


static rt_err_t rs485_recv_ind_hook(rt_device_t dev, rt_size_t size)
{
    rs485_inst_t *hinst = (rs485_inst_t *)(dev->user_data);

    if (hinst->evt){
        rt_event_send(hinst->evt, RS485_EVT_RX_IND);
    }
    return(RT_EOK);
}



/* rs485_cal_byte_tmo() 就是“波特率→毫秒”的换算表，把 3.5 字符时间换算出来并锁在 [1,20] ms 安全区，给接收状态机当“帧间隔”用 */
static int rs485_cal_byte_tmo(int baudrate)
{
    /* 发送 40 bit 需要的时间折成毫秒 */
    int tmo = (40 * 1000) / baudrate;

    /* 算得太小（高速波特率）时，给个下限，防止系统定时器精度不够导致超时 */
    if (tmo < RS485_BYTE_TMO_MIN){
        tmo = RS485_BYTE_TMO_MIN;
    }
    /* 算得太大（低速波特率）时，给上限，避免一帧要等几百毫秒，系统响应太慢 */
    else if (tmo > RS485_BYTE_TMO_MAX){
        tmo = RS485_BYTE_TMO_MAX;
    }
    return (tmo);
}



// mode : 0--receive mode, 1--send mode
static void rs485_mode_set(rs485_inst_t *hinst, int mode)
{
    /* 若板子用硬件自动控制 DE/RE（如带 auto-485 的 UART），GPIO 脚未配置，直接退出 */
    if (hinst->pin < 0){
        return;
    }

    //切到发送分支
    if (mode)
    {
        rt_pin_write(hinst->pin, hinst->level);

        /*某些收发器要求 DE 上升沿后 延迟 ≥ x µs 才能开始发第一个字节，否则首字节会丢*/
#if (RS485_SW_DLY_US > 0)
        rt_hw_us_delay(RS485_SW_DLY_US);
#endif
    }
    // 切到接收分支
    else
    {
        /* 必须先保证 最后一位数据已经离开 UART 引脚，否则 DE 一关就把尾巴砍了 */
        /***
         * a. 若启用了 DMA 发送：rt_completion_wait(&hinst->tx_comp, RS485_TX_COMP_TMO_MAX)
                                  → 等 DMA 传输完成中断里 rt_completion_done() 释放；
                                                                        再 rt_thread_mdelay(hinst->tx_dly_ms)
                                  → 有些收发器 停止位后仍需保持 DE 有效 1-2 ms，靠这个参数补
         */
#ifdef RS485_USING_DMA_TX
        rt_completion_wait(&(hinst->tx_comp), RS485_TX_COMP_TMO_MAX);
        rt_thread_mdelay(hinst->tx_dly_ms);//等待末尾数据传输完成

       /**
        * b. 若只是 中断/polling 发送 且定义了 RS485_SW_DLY_US：直接阻塞 rt_hw_us_delay()，让最后一个停止位有时间输出
        */
#elif (RS485_SW_DLY_US > 0)
        rt_hw_us_delay(RS485_SW_DLY_US);
#endif
        rt_pin_write(hinst->pin, !hinst->level);
    }
}




static int rs485_dev_open(rs485_inst_t *hinst)
{
#ifdef RS485_USING_DMA_RX

    #ifdef RS485_USING_DMA_TX
    if (rt_device_open(hinst->serial, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_DMA_RX | RT_DEVICE_FLAG_DMA_TX) == RT_EOK)
    {
        return(RT_EOK);
    }
    #endif

    #ifdef RS485_USING_INT_TX
    if (rt_device_open(hinst->serial, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_DMA_RX | RT_DEVICE_FLAG_INT_TX) == RT_EOK)
    {
        return(RT_EOK);
    }
    #endif

    if (rt_device_open(hinst->serial, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_DMA_RX) == RT_EOK)
    {
        return(RT_EOK);
    }

#endif

    #ifdef RS485_USING_DMA_TX
    if (rt_device_open(hinst->serial, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_DMA_TX) == RT_EOK)
    {
        return(RT_EOK);
    }
    #endif

    #ifdef RS485_USING_INT_TX
    if (rt_device_open(hinst->serial, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_INT_TX) == RT_EOK)
    {
        return(RT_EOK);
    }
    #endif

    if (rt_device_open(hinst->serial, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX) == RT_EOK)
    {
        return(RT_EOK);
    }

    return(-RT_ERROR);
}





/*
 * @brief   create rs485 instance dynamically
 * @param   serial      - serial device name
 * @param   baudrate    - serial baud rate
 * @param   parity      - serial parity mode
 * @param   pin         - mode contrle pin
 * @param   level       - send mode level
 * @retval  instance handle
 */
rs485_inst_t * rs485_create(const char *name, int baudrate, int parity, int pin, int level)
{
    rs485_inst_t *hinst;
    rt_device_t rs485_dev;

    /* 1.查找设备 */
    rs485_dev = rt_device_find(name);
    if (rs485_dev == RT_NULL)
    {
        LOG_E("rs485 instance initiliaze error, the serial device(%s) no found.", name);
        return(RT_NULL);
    }

    /* 2.类型安检：RT-Thread里所有设备都用统一设备框架注册，但类型分得细——只有 RT_Device_Class_Char 才是“串口” */
    if (rs485_dev->type != RT_Device_Class_Char)
    {
        LOG_E("rs485 instance initiliaze error, the serial device(%s) type is not char.", name);
        return(RT_NULL);
    }

    /* 3. 给RS485创建一个内存块 */
    hinst = rt_malloc(sizeof(struct rs485_inst));
    if (hinst == RT_NULL)
    {
        LOG_E("rs485 create fail. no memory for rs485 create instance.");
        return(RT_NULL);
    }

    /* 4. 创建一个互斥锁：保证多线程同时调用 send/recv 不会撞车，同时也防止低优先级进程长期占用把高优先级饿死 */
    hinst->lock = rt_mutex_create(name, RT_IPC_FLAG_FIFO);
    if (hinst->lock == RT_NULL)
    {
        rt_free(hinst);
        LOG_E("rs485 create fail. no memory for rs485 create mutex.");
        return(RT_NULL);
    }

    /* 5. 给RS-485实例配一个事件对象，用来让中断通知线程“有字节到了 / 帧结束了 */
    hinst->evt = rt_event_create(name, RT_IPC_FLAG_FIFO);
    if (hinst->evt == RT_NULL)
    {
        rt_mutex_delete(hinst->lock);
        rt_free(hinst);
        LOG_E("rs485 create fail. no memory for rs485 create event.");
        return(RT_NULL);
    }

#ifdef RS485_USING_DMA_TX
    hinst->tx_dly_ms = ((2 * 11 *1000) / baudrate) + 1;
    rt_completion_init(&(hinst->tx_comp));
#endif

    hinst->serial = rs485_dev;
    hinst->status = 0;
    hinst->pin = pin;
    hinst->level = (level != 0);
    hinst->timeout = 0;
    hinst->byte_tmo = rs485_cal_byte_tmo(baudrate);

    rs485_config(hinst, baudrate, 8, parity, 0);

    LOG_D("rs485 create success.");

    return(hinst);
}


/*
 * @brief   destory rs485 instance created dynamically
 * @param   hinst       - instance handle
 * @retval  0 - success, other - error
 */
int rs485_destory(rs485_inst_t * hinst)
{
    if (hinst == RT_NULL){
        LOG_E("rs485 destory fail. hinst is NULL.");
        return(-RT_ERROR);
    }

    rs485_disconn(hinst);

    /* 锁存在才删，删完立即清零指针，防止野指针和重复删除 */
    if (hinst->lock){
        rt_mutex_delete(hinst->lock);
        hinst->lock = RT_NULL;
    }

    /* 安全删除事件集 */
    if (hinst->evt){
        rt_event_delete(hinst->evt);
        hinst->evt = RT_NULL;
    }

    /* 释放空间 */
    rt_free(hinst);

    LOG_D("rs485 destory success.");

    return(RT_EOK);
}



/*
 * @brief   config rs485 params
 * @param   hinst       - instance handle
 * @param   baudrate    - baudrate of communication
 * @param   databits    - data bits, 5~8
 * @param   parity      - parity bit, 0~2, 0 - none, 1 - odd, 2 - even
 * @param   stopbits    - stop bits, 0~1, 0 - 1 stop bit, 1 - 2 stop bits
 * @retval  0 - success, other - error
 */
int rs485_config(rs485_inst_t * hinst, int baudrate, int databits, int parity, int stopbits)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

    if (hinst == RT_NULL){
        LOG_E("rs485 config fail. hinst is NULL.");
        return(-RT_ERROR);
    }

#ifdef RS485_USING_DMA_TX
    hinst->tx_dly_ms = ((2 * 11 *1000) / baudrate) + 1;
#endif

    hinst->byte_tmo = rs485_cal_byte_tmo(baudrate);

    config.baud_rate = baudrate;
    config.data_bits = databits;
    config.parity = parity;
    config.stop_bits = stopbits;
    rt_device_control(hinst->serial, RT_DEVICE_CTRL_CONFIG, &config);

    return(RT_EOK);
}

/*
 * @brief   set wait datas timeout for receiving
 * @param   hinst       - instance handle
 * @param   tmo_ms      - receive wait timeout, 0--no wait, <0--wait forever, >0--wait timeout, default = 0
 * @retval  0 - success, other - error
 */
int rs485_set_recv_tmo(rs485_inst_t * hinst, int tmo_ms)
{
    if (hinst == RT_NULL){
        LOG_E("rs485 set recv timeout fail. hinst is NULL.");
        return(-RT_ERROR);
    }

    /* 把超时时间写进实例 */
    hinst->timeout = tmo_ms;

    LOG_D("rs485 set recv timeout success. the value is %d.", tmo_ms);

    return(RT_EOK);
}

/*
 * @brief   set byte interval timeout for receiving
 * @param   hinst       - instance handle
 * @param   tmo_ms      - byte interval timeout, default is calculated from baudrate
 * @retval  0 - success, other - error
 * @note    “帧结束标尺”的旋钮，设得太短易断帧、太长拖响应，函数只做范围限定和存值，具体判帧逻辑在接收流程里兑现
 */
int rs485_set_byte_tmo(rs485_inst_t * hinst, int tmo_ms)
{
    if (hinst == RT_NULL){
        LOG_E("rs485 set byte timeout fail. hinst is NULL.");
        return(-RT_ERROR);
    }

    if (tmo_ms < RS485_BYTE_TMO_MIN){
        tmo_ms = RS485_BYTE_TMO_MIN;
    }
    else if (tmo_ms > RS485_BYTE_TMO_MAX){
        tmo_ms = RS485_BYTE_TMO_MAX;
    }

    hinst->byte_tmo = tmo_ms;

    LOG_D("rs485 set byte timeout success. the value is %d.", tmo_ms);

    return(RT_EOK);
}

/*
 * @brief   open rs485 connect
 * @param   hinst       - instance handle
 * @retval  0 - success, other - error
 */
int rs485_connect(rs485_inst_t * hinst)
{
    if (hinst == RT_NULL){
        LOG_E("rs485 connect fail. hinst is NULL.");
        return(-RT_ERROR);
    }

    if (hinst->status == 1){
        LOG_D("rs485 is connected.");
        return(RT_EOK);
    }

    if ( rs485_dev_open(hinst) != RT_EOK){
        LOG_E("rs485 instance connect error. serial open fail.");
        return(-RT_ERROR);
    }

    if (hinst->pin >= 0){
        rt_pin_mode(hinst->pin, PIN_MODE_OUTPUT);
        rt_pin_write(hinst->pin, ! hinst->level);
    }


    hinst->serial->user_data = hinst;
    hinst->serial->rx_indicate = rs485_recv_ind_hook;
#ifdef RS485_USING_DMA_TX
    hinst->serial->tx_complete = rs485_send_comp_hook;
#endif
    hinst->status = 1;

    LOG_D("rs485 connect success.");

    return(RT_EOK);
}

/*
 * @brief   close rs485 connect
 * @param   hinst       - instance handle
 * @retval  0 - success, other - error
 */
int rs485_disconn(rs485_inst_t * hinst)
{
    if (hinst == RT_NULL)
    {
        LOG_E("rs485 disconnect fail. hinst is NULL.");
        return(-RT_ERROR);
    }

    if (hinst->status == 0)//is not connected
    {
        LOG_D("rs485 is not connected.");
        return(RT_EOK);
    }

    rt_mutex_take(hinst->lock, RT_WAITING_FOREVER);

    if (hinst->serial)
    {
        hinst->serial->rx_indicate = RT_NULL;
#ifdef RS485_USING_DMA_TX
        hinst->serial->tx_complete = RT_NULL;
#endif
        rt_device_close(hinst->serial);
    }

    if (hinst->pin >= 0)
    {
        rt_pin_mode(hinst->pin, PIN_MODE_INPUT);
    }

    hinst->status = 0;

    rt_mutex_release(hinst->lock);

    LOG_D("rs485 disconnect success.");

    return(RT_EOK);
}

/*
 * @brief   receive datas from rs485
 * @param   hinst       - instance handle
 * @param   buf         - buffer addr
 * @param   size        - maximum length of received datas
 * @retval  >=0 - length of received datas, <0 - error
 */
int rs485_recv(rs485_inst_t * hinst, void *buf, int size)
{
    int recv_len = 0;
    rt_uint32_t recved = 0;

    if (hinst == RT_NULL || buf == RT_NULL || size == 0)
    {
        LOG_E("rs485 receive fail. param error.");
        return(-RT_ERROR);
    }

    if (hinst->status == 0)
    {
        LOG_E("rs485 receive fail. it is not connected.");
        return(-RT_ERROR);
    }

    if (rt_mutex_take(hinst->lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        LOG_E("rs485 receive fail. it is destoried.");
        return(-RT_ERROR);
    }

    while(size)
    {
        int len = rt_device_read(hinst->serial, 0, (char *)buf + recv_len, size);
        if (len)
        {
            recv_len += len;
            size -= len;
            continue;
        }
        rt_event_control(hinst->evt, RT_IPC_CMD_RESET, RT_NULL);
        if (recv_len)
        {
            if (rt_event_recv(hinst->evt, RS485_EVT_RX_IND,
                    (RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR), hinst->byte_tmo, &recved) != RT_EOK)
            {
                break;
            }
        }
        else
        {
            if (rt_event_recv(hinst->evt, (RS485_EVT_RX_IND | RS485_EVT_RX_BREAK),
                    (RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR), hinst->timeout, &recved) != RT_EOK)
            {
                break;
            }
            if ((recved & RS485_EVT_RX_BREAK) != 0)
            {
                rt_mutex_release(hinst->lock);
                rt_thread_delay(2);
                return(0);
            }
        }
    }

    rt_mutex_release(hinst->lock);

    return(recv_len);
}

/*
 * @brief   send datas to rs485
 * @param   hinst       - instance handle
 * @param   buf         - buffer addr
 * @param   size        - length of send datas
 * @retval  >=0 - length of sent datas, <0 - error
 */
int rs485_send(rs485_inst_t * hinst, void *buf, int size)
{
    int send_len = 0;

    if (hinst == RT_NULL || buf == RT_NULL || size == 0)
    {
        LOG_E("rs485 send fail. param is error.");
        return(-RT_ERROR);
    }

    if (hinst->status == 0)
    {
        LOG_E("rs485 send fail. it is not connected.");
        return(-RT_ERROR);
    }

    if (rt_mutex_take(hinst->lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        LOG_E("rs485 send fail. it is destoried.");
        return(-RT_ERROR);
    }

    rs485_mode_set(hinst, 1);//set to send mode

    send_len = rt_device_write(hinst->serial, 0, buf, size);

    rs485_mode_set(hinst, 0);//set to receive mode

    rt_mutex_release(hinst->lock);

    return(send_len);
}

/*
 * @brief   break rs485 receive wait
 * @param   hinst       - instance handle
 * @retval  0 - success, other - error
 */
int rs485_break_recv(rs485_inst_t * hinst)
{
    if ((hinst == RT_NULL) || (hinst->evt == RT_NULL))
    {
        return(-RT_ERROR);
    }

    rt_event_send(hinst->evt, RS485_EVT_RX_BREAK);

    return (RT_EOK);
}

/*
 * @brief   send data to rs485 and then receive response data from rs485
 * @param   hinst       - instance handle
 * @param   send_buf    - send buffer addr
 * @param   send_len    - length of send datas
 * @param   recv_buf    - recv buffer addr
 * @param   recv_size   - maximum length of received datas
 * @retval  >=0 - length of received datas, <0 - error
 */
int rs485_send_then_recv(rs485_inst_t * hinst, void *send_buf, int send_len, void *recv_buf, int recv_size)
{
    int recv_len = 0;
    rt_uint32_t recved = 0;

    if (hinst == RT_NULL || send_buf == RT_NULL || send_len == 0 || recv_buf == RT_NULL || recv_size == 0)
    {
        LOG_E("rs485 send then recv fail. param is error.");
        return(-RT_ERROR);
    }

    if (hinst->status == 0)
    {
        LOG_E("rs485 send_then_recv fail. it is not connected.");
        return(-RT_ERROR);
    }

    if (rt_mutex_take(hinst->lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        LOG_E("rs485 send_then_recv fail. it is destoried.");
        return(-RT_ERROR);
    }

    rs485_mode_set(hinst, 1);//set to send mode
    send_len = rt_device_write(hinst->serial, 0, send_buf, send_len);
    rs485_mode_set(hinst, 0);//set to receive mode
    if (send_len < 0)
    {
        rt_mutex_release(hinst->lock);
        LOG_E("rs485 send_then_recv fail. send datas error.");
        return(-RT_ERROR);
    }

    while(recv_size)
    {
        int len = rt_device_read(hinst->serial, 0, (char *)recv_buf + recv_len, recv_size);
        if (len)
        {
            recv_len += len;
            recv_size -= len;
            continue;
        }
        rt_event_control(hinst->evt, RT_IPC_CMD_RESET, RT_NULL);
        if (recv_len)
        {
            if (rt_event_recv(hinst->evt, RS485_EVT_RX_IND,
                    (RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR), hinst->byte_tmo, &recved) != RT_EOK)
            {
                break;
            }
        }
        else
        {
            if (rt_event_recv(hinst->evt, RS485_EVT_RX_IND,
                    (RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR), hinst->timeout, &recved) != RT_EOK)
            {
                break;
            }
        }
    }

    rt_mutex_release(hinst->lock);

    return(recv_len);
}








#ifdef RS485_USING_TEST

#ifndef RS485_TEST_SERIAL
#define RS485_TEST_SERIAL       "uart3"         //default test serial
#endif

#ifndef RS485_TEST_BAUDRATE
#define RS485_TEST_BAUDRATE     115200          //defalut test baudrate
#endif

#ifndef RS485_TEST_PARITY
#define RS485_TEST_PARITY       0               //defalut test parity
#endif

#ifndef RS485_TEST_PIN
#define RS485_TEST_PIN          79              // 表示“无引脚” → 硬件自动切换
#endif

#ifndef RS485_TEST_LEVEL
#define RS485_TEST_LEVEL        1              // 表示“无电平” → 硬件自动切换
#endif

#ifndef RS485_TEST_BUF_SIZE
#define RS485_TEST_BUF_SIZE     1024            //default test buffer size
#endif

#ifndef RS485_TEST_RECV_TMO
#define RS485_TEST_RECV_TMO     30000           //default test recicve timeout
#endif

static rs485_inst_t * test_hinst = RT_NULL;
static char test_buf[RS485_TEST_BUF_SIZE];


static const char *cmd_info[] =
{
    "Usage: \n",
    "rs485 create [serial] [baudrate] [parity] [pin] [level] - create rs485 instance.\n",
    "rs485 destory                                           - destory rs485 instance.\n",
    "rs485 set_recv_tmo [tmo_ms]                             - set recieve timeout.\n",
    "rs485 set_byte_tmo [tmo_ms]                             - set byte timeout.\n",
    "rs485 connect                                           - open rs485 connect.\n",
    "rs485 disconn                                           - close rs485 connect.\n",
    "rs485 recv [size]                                       - receive from rs485.\n",
    "rs485 send [size]                                       - send to rs485.\n",
    "rs485 cfg [baudrate] [databits] [parity] [stopbits]     - config rs485.\n",
    "rs485 send_then_recv [send_size] [recv_size]            - send to rs485 and then receive from rs485.\n",
    "\n"
};

static void show_cmd_info(void)
{
    for(int i=0; i<sizeof(cmd_info)/sizeof(char*); i++)
    {
        rt_kprintf(cmd_info[i]);
    }
}


/**
 * @brief RS485 模块测试命令（FinSH shell 命令）
 *
 * 使用方式：
 *   rs485 create [uart] [baud] [parity] [pin] [level]
 *   rs485 send 16
 *   rs485 recv 32
 *   rs485 send_then_recv 8 32
 *   rs485 destory
 *
 * @param argc  参数总数（包含命令本身）
 * @param argv  参数字符串数组
 */
static void rs485_test(int argc, char **argv)
{

    /* ============================================================= */
    /* 1. 参数检查：至少要有 2 个参数（命令 + 子命令）                                     */
    /* ============================================================= */
    if (argc < 2)
    {
        show_cmd_info();    // 打印帮助信息
        return ;
    }

    /* ============================================================= */
    /* 2. 子命令：create —— 创建 RS485 实例                                                       */
    /* ============================================================= */
    if (strcmp(argv[1], "create") == 0)
    {
        // 默认参数（可通过宏配置）
        char *serial = RS485_TEST_SERIAL;
        int baudrate = RS485_TEST_BAUDRATE;
        int parity = RS485_TEST_PARITY;
        int pin = RS485_TEST_PIN;
        int level = RS485_TEST_LEVEL;

        // 防止重复创建
        if (test_hinst != NULL)
        {
            rt_kprintf("the test instance is not NULL, please deinit/destory first.\n");
            return;
        }

        // 解析用户输入参数（从 argv[2] 开始）
        if (argc >= 3)  serial   = argv[2];
        if ( argc >= 4) baudrate = atoi(argv[3]);
        if (argc >= 5)  parity   = atoi(argv[4]);
        if (argc >= 6)  pin      = atoi(argv[5]);
        if (argc >= 7)  level    = atoi(argv[6]);

        // 创建底层 RS485 实例
        test_hinst = rs485_create(serial, baudrate, parity, pin, level);


        if (test_hinst != NULL)
        {
            rt_kprintf("rs485 instance create success.\n");
            rt_kprintf("rs485 serial            : %s \n", serial);
            rt_kprintf("rs485 baudrate          : %d \n", baudrate);
            rt_kprintf("rs485 parity            : %d \n", parity);
            rt_kprintf("rs485 control pin       : %d \n", pin);
            rt_kprintf("rs485 send mode level   : %d \n", level);

            // 设置默认接收超时
            rs485_set_recv_tmo(test_hinst, RS485_TEST_RECV_TMO);
            rt_kprintf("rs485 receive timeout   : %d \n", RS485_TEST_RECV_TMO);
        }
        return;
    }

    /* ============================================================= */
    /* 3. 子命令：destory —— 销毁实例                                                                  */
    /* ============================================================= */
    if (strcmp(argv[1], "destory") == 0)
    {
        rs485_destory(test_hinst);
        test_hinst = NULL;
        return;
    }

    /* ============================================================= */
    /* 4. 子命令：set_recv_tmo —— 设置应答超时                                                 */
    /* ============================================================= */
    if (strcmp(argv[1], "set_recv_tmo") == 0)
    {
        int tmo_ms = RS485_TEST_RECV_TMO;
        if (argc >= 3)
        {
            tmo_ms = atoi(argv[2]);
        }
        rs485_set_recv_tmo(test_hinst, tmo_ms);
        return;
    }

    /* ============================================================= */
    /* 5. 子命令：set_byte_tmo —— 设置字节间超时                                             */
    /* ============================================================= */
    if (strcmp(argv[1], "set_byte_tmo") == 0)
    {
        int tmo_ms = 0;
        if (argc >= 3)
        {
            tmo_ms = atoi(argv[2]);
        }
        rs485_set_byte_tmo(test_hinst, tmo_ms);
        return;
    }

    /* ============================================================= */
    /* 6. 子命令：connect / disconn —— 连接/断开设备                                    */
    /* ============================================================= */
    if (strcmp(argv[1], "connect") == 0)
    {
        if (test_hinst == NULL)
        {
            rt_kprintf("the test instance is NULL, please create first.\n");
            return;
        }
        if (rs485_connect(test_hinst) == RT_EOK)
        {
            rt_kprintf("rs485 instance connect success.\n");
        }
        else
        {
            rt_kprintf("rs485 instance connect fail.\n");
        }
        return;
    }


    if (strcmp(argv[1], "disconn") == 0)
    {
        rs485_disconn(test_hinst);
        return;
    }

    /* ============================================================= */
    /* 7. 子命令：recv —— 接收数据                                                                        */
    /* ============================================================= */
    if (strcmp(argv[1], "recv") == 0)
    {
        int size = RS485_TEST_BUF_SIZE;
        int len = 0;

        if (test_hinst == NULL)
        {
            rt_kprintf("the test instance is NULL, please create first.\n");
            return;
        }
        if (argc >= 3)
        {
            size = atoi(argv[2]);
            if (size > RS485_TEST_BUF_SIZE)
            {
                size = RS485_TEST_BUF_SIZE;
            }
        }
        rt_kprintf("rs485 start receiving, max length : %d .\n", size);
        len = rs485_recv(test_hinst, test_buf, size);
        if (len == 0)
        {
            rt_kprintf("rs485 receive timeout.\n");
            return;
        }
        rt_kprintf("rs485 received %d datas (hex) : ", len);
        for (int i=0; i<len; i++)
        {
            rt_kprintf("%02X ", test_buf[i]);
        }
        rt_kprintf("\n");
        return;
    }


    /* ============================================================= */
    /* 8. 子命令：send —— 发送数据                                                                        */
    /* ============================================================= */
    if (strcmp(argv[1], "send") == 0)
    {
        int size = RS485_TEST_BUF_SIZE;

        if (test_hinst == NULL)
        {
            rt_kprintf("the test instance is NULL, please create first.\n");
            return;
        }
        if (argc >= 3)
        {
            size = atoi(argv[2]);
            if (size > RS485_TEST_BUF_SIZE)
            {
                size = RS485_TEST_BUF_SIZE;
            }
        }
        for (int i=0; i<size; i++)
        {
            test_buf[i] = i;
        }
        size = rs485_send(test_hinst, test_buf, size);
        rt_kprintf("rs485 transmit completed. length : %d .\n", size);
        return;
    }


    /* ============================================================= */
    /* 9. 子命令：cfg —— 配置串口参数                                                                   */
    /* ============================================================= */
    if (strcmp(argv[1], "cfg") == 0)
    {
        int baudrate = RS485_TEST_BAUDRATE;
        int databits = 8;
        int parity = RS485_TEST_PARITY;
        int stopbits = 1;

        if (test_hinst == NULL)
        {
            rt_kprintf("the test instance is NULL, please create first.\n");
            return;
        }
        if (argc >= 3)
        {
            baudrate = atoi(argv[2]);
        }
        if ( argc >= 4)
        {
            databits = atoi(argv[3]);
        }
        if (argc >= 5)
        {
            parity = atoi(argv[4]);
        }
        if (argc >= 6)
        {
            stopbits = atoi(argv[5]);
        }
        rs485_config(test_hinst, baudrate, databits, parity, stopbits);
        return;
    }


    /* ============================================================= */
    /* 10. 子命令：send_then_recv —— 一键发收（主站典型操作）                    */
    /* ============================================================= */
    if (strcmp(argv[1], "send_then_recv") == 0)
    {
        int send_size = RS485_TEST_BUF_SIZE;
        int recv_size = RS485_TEST_BUF_SIZE;
        int len = 0;

        if (test_hinst == NULL)
        {
            rt_kprintf("the test instance is NULL, please create first.\n");
            return;
        }
        if (argc >= 3)
        {
            send_size = atoi(argv[2]);
        }
        if (argc >= 4)
        {
            recv_size = atoi(argv[3]);
        }
        for (int i=0; i<send_size; i++)
        {
            test_buf[i] = i;
        }
        rt_kprintf("rs485 send %d datas, then receive max length : %d .\n", send_size, recv_size);
        len = rs485_send_then_recv(test_hinst, test_buf, send_size, test_buf, recv_size);
        if (len == 0)
        {
            rt_kprintf("rs485 receive timeout.\n");
            return;
        }
        rt_kprintf("rs485 received %d datas (hex) : ", len);
        for (int i=0; i<len; i++)
        {
            rt_kprintf("%02X ", test_buf[i]);
        }
        rt_kprintf("\n");
        return;
    }


    /* ============================================================= */
    /* 11. 未知命令                                                                                                     */
    /* ============================================================= */
    rt_kprintf("error ! unsupported command .\n");
}
MSH_CMD_EXPORT_ALIAS(rs485_test, rs485, test rs485 module functions);




#endif


