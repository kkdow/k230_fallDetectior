#ifndef RTSP_PACKET_BUFFER_H
#define RTSP_PACKET_BUFFER_H

#include <stdint.h>
#include <stddef.h>

#define RTSP_MAX_SLOTS 64             // 缓冲区队列深度（可容纳 64 帧，防止网络抖动导致的丢帧）
#define RTSP_MAX_PACKET_SIZE (200 * 1024) // 单个 H.264 帧最大支持 128KB（适合 1080P/720P 码流）

// 视频帧结构体
typedef struct {
    uint8_t *data;         // 帧数据指针
    size_t size;           // 帧大小
    uint64_t pts;          // 时间戳 (Presentation Timestamp)
    int is_keyframe;       // 是否为 I 帧（关键帧）
} rtsp_packet_t;

// 初始化与释放
int rtsp_buffer_init(void);
void rtsp_buffer_deinit(void);

// 写入端（由 MicroPython 获取到 VENC 编码数据后调用，非阻塞/或队列满时抛弃老帧）
int rtsp_buffer_push(const uint8_t *data, size_t size, uint64_t pts, int is_keyframe);

// 消费端（由 C 侧后台 RTSP 推流线程或 MP4 录制线程调用，支持阻塞等待）
int rtsp_buffer_pop_blocking(uint8_t *out_data, size_t *out_size, uint64_t *out_pts, int *out_is_key, int timeout_ms);

// 获取当前缓冲区积压的帧数
int rtsp_buffer_get_count(void);

void rtsp_buffer_wakeup_all(void);

#endif // RTSP_PACKET_BUFFER_H