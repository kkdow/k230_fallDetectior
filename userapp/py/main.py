import _thread
import time
import network
import gc
import micropython  # 👈 核心导入：用于处理中断调度


# ========================================================
# 1. 抢先使用验证成功的【硬件 I2C3 (44/45)】初始化屏幕
# ========================================================
from machine import I2C, Pin
from ssd1306 import SSD1306_I2C

oled = None
i2c_lock = _thread.allocate_lock()  # 互斥锁：确保多线程并发时，刷屏不冲突

def oled_show(lines):
    """全局线程安全刷屏函数"""
    if oled is None:
        print(f"[OLED Log]: {lines}")
        return
    i2c_lock.acquire()
    try:
        oled.fill(0)
        oled.rect(0, 0, 128, 64, 1)
        for idx, text in enumerate(lines):
            oled.text(text, 5, 4 + idx * 14, 1)
        oled.show()
    except Exception as e:
        print(f"OLED Show Error: {e}")
    finally:
        i2c_lock.release()

print("[OLED] 正在根据验证成功的 K230 规范初始化硬件 I2C3...")
try:
    i2c = I2C(3, scl=44, sda=45, freq=400000)
    devices = i2c.scan()
    print("[OLED] I2C3 扫描成功！发现设备:", [hex(d) for d in devices])

    if 0x3C in devices:
        oled = SSD1306_I2C(128, 64, i2c, addr=0x3C)
        oled_show(["K230 System", "OLED: OK (I2C3)", "Connecting WiFi"])
    else:
        print("[OLED Alert] 未在 0x3C 发现屏幕，请检查物理线是否松动！")
except Exception as e:
    print(f"[OLED Init Error]: {e}")

# ========================================================
# 2. 屏幕安全引导后，导入多媒体与 AI 业务模块
# ========================================================
from media.media import *
from libs.PipeLine import PipeLine
from libs.WBCRtsp import WBCRtsp
from media.vencoder import *
from ai_fall_rtsp import FallDetectionApp

from mp4_record import mp4_thread
from audio_alert import audio_thread
import k230_alert  # 👈 导入编译内置的全新 C 扩展模块

# --- 全局配置 ---
SSID = "HONOR 100"
PASSWORD = "qwertyuiop"
CONFIG = {
    "kmodel": "/data/best.kmodel",
    "labels": ["fall", "head", "stand"],
    "rgb_size": [320, 320],
    "input_size": [320, 320],
    "conf": 0.45,
    "nms": 0.45
}

# ========================================================
# 3. 【核心修改】方案一：异步安全中断复位系统
# ========================================================
pin_d9 = None
pin_d10 = None
global_fall_start_ref = None  # 用于中断异步修改状态的全局指针引用

def safe_reset_execution(pin_num):
    """
    运行在安全上下文（非硬中断环境，堆内存未锁定）的复位具体执行函数。
    在这里允许任何动态内存分配，可以自由安全地进行 print、log_event、刷新 OLED。
    """
    global global_fall_start_ref
    if global_fall_start_ref and global_fall_start_ref[0]:
        print(f"[硬件中断复位] 异步队列接收响应：引脚(GPIO {pin_num})按下，正在清除报警锁...")
        k230_alert.log_event("USER_RESET")
        global_fall_start_ref[0] = False  # 清除跌倒报警状态状态锁

def hardware_reset_callback(pin):
    """
    D9/D10 硬件硬中断响应函数。
    【注意】：此时 CPU 处于中断上下文，Heap 处于锁定状态，严禁在此处执行 print()、f-string 拼接。
    """
    global global_fall_start_ref
    # 只有当前在报警锁定时，才把复位任务扔进主线程空闲队列，避免平时按键机械抖动浪费队列
    if global_fall_start_ref and global_fall_start_ref[0]:
        try:
            # 兼容判断是哪个引脚触发，无法获取对象字符串时默认传 0，不影响整体业务
            pin_num = 43 if "43" in str(pin) else 14
            # 使用 micropython.schedule 将复位任务安全投递，交给主线程空闲时立即安全执行
            micropython.schedule(safe_reset_execution, pin_num)
        except Exception:
            micropython.schedule(safe_reset_execution, 0)

def init_hardware_reset_interrupts(fall_start_list):
    """
    初始化 D9(GPIO43) 和 D10(GPIO14) 为开漏输入中断模式
    """
    global pin_d9, pin_d10, global_fall_start_ref
    global_fall_start_ref = fall_start_list

    print("[Reset Init] 正在初始化 D9/D10 硬件复位中断...")

    # 配置为输入模式 + 开启上拉电阻（模拟开漏环境，外部传感器接地时拉低电平产生下降沿）
    pin_d9 = Pin(43, Pin.IN, pull=Pin.PULL_UP)
    pin_d10 = Pin(14, Pin.IN, pull=Pin.PULL_UP)

    # 注册下降沿中断触发器，防抖时间设为 20ms
    pin_d9.irq(handler=hardware_reset_callback, trigger=Pin.IRQ_FALLING, debounce=20)
    pin_d10.irq(handler=hardware_reset_callback, trigger=Pin.IRQ_FALLING, debounce=20)
    print("[Reset Init] D9 (GPIO43) & D10 (GPIO14) 硬件中断安全绑定成功，原按键扫描线程已注销。")

# ========================================================
# 4. 网络与推流线程配置
# ========================================================
def connect_wifi(ssid, password):
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    wlan.connect(ssid, password)
    for i in range(15):
        if wlan.isconnected():
            return wlan.ifconfig()[0]
        time.sleep(1)
    return None

def rtsp_polling_thread():
    print("[RTSP] 独立推流线程启动")
    while True:
        WBCRtsp.poll_handler()
        time.sleep_ms(1)


# ========================================================
# 5. 主程序主流程
# ========================================================
def main():
    exit_flag = [False]
    encoder = None
    fall_start = [False]
    gc.enable()

    # 连接无线并获取 IP
    local_ip = connect_wifi(SSID, PASSWORD)
    if not local_ip:
        oled_show(["WiFi Error", "Connect Failed", "System Stop"])
        return

    # 屏幕即时更新推流流地址
    oled_show([
        "SYSTEM: READY",
        "rtsp://",
        f" {local_ip}",
        " :8554/text"
    ])
    print("[System] Activating native C module background daemon...")
    k230_alert.init()



    pl = PipeLine(rgb888p_size=CONFIG["rgb_size"], display_mode="nt35516")
    pl.create(hmirror=True, vflip=True, fps=20, to_ide=False)

    # 硬件链路绑定
    sensor_info = pl.sensor.bind_info(chn=CAM_CHN_ID_1)
    MediaManager.link(sensor_info['src'], (VIDEO_ENCODE_MOD_ID, VENC_DEV_ID, VENC_CHN_ID_1))
    MediaManager.init()
    pl.sensor.run()

    # 将 WBC 画面比例修正为标准的 16:9 比例，匹配 1080P 摄像头输入
    WBCRtsp.configure(wbc_width=640, wbc_height=360)
    WBCRtsp.start()

    # 配置常驻编码器
    width, height = pl.display_size[0], pl.display_size[1]
    width = ALIGN_UP((width + 63) // 64 * 64, 16)
    height = (height + 15) // 16 * 16
    venc_chn = VENC_CHN_ID_1

    encoder = Encoder()
    encoder.SetOutBufs(venc_chn, 12, width, height)
    chnAttr = ChnAttrStr(encoder.PAYLOAD_TYPE_H264, encoder.H264_PROFILE_BASELINE,
                         width, height, bit_rate=2048,src_frame_rate=20,dst_frame_rate=10, gopLen=50)
    encoder.Create(venc_chn, chnAttr)
    encoder.Start(venc_chn)

    # 初始化 AI 业务类
    # 初始化 AI 业务类
    app = FallDetectionApp(pl, rgb888p_size=CONFIG["rgb_size"])

    # 👈 就地激活基于底层中断的复位机制，彻底摆脱 fall_rest_thread
    init_hardware_reset_interrupts(fall_start)

    # 启动精简后的后台业务线程池（注意：这里已经删除了 fall_rest_thread 的新建项）
    _thread.stack_size(128 * 1024)

    _thread.start_new_thread(app.run_inference_loop, (fall_start,))
    _thread.start_new_thread(audio_thread, (fall_start,))
    _thread.start_new_thread(mp4_thread, (fall_start, pl, encoder, exit_flag))
    _thread.start_new_thread(rtsp_polling_thread, ())

    gc.collect()

    try:
        # 主循环：每 5 秒读取一次 AI 状态并刷新 OLED 屏幕看板
        while True:
            if fall_start[0]:
                status_str = "!! FALL DETECT !!"
            else:
                status_str = "Status: Monitor"

            oled_show([
                status_str,
                "rtsp://",
                f" {local_ip}",
                " :8554/text"
            ])
            time.sleep(15)  # 缩短至15秒，让屏幕对复位的响应更加灵敏
            gc.collect()
    except Exception as e:
        print(f"[Main Loop Error] {e}")
    finally:
        print("[Main] Cleaning up...")
        exit_flag[0] = True
        oled_show(["System Stop", "Cleaning up..."])
        time.sleep(1)

        if encoder:
            encoder.Stop(venc_chn)
            encoder.Destroy(venc_chn)
        app.deinit()
        WBCRtsp.stop()
        pl.destroy()

if __name__ == "__main__":
    main()
