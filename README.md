# k230_fallDetectior
基于rtsmart ai套件的跌倒检测系统
本项目是一个基于 **嘉楠 K230 (CanMV)** 开发板的完整智能跌倒检测监护系统。系统采用 **C/Python 混合架构**与**双进程（LWP）设计**，实现了从模型训练、量化部署到端侧实时推理与多媒体联动的全链路解决方案。

- **高性能 AI 推理**：基于 YOLOv8-pose 的姿态估计模型，经 nncase 量化后运行于 K230 KPU，实时提取 17 个骨骼关键点。
- **双进程解耦架构**：MicroPython 进程负责多媒体与业务逻辑，C++ 原生进程专职 AI 推理，通过共享内存（IPC）零拷贝通信。
- **C 扩展性能加速**：RTSP 推流、MP4 录制、MQTT 加密推送等模块下沉至 C 层，释放 GIL 锁，确保 20fps 高负载下零丢帧。
- **全链路报警联动**：跌倒触发后，OSD 警示、语音播报、MP4 落盘、MQTT 云端推送与本地日志原子追加五维联动。
- **白盒几何判决**：融合 14 种骨骼拓扑特征与 ATHERPOSE 动态积分消抖，室内准确率 >90%，端到端延迟 <500ms。
- **自主硬件电源保障**：外部 12V 转 5V BUCK 电路独立供电，保障突发音频大功率输出时系统稳定。

---


## 📸 演示效果

<video src="./output.mp4" controls width="600"></video>

---

## 🏗 项目目录结构

```text
代码/
├── coco-2017-keypoints/      # 数据集处理与 YOLOv8-pose 模型训练 (PC 端)
│   ├── cocotoyolo.py          # COCO → YOLO 格式转换脚本
│   ├── yolo_pose_dataset/     # YOLO 格式数据集 (data.yaml, images/, labels/)
│   ├── runs/pose/train-10/    # 训练产出 (权重 best.pt, last.pt, 曲线 results.png)
│   └── README.md
├── kmodel_build/              # ONNX → K230 kmodel 模型转换工具
│   ├── to_kmodel.py           # nncase 量化编译脚本 (ptq_option=5)
│   ├── best.onnx              # 训练导出的 ONNX 模型
│   ├── best.kmodel            # 转换后的 K230 模型 (可直接部署)
│   ├── images/                # 校准图片目录 (需自行准备)
│   └── README.md
├── userapp/                   # K230 端侧应用代码
│   ├── py/                    # MicroPython 业务代码 (8 个 .py 文件)
│   ├── init_scripts/          # 自启动脚本 (init.sh)
│   ├── ubuntu/                # C/C++ 原生源码 (AI 进程 + C 扩展)
│   └── README.md
├── README.md                  # 项目总览 (本文件)
└── 参考文献
```

---

## 🚀 快速开始

### 阶段 1：模型准备 (PC 端)

1. **数据集准备**：进入 `coco-2017-keypoints/` 目录，参考其 README 完成 COCO 数据集下载与 YOLO 格式转换。
2. **模型训练**：运行 `yolo pose train` 命令训练 YOLOv8-pose 模型，产出 `best.pt` 权重文件。
3. **模型导出**：将 `best.pt` 导出为 ONNX 格式 (`best.onnx`)。
4. **模型量化**：进入 `kmodel_build/` 目录，运行 `to_kmodel.py --ptq_option 5` 将 ONNX 转换为 K230 专用的 `best.kmodel`。

### 阶段 2：C 扩展编译 (固件集成)

1. 将 `userapp/ubuntu/canmv/port/mymodules/` 下的 C 扩展源码集成到 CanMV SDK 的 MicroPython 移植目录。
2. 修改 `Makefile`，添加新模块编译选项。
3. 重新编译 CanMV 固件，生成包含自定义模块的镜像并烧录至 SD 卡。

阶段 3：端侧部署 (K230 设备端)
1. 文件传输：
   - userapp/py/*.py → /sdcard/
   - kmodel_build/best.kmodel → /data/
   - /data/alert/output.wav (音频文件需自行准备)
   - 编译好的 ai_pose 可执行文件 → /sdcard/app/

2. 自启动运行：
    main.py

3. 远程查看：通过 VLC 访问 rtsp://<K230_IP>:8554/test 查看实时推流。

---

## 📚 子模块文档导航

| 目录 | 说明 | 入口文档 |
| --- | --- | --- |
| `coco-2017-keypoints/` | COCO 姿态数据集转换与 YOLOv8-pose 训练 | [README](./coco-2017-keypoints/README.md) |
| `kmodel_build/` | ONNX 模型量化编译为 K230 kmodel | [README](./kmodel_build/README.md) |
| `userapp/` | K230 端侧 Python 业务代码 | [README](./userapp/README.md) |
| `userapp/ubuntu/` | C/C++ 原生应用与 MicroPython C 扩展源码 | [README](./userapp/ubuntu/README.md) |

---

## 📜 参考文献

[1] SEOYUNJE. Endoscope-Object-Detection: Gastroscopy & Colonoscopy Object Detection 1Stage, 2Stage Model with Detectron2, Yolo, EfficientDet[CP/OL]. (2025-04-01)[2026-06-19]. https://github.com/SEOYUNJE/Endoscope-Object-Detection.

[2] open-source-toolkit. 基于YOLOV8的姿态检测实现坐站立跌倒姿态的推理与评估: 基于YOLOV8的坐站立跌倒姿态检测与评估工具[CP/OL]. (2025)[2026-06-19]. https://gitcode.com/open-source-toolkit/8e01c.

[3] zhahoi. yolov8-pose-fall-detection: 使用YOLOv8-Pose进行人体关键点检测，通过计算人体各关键点关系进行人体摔倒检测（ncnn框架实现）[CP/OL]. (2024)[2026-06-19]. https://github.com/zhahoi/yolov8-pose-fall-detection.

[4] 嘉楠科技. CanMV K230文档[EB/OL]. (2024)[2026-06-19]. https://www.kendryte.com/zh/sdkResource/230canmv.

[5] RT-Thread. RT-Thread Smart 文档[EB/OL]. (2024)[2026-06-19]. https://www.rt-thread.org.

[6] EMQX Cloud. EMQX Cloud 文档[EB/OL]. (2024)[2026-06-19]. https://www.emqx.com/zh/cloud.

[7] 嘉楠科技. K230 YOLO 大作战[EB/OL]. (2024)[2026-06-19]. https://www.kendryte.com/k230_canmv/zh/main/example/ai/YOLO大作战.html.

---
