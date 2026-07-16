/*
 * ================================================================
 * bw21_l610_ecm.c — BW21 + L610 4G ECM 远程机器人视频推流与控制系统
 * ================================================================
 *
 * 通信方案: USB CDC ECM (Ethernet Control Model)
 *   BW21 (USB Host)  <--ECM-->  L610 Cat.1 4G模组  <--4G-->  云服务器
 *
 * 功能模块:
 *   [视频流] GC2053摄像头 → H.264硬编码 → UDP/Ethernet组包 → ECM发送 → 服务器:9091
 *   [传感器] STM32 UART → 解析T/H/A/P格式 → JSON封装 → ECM发送 → 服务器:9093
 *   [GPS]    GPS STM32 UART → 解析Lat/Lon/Sat格式 → 合并入传感器JSON → 服务器:9093
 *   [控制]   服务器 UDP:9093回传 → JSON解析 → 单字符标志位(F/B/L/R/S) → UART → STM32
 *
 * 安全机制 — 自动停止位:
 *   若 500ms 内未收到移动指令(F/B/L/R)，自动发送停止标志'S'，
 *   防止网络中断/前端异常时机器人失控。此超时停止即为"停止位"的实现。
 *
 * FreeRTOS 任务架构（4任务）:
 *   sender_thread (prio +5)  — 从队列取H.264帧 → ECM发送
 *   sensor_task   (prio +2)  — UART传感器接收 + ECM控制收发 + 自动停止
 *   stats_thread  (prio +1)  — 每秒性能统计
 *   video_cb      (ISR回调)  — 编码器输出 → 非阻塞入队
 *
 * 硬件接线:
 *   BW21 USB Host ←→ L610 USB Device (ECM模式)
 *   BW21 IOA2(TX) → STM32 UART RX  (控制标志位)
 *   BW21 IOA3(RX) ← STM32 UART TX  (传感器数据 + GPS数据, 共线)
 *   GND ↔ GND (共地)
 *   L610 12V DC ≥2A 独立供电
 *
 * 传感器/GPS 数据格式 (两种格式共线, 固件自动识别):
 *   传感器: T:25.3,H:60.1,A:120.5,P:1013.2\n
 *   GPS(新): G:39.904200,116.407400,12\n
 *   GPS(旧): Time: 12:30:45  Lat: N 39.904200  Lon: E 116.407400  Sat: 12\n
 *
 * 参考规范:
 *   USB CDC ECM 1.2, RFC 791(IP), RFC 768(UDP), IEEE 802.3(Ethernet)
 * ================================================================
 */

#include <Arduino.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <string.h>

/* ---- USB ECM 驱动 (BW21 SDK) ---- */
#include "ameba_usb.h"
#include "usbh.h"
#include "usbh_cdc_ecm.h"
#include "usbh_cdc_ecm_hal.h"

/* ================================================================
 * 视频编码 API (BW21 SDK 硬件编码器, C 链接)
 * ================================================================ */
extern int  video_voe_presetting(int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int);
extern int  voe_get_sensor_info(int id, int *iq_data, int *sensor_data);
extern void *video_init(int iq_start_addr, int sensor_start_addr);
extern int  video_open(void *v_stream, void *output_cb, void *ctx);
extern int  video_ctrl(int ch, int cmd, int arg);
extern int  video_encbuf_release(int ch, int codec, int mode);

/* 硬件缓存刷新桩 */
typedef struct { void (*dcache_invalidate_by_addr)(uint32_t *, unsigned int); } hal_cache_stubs_t;
extern const hal_cache_stubs_t hal_cache_stubs;

/* 编码器输出结构体 (32字节对齐) */
typedef struct {
    int width, height, codec, ch;
    unsigned int enc_len, jpg_len;
    int *enc_addr, *jpg_addr, *isp_addr;
    int qp, finish, type;
    int enc_used, jpg_used, enc_slot, jpg_slot;
    volatile int roi_time, enc_time, jpg_time;
    int cmd, cmd_status;
    unsigned int time_stamp;
    unsigned int enc_meta_offset, enc_meta_size;
    unsigned int jpg_meta_offset, jpg_meta_size;
    int exposure_h, gain_h, exposure_l, gain_l;
    int wb_r_gain, wb_b_gain, wb_g_gain, colot_temperature;
    int y_average; unsigned int white_num, rg_sum, bg_sum;
    int hdr_mode, sensor_fps, max_fps, frame_count;
    unsigned int wdr_hist_contrast, wdr_hist_contrast_origin, reserved;
    int max_width, max_height, out_width, out_height;
    int crop_x, crop_y, crop_width, crop_height;
    unsigned short skip_m, skip_n;
} __attribute__((aligned(32))) enc2out_t;

/* 视频参数结构体 */
typedef struct {
    unsigned int stream_id, type, resolution, width, height;
    unsigned int bps, fps, gop, rc_mode;
    unsigned int jpeg_qlevel, rotation;
    unsigned int out_buf_size, out_rsvd_size;
    unsigned int direct_output, use_static_addr, fcs, use_roi;
    unsigned int roi_padding[16];
    unsigned int level, profile, cavlc;
    unsigned int sps_pps_padding[16];
    unsigned int out_mode, ext_fmt, minQp, maxQp;
    unsigned int fast_osd_en, vui_disable, meta_enable;
    unsigned int jpeg_crop_padding[8];
    unsigned int dyn_scale_up_en;
} video_params_t;

#define CODEC_H264        0x02
#define VIDEO_H264        1
#define VIDEO_FORCE_IFRAME 0x11

/* ================================================================
 * 系统配置
 * ================================================================ */
/* 部署地址不得提交到仓库；以下均为 RFC 5737 TEST-NET 占位值。 */
#define SERVER_IP_B1  192
#define SERVER_IP_B2  0
#define SERVER_IP_B3  2
#define SERVER_IP_B4  1

/* L610/BW21 地址也必须由私有部署配置替换。 */
#define L610_IP_B1  192
#define L610_IP_B2  0
#define L610_IP_B3  2
#define L610_IP_B4  2

/* 端口 */
#define VIDEO_PORT   9091
#define SENSOR_PORT  9093
#define LOCAL_PORT   12345

/* 视频参数 */
#define VID_W     640
#define VID_H     480
#define VID_FPS   30
#define VID_GOP   1
#define VID_BPS   (8 * 1024 * 1024)   /* 8Mbps VBR */

/* FreeRTOS */
#define QDEPTH    15
#define SSTACK    8192
#define SPRIO     (tskIDLE_PRIORITY + 5)
#define SIVAL     5000UL

/* 传感器 UART */
#define SENSOR_BAUD  9600
#define SENSOR_BUF   128

/* ECM MTU限制: 1514 - Eth(14) - IP(20) - UDP(8) = 1472B 负载上限
 * 保守取 1400B 留余量 */
#define MAX_PAYLOAD  1400

/* 自动停止超时 (ms) — 即"停止位"的时间阈值 */
#define AUTO_STOP_TIMEOUT_MS  500

/* ================================================================
 * ECM 全局状态 (volatile = 中断安全访问)
 * ================================================================ */
static volatile int g_ecm_attach     = 0;   /* L610 USB设备已插入 */
static volatile int g_ecm_setup_done = 0;   /* ECM配置完成,可发送 */
static volatile int g_ecm_conn_ok    = 0;   /* 4G网络连接就绪 */
static volatile int g_ecm_eth_ok     = 0;   /* 以太网层正常 */
static volatile int g_ecm_rx_count   = 0;   /* 接收帧计数 */
static volatile int g_ecm_tx_count   = 0;   /* 发送帧计数 */

/* ECM 接收缓冲区 (控制指令) */
static volatile char  g_ecm_rx_buf[512];
static volatile int   g_ecm_rx_len = 0;

/* ================================================================
 * 视频流水线全局变量
 * ================================================================ */
typedef struct {
    unsigned int *da;       /* 数据地址 */
    unsigned int  sz;       /* 数据长度 */
    unsigned int  ch;       /* 通道号 */
    unsigned int  co;       /* 编解码器类型 */
} h264_frame_t;

static QueueHandle_t       g_frame_queue = NULL;
static SemaphoreHandle_t   g_send_mutex  = NULL;
static volatile unsigned int g_sent  = 0;  /* 发送成功计数 */
static volatile unsigned int g_drop  = 0;  /* 丢帧计数 */
static volatile unsigned int g_total = 0;  /* 编码器输出总数 */

static uint8_t         g_send_buf[256 * 1024 + 4];
static const uint8_t   FRAME_MARKER[4] = {0x64, 0x00, 0x00, 0x00};

/* ================================================================
 * 自动停止状态 (安全机制核心)
 * ================================================================ */
static uint32_t g_last_move_ms = 0;       /* 上次收到移动指令的时间戳 */
static char     g_last_move_flag = 'S';   /* 当前运动状态 (S=停止) */
static char     g_pending_stop = 0;       /* 待发送停止标志 */

/* ================================================================
 * IP/UDP/Ethernet 协议栈 (手动构造)
 * ================================================================ */

/*
 * IP 头部校验和 (RFC 791, 16位补码)
 * 对 nwords 个 16 位字求和, 取反
 */
static uint16_t ip_checksum(const uint16_t *buf, int nwords)
{
    uint32_t sum = 0;
    int i;
    for (i = 0; i < nwords; i++) {
        sum += buf[i];
    }
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum = (sum >> 16) + (sum & 0xFFFF);
    return (uint16_t)(~sum);
}

/*
 * 构造完整 Ethernet/IPv4/UDP 帧
 *
 * 帧结构 (共 frame_len 字节):
 *   [ Ethernet: 14B ][ IPv4: 20B ][ UDP: 8B ][ Payload ]
 *
 * 参数:
 *   buf        - 输出缓冲区 (需≥1536字节)
 *   out_len    - [出] 实际帧长
 *   src_ip[4]  - 源 IP (BW21/L610侧)
 *   dst_ip[4]  - 目标 IP (云服务器)
 *   src_port   - 源端口
 *   dst_port   - 目标端口
 *   payload    - 应用层数据
 *   payload_len- 应用层数据长度
 *
 * 返回: 0=成功, -1=负载过大(超MTU)
 */
static int build_udp_frame(
    uint8_t *buf, uint16_t *out_len,
    const uint8_t src_ip[4], const uint8_t dst_ip[4],
    uint16_t src_port, uint16_t dst_port,
    const uint8_t *payload, uint16_t payload_len)
{
    uint16_t udp_len   = 8 + payload_len;
    uint16_t ip_total  = 20 + udp_len;
    uint16_t frame_len = 14 + ip_total;
    uint8_t *ip, *udp;

    if (frame_len > 1514) return -1;  /* 超过 Ethernet MTU */

    /* --- Ethernet 头部 14B --- */
    memset(buf, 0xFF, 6);           /* dst MAC = 广播 */
    memset(buf + 6, 0x00, 6);       /* src MAC = 00:00:00:00:00:00 */
    buf[6] = 0x02;                  /* 本地管理位 */
    buf[12] = 0x08; buf[13] = 0x00; /* EtherType = IPv4 (0x0800, 大端) */

    /* --- IPv4 头部 20B --- */
    ip = buf + 14;
    ip[0] = 0x45;                   /* Ver=4, IHL=5(20B) */
    ip[1] = 0x00;                   /* DSCP=0, ECN=0 */
    ip[2] = (uint8_t)(ip_total >> 8);   /* Total Length 高字节 */
    ip[3] = (uint8_t)(ip_total);        /* Total Length 低字节 */
    ip[4] = 0x00; ip[5] = 0x01;     /* Identification */
    ip[6] = 0x40; ip[7] = 0x00;     /* Flags=DF, Fragment Offset=0 */
    ip[8] = 0x80;                   /* TTL = 128 */
    ip[9] = 0x11;                   /* Protocol = UDP */
    ip[10] = 0x00; ip[11] = 0x00;   /* Header Checksum (先填0) */
    memcpy(ip + 12, src_ip, 4);     /* Source IP */
    memcpy(ip + 16, dst_ip, 4);     /* Destination IP */

    /* IP头部校验和 */
    {
        uint16_t cs = ip_checksum((uint16_t *)ip, 10);
        ip[10] = (uint8_t)(cs >> 8);
        ip[11] = (uint8_t)(cs);
    }

    /* --- UDP 头部 8B --- */
    udp = buf + 34;  /* 14+20 */
    udp[0] = (uint8_t)(src_port >> 8);   /* Src Port 高字节 */
    udp[1] = (uint8_t)(src_port);        /* Src Port 低字节 */
    udp[2] = (uint8_t)(dst_port >> 8);   /* Dst Port 高字节 */
    udp[3] = (uint8_t)(dst_port);        /* Dst Port 低字节 */
    udp[4] = (uint8_t)(udp_len >> 8);    /* Length 高字节 */
    udp[5] = (uint8_t)(udp_len);         /* Length 低字节 */
    udp[6] = 0x00; udp[7] = 0x00;        /* Checksum = 0 (IPv4 UDP可不校验) */

    /* --- Payload --- */
    memcpy(buf + 42, payload, payload_len);  /* 14+20+8 */

    *out_len = frame_len;
    return 0;
}

/* ================================================================
 * ECM 回调函数 (USB Host 事件)
 * ================================================================ */

/* USB 设备插入 */
static void ecm_on_attach(void)
{
    g_ecm_attach = 1;
    g_ecm_setup_done = 0;
    printf("[ECM] L610 4G模组已插入\r\n");
}

/* USB 设备拔出 */
static void ecm_on_detach(void)
{
    g_ecm_setup_done = 0;
    g_ecm_attach = 0;
    g_ecm_conn_ok = 0;
    printf("[ECM] L610 4G模组已拔出\r\n");
}

/* 4G 网络连接就绪 */
static void ecm_on_connect(void)
{
    g_ecm_conn_ok = 1;
    printf("[ECM] *** 4G网络连接就绪 ***\r\n");
}

/* 4G 网络断开 */
static void ecm_on_disconnect(void)
{
    g_ecm_conn_ok = 0;
    printf("[ECM] 4G网络断开\r\n");
}

/*
 * ECM 数据接收回调 (来自服务器的控制指令)
 *
 * 接收完整 Ethernet 帧 → 解析 IP/UDP 头 → 提取 JSON 控制指令 →
 * 存入全局缓冲区供 sensor_task 处理
 *
 * 在USB中断上下文中运行，仅做最小处理
 */
static void ecm_on_rx_data(uint8_t *buf, uint32_t len)
{
    uint8_t *ip, *udp;
    uint16_t dst_port, udp_len, payload_len;
    uint8_t *payload;

    g_ecm_rx_count++;

    /* 最小帧长: Eth(14) + IP(20) + UDP(8) = 42 */
    if (len < 42) return;

    /* 检查 EtherType == IPv4 */
    if (buf[12] != 0x08 || buf[13] != 0x00) return;

    ip = buf + 14;

    /* 检查 IP Protocol == UDP */
    if (ip[9] != 0x11) return;

    /* 检查 目标端口 == SENSOR_PORT */
    udp = buf + 34;
    dst_port = ((uint16_t)udp[2] << 8) | udp[3];
    if (dst_port != SENSOR_PORT) return;

    /* 提取负载 */
    udp_len     = ((uint16_t)udp[4] << 8) | udp[5];
    payload_len = udp_len - 8;
    payload     = buf + 42;

    /* 存入全局缓冲 (临界区保护) */
    if (payload_len < sizeof(g_ecm_rx_buf) - 1) {
        taskDISABLE_INTERRUPTS();
        memcpy((void *)g_ecm_rx_buf, payload, payload_len);
        ((char *)g_ecm_rx_buf)[payload_len] = '\0';
        g_ecm_rx_len = (int)payload_len;
        taskENABLE_INTERRUPTS();
    }

    if (g_ecm_rx_count <= 3) {
        printf("[ECM-RX] #%d len=%lu payload=%d\r\n",
               g_ecm_rx_count, (unsigned long)len, payload_len);
    }
}

/* ECM 回调注册表 */
static usbh_cdc_ecm_user_cb_t g_ecm_cb = {
    ecm_on_rx_data,    /* .on_rx_data      数据接收回调 */
    ecm_on_attach,     /* .on_attach       设备插入回调 */
    ecm_on_detach,     /* .on_detach       设备拔出回调 */
    3,                 /* .intf_num        接口编号 */
    ecm_on_connect,    /* .on_connect      网络连接回调 */
    ecm_on_disconnect  /* .on_disconnect   网络断开回调 */
};

/* ================================================================
 * ECM 初始化
 * ================================================================ */

/*
 * 启动 USB Host CDC ECM 驱动
 *
 * 关键: BW21 SDK 要求 _usb_init() 在 usbh_init() 之前调用,
 * 且 dma_enable=0，否则 USB Host 无法正常枚举 ECM 设备。
 */
static int ecm_init(void)
{
    printf("\r\n===== BW21 + L610 ECM 4G 初始化 =====\r\n\r\n");

    if (!usbh_cdc_ecm_on(&g_ecm_cb)) {
        printf("[ECM] 驱动启动失败\r\n");
        return -1;
    }

    printf("[ECM] 驱动已启动，等待L610插入...\r\n");
    return 0;
}

/*
 * ECM 接口配置 (设备插入后6秒执行)
 *
 * ECM 需要3个控制请求来激活网络:
 *   1. SET_INTERFACE (数据接口, alt=1)       — 激活数据接口
 *   2. SET_ETHERNET_PACKET_FILTER (0x000C)   — Directed + Broadcast
 *   3. SET_ETHERNET_START_TRANSFER           — 启动Bulk传输
 */
static int ecm_do_setup(void)
{
    uint8_t r1, r2, r3;

    printf("[ECM] 配置ECM接口...\r\n");

    vTaskDelay(pdMS_TO_TICKS(200));
    r1 = usbh_cdc_ecm_alt_setting();
    printf("[ECM]   alt_setting = %u\r\n", r1);

    vTaskDelay(pdMS_TO_TICKS(200));
    r2 = usbh_cdc_ecm_set_ethernet_packetfilter(0x000C);
    printf("[ECM]   packet_filter(0x0C) = %u\r\n", r2);

    vTaskDelay(pdMS_TO_TICKS(200));
    r3 = usbh_cdc_ecm_set_ethernet_start_transfer();
    printf("[ECM]   start_transfer = %u\r\n", r3);

    return (r1 && r2 && r3) ? 0 : -1;
}

/* ECM 状态轮询 (每秒) */
static void ecm_poll(void)
{
    g_ecm_eth_ok = usbh_cdc_ecm_ethernt_status() ? 1 : 0;
}

/* ================================================================
 * H.264 编码器回调 (ISR 上下文)
 * ================================================================ */

/*
 * 编码器输出一帧 → 非阻塞入队 FreeRTOS队列
 * 队列满则丢弃 (ec++), 不阻塞 ISR
 */
static void video_callback(void *p1, void *p2, unsigned int arg)
{
    enc2out_t *enc = (enc2out_t *)p1;
    h264_frame_t frame;
    uint8_t *dp;

    g_total++;

    /* 仅处理 H.264 帧 */
    if (!(enc->codec & CODEC_H264) || !enc->enc_addr || enc->enc_len == 0) {
        video_encbuf_release(enc->ch, enc->codec, enc->enc_len);
        return;
    }

    /* DMA Cache 一致性刷新 */
    hal_cache_stubs.dcache_invalidate_by_addr(
        (uint32_t *)enc->enc_addr, enc->enc_len);

    /* 检查 H.264 Annex B 起始码 (0x00 0x00 ...) */
    dp = (uint8_t *)enc->enc_addr;
    if (dp[0] != 0x00 || dp[1] != 0x00) {
        video_encbuf_release(enc->ch, enc->codec, enc->enc_len);
        video_ctrl(0, VIDEO_FORCE_IFRAME, 1);  /* 强制I帧重同步 */
        return;
    }

    /* 构造帧描述符, 非阻塞入队 */
    frame.da = (unsigned int *)enc->enc_addr;
    frame.sz = enc->enc_len;
    frame.ch = (unsigned int)enc->ch;
    frame.co = CODEC_H264;

    if (xQueueSend(g_frame_queue, &frame, 0) != pdTRUE) {
        video_encbuf_release(enc->ch, enc->codec, enc->enc_len);
        g_drop++;
    }
}

/* ================================================================
 * sender_thread — H.264帧 ECM UDP 发送任务 (优先级 +5)
 * ================================================================ */

/*
 * 从队列取帧 → 附加帧标记 → 构造UDP/Ethernet帧 → ECM Bulk发送
 *
 * MTU处理: H.264帧若超过1400B，截断发送 (生产环境应做IP分片)
 */
static void sender_thread(void *arg)
{
    h264_frame_t frame;
    int seq = 0;
    uint32_t total;
    uint16_t send_len;
    uint8_t pkt[1536];
    uint16_t pkt_len;

    /* 服务器IP + 本机IP */
    const uint8_t dst_ip[4] = {SERVER_IP_B1, SERVER_IP_B2, SERVER_IP_B3, SERVER_IP_B4};
    const uint8_t src_ip[4] = {L610_IP_B1, L610_IP_B2, L610_IP_B3, L610_IP_B4};

    (void)arg;

    /* 等待 ECM 就绪 */
    printf("[SEND] 等待ECM就绪...\r\n");
    while (!g_ecm_setup_done) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    printf("[SEND] ECM就绪, src=%d.%d.%d.%d:%d -> dst=%d.%d.%d.%d:%d\r\n",
           src_ip[0], src_ip[1], src_ip[2], src_ip[3], LOCAL_PORT,
           dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], VIDEO_PORT);

    for (;;) {
        /* 阻塞等待新帧 (1s超时) */
        if (xQueueReceive(g_frame_queue, &frame, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        xSemaphoreTake(g_send_mutex, portMAX_DELAY);

        if (frame.sz > 0 && frame.sz <= (256U * 1024U)) {
            /* 组装: H.264 Annex B + 4字节帧标记 */
            total = frame.sz + 4;
            memcpy(g_send_buf, frame.da, frame.sz);
            memcpy(g_send_buf + frame.sz, FRAME_MARKER, 4);

            /* MTU限制截断 */
            send_len = (total > MAX_PAYLOAD) ? (uint16_t)MAX_PAYLOAD : (uint16_t)total;

            if (build_udp_frame(pkt, &pkt_len,
                                src_ip, dst_ip,
                                LOCAL_PORT, VIDEO_PORT,
                                g_send_buf, send_len) == 0) {
                uint8_t rs = usbh_cdc_ecm_bulk_send(pkt, pkt_len);
                if (rs == 0) {
                    g_sent++;
                } else {
                    g_drop++;
                    if (seq < 5) {
                        printf("[SEND] err#%d sz=%u ret=%u\r\n", seq, frame.sz, rs);
                    }
                }
                g_ecm_tx_count++;
            } else {
                g_drop++;
            }
        }

        video_encbuf_release(frame.ch, frame.co, frame.sz);
        xSemaphoreGive(g_send_mutex);
        seq++;
    }
}

/* ================================================================
 * stats_thread — 统计输出任务 (优先级 +1)
 * ================================================================ */
static void stats_thread(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SIVAL));
        printf("[STAT] %.1ffps sent=%lu drop=%lu total=%lu | "
               "ECM eth=%d attach=%d conn=%d tx=%d rx=%d\r\n",
               g_sent / (SIVAL / 1000.0f),
               (unsigned long)g_sent, (unsigned long)g_drop,
               (unsigned long)g_total,
               g_ecm_eth_ok, g_ecm_attach, g_ecm_conn_ok,
               g_ecm_tx_count, g_ecm_rx_count);
        g_sent = g_drop = g_total = 0;
    }
}

/* ================================================================
 * sensor_task — 传感器 + 控制指令 + 自动停止 (优先级 +2)
 * ================================================================ */

/*
 * 200ms 轮询周期, 三件事:
 *
 *   [1] 读 Serial1 → 解析 T/H/A/P 传感器数据 → JSON → ECM UDP → 服务器 :9093
 *   [2] 检查 ECM RX 缓冲 → 有控制指令 → 解析 JSON → 发标志位 → STM32
 *   [3] ★ 自动停止位 ★ — 超过 AUTO_STOP_TIMEOUT_MS(500ms) 未收到移动指令 →
 *      自动发送 'S' 停止标志, 防止失控
 */
static void sensor_task(void *arg)
{
    char buf[SENSOR_BUF];
    int  idx = 0;
    float temp = 0.0f, hum = 0.0f, alt = 0.0f, pres = 0.0f;
    uint32_t now;

    /* GPS 数据 (来自 GPS STM32 → Serial1 RX) */
    static double gps_lat  = 0.0;
    static double gps_lng  = 0.0;
    static int    gps_sat  = 0;
    static char   gps_valid = 0;

    const uint8_t dst_ip[4] = {SERVER_IP_B1, SERVER_IP_B2, SERVER_IP_B3, SERVER_IP_B4};
    const uint8_t src_ip[4] = {L610_IP_B1, L610_IP_B2, L610_IP_B3, L610_IP_B4};

    (void)arg;

    printf("[SENSOR] 任务启动, UART=%d baud\r\n", SENSOR_BAUD);

    for (;;) {
        now = millis();
        int new_data = 0;  /* 标记本轮是否有新数据需发送 */

        /* ========================================================
         * [1] 接收传感器 + GPS 数据 (Serial1 RX, 来自 STM32)
         *
         * 传感器格式: T:25.3,H:60.1,A:120.5,P:1013.2\n
         * GPS新格式:  G:39.904200,116.407400,12\n
         * GPS旧格式:  Time: HH:MM:SS  Lat: N dd.dddddd  Lon: E ddd.dddddd  Sat: N\n
         *
         * 两种格式共线, 固件自动识别。解析后封装 JSON → ECM UDP → 服务器 :9093
         * ======================================================== */
        while (Serial1.available()) {
            char c = (char)Serial1.read();

            if (c == '\n' || c == '\r') {
                if (idx > 0) {
                    buf[idx] = '\0';

                    /* ---- GPS 数据行 ---- */
                    if (*buf == 'G' && *(buf+1) == ':') {
                        /* 新格式: G:lat,lng,satellites (与传感器风格统一) */
                        char *p = buf + 2;
                        gps_lat = atof(p);      /* 纬度 */
                        while (*p && *p != ',') p++; if (*p == ',') p++;
                        gps_lng = atof(p);      /* 经度 */
                        while (*p && *p != ',') p++; if (*p == ',') p++;
                        gps_sat = atoi(p);      /* 卫星数 */
                        gps_valid = 1;
                        printf("[GPS] lat=%.6f lng=%.6f sat=%d\r\n", gps_lat, gps_lng, gps_sat);
                        new_data = 1;
                    }
                    else if (strstr(buf, "Lat:") && strstr(buf, "Lon:")) {
                        /* 旧格式兼容: Time: ... Lat: N dd.dddddd Lon: E ddd.dddddd Sat: N */
                        char ns = 'N', ew = 'E';
                        double lat = 0.0, lon = 0.0;
                        int sat = 0;
                        char *pLat = strstr(buf, "Lat:");
                        char *pLon = strstr(buf, "Lon:");
                        char *pSat = strstr(buf, "Sat:");
                        if (pLat && pLon) {
                            sscanf(pLat, "Lat: %c %lf", &ns, &lat);
                            sscanf(pLon, "Lon: %c %lf", &ew, &lon);
                            if (pSat) sscanf(pSat, "Sat: %d", &sat);
                            if (ns == 'S') lat = -lat;
                            if (ew == 'W') lon = -lon;
                            gps_lat = lat; gps_lng = lon; gps_sat = sat;
                            gps_valid = 1;
                            printf("[GPS] %.6f%c %.6f%c sat=%d\r\n", lat, ns, lon, ew, sat);
                            new_data = 1;
                        }
                    }

                    /* ---- 传感器数据行 (主控 STM32 格式) ---- */
                    else if (strchr(buf, ':') && (strchr(buf, 'T') || strchr(buf, 'H') ||
                                                   strchr(buf, 'A') || strchr(buf, 'P'))) {
                        char *p;
                        for (p = buf; *p; ) {
                            if (*p == 'T' && *(p+1) == ':') { temp = (float)atof(p+2); }
                            if (*p == 'H' && *(p+1) == ':') { hum  = (float)atof(p+2); }
                            if (*p == 'A' && *(p+1) == ':') { alt  = (float)atof(p+2); }
                            if (*p == 'P' && *(p+1) == ':') { pres = (float)atof(p+2); }
                            while (*p && *p != ',') p++;
                            if (*p == ',') p++;
                        }
                        printf("[SENSOR] %.1fC %.1f%% %.1fm %.1fhPa\r\n",
                               temp, hum, alt, pres);
                        new_data = 1;
                    }

                    /* ---- JSON → ECM UDP 发送 ---- */
                    if (new_data && g_ecm_setup_done) {
                        char json[320];
                        int json_len;

                        if (gps_valid) {
                            json_len = snprintf(json, sizeof(json),
                                "{\"temperature\":%.1f,\"humidity\":%.1f,"
                                "\"altitude\":%.1f,\"pressure\":%.1f,"
                                "\"lat\":%.6f,\"lng\":%.6f,\"satellites\":%d}",
                                temp, hum, alt, pres,
                                gps_lat, gps_lng, gps_sat);
                        } else {
                            json_len = snprintf(json, sizeof(json),
                                "{\"temperature\":%.1f,\"humidity\":%.1f,"
                                "\"altitude\":%.1f,\"pressure\":%.1f}",
                                temp, hum, alt, pres);
                        }

                        if (json_len > 0 && json_len < 320) {
                            uint8_t pkt[1536];
                            uint16_t pkt_len = 0;
                            if (build_udp_frame(pkt, &pkt_len,
                                                src_ip, dst_ip,
                                                LOCAL_PORT + 1, SENSOR_PORT,
                                                (uint8_t *)json,
                                                (uint16_t)json_len) == 0) {
                                usbh_cdc_ecm_bulk_send(pkt, pkt_len);
                            }
                        }

                        printf("[SEND] %s\r\n", json);
                    }

                    idx = 0;
                }
            } else if (idx < SENSOR_BUF - 1) {
                buf[idx++] = c;
            }
        }

        /* ========================================================
         * [2] 处理控制指令 (ECM RX 缓冲 → JSON解析 → STM32)
         *
         * JSON指令格式: {"cmd":"move","direction":"forward","speed":100}
         * {"cmd":"stop"}
         *
         * 转换表 (单字符标志位 → STM32):
         *   forward  → 'F'
         *   backward → 'B'
         *   left     → 'L'
         *   right    → 'R'
         *   stop     → 'S'
         * ======================================================== */
        if (g_ecm_rx_len > 0) {
            char rbuf[256];
            char flag = 0;
            int n;

            /* 临界区拷贝 */
            taskDISABLE_INTERRUPTS();
            n = g_ecm_rx_len;
            if (n > 255) n = 255;
            memcpy(rbuf, (const void *)g_ecm_rx_buf, (size_t)n);
            rbuf[n] = '\0';
            g_ecm_rx_len = 0;
            taskENABLE_INTERRUPTS();

            printf("[CTRL] RX: %s\r\n", rbuf);

            /* 字符串匹配 JSON 指令 */
            if      (strstr(rbuf, "\"cmd\":\"move\"") && strstr(rbuf, "\"forward\""))  flag = 'F';
            else if (strstr(rbuf, "\"cmd\":\"move\"") && strstr(rbuf, "\"backward\"")) flag = 'B';
            else if (strstr(rbuf, "\"cmd\":\"move\"") && strstr(rbuf, "\"left\""))     flag = 'L';
            else if (strstr(rbuf, "\"cmd\":\"move\"") && strstr(rbuf, "\"right\""))    flag = 'R';
            else if (strstr(rbuf, "\"cmd\":\"stop\""))                                  flag = 'S';

            if (flag) {
                Serial1.print(flag);
                Serial1.print('\n');
                printf("[CTRL] ->STM32: %c\r\n", flag);

                /* 更新自动停止计时器 */
                if (flag == 'F' || flag == 'B' || flag == 'L' || flag == 'R') {
                    g_last_move_ms  = now;
                    g_last_move_flag = flag;
                    g_pending_stop   = 0;  /* 清除待停止标记 */
                } else if (flag == 'S') {
                    g_last_move_flag = 'S';
                    g_pending_stop   = 0;
                }
            }
        }

        /* ========================================================
         * [3] ★ 自动停止位 (Stop Bit) ★
         *
         * 安全机制: 如果当前处于移动状态 (F/B/L/R) 且超过
         * AUTO_STOP_TIMEOUT_MS(500ms) 未收到任何移动指令,
         * 自动发送 'S' 停止标志。
         *
         * 触发场景:
         *  - 前端页面关闭/崩溃 (不再发指令)
         *  - 4G网络中断 (指令无法送达)
         *  - 用户松开按钮后前端未发stop
         *
         * 此机制即为"停止位"的核心实现:
         *   不是额外的数据位, 而是时间维度的安全超时。
         * ======================================================== */
        if (g_last_move_flag == 'F' || g_last_move_flag == 'B' ||
            g_last_move_flag == 'L' || g_last_move_flag == 'R') {

            if (now - g_last_move_ms > AUTO_STOP_TIMEOUT_MS) {
                /* 超时 → 自动停止 (仅发一次) */
                if (!g_pending_stop) {
                    Serial1.print('S');
                    Serial1.print('\n');
                    g_last_move_flag = 'S';
                    g_pending_stop   = 1;
                    printf("[CTRL] *** AUTO-STOP *** 超时%lums无指令, 自动发送S\r\n",
                           (unsigned long)(now - g_last_move_ms));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ================================================================
 * setup() — 系统初始化入口
 * ================================================================ */
void setup(void)
{
    video_params_t vp;
    int heap, iqa = 0, sna = 0;
    uint32_t start_ms;

    Serial.begin(115200);
    delay(2000);
    printf("\r\n===== BW21 + L610 ECM 4G 推流 v2.0 (C) =====\r\n\r\n");

    /* ---- 1. ECM 4G 初始化 ---- */
    if (ecm_init() != 0) {
        printf("[ECM] 初始化失败, 系统停止\r\n");
        return;
    }

    /* 等待 L610 插入 (最长30秒) */
    printf("[ECM] 等待L610插入...\r\n");
    start_ms = millis();
    while (!g_ecm_attach) {
        delay(200);
        if (millis() - start_ms > 30000UL) {
            printf("[ECM] 等待超时(30s), 请检查接线和供电\r\n");
            return;
        }
    }
    printf("[ECM] L610已检测 (%lums)\r\n", (unsigned long)(millis() - start_ms));

    /* 等USB枚举+ECM配置 (6秒) */
    printf("[ECM] 等待USB枚举+ECM启动(6s)...\r\n");
    delay(6000);

    if (ecm_do_setup() != 0) {
        printf("[ECM] 配置部分失败, 尝试继续...\r\n");
    }
    g_ecm_setup_done = 1;
    printf("[ECM] 4G网络就绪\r\n\r\n");

    /* ---- 2. 视频编码器初始化 ---- */
    heap = video_voe_presetting(1, VID_W, VID_H, VID_BPS,
                                 0,0,0,0,0,0, 0,0,0,0,0, 0,0,0);
    printf("[VID] heap=%d\r\n", heap);

    voe_get_sensor_info(1, &iqa, &sna);
    printf("[VID] iq=0x%X sensor=0x%X\r\n", iqa, sna);
    video_init(iqa, sna);
    vTaskDelay(1000);

    memset(&vp, 0, sizeof(vp));
    vp.stream_id  = 0;
    vp.type       = VIDEO_H264;
    vp.resolution = 3;
    vp.width      = VID_W;
    vp.height     = VID_H;
    vp.bps        = VID_BPS;
    vp.fps        = VID_FPS;
    vp.gop        = VID_GOP;
    vp.rc_mode    = 2;     /* VBR */
    vp.profile    = 11;    /* H.264 High Profile */
    vp.level      = 40;    /* Level 4.0 */
    vp.cavlc      = 1;     /* CABAC */
    vp.minQp      = 10;
    vp.maxQp      = 40;

    printf("[VID] open=%d\r\n", video_open(&vp, (void*)video_callback, NULL));

    /* ---- 3. FreeRTOS 队列 + 任务 ---- */
    g_frame_queue = xQueueCreate(QDEPTH, sizeof(h264_frame_t));
    g_send_mutex  = xSemaphoreCreateMutex();

    xTaskCreate((TaskFunction_t)sender_thread, "send",  SSTACK, NULL, SPRIO, NULL);
    xTaskCreate((TaskFunction_t)stats_thread,  "stats", 512,    NULL, tskIDLE_PRIORITY + 1, NULL);

    /* ---- 4. 传感器 Serial1 ---- */
    Serial1.begin(SENSOR_BAUD);
    printf("[SENSOR] Serial1 %d baud (IOA2->STM32_RX, IOA3<-STM32_TX)\r\n", SENSOR_BAUD);
    xTaskCreate((TaskFunction_t)sensor_task, "sensor", 1024, NULL, tskIDLE_PRIORITY + 2, NULL);

    /* ---- 5. 初始化自动停止状态 ---- */
    g_last_move_ms  = millis();
    g_last_move_flag = 'S';
    g_pending_stop   = 0;

    printf("\r\n[INIT] 全部就绪 "
           "Q=%d stk=%d prio=%d "
           "auto_stop=%lums\r\n\r\n",
           QDEPTH, SSTACK, SPRIO, (unsigned long)AUTO_STOP_TIMEOUT_MS);
}

/* ================================================================
 * loop() — 主循环 (FreeRTOS 已接管, 此处仅做 ECM 状态轮询)
 * ================================================================ */
void loop(void)
{
    static uint32_t last_poll = 0;
    uint32_t now = millis();

    if (now - last_poll >= 1000UL) {
        last_poll = now;
        ecm_poll();
    }

    vTaskDelay(100);
}
