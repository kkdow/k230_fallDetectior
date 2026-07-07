# C/C++ 原生源码 (ubuntu)

本目录包含 K230 平台 C/C++ 层全部源码，分为 **MicroPython C 扩展模块** 与 **独立 C++ AI 推理进程** 两大部分，是实现高性能端侧 AI 与多媒体联动的底层核心。

- **C 扩展模块** (`canmv/port/mymodules/`)：编译进 CanMV 固件，为 Python 提供高性能底层能力。
- **C++ AI 进程** (`applications/ai_pose/`)：独立 LWP 进程，专职 KPU 推理，与 Python 进程通过共享内存通信。

---

## 📂 目录结构

```text
ubuntu/
├── applications/ai_pose/          # C++ AI 推理进程 (独立 LWP)
│   ├── main.cc                    # 入口：共享内存初始化 + 线程启动
│   ├── ai_worker.cc               # 采集线程 (aicap) + 推理线程 (aicons)
│   ├── ai_base.cc/h               # nncase 推理基类
│   ├── pose_detect.cc/h           # YOLOv8-pose 检测 (预处理/推理/后处理)
│   ├── ai_utils.cc/h              # ai2d 配置 + transpose 加速
│   ├── ai_shm_proto.h             # 共享内存协议 (与 Python 端共用)
│   ├── scoped_timing.h            # RAII 性能计时
│   ├── Kconfig                    # 编译配置
│   └── Makefile                   # 编译脚本
└── canmv/port/                    # MicroPython 移植目录
    ├── Makefile                   # 主 Makefile (需集成 mymodules)
    └── mymodules/                 # C 扩展模块 (编译进固件)
        ├── ai_shm.c               # 共享内存 Python 绑定
        ├── k230_alert.c           # MQTT over TLS (mbedTLS) + 本地日志
        ├── mp4_c_helper.c         # MP4 录制 (含 GIL 释放)
        ├── rtsmart_rtsp_module.c  # RTSP 推流 Python 绑定
        ├── rtsp_native_engine.cpp # RTSP 消费线程 + KdRtspServer
        ├── rtsp_packet_buffer.c   # 64 槽位环形缓冲区
        ├── ai_shm_proto.h         # 共享内存协议 (与 AI 进程共用)
        ├── rtsp_packet_buffer.h
        └── rtsp_native_engine.h
```

---

## 🚀 编译指南

### 1. 编译 C++ AI 进程 (ai_pose)
```bash
cd applications/ai_pose
make clean && make
```

编译产物 `ai_pose` 部署到 `/sdcard/app/`，由 `init.sh` 自启动拉起。

**关键依赖**：nncase 运行时、OpenCV、lwp_shm (RT-Smart)

### 2. 编译 C 扩展模块 (固件集成)
将 `mymodules/` 源码集成到 CanMV SDK 的 `port/` 目录，修改 Makefile：

```makefile
SRC_C += $(wildcard mymodules/*.c)
SRC_CXX += $(wildcard mymodules/*.cpp)
```

重新编译 CanMV 固件并烧录。**必须编译进固件，不支持动态加载。**

---

## 🔌 模块依赖说明

| 模块 | 依赖库 | 功能 |
| --- | --- | --- |
| `ai_shm` | lwp_shm, pthread | 跨进程共享内存读写 |
| `k230_alert` | mbedtls, pthread, semaphore | MQTT over TLS 加密推送 + 本地 Flash 日志 |
| `mp4_c_helper` | mp4_format (K230 SDK) | MP4 录制零拷贝接口，释放 GIL |
| `rtsmart_rtsp` | rtsp_server (K230 SDK), pthread | RTSP 推流 + 环形缓冲 (64 帧) |
| `ai_pose` | nncase, opencv, lwp_shm | KPU 推理 + 姿态估计 |

---

## 📝 共享内存协议 (IPC)

两进程通过 RT-Smart 的 `lwp_shm` 交换数据，协议定义在 `ai_shm_proto.h`：

| 内存 Key | 结构体 | 用途 | 大小 |
| --- | --- | --- | --- |
| `0x3000` | `ImgShm` | 3 槽位 Ping-Pong 图像缓冲 | 320×320×3 RGB888 |
| `0x3005` | `PoseResultShm` | 检测结果 (最多 10 人 × 17 点) | 含 bbox + kps[51] |

**同步方式**：跨进程互斥锁 (`PTHREAD_PROCESS_SHARED`) + 轮询，无信号量。

**关键字段**：
- `ImgFrame.ready`：0=空闲，1=已写待读，2=推理中
- `PoseResultShm.has_new`：1=有新结果待读取

---

## 🔧 核心模块设计

### C++ AI 进程 (ai_pose)

| 线程 | 职责 | 关键逻辑 |
| --- | --- | --- |
| `aicap_start` | 采集 | 轮询 `ready==1` 槽位 → 置 `ready=2` → 打包 `InferTask` 入队 → 条件唤醒推理线程 |
| `aicons_start` | 推理 | 阻塞取任务 → ai2d padding resize → KPU 推理 → NMS 后处理 → 写回结果 (`has_new=1`) → 释放槽位 (`ready=0`) |

**输入/输出**：输入 `task.data` (图像指针) + `frame_id`；输出 `PoseBox[]` (含 bbox、置信度、51 维 kps)。

### C 扩展模块 (mymodules)

| 模块 | 功能 | 关键技术 |
| --- | --- | --- |
| `ai_shm` | Python 端共享内存 | `lwp_shmget`/`lwp_shmat`，魔数校验，`usleep(2000)` 背压 |
| `k230_alert` | MQTT 加密推送 | mbedTLS + EMQX Cloud 8883，信号量异步唤醒，25s 心跳 |
| `mp4_c_helper` | MP4 录制 | `MP_THREAD_GIL_EXIT()` 释放 GIL，`kd_mp4_write_frame` 零拷贝 |
| `rtsmart_rtsp` | RTSP 推流 | 环形缓冲 64 帧，C++ 消费线程阻塞取帧 |

**GIL 释放时机** (核心优化)：
```c
MP_THREAD_GIL_EXIT();   // 必须在获取 Python 对象之后
kd_mp4_write_frame();   // 阻塞 I/O
MP_THREAD_GIL_ENTER();  // 落盘完成后重新获取
```

---

## 📄 文件清单 (mymodules)

| 文件名 | 大小 | 说明 |
| --- | --- | --- |
| `ai_shm.c` | 11,332 | 共享内存 Python 绑定 |
| `k230_alert.c` | 16,666 | MQTT over TLS 推送 |
| `mp4_c_helper.c` | 3,850 | MP4 录制 GIL 释放 |
| `rtsmart_rtsp_module.c` | 3,595 | RTSP 推流 Python 绑定 |
| `rtsp_native_engine.cpp` | 4,758 | RTSP 原生引擎 |
| `rtsp_packet_buffer.c` | 6,595 | 环形缓冲区 |
| `ai_shm_proto.h` | 1,999 | 共享内存协议 |
| `rtsp_packet_buffer.h` | 1,275 | 环形缓冲头文件 |
| `rtsp_native_engine.h` | 393 | RTSP 引擎头文件 |

---

## 📝 常见问题 (FAQ)

**Q: `ai_pose` 启动后无法连接共享内存？**
A: 确保 Python 主进程已先运行 `ai_shm.init()`。检查 `lwp_shmget` 返回值和 `magic` 魔数。

**Q: C 扩展模块报 `ImportError: no module named 'xxx'`？**
A: 检查 `MP_REGISTER_MODULE` 宏中的模块名，确认固件已重新编译烧录。

**Q: 共享内存跨进程锁死锁？**
A: 确认 `pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)` 已设置。Python 端挂载时需校验 `magic`。

**Q: RTSP 推流卡顿/丢帧？**
A: 调用 `rtsmart_rtsp.get_count()` 检查环形缓冲积压。若持续满 64 帧，说明网络带宽不足。

**Q: MQTT 推送失败？**
A: 检查 `k230_alert.c` 中 EMQX Cloud 服务器地址、端口、用户名/密码。查看串口 `[C TLS ERROR]` 信息。

**Q: MP4 录制文件损坏？**
A: 确保录制结束调用了 `kd_mp4_destroy_tracks` 和 `kd_mp4_destroy`，避免文件未正确关闭。

---

## 📜 参考文献

[1] mbed TLS 官方文档[EB/OL]. (2024)[2026-06-19]. https://www.trustedfirmware.org/projects/mbed-tls.

[2] 嘉楠科技. CanMV K230文档[EB/OL]. (2024)[2026-06-19]. https://www.kendryte.com/zh/sdkResource/230canmv.

[3] RT-Thread. RT-Thread Smart 文档[EB/OL]. (2024)[2026-06-19]. https://www.rt-thread.org.