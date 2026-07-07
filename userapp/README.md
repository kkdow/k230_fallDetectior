# K230 端侧应用代码 (userapp)

本目录包含 K230 设备端运行的全部 Python 业务代码与启动脚本，是系统的顶层控制与业务逻辑层。

- **业务代码**：`py/` 目录下 8 个 MicroPython 文件，涵盖主控、AI 推理、音频、录制、推流等完整功能。
- **自启动脚本**：`init_scripts/init.sh` 用于系统上电后自动拉起 AI 推理进程。
- **C/C++ 源码**：`ubuntu/` 目录包含底层 C 扩展与 AI 进程源码（详见其 README）。

---

## 📂 目录结构

```text
userapp/
├── py/                        # MicroPython 业务代码 (部署到 /sdcard/)
│   ├── main.py                # 主程序入口 (硬件初始化 + 线程调度)
│   ├── ai_fall_rtsp.py        # AI 推理循环 + OSD 渲染
│   ├── decision.py            # 白盒几何判决内核 (14 特征 + ATHERPOSE)
│   ├── audio_alert.py         # 常开音频播放线程
│   ├── mp4_record.py          # MP4 断点录制线程
│   ├── WBCRtsp.py             # RTSP 推流封装 (WBC + C扩展)
│   ├── PipeLine.py            # 视频流水线封装
│   └── ssd1306.py             # SSD1306 OLED 驱动
├── init_scripts/              # 自启动脚本
│   └── init.sh                # 上电拉起 /sdcard/app/ai_pose
└── ubuntu/                    # C/C++ 原生源码 (详见其 README)
```

---

## 🚀 部署与运行

### 1. 文件拷贝
将以下文件传输到 K230：

```bash
# Python 业务代码 → /sdcard/
userapp/py/*.py → /sdcard/

# AI 模型 → /data/
kmodel_build/best.kmodel → /data/

# 音频文件 → /data/alert/ (目录需自行创建)
/data/alert/output.wav
```

### 2. 自启动配置
将 `init_scripts/init.sh` 内容追加到系统启动脚本，确保 AI 进程随系统启动：

```bash
/bin/preload &
/sdcard/app/ai_pose &
```

### 3. 启动系统
```bash
cd /sdcard
python main.py
```

### 4. 远程查看
VLC 访问 `rtsp://<K230_IP>:8554/test` 查看实时推流。

---

## 📝 核心模块说明

| 模块 | 功能 | 关键特性 |
| --- | --- | --- |
| `main.py` | 系统主控 | I2C OLED (SCL=44, SDA=45) 初始化、WiFi 连接、按键中断 (GPIO43/14)、四线程调度、15s OLED 刷新 |
| `ai_fall_rtsp.py` | AI 推理循环 | 共享内存交互 (`ai_shm.get_result()`)、OSD 渲染、时序消抖 (`FALL_TRIGGER_FRAMES=2`) |
| `decision.py` | 判决内核 | 14 种几何特征 (关节角、宽高比、躯干倾角等) + ATHERPOSE 衰减积分 (0.98) |
| `audio_alert.py` | 音频报警 | 常开 PyAudio 流，触发时循环播放 `/data/alert/output.wav`，未触发时 `sleep_ms(20)` |
| `mp4_record.py` | 视频录制 | 跌倒触发后断点录制 MP4，SPS/PPS + I 帧合并写入，调用 `mp4_c_helper` C 扩展 |
| `WBCRtsp.py` | RTSP 推流 | WBC 回写 + C 扩展 `rtsmart_rtsp` 推送 H.264，`poll_handler` 轮询 |
| `PipeLine.py` | 视频流水线 | 多通道 Sensor 采集、Display 初始化 (NT35516)、OSD 图层管理 |
| `ssd1306.py` | OLED 驱动 | I2C3 (SCL=44, SDA=45)，128×64，线程安全刷屏 |

---

## 🔧 配置修改

### WiFi 配置 (`main.py`)
```python
SSID = "你的WiFi名称"
PASSWORD = "你的WiFi密码"
```

### FallConfig 参数 (`ai_fall_rtsp.py`)

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `FALL_TRIGGER_FRAMES` | `2` | 连续跌倒帧数 (时序确认) |
| `FALL_DAMPING_RATE` | `1` | 阻尼降阶系数 |
| `DEBUG_MODE` | `False` | 是否显示右侧白盒诊断看板 |
| `TARGET_FPS` | `30` | 目标帧率 (实际受 AI 推理限制约 20fps) |
| `GC_INTERVAL_FRAMES` | `3000` | 垃圾回收间隔帧数 |

---

## 📝 常见问题 (FAQ)

**Q: 运行 `main.py` 报错 `ImportError: no module named 'ai_shm'`？**
A: C 扩展未编译进固件。需将 `ubuntu/canmv/port/mymodules/` 集成到 CanMV SDK 并重新编译烧录。

**Q: OLED 屏幕不显示？**
A: 检查 I2C3 (SCL=44, SDA=45) 接线，运行以下命令确认 0x3C 设备存在：
```python
from machine import I2C, Pin
i2c = I2C(3, scl=44, sda=45)
print(i2c.scan())  # 应输出 [60] (0x3C)
```

**Q: RTSP 推流无画面？**
A: 检查 WiFi 连接，确认 `rtsmart_rtsp.rtsp_start()` 返回 True。VLC 访问 `rtsp://<IP>:8554/test`。

**Q: 报警触发后 MP4 文件未生成？**
A: 检查 `/data/mp4/` 目录是否存在，系统会自动创建。确保 SD 卡有足够存储空间。

**Q: 按键复位无效？**
A: 确认 GPIO43 (D9) 和 GPIO14 (D10) 接线正确，按键按下时电平从高到低变化。查看串口日志确认中断是否触发。

**Q: 运行时提示内存不足？**
A: 调整 `GC_INTERVAL_FRAMES` 参数 (减小到 1500) 或减少线程栈大小 (`_thread.stack_size(64 * 1024)`)。

---

## 📄 文件清单 (py/)

| 文件名 | 大小 | 说明 |
| --- | --- | --- |
| `main.py` | 9,035 | 主程序入口 |
| `ai_fall_rtsp.py` | 13,228 | AI 推理循环 |
| `decision.py` | 9,697 | 白盒判决内核 |
| `audio_alert.py` | 2,373 | 音频播放 |
| `mp4_record.py` | 5,889 | MP4 录制 |
| `WBCRtsp.py` | 4,874 | RTSP 推流 |
| `PipeLine.py` | 6,963 | 视频流水线 |
| `ssd1306.py` | 3,246 | OLED 驱动 |

---

## 📜 参考文献

[1] 嘉楠科技. CanMV K230文档[EB/OL]. (2024)[2026-06-19]. https://www.kendryte.com/zh/sdkResource/230canmv.

[2] RT-Thread. RT-Thread Smart MicroPython 文档[EB/OL]. (2024)[2026-06-19]. https://www.rt-thread.org.