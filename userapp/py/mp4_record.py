from mpp.mp4_format import *
from mpp.mp4_format_struct import *
from media.vencoder import *
from media.sensor import *
from media.media import *
import uctypes
import time
import os
import gc
import k230_alert
import mp4_c_helper

# --- MP4 容器辅助函数（保持你验证成功的逻辑） ---
def mp4_muxer_init(file_name):
    """初始化 MP4 复用器"""
    mp4_cfg = k_mp4_config_s()
    mp4_cfg.config_type = K_MP4_CONFIG_MUXER
    mp4_cfg.muxer_config.file_name[:] = bytes(file_name, 'utf-8')
    mp4_cfg.muxer_config.fmp4_flag = True  # 使用标准 MP4 格式或者fmp4，兼容性最强
    mp4_cfg.muxer_config.moov_at_flag = True  # 立即写入 moov atom，确保文件完整性和兼容性
    handle = k_u64_ptr()
    if kd_mp4_create(handle, mp4_cfg):
        raise OSError(f"MP4 容器创建失败: {file_name}")
    return handle.value

def mp4_muxer_create_video_track(mp4_handle, width, height):
    """创建 H.264 视频轨道"""
    video_track_info = k_mp4_track_info_s()
    video_track_info.track_type = K_MP4_STREAM_VIDEO
    video_track_info.time_scale = 1000  # 毫秒级时间轴
    video_track_info.video_info.width = width
    video_track_info.video_info.height = height
    video_track_info.video_info.codec_id = K_MP4_CODEC_ID_H264
    video_track_handle = k_u64_ptr()
    if kd_mp4_create_track(mp4_handle, video_track_handle, video_track_info):
        raise OSError("MP4 轨道创建失败")
    return video_track_handle.value

# --- 核心录制线程 ---
def mp4_thread(fall_start, pl, encoder, exit_flag):
    """
    fall_start: 共享列表 [bool], 控制录制启停
    pl:         PipeLine 实例，用于获取分辨率
    encoder:    主线程中常驻运行的 Encoder 实例
    exit_flag:  控制线程安全退出的标志
    """
    venc_chn = VENC_CHN_ID_1
    # 自动获取分辨率并进行硬件对齐
    width = ALIGN_UP(pl.display_size[0], 16)
    height = ALIGN_UP(pl.display_size[1], 16)

    # 状态管理
    is_recording = False
    mp4_handle = None
    mp4_video_track = None

    # 预分配缓冲区，降低循环内的内存开销
    streamData = StreamData()

    # 预留 IDR 帧合并空间（防止 SPS/PPS/I 帧拼接时溢出）
    save_idr = bytearray(width * height // 2)

    video_start_pts = 0
    get_first_I_frame = False

    print("[录制线程] 启动成功，监控编码流中...")
    #clock=time.clock()

    while not exit_flag[0]:
        try:
            # 必须持续 GetStream，否则硬件 VENC 缓冲区填满会导致画面卡死
            #clock.tick()
            if encoder.GetStream(venc_chn, streamData, timeout=200) == 0:
                if streamData.pack_cnt == 0:
                    encoder.ReleaseStream(venc_chn, streamData)
                    continue

                st = streamData.stream_type[0]
                sz = streamData.data_size[0]
                pts = streamData.pts[0]
                addr = streamData.data[0]

                # --- 1. 触发逻辑：开始录制 ---
                if fall_start[0] and not is_recording:
                    t = time.localtime()
                    file_path = "/data/mp4"
                    if 'mp4' not in os.listdir('/data'): os.mkdir(file_path)

                    file_name = f"{file_path}/fall_{t[0]:04d}{t[1]:02d}{t[2]:02d}_{t[3]:02d}{t[4]:02d}{t[5]:02d}.mp4"

                    try:
                        mp4_handle = mp4_muxer_init(file_name)
                        mp4_video_track = mp4_muxer_create_video_track(mp4_handle, width, height)

                        is_recording = True
                        get_first_I_frame = False
                        video_start_pts = 0
                        idr_index = 0
                        print(f"[录制线程] >>> 开始写入文件: {file_name}")
                        k230_alert.log_event("RECORD_START")
                    except Exception as e:
                        print(f"[录制线程] 容器初始化失败: {e}")

                # --- 2. 写入逻辑：数据同步 ---
                if is_recording:
                    if not get_first_I_frame:
                        # A. 寻找并缓存 Header (SPS/PPS) 和第一个 I 帧
                        if st == encoder.STREAM_TYPE_HEADER:
                            save_idr[idr_index : idr_index+sz] = uctypes.bytearray_at(addr, sz)
                            idr_index += sz
                        elif st == encoder.STREAM_TYPE_I:
                            get_first_I_frame = True
                            video_start_pts = pts # 将第一个 I 帧的 PTS 设为 0 基准

                            save_idr[idr_index : idr_index+sz] = uctypes.bytearray_at(addr, sz)
                            idr_index += sz

                            mp4_c_helper.write_frame(mp4_handle, mp4_video_track, save_idr, idr_index, 0)
                    else:
                        # B. 正常写入后续 P/I 帧
                        frame_bytes = uctypes.bytearray_at(addr, sz)
                        timestamp_ms = (pts - video_start_pts)
                        mp4_c_helper.write_frame(mp4_handle, mp4_video_track, frame_bytes, sz, timestamp_ms)
                        os.exitpoint()


                # --- 3. 触发逻辑：停止录制 ---
                if not fall_start[0] and is_recording:
                    kd_mp4_destroy_tracks(mp4_handle)
                    kd_mp4_destroy(mp4_handle)
                    is_recording = False
                    print("[录制线程] <<< 录制停止，文件已安全关闭")
                    gc.collect()

                # 必须释放码流句柄
                encoder.ReleaseStream(venc_chn, streamData)
                #print("fps:{}".format(clock.fps()))
        except Exception as e:
            print(f"[录制线程异常] {e}")
            time.sleep_ms(50)
