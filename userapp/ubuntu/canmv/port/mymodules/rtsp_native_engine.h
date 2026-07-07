#ifndef RTSP_NATIVE_ENGINE_H
#define RTSP_NATIVE_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

// 启动原生 C++ RTSP 服务，并拉起后台消费线程
int rtsp_native_engine_start(int port, const char* session_name);

// 停止 RTSP 服务，安全销毁消费线程
void rtsp_native_engine_stop(void);

#ifdef __cplusplus
}
#endif

#endif // RTSP_NATIVE_ENGINE_H