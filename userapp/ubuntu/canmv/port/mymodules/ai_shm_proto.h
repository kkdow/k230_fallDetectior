// ai_shm_proto.h
#ifndef AI_SHM_PROTO_H
#define AI_SHM_PROTO_H

#include <stdint.h>
#include <pthread.h>

/* --- 基础配置 --- */
#define IMG_SHM_KEY      0x3000      // 图像共享内存 Key (保持不变)
#define AI_RESULT_KEY    0x3005      // 融合姿态分析的专属结果共享内存 Key

#define IMG_BUF_NUM      3           // 双缓冲（Ping-Pong Buffer）
#define IMG_SIZE         (320*320*3) // 输入尺寸 320x320, RGB888 格式
#define MAX_POSE_BOXES   10          // 单帧最大支持的人数

/* --- 图像数据结构 (零拷贝硬件对齐) --- */
typedef struct {
    uint8_t data[IMG_SIZE] __attribute__((aligned(64))); 
    uint32_t frame_id;
    volatile int ready;              // 0: 空闲, 1: 已写待读, 2: 硬件推理中
} ImgFrame;

typedef struct {
    pthread_mutex_t mutex;           // 进程间互斥锁
    uint32_t magic;                  // 校验位 (0xAA1234)
    uint32_t write_idx;              // Python 当前写入的槽位
    ImgFrame frames[IMG_BUF_NUM];    // 缓冲池
} ImgShm;

/* --- 融合目标识别与姿态分析的结果结构 --- */
typedef struct {
    float x1, y1, x2, y2;            // 1. 矩形目标边界框（Boxes）
    float confidence;                // 2. 置信度标签分值（Score）
    uint8_t class_id;                // 2. 目标类别索引（Class，人通常为 0）
    float kps[51];                   // 3 & 4. 17个关键点原始数据：[x0, y0, s0, x1, y1, s1, ...]
} PoseBox;

// 结果共享内存总布局
typedef struct {
    pthread_mutex_t mutex;           // 进程间结果锁
    uint32_t magic;                  // 初始化校验位 (0xEE5678)
    volatile uint32_t frame_id;      // 当前帧 ID
    volatile uint32_t has_new;       // 消费激活信号标志
    uint32_t box_count;              // 画面中实际检出的人数
    PoseBox boxes[MAX_POSE_BOXES];   // 姿态框体列表
} PoseResultShm;

#endif /* AI_SHM_PROTO_H */