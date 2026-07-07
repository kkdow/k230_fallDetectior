#include "rtsp_native_engine.h"
extern "C" {
    #include "rtsp_packet_buffer.h" 
}
#include "rtsp_server.h"        

#include <pthread.h>  // 替代 std::thread 以便控制栈大小
#include <atomic>
#include <cstdio>  
#include <string>
#include <unistd.h> 

static KdRtspServer* g_rtsp_server = nullptr;
static pthread_t g_consumer_tid;
static std::atomic<bool> g_engine_running{false};
static std::atomic<bool> g_consumer_exited{true}; // 追踪子线程是否彻底退出
static std::string g_session_name = "test";

// 后台 C 消费线程
static void* rtsp_consumer_worker_adapter(void* arg) {
    g_consumer_exited = false;
    printf("[Native Engine] Consumer thread running (Stack: 128KB).\n");
    
    // 预分配缓冲区，避免在循环中频繁申请
    uint8_t* frame_buf = new (std::nothrow) uint8_t[RTSP_MAX_PACKET_SIZE];
    if (!frame_buf) {
        g_consumer_exited = true;
        return nullptr;
    }

    while (g_engine_running) {
        size_t size = 0;
        uint64_t pts = 0;
        int is_key = 0;

        // 阻塞获取数据（超时时间 100ms）
        int ret = rtsp_buffer_pop_blocking(frame_buf, &size, &pts, &is_key, 100);
        
        // 核心保护：只有在运行标志有效且指针非空时才调用 SDK
        // 使用局部指针拷贝防止 stop 函数在另一线程并发置空
        KdRtspServer* server_ptr = g_rtsp_server;
        if (g_engine_running && ret == 0 && size > 0 && server_ptr) {
            server_ptr->SendVideoData(g_session_name, frame_buf, size, pts);
        }
    }

    delete[] frame_buf;
    printf("[Native Engine] Consumer thread safe exit.\n");
    g_consumer_exited = true;
    return nullptr;
}

extern "C" {

int rtsp_native_engine_start(int port, const char* session_name) {
    if (g_engine_running || g_rtsp_server != nullptr) return 0; // 已经在运行中
    
    g_session_name = session_name ? session_name : "test";
    g_rtsp_server = new (std::nothrow) KdRtspServer();
    if (!g_rtsp_server) return -1;

    // 1. 初始化
    if (g_rtsp_server->Init(port, nullptr) != 0) {
        delete g_rtsp_server;
        g_rtsp_server = nullptr;
        return -2;
    }

    // 2. 创建 Session
    SessionAttr attr;
    attr.with_video = true;
    attr.video_type = VideoType::kVideoTypeH264;
    attr.with_audio = false;

    if (g_rtsp_server->CreateSession(g_session_name, attr) != 0) {
        g_rtsp_server->DeInit();
        delete g_rtsp_server;
        g_rtsp_server = nullptr;
        return -3;
    }

    // 3. 启动底层 Live555 线程
    g_rtsp_server->Start();

    // 4. 启动消费线程
    pthread_attr_t attr_p;
    pthread_attr_init(&attr_p);
    pthread_attr_setstacksize(&attr_p, 128 * 1024); 
    
    g_engine_running = true;
    if (pthread_create(&g_consumer_tid, &attr_p, rtsp_consumer_worker_adapter, NULL) != 0) {
        printf("[Native Engine] CRITICAL: Failed to create consumer thread! Rolling back...\n");
        
        // 🎯 核心修复：启动失败时的回滚清理
        g_engine_running = false; 
        g_rtsp_server->Stop();
        usleep(100000); // 短暂等待线程回收
        g_rtsp_server->DeInit();
        delete g_rtsp_server;
        g_rtsp_server = nullptr;

        pthread_attr_destroy(&attr_p);
        return -4;
    }
    pthread_attr_destroy(&attr_p);

    printf("[Native Engine] RTSP Server started on port %d\n", port);
    return 0;
}

void rtsp_native_engine_stop(void) {
    // 🎯 核心修复：不再单纯依赖 g_engine_running
    // 只要有 server 对象或消费线程在运行，就进入清理流程
    if (!g_engine_running && g_rtsp_server == nullptr) {
        return;
    }

    printf("[Native Engine] Stopping and cleaning up... \n");

    // 1. 先停数据源消费线程
    if (g_engine_running) {
        g_engine_running = false;
        rtsp_buffer_wakeup_all(); 
        pthread_join(g_consumer_tid, NULL);
        printf("[Native Engine] Consumer thread joined.\n");
    }

    // 2. 清理 SDK 资源
    if (g_rtsp_server) {
        printf("[Native Engine] Stopping Live555 Server...\n");
        g_rtsp_server->Stop(); 
        
        usleep(300000); // 留出足够的静默期给 Live555 内部 Socket 释放

        // 采用临时指针防止多线程二次进入时踩踏
        KdRtspServer* temp_ptr = g_rtsp_server;
        g_rtsp_server = nullptr; 

        printf("[Native Engine] De-initializing SDK...\n");
        temp_ptr->DeInit();
        delete temp_ptr;
        
        printf("[Native Engine] Server cleanup completed safely.\n");
    }
}

} // extern "C"