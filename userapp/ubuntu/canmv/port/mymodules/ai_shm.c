// ai_shm.c
#include "py/obj.h"
#include "py/runtime.h"

// 引入 MicroPython 线程与 GIL 相关的系统头文件
#if MICROPY_PY_THREAD
#include "py/mpthread.h"
#endif

#ifndef NO_QSTR
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h> // 引入头文件以使用 usleep

// 包含 RT-Smart 共享内存系统调用头文件
#include <lwp_shm.h> 

// 显式声明系统调用，防止部分旧版 SDK 环境下头文件引用不全
#ifndef LWP_IPC_CREAT
#define LWP_IPC_CREAT 01000
#endif
#endif

#include "ai_shm_proto.h"

// 全局共享内存指针句柄
static ImgShm *g_img_shm = NULL;
static PoseResultShm *g_res_shm = NULL;

/**
 * ai_shm.init()
 * 职责：初始化或挂载图像缓冲 SHM 与姿态分析结果 SHM，支持双端任意顺序启动。
 * 若 Python 先启动，则负责洗白内存并初始化跨进程互斥锁。
 */
STATIC mp_obj_t ai_shm_init(void) {
    // ------------------ 1. 图像缓冲区初始化与挂载 ------------------
    int img_id = lwp_shmget(IMG_SHM_KEY, sizeof(ImgShm), 0666 | LWP_IPC_CREAT);
    if (img_id < 0) {
        printf("[Python SHM] 错误: 无法获取/创建图像共享内存!\n");
        mp_raise_OSError(1);
    }
    g_img_shm = (ImgShm*)lwp_shmat(img_id, NULL);

    // 鲁棒性设计：若 Python 先启动，此时 magic 尚未初始化，由 Python 抢先洗白并建立跨进程锁
    if (g_img_shm->magic != 0xAA1234) {
        memset(g_img_shm, 0, sizeof(ImgShm));
        
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED); // 必须设置为跨进程共享
        pthread_mutex_init(&g_img_shm->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        
        g_img_shm->magic = 0xAA1234; // 盖上初始化戳
        printf("[Python SHM] Python 端抢先初始化图像共享内存及跨进程锁成功。\n");
    }

    // ------------------ 2. 姿态检测结果缓冲区初始化与挂载 ------------------
    int res_id = lwp_shmget(AI_RESULT_KEY, sizeof(PoseResultShm), 0666 | LWP_IPC_CREAT);
    if (res_id < 0) {
        printf("[Python SHM] 错误: 无法获取/创建结果共享内存!\n");
        mp_raise_OSError(1);
    }
    g_res_shm = (PoseResultShm*)lwp_shmat(res_id, NULL);

    // 鲁棒性设计：若 Python 先启动结果内存处理
    if (g_res_shm->magic != 0xBB5678) {
        memset(g_res_shm, 0, sizeof(PoseResultShm));
        
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&g_res_shm->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        
        g_res_shm->magic = 0xBB5678; // 盖上初始化戳
        printf("[Python SHM] Python 端抢先初始化结果共享内存及跨进程锁成功。\n");
    }

    printf("[Python SHM] 成功绑定生态通道. 槽位数: %d, 图像尺寸: %d B\n", IMG_BUF_NUM, IMG_SIZE);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(ai_shm_init_obj, ai_shm_init);


/**
 * ai_shm.deinit()
 * 职责：解除共享内存的映射
 */
STATIC mp_obj_t ai_shm_deinit(void) {
    if (g_img_shm) {
        lwp_shmdt(g_img_shm);
        g_img_shm = NULL;
    }
    if (g_res_shm) {
        lwp_shmdt(g_res_shm);
        g_res_shm = NULL;
    }
    printf("[Python SHM] 共享内存解除挂载完成。\n");
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(ai_shm_deinit_obj, ai_shm_deinit);


/**
 * ai_shm.send_frame(frame_id, bytes_data)
 * 职责：阻塞等待空闲槽位（带背压控制，绝对不丢帧）
 * 通过释放 Python GIL 锁，配合低功耗微秒休眠，既保证数据完整性，又不卡死其他 Python 业务。
 */
STATIC mp_obj_t ai_shm_send_frame(mp_obj_t frame_id_obj, mp_obj_t data_obj) {
    if (g_img_shm == NULL) {
        mp_raise_ValueError(MP_OBJ_NEW_QSTR(MP_QSTR_NotInitialized));
    }

    uint32_t fid = mp_obj_get_int(frame_id_obj);
    mp_buffer_info_t bufinfo;
    
    // 必须在释放 GIL 锁前，在解释器主上下文中提取外部传入的 Buffer 数据指针
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);

    if (bufinfo.len != IMG_SIZE) {
        printf("[Python SHM] 错误: 数据大小 %d 不匹配模型所需的 %d 字节!\n", (int)bufinfo.len, IMG_SIZE);
        mp_raise_ValueError(MP_OBJ_NEW_QSTR(MP_QSTR_SizeMismatch));
    }

    // ================== [ 关键优化：释放 Python 全局 GIL 锁 ] ==================
    // 释放锁后，即使当前搬运线程进入阻塞等待，外部的 MQTT 线程和 RTSP 推流线程依旧能高速并发
    MP_THREAD_GIL_EXIT();

    uint32_t curr_slot;

    while (1) {
        // 抢占跨进程互斥锁
        pthread_mutex_lock(&g_img_shm->mutex);
        
        curr_slot = g_img_shm->write_idx;
        
        // 场景 A：当前轮转的预设槽位正处于空闲状态 (ready == 0)
        if (g_img_shm->frames[curr_slot].ready == 0) {
            break; // 成功锁定了合法的空闲槽位，跳出轮询（注意：此时仍持有 mutex 锁）
        }
        
        // 场景 B：轮转指针被占，全局检索是否有其他已被 C++ 消费完释放的槽位
        int found_free = -1;
        for (int i = 0; i < IMG_BUF_NUM; i++) {
            if (g_img_shm->frames[i].ready == 0) {
                found_free = i;
                break;
            }
        }
        
        if (found_free >= 0) {
            curr_slot = (uint32_t)found_free;
            g_img_shm->write_idx = curr_slot; // 校正写入索引
            break; 
        }

        // 场景 C：当前所有缓冲槽位全部处于满载状态（C++ 正在进行 KPU 硬件推理或积压）
        // 核心安全点：必须立刻解开进程锁，否则 C++ 采集端拿不到锁无法消费，会导致双端死锁！
        pthread_mutex_unlock(&g_img_shm->mutex);
        
        // 执行 2 毫秒微秒级休眠，让出 K230 CPU 时间片供 C++ 快速消化帧数据
        usleep(2000); 
    }

    // --- 成功捕获到一个空闲槽位(curr_slot)，且当前持有进程间互斥锁 ---

    // 内存对齐高速拷贝，搬运 320x320x3 原始图像数据
    memcpy(g_img_shm->frames[curr_slot].data, bufinfo.buf, IMG_SIZE);
    g_img_shm->frames[curr_slot].frame_id = fid;
    
    // 变更状态：标记为已写待读(1)，交给 C++ 端的 aicap_start 线程抓取
    g_img_shm->frames[curr_slot].ready = 1; 

    // 轮转下一个期望写入的槽位
    g_img_shm->write_idx = (curr_slot + 1) % IMG_BUF_NUM;

    // 释放进程间互斥锁
    pthread_mutex_unlock(&g_img_shm->mutex);

    // ================== [ 关键优化：重新夺回 Python GIL 锁 ] ==================
    MP_THREAD_GIL_ENTER();

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(ai_shm_send_frame_obj, ai_shm_send_frame);


/**
 * ai_shm.get_result()
 * 职责：Python 端获取最新姿态/跌倒结果。
 * 内含 has_new 校验：若 C++ 未更新结果，直接返回 (0, [])，防止频繁重复画框与重复触发 MQTT 跌倒报警。
 */
STATIC mp_obj_t ai_shm_get_result(void) {
    if (g_res_shm == NULL) {
        mp_raise_ValueError(MP_OBJ_NEW_QSTR(MP_QSTR_NotInitialized));
    }

    // 抢占结果缓冲区互斥锁
    pthread_mutex_lock(&g_res_shm->mutex);
    
    // 检查是否有全新推理结果到达
    if (g_res_shm->has_new == 0) {
        // 无新结果，直接解锁并返回空元组，避免应用层重复刷新
        pthread_mutex_unlock(&g_res_shm->mutex);
        
        mp_obj_t empty_list = mp_obj_new_list(0, NULL);
        mp_obj_t res[2] = { mp_obj_new_int(0), empty_list };
        return mp_obj_new_tuple(2, res);
    }
    
    // 存在全新结果，快速拉取核心状态到本地局部变量，缩短锁的占用时间
    uint32_t fid = g_res_shm->frame_id;
    uint32_t count = g_res_shm->box_count;
    
    static PoseBox local_boxes[MAX_POSE_BOXES];
    if (count > MAX_POSE_BOXES) count = MAX_POSE_BOXES;
    memcpy(local_boxes, g_res_shm->boxes, count * sizeof(PoseBox));
    
    // 【状态清零】通知 C++ 进程，当前最新的数据已经被 Python 顺利消费
    g_res_shm->has_new = 0;
    
    pthread_mutex_unlock(&g_res_shm->mutex);

    // 将 C 结构体数据还原并构建为 MicroPython 原生高级对象（Dict & List）
    mp_obj_t person_list = mp_obj_new_list(0, NULL);
    
    for (uint32_t i = 0; i < count; i++) {
        mp_obj_t d = mp_obj_new_dict(0);
        
        // 写入目标边界框、目标类别、检测置信度
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_x1),    mp_obj_new_float(local_boxes[i].x1));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_y1),    mp_obj_new_float(local_boxes[i].y1));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_x2),    mp_obj_new_float(local_boxes[i].x2));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_y2),    mp_obj_new_float(local_boxes[i].y2));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_class), mp_obj_new_int(local_boxes[i].class_id));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_score), mp_obj_new_float(local_boxes[i].confidence));

        // 写入 YOLOv5/v8-Pose 核心的 17 个骨骼关键点 [x, y, confidence_score]
        mp_obj_t kps_list = mp_obj_new_list(0, NULL);
        for (int k = 0; k < 17; k++) {
            mp_obj_t kpt_tuple[3] = {
                mp_obj_new_float(local_boxes[i].kps[k * 3]),     // X
                mp_obj_new_float(local_boxes[i].kps[k * 3 + 1]), // Y
                mp_obj_new_float(local_boxes[i].kps[k * 3 + 2])  // 关键点置信度
            };
            mp_obj_list_append(kps_list, mp_obj_new_tuple(3, kpt_tuple));
        }
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_kps), kps_list);

        mp_obj_list_append(person_list, d);
    }

    mp_obj_t res[2] = { mp_obj_new_int(fid), person_list };
    return mp_obj_new_tuple(2, res);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(ai_shm_get_result_obj, ai_shm_get_result);


/* ------------------ MicroPython 模块成员及全局字典映射 ------------------ */
STATIC const mp_rom_map_elem_t ai_shm_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),   MP_ROM_QSTR(MP_QSTR_ai_shm) },
    
    // 映射外部调用接口
    { MP_ROM_QSTR(MP_QSTR_init),       MP_ROM_PTR(&ai_shm_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),     MP_ROM_PTR(&ai_shm_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_frame), MP_ROM_PTR(&ai_shm_send_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_result), MP_ROM_PTR(&ai_shm_get_result_obj) },
};
STATIC MP_DEFINE_CONST_DICT(ai_shm_module_globals, ai_shm_module_globals_table);


// 严格匹配 CanMV / MicroPython 自动化 cmodule 链接正则的标准命名
const mp_obj_module_t ai_shm_user_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&ai_shm_module_globals,
};

// 注册至系统基础运行库中
MP_REGISTER_MODULE(MP_QSTR_ai_shm, ai_shm_user_module);