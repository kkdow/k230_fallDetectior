#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "main.h"
#include "ai_shm_proto.h" // 必须引入共享内存结构体

extern "C" {
    #include <lwp_shm.h>
}

pthread_t aicap_thread;
pthread_t aicon_thread;

void signal_handler(int sig) {
    printf("\n[Main] 捕获信号 %d，正在通知线程退出...\n", sig);
    exit(0);
}

// 核心修改：由 C++ 负责共享内存与跨进程锁的绝对主导初始化
void init_shared_memory() {
    printf("[Main] 正在强制初始化共享内存环境...\n");

    // 1. 图像共享内存初始化
    int img_id = lwp_shmget(IMG_SHM_KEY, sizeof(ImgShm), 0666 | 01000); // 01000 即 IPC_CREAT
    if (img_id >= 0) {
        ImgShm *img_shm = (ImgShm*)lwp_shmat(img_id, NULL);
        
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED); // 必须是跨进程共享锁
        pthread_mutex_init(&img_shm->mutex, &attr);
        pthread_mutexattr_destroy(&attr);

        // 彻底清空所有槽位脏数据
        img_shm->write_idx = 0;
        for(int i = 0; i < IMG_BUF_NUM; i++) {
            img_shm->frames[i].ready = 0;
            img_shm->frames[i].frame_id = 0;
        }
        img_shm->magic = 0xAA1234; // 盖上认证戳
        printf("[Main] -> 图像共享内存 (0x%X) 及跨进程锁初始化成功！\n", IMG_SHM_KEY);
    }

    // 2. 结果共享内存初始化
    int res_id = lwp_shmget(AI_RESULT_KEY, sizeof(PoseResultShm), 0666 | 01000);
    if (res_id >= 0) {
        PoseResultShm *res_shm = (PoseResultShm*)lwp_shmat(res_id, NULL);
        
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&res_shm->mutex, &attr);
        pthread_mutexattr_destroy(&attr);

        res_shm->frame_id = 0;
        res_shm->has_new = 0;
        res_shm->box_count = 0;
        res_shm->magic = 0xBB5678; // 盖上认证戳
        printf("[Main] -> 结果共享内存 (0x%X) 及跨进程锁初始化成功！\n", AI_RESULT_KEY);
    }
}

int main()
{   
    int rc;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 启动任何线程前，先完成内存洗白与锁初始化
    init_shared_memory();

    rc = pthread_create(&aicap_thread, NULL, aicap_start, NULL);
    if (rc) {
        printf("ERROR; return code from pthread_create() is %d\n", rc);
        exit(-1);
    }
    rc = pthread_create(&aicon_thread, NULL, aicons_start, NULL);
    if (rc) {
        printf("ERROR; return code from pthread_create() is %d\n", rc);
        exit(-1);
    }
    
    printf("[Main] 服务已全部就绪，正在后台监听 Python 数据，按 Ctrl+C 退出...\n");
    while (1) {
        sleep(100); 
    }
    
    pthread_join(aicap_thread, NULL);
    pthread_join(aicon_thread, NULL);
    return 0;
}