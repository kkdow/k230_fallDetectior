#include <stdio.h>
#include <stdint.h>
#include <string.h>

// 1. 核心 MicroPython 虚拟机运行时 API
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mpthread.h"

// 2. 引入嘉楠 K230 底层 MP4 格式 Muxer 头文件
#include "mp4_format.h"

/**
 * 核心绑定函数：mp4_c_helper.write_frame(...)
 * 参数说明 (5个固定参数，通过参数数组透传)：
 * args[0]: mp4_handle (整型指针) -> 对应 C 端的 KD_HANDLE
 * args[1]: track_handle (整型指针) -> 对应视频轨轨道句柄
 * args[2]: buf_obj (Python bytes 或 bytearray 缓冲区) -> 零拷贝直连
 * args[3]: data_length (整型) -> 当前帧的实际有效字节长度
 * args[4]: time_stamp (64位整型) -> 毫秒级相对时间轴时间戳
 */
STATIC mp_obj_t mp4_c_helper_write_frame(size_t n_args, const mp_obj_t *args) {
    (void)n_args; // 消除编译期未引用变量警告

    // 安全将 Python 整型对象还原为 K230 的底层通用句柄指针
    KD_HANDLE mp4_handle = (KD_HANDLE)(uintptr_t)mp_obj_get_int(args[0]);
    KD_HANDLE track_handle = (KD_HANDLE)(uintptr_t)mp_obj_get_int(args[1]);

    // 🎯 零拷贝核心：直接提取 Python 传入对象的底层连续内存缓冲区地址与视图
    mp_buffer_info_t buf_info;
    mp_get_buffer_raise(args[2], &buf_info, MP_BUFFER_READ);

    uint32_t data_length = (uint32_t)mp_obj_get_int(args[3]);

    // 🚨 关键排雷点：使用 mp_obj_get_int_truncated 安全提取 64 位宽时间戳。
    // 在 32 位位宽系统环境下，原生的 mp_obj_get_int 会直接截断高位导致时间轴错乱，引起 0 字节损坏。
    uint64_t time_stamp = (uint64_t)mp_obj_get_int_truncated(args[4]);

    // 根据你提供的 mp4_format.h 封装底层结构体
    k_mp4_frame_data_s frame_data;
    memset(&frame_data, 0, sizeof(frame_data));
    frame_data.codec_id = K_MP4_CODEC_ID_H264;       // 指定为 H264 编码格式
    frame_data.data = (uint8_t *)buf_info.buf;       // 直接透传 Python 字节流内存首地址
    frame_data.data_length = data_length;
    frame_data.time_stamp = time_stamp;
    frame_data.eof = 0;                              // 默认未到文件流结束符

    // 🎯 终极优化：在纯 C 环境下释放 MicroPython 虚拟机的全局解释器锁（GIL）
    // 这意味着：当底层调用 `kd_mp4_write_frame` 发生重负载文件系统 SD 卡盘落阻塞时，
    // 另外的核心和线程（AI 推理线程、OLED 刷新、RTSP 推流）依然可以并发欢快运行，彻底终结丢帧！
    MP_THREAD_GIL_EXIT();
    
    int ret = kd_mp4_write_frame(mp4_handle, track_handle, &frame_data);
    
    // 重新夺回全局虚拟机锁，安全返回 Python 空间环境
    MP_THREAD_GIL_ENTER();

    if (ret != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("kd_mp4_write_frame write failed"));
    }

    return mp_const_none;
}
// 注册参数约束：最少 5 个参数，最多 5 个参数
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp4_write_frame_obj, 5, 5, mp4_c_helper_write_frame);

// 3. 组装内建模块的全局映射全局表
STATIC const mp_rom_map_elem_t mp4_c_helper_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_mp4_c_helper) },
    { MP_ROM_QSTR(MP_QSTR_write_frame), MP_ROM_PTR(&mp4_write_frame_obj) },
};
STATIC MP_DEFINE_CONST_DICT(mp4_c_helper_globals, mp4_c_helper_globals_table);

// 4. 定义内建模块的本体结构
const mp_obj_module_t mp4_c_helper_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp4_c_helper_globals,
};

// 5. 核心内建注册宏：通知 MicroPython 编译器在链接阶段直接将本模块注册为 Python 内建空间的一部分
MP_REGISTER_MODULE(MP_QSTR_mp4_c_helper, mp4_c_helper_module);