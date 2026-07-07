#ifndef __MAIN_H__
#define __MAIN_H__

#include <vector>
#include <queue>
#include <pthread.h>
#include "ai_shm_proto.h"

// 内部任务简报：用于线程间传递
struct InferTask {
    uint8_t* data;      // 图像数据指针
    uint32_t frame_id;  // 帧 ID
    int slot_idx;       // 对应的 SHM 槽位索引
};

// 线程启动函数
void* aicons_start(void* arg);   // 推理核心线程
void* aicap_start(void* arg);    // SHM 采集线程

#endif