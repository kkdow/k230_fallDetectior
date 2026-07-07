# audio_alert.py
import time
import gc
from media.pyaudio import *
import media.wave as wave
import k230_alert

def audio_thread(fall_start):
    audio_file = '/data/alert/output.wav'
    has_logged_audio = False  

    print("[音频线程] 正在初始化常开音频硬件...")
    try:
        # === 💡 【常开核心】在死循环外一次性打开文件和音频流 ===
        wf = wave.open(audio_file, 'rb')
        CHUNK = int(wf.get_framerate() / 25)
        p = PyAudio()

        # 保持音频流通道常开
        stream = p.open(format=p.get_format_from_width(wf.get_sampwidth()),
                        channels=wf.get_channels(),
                        rate=wf.get_framerate(),
                        output=True,
                        frames_per_buffer=CHUNK)
        stream.volume(vol=95)
        print("[音频线程] 音频通道已进入常开常驻状态，等待触发...")

        while True:
            if fall_start[0]:
                # 如果触发了跌倒，进入播放状态
                if not has_logged_audio:
                    k230_alert.log_event("AUDIO_START")
                    has_logged_audio = True

                # 指针重置到音频开头
                wf.rewind()
                data = wf.read_frames(CHUNK)
                
                # 循环播放，直到被外部复位
                while data and fall_start[0]:
                    stream.write(data)
                    data = wf.read_frames(CHUNK)
                    time.sleep_ms(2) # 释放 CPU 总线给其他高并发线程（如 MP4 和 RTSP）
            else:
                # === 💡 【常开核心】未触发时，不销毁硬件，只是让线程静默休眠 ===
                if has_logged_audio:
                    has_logged_audio = False
                    print("[音频线程] 警报解除，音频进入静默待机状态。")
                
                time.sleep_ms(20) # 适当降低未触发时的轮询开销

    except Exception as e:
        print(f"[音频线程异常] {e}")
        # 如果中间发生异常，安全释放一次，防止通道死锁
        try:
            stream.stop_stream()
            stream.close()
            p.terminate()
            wf.close()
        except:
            pass
        time.sleep(2)