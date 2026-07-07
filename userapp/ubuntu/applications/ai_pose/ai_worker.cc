#include <stdio.h>
#include <unistd.h>
#include <vector>
#include <queue>
#include <string>
#include <string.h>
#include <pthread.h>

#include "main.h"
#include "ai_shm_proto.h"
#include "pose_detect.h"  

extern "C" {
    #include <lwp_shm.h>
}

using namespace nncase;
using namespace nncase::runtime;

static std::queue<InferTask> g_task_queue;
static pthread_mutex_t g_queue_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;
static bool g_worker_running = true;

// 线程 A：图像采集线程
void* aicap_start(void* arg) {
    (void)arg;
    int sid = lwp_shmget(IMG_SHM_KEY, sizeof(ImgShm), 0666);
    if (sid < 0) { 
        printf("[AICap] 致命错误: 无法连接图像共享内存!\n"); 
        return NULL; 
    }
    ImgShm *img_shm = (ImgShm*)lwp_shmat(sid, NULL);
    
    printf("[AICap] 采集轮询线程启动，等待 Python 传入帧...\n");

    while (g_worker_running) {
        bool found_task = false;
        
        // 核心修改 1：轮询必须加跨进程锁！
        pthread_mutex_lock(&img_shm->mutex);
        
        // 核心修改 2：不要信 write_idx，直接遍历 3 个缓冲槽位
        for (int idx = 0; idx < IMG_BUF_NUM; idx++) {
            if (img_shm->frames[idx].ready == 1) {
                // 找到新数据，立刻锁定状态为 2 (推理中)
                img_shm->frames[idx].ready = 2;
                
                InferTask task;
                task.data = img_shm->frames[idx].data;
                task.frame_id = img_shm->frames[idx].frame_id;
                task.slot_idx = idx;

                pthread_mutex_lock(&g_queue_mtx);
                g_task_queue.push(task);
                pthread_cond_signal(&g_queue_cond);
                pthread_mutex_unlock(&g_queue_mtx);

                // 打印成功捕获轨迹
                // printf("[AICap] 成功从 Python 捕获帧 ID: %d (利用槽位: %d)\n", task.frame_id, idx);
                
                found_task = true;
                break; // 每次只取一帧，及时释放锁让 Python 继续写
            }
        }
        pthread_mutex_unlock(&img_shm->mutex);

        if (!found_task) {
            usleep(1000); // 没数据时休眠 1ms 避免 CPU 空转
        }
    }
    return NULL;
}

// 线程 B：姿态推理线程
void* aicons_start(void* arg) {
    (void)arg;
    FrameCHWSize image_size = {3, 320, 320}; 
    poseDetect pd((char*)"/data/best.kmodel", 0.25f, 0.45f, image_size, 0);

    int rsid = lwp_shmget(AI_RESULT_KEY, sizeof(PoseResultShm), 0666);
    PoseResultShm *res_shm = (PoseResultShm*)lwp_shmat(rsid, NULL);
    
    int isid = lwp_shmget(IMG_SHM_KEY, sizeof(ImgShm), 0666);
    ImgShm *img_shm = (ImgShm*)lwp_shmat(isid, NULL);

    printf("[AIWorker] KPU 推理核心启动，等待队列分配任务...\n");

    while (g_worker_running) {
        InferTask task;
        pthread_mutex_lock(&g_queue_mtx);
        while (g_task_queue.empty() && g_worker_running) {
            pthread_cond_wait(&g_queue_cond, &g_queue_mtx);
        }
        if (!g_worker_running) {
            pthread_mutex_unlock(&g_queue_mtx);
            break;
        }
        task = g_task_queue.front();
        g_task_queue.pop();
        pthread_mutex_unlock(&g_queue_mtx);

        // --- 硬件推理 ---
        runtime_tensor input_tensor = pd.get_input_tensor(0);
        auto mapped_buf = host_runtime_tensor::map(input_tensor, map_access_::map_write).unwrap();
        memcpy(mapped_buf.buffer().data(), task.data, IMG_SIZE);
        hrt::sync(input_tensor, sync_op_t::sync_write_back, true).expect("sync write_back failed");

        pd.pre_process(input_tensor); 
        pd.inference(); 
        
        std::vector<OutputPose> results;
        pd.post_process(results);

        // --- 写回结果内存 ---
        pthread_mutex_lock(&res_shm->mutex);
        res_shm->frame_id = task.frame_id;
        res_shm->box_count = (results.size() > MAX_POSE_BOXES) ? MAX_POSE_BOXES : (uint32_t)results.size();

        for (uint32_t i = 0; i < res_shm->box_count; i++) {
            res_shm->boxes[i].x1 = (float)results[i].box.x;
            res_shm->boxes[i].y1 = (float)results[i].box.y;
            res_shm->boxes[i].x2 = (float)(results[i].box.x + results[i].box.width);
            res_shm->boxes[i].y2 = (float)(results[i].box.y + results[i].box.height);
            res_shm->boxes[i].class_id = (uint8_t)results[i].index;
            res_shm->boxes[i].confidence = results[i].confidence;

            for(int k = 0; k < 51; k++) {
                if (k < (int)results[i].kps.size()) res_shm->boxes[i].kps[k] = results[i].kps[k];
                else res_shm->boxes[i].kps[k] = 0.0f;
            }
        }
        res_shm->has_new = 1; 
        pthread_mutex_unlock(&res_shm->mutex);
        
        // printf("[AIWorker] 帧 %d 推理完成! 检出目标数: %d\n", task.frame_id, res_shm->box_count);

        // --- 释放图像内存槽位 ---
        // 核心修改 3：释放槽位状态必须加锁，防冲突！
        pthread_mutex_lock(&img_shm->mutex);
        img_shm->frames[task.slot_idx].ready = 0; 
        pthread_mutex_unlock(&img_shm->mutex);
    }
    return NULL;
}