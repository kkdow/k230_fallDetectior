import time
import os
import multimedia as mm
from mpp import *
from media.vencoder import *
from _media import Display

# 尝试导入高性能 C 扩展推流模块
try:
    import rtsmart_rtsp
except ImportError:
    rtsmart_rtsp = None
    print("[WBCRtsp] Warning: rtsmart_rtsp module not found!")

class RtspServer:
    def __init__(self, session_name="test", port=8554, width=1280, height=720):
        self.session_name = session_name
        self.port = port
        self.venc_chn = VENC_CHN_ID_0
        self.width = ALIGN_UP(width, 16)
        self.height = height
        self.encoder = Encoder()
        # 预留足够的输出缓冲区
        self.encoder.SetOutBufs(self.venc_chn, 20, self.width, self.height)

    def start(self):
        # 适配 v1.5-legacy 的 ChnAttrStr 接口
        chnAttr = ChnAttrStr(self.encoder.PAYLOAD_TYPE_H264, self.encoder.H264_PROFILE_MAIN, 
                             self.width, self.height, bit_rate=1536)
        self.encoder.Create(self.venc_chn, chnAttr)
        self.encoder.Start(self.venc_chn)

    def stop(self):
        self.encoder.Stop(self.venc_chn)
        self.encoder.Destroy(self.venc_chn)

class WBCRtsp:
    rtspserver = None
    _running = False

    @classmethod
    def configure(cls, wbc_width=960, wbc_height=536, port=8554, session_name="test"):
        cls.rtspserver = RtspServer(
            session_name=session_name,
            port=port,
            width=wbc_width,
            height=wbc_height
        )
        if rtsmart_rtsp:
            rtsmart_rtsp.init()
        
    @classmethod
    def poll_handler(cls):
        if not cls._running or cls.rtspserver is None:
            return

        # 1. 抓取屏幕或显示链路的写回帧 (WBC)
        vf = Display.writeback_dump(100)
        if not vf:
            return

        try:
            # 2. 异步送入硬件编码器
            if cls.rtspserver.encoder.SendFrame(cls.rtspserver.venc_chn, vf) == 0:
                s_data = StreamData()
                # 3. 获取码流（此处 200ms 超时对于 1080P/720P 至关重要）
                if cls.rtspserver.encoder.GetStream(cls.rtspserver.venc_chn, s_data, 200) == 0:
                    try:
                        if rtsmart_rtsp:

                            # 提取元数据：st_type 2=IDR, 3=SPS/PPS
                            st_type = s_data.stream_type[0]
                            is_key = (st_type == 2 or st_type == 3)
                            
                            # 规范化 PTS：处理固件中的 -1 (0xFFFFFF...) 情况
                            raw_pts = s_data.pts[0]
                            pts_val = 0 if raw_pts > 0x7FFFFFFFFFFFFFFF else raw_pts

                            # 4. 遍历所有 NALU 包并推流
                            for i in range(s_data.pack_cnt):
                                addr = s_data.data[i]
                                size = s_data.data_size[i]
                                
                                if addr > 0 and size > 0:
                                    # 物理地址 -> Python Bytes 视图
                                    # 注意：如果画面出现撕裂，请改为 bytes(uctypes.bytes_at(addr, size))
                                    rtsmart_rtsp.push_packet(uctypes.bytes_at(addr, size), pts_val, is_key)
                                    
                    finally:
                        # 5. 释放码流句柄（这一步如果不做，编码器很快会挂起）
                        cls.rtspserver.encoder.ReleaseStream(cls.rtspserver.venc_chn, s_data)
        
        except Exception as e:
            # 这里的 print 在正式部署时可以关掉
            print(f"[WBC] Flow Error: {e}")
        finally:
            # 6. 释放 WBC 帧，确保硬件 VB 缓冲区可以被循环使用
            if vf:
                try:
                    Display.writeback_release(vf)
                except:
                    pass



    @classmethod
    def start(cls):
        if not cls._running and cls.rtspserver:
            if not Display.writeback(True):
                print("[WBCRtsp] Error: WBC enable failed")
                return

            cls.rtspserver.start()
            if rtsmart_rtsp:
                rtsmart_rtsp.rtsp_start(cls.rtspserver.port, cls.rtspserver.session_name)

            cls._running = True
            print(f"[WBCRtsp] Native Polling Mode Active")

    @classmethod
    def stop(cls):
        if not cls._running:
            return
        cls._running = False
        if rtsmart_rtsp:
            rtsmart_rtsp.rtsp_stop()
            rtsmart_rtsp.deinit()
        if cls.rtspserver:
            cls.rtspserver.stop()
        Display.writeback(False)