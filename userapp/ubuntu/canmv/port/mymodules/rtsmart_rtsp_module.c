#include "py/obj.h"
#include "py/runtime.h"
#include "py/objstr.h"
#include "py/mperrno.h"

#include "rtsp_packet_buffer.h"
#include "rtsp_native_engine.h" // 引入新文件的头

// rtsmart_rtsp.init()
STATIC mp_obj_t rtsmart_rtsp_init(void) {
    if (rtsp_buffer_init() == 0) {
        return mp_const_true;
    }
    return mp_const_false;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(rtsmart_rtsp_init_obj, rtsmart_rtsp_init);

// rtsmart_rtsp.deinit()
STATIC mp_obj_t rtsmart_rtsp_deinit(void) {
    rtsp_buffer_deinit();
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(rtsmart_rtsp_deinit_obj, rtsmart_rtsp_deinit);

// rtsmart_rtsp.push_packet(bytes_data, pts, is_key)
STATIC mp_obj_t rtsmart_rtsp_push_packet(size_t n_args, const mp_obj_t *args) {
    if (n_args < 3) {
        mp_raise_TypeError(MP_ERROR_TEXT("push_packet requires: bytes, pts_int, is_keyframe_bool"));
    }

    // 1. 获取字节数据
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[0], &bufinfo, MP_BUFFER_READ);

    // 2. 获取 PTS 和是否为 I 帧标志
    uint64_t pts = (uint64_t)mp_obj_get_int_truncated(args[1]);
    int is_key = mp_obj_is_true(args[2]) ? 1 : 0;
    MP_THREAD_GIL_EXIT();

    // 3. 压入底层高速 FIFO C 缓冲区
    int ret = rtsp_buffer_push((const uint8_t *)bufinfo.buf, bufinfo.len, pts, is_key);
     MP_THREAD_GIL_ENTER();
    if (ret != 0) {
        // 返回错误码而不是崩溃，让 Python 层决定是否需要降码率
        return mp_obj_new_int(ret);
    }
   

    return mp_obj_new_int(0);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(rtsmart_rtsp_push_packet_obj, 3, 3, rtsmart_rtsp_push_packet);

// rtsmart_rtsp.get_count()
STATIC mp_obj_t rtsmart_rtsp_get_count(void) {
    return mp_obj_new_int(rtsp_buffer_get_count());
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(rtsmart_rtsp_get_count_obj, rtsmart_rtsp_get_count);


STATIC mp_obj_t rtsmart_rtsp_start_server(mp_obj_t port_obj, mp_obj_t session_obj) {
    int port = mp_obj_get_int(port_obj);
    const char *session_name = mp_obj_str_get_str(session_obj);
    
    int ret = rtsp_native_engine_start(port, session_name);
    return ret == 0 ? mp_const_true : mp_const_false;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(rtsmart_rtsp_start_server_obj, rtsmart_rtsp_start_server);

STATIC mp_obj_t rtsmart_rtsp_stop_server(void) {
    rtsp_native_engine_stop();
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(rtsmart_rtsp_stop_server_obj, rtsmart_rtsp_stop_server);


// 注册全局模块属性
STATIC const mp_rom_map_elem_t rtsmart_rtsp_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),          MP_ROM_QSTR(MP_QSTR_rtsmart_rtsp) },
    { MP_ROM_QSTR(MP_QSTR_init),              MP_ROM_PTR(&rtsmart_rtsp_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),            MP_ROM_PTR(&rtsmart_rtsp_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_push_packet),        MP_ROM_PTR(&rtsmart_rtsp_push_packet_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_count),         MP_ROM_PTR(&rtsmart_rtsp_get_count_obj) },
    
   { MP_ROM_QSTR(MP_QSTR_rtsp_start),        MP_ROM_PTR(&rtsmart_rtsp_start_server_obj) },
{ MP_ROM_QSTR(MP_QSTR_rtsp_stop),         MP_ROM_PTR(&rtsmart_rtsp_stop_server_obj) },
};
STATIC MP_DEFINE_CONST_DICT(rtsmart_rtsp_module_globals, rtsmart_rtsp_module_globals_table);

const mp_obj_module_t mp_module_rtsmart_rtsp = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&rtsmart_rtsp_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_rtsmart_rtsp, mp_module_rtsmart_rtsp);