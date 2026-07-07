#include "rtsp_packet_buffer.h"

#ifndef RTSMART_WEB_PORTABLE
#include <rtthread.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// 兼容标准 POSIX 接口
typedef pthread_mutex_t rt_mutex_t;
typedef sem_t rt_sem_t;
#define rt_malloc malloc
#define rt_free free
#define rt_kprintf printf
static inline void rt_mutex_init(rt_mutex_t *m, const char *n, int f) { pthread_mutex_init(m, NULL); }
static inline void rt_mutex_detach(rt_mutex_t *m) { pthread_mutex_destroy(m); }
static inline int rt_mutex_take(rt_mutex_t *m, int t) { return pthread_mutex_lock(m); }
static inline void rt_mutex_release(rt_mutex_t *m) { pthread_mutex_unlock(m); }
static inline void rt_sem_init(rt_sem_t *s, const char *n, int v, int f) { sem_init(s, 0, v); }
static inline void rt_sem_detach(rt_sem_t *s) { sem_destroy(s); }
static inline void rt_sem_release(rt_sem_t *s) { sem_post(s); }
#endif

typedef struct {
    rtsp_packet_t slots[RTSP_MAX_SLOTS];
    int head;                  // 读指针
    int tail;                  // 写指针
    int count;                 // 当前队列中积压的帧数
    int initialized;
    rt_mutex_t mutex;          // 互斥锁保护队列结构
    rt_sem_t sem;              // 信号量用于阻塞唤醒消费者线程
} rtsp_ring_buffer_t;

static rtsp_ring_buffer_t g_rtsp_pool = {0};

int rtsp_buffer_init(void) {
    if (g_rtsp_pool.initialized) return 0;

    rt_mutex_init(&g_rtsp_pool.mutex, "rtsp_mtx", 0);
    rt_sem_init(&g_rtsp_pool.sem, "rtsp_sem", 0, 0);

    for (int i = 0; i < RTSP_MAX_SLOTS; i++) {
        g_rtsp_pool.slots[i].data = (uint8_t *)rt_malloc(RTSP_MAX_PACKET_SIZE);
        if (!g_rtsp_pool.slots[i].data) {
            rt_kprintf("[RTSP Buffer] 分配内存失败 slot %d\n", i);
            return -1;
        }
        g_rtsp_pool.slots[i].size = 0;
        g_rtsp_pool.slots[i].pts = 0;
        g_rtsp_pool.slots[i].is_keyframe = 0;
    }

    g_rtsp_pool.head = 0;
    g_rtsp_pool.tail = 0;
    g_rtsp_pool.count = 0;
    g_rtsp_pool.initialized = 1;
    rt_kprintf("[RTSP Buffer] H264 队列初始化成功。容量: %d 帧\n", RTSP_MAX_SLOTS);
    return 0;
}

void rtsp_buffer_deinit(void) {
    if (!g_rtsp_pool.initialized) return;

    rt_mutex_take(&g_rtsp_pool.mutex, -1);
    for (int i = 0; i < RTSP_MAX_SLOTS; i++) {
        if (g_rtsp_pool.slots[i].data) {
            rt_free(g_rtsp_pool.slots[i].data);
            g_rtsp_pool.slots[i].data = NULL;
        }
    }
    g_rtsp_pool.initialized = 0;
    rt_mutex_release(&g_rtsp_pool.mutex);

    rt_mutex_detach(&g_rtsp_pool.mutex);
    rt_sem_detach(&g_rtsp_pool.sem);
}

// 写入数据（MicroPython 压入 H.264 编码帧）
int rtsp_buffer_push(const uint8_t *data, size_t size, uint64_t pts, int is_keyframe) {
    if (!g_rtsp_pool.initialized) return -1;
    if (size > RTSP_MAX_PACKET_SIZE) {
        rt_kprintf("[RTSP Buffer] 帧大小超过上限: %zu\n", size);
        return -2;
    }

    rt_mutex_take(&g_rtsp_pool.mutex, -1);

    // 如果队列已满，强制抛弃最老的一帧，腾出位置
    if (g_rtsp_pool.count >= RTSP_MAX_SLOTS) {
        // 抛弃头部老帧
        g_rtsp_pool.head = (g_rtsp_pool.head + 1) % RTSP_MAX_SLOTS;
        g_rtsp_pool.count--;
        // 消耗一个信号量，防止信号计数与队列数量不一致
#ifndef RTSMART_WEB_PORTABLE
        rt_sem_take(&g_rtsp_pool.sem, 0); // 立即返回不等待
#else
        sem_trywait(&g_rtsp_pool.sem);
#endif
    }

    // 写入尾部
    int idx = g_rtsp_pool.tail;
    memcpy(g_rtsp_pool.slots[idx].data, data, size);
    g_rtsp_pool.slots[idx].size = size;
    g_rtsp_pool.slots[idx].pts = pts;
    g_rtsp_pool.slots[idx].is_keyframe = is_keyframe;

    g_rtsp_pool.tail = (idx + 1) % RTSP_MAX_SLOTS;
    g_rtsp_pool.count++;

    rt_mutex_release(&g_rtsp_pool.mutex);

    // 释放信号量，通知等待的 C 推流线程去消费
    rt_sem_release(&g_rtsp_pool.sem);
    return 0;
}

// C 侧消费数据（多线程推流/录制核心，带超时阻塞）
int rtsp_buffer_pop_blocking(uint8_t *out_data, size_t *out_size, uint64_t *out_pts, int *out_is_key, int timeout_ms) {
    if (!g_rtsp_pool.initialized) return -1;

    // 阻塞等待信号量（如果有帧则不等待直接过，无帧则休眠直到 push 动作唤醒）
#ifndef RTSMART_WEB_PORTABLE
    int rt = rt_sem_take(&g_rtsp_pool.sem, timeout_ms == -1 ? RT_WAITING_FOREVER : rt_tick_from_millisecond(timeout_ms));
    if (rt != 0) return -2; // 超时或失败
#else
  if (timeout_ms == -1) {
        sem_wait(&g_rtsp_pool.sem);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        
        // 1. 先计算秒和纳秒的增量
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;

        // 🎯 核心修复：处理纳秒满 1 秒向秒进位的逻辑。
        // 如果不加此判断，当系统当前时间的 tv_nsec 加上增量后超过 999999999，
        // 会违反 POSIX 规范，导致 sem_timedwait 直接报 EINVAL 错误拒绝阻塞，引发线程高频空转。
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }

        if (sem_timedwait(&g_rtsp_pool.sem, &ts) != 0) return -2;
    }
#endif

    rt_mutex_take(&g_rtsp_pool.mutex, -1);

    if (g_rtsp_pool.count <= 0) {
        rt_mutex_release(&g_rtsp_pool.mutex);
        return -3; // 异常防护：队列为空
    }

    int idx = g_rtsp_pool.head;
    size_t size = g_rtsp_pool.slots[idx].size;
    memcpy(out_data, g_rtsp_pool.slots[idx].data, size);
    *out_size = size;
    *out_pts = g_rtsp_pool.slots[idx].pts;
    *out_is_key = g_rtsp_pool.slots[idx].is_keyframe;

    g_rtsp_pool.head = (idx + 1) % RTSP_MAX_SLOTS;
    g_rtsp_pool.count--;

    rt_mutex_release(&g_rtsp_pool.mutex);
    return 0;
}

int rtsp_buffer_get_count(void) {
    return g_rtsp_pool.count;
}


void rtsp_buffer_wakeup_all(void) {
    if (!g_rtsp_pool.initialized) return;

    rt_mutex_take(&g_rtsp_pool.mutex, -1);
    
    // 释放足够多次，确保所有线程（推流、录制等）都能被唤醒
    for (int i = 0; i < 8; i++) { 
#ifndef RTSMART_WEB_PORTABLE
        rt_sem_release(&g_rtsp_pool.sem);
#else
        sem_post(&g_rtsp_pool.sem);
#endif
    }
    
    rt_mutex_release(&g_rtsp_pool.mutex);
}
