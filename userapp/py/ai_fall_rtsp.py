# ai_fall_rtsp.py
import gc
import os
import time
import math
import ai_shm
import k230_alert  # 编译内置的全新 C 扩展模块
from decision import Decision  # 核心导入：接入解耦后的高级白盒博弈模型

class FallConfig:
    """
    ================================================================
    跌倒检测业务层配置中心 (全面对接 Decision 白盒内核)
    ================================================================
    """
    # 1. 时序消抖与阻尼系统参数 (仅保留时序确认，防止误触发)
    FALL_TRIGGER_FRAMES = 2      # 触发硬件报警所需的连续有效跌倒帧数 (时序确认机制)
    FALL_DAMPING_RATE = 1        # 阻尼降阶系数。当姿态恢复直立时，计数器扣减的步长

    # 2. 系统控制与性能调优
    GC_INTERVAL_FRAMES = 3000    # 每隔多少帧进行一次内存检查
    GC_INTERVAL_MS = 5000        # 两次强制 gc.collect() 之间的最小时间间隔(毫秒)
    TARGET_FPS = 30              # 期望锁定的帧率上限

    # 3. 调试看板开关
    DEBUG_MODE = False            # 强行锁死为 True：全量渲染右侧白盒拓扑看板


class FallDetectionApp:
    def __init__(self, pl, rgb888p_size=[320, 320]):
        self.pl = pl
        self.cfg = FallConfig()

        # 1. 初始化共享内存
        try:
            print("[AI] Initializing Shared Memory Communication...")
            ai_shm.init()
        except Exception as e:
            print(f"[AI] SHM Init Error: {e}")

        self.display_size = pl.get_display_size()

        # 2. 实例化底层的白盒化几何博弈决策内核
        self.fall_detector = Decision()

        # 3. 状态机与性能变量
        self.fall_frame_counter = 0
        self.last_gc_time = time.ticks_ms()
        self.is_running = False

        # 4. 动态坐标映射 (320x320 -> 实际显示分辨率)
        display_size = pl.get_display_size()
        self.scale_w = display_size[0] / rgb888p_size[0]
        self.scale_h = display_size[1] / rgb888p_size[1]

        # 5. COCO 17 骨骼拓扑连线索引与颜色配置
        self.SKELETON_LINES = [
            (15, 13), (13, 11), (16, 14), (14, 12), (11, 12), (5, 11),
            (6, 12), (5, 6), (5, 7), (6, 8), (7, 9), (8, 10),
            (1, 2), (0, 1), (0, 2), (1, 3), (2, 4), (3, 5), (4, 6)
        ]
        self.COLOR_NORMAL = (255, 0, 255, 0)       # 正常/直立：绿色
        self.COLOR_FALL = (255, 255, 0, 0)         # 单帧/最终跌倒：红橙色
        self.COLOR_TEXT = (255, 255, 255, 255)     # 主文字：白色
        self.COLOR_PARAM = (255, 255, 255, 0)      # 参数区：黄色
        self.COLOR_LINE = (255, 0, 100, 255)       # 骨骼连线颜色

    def run_inference_loop(self, fall_start):
        print(f"[AI] Decision内核级状态机全面启动！右侧白盒看板: {self.cfg.DEBUG_MODE}")
        self.is_running = True
        frame_cnt = 0
        frame_target_ms = int(1000 / self.cfg.TARGET_FPS)
        clock=time.clock()

        while self.is_running:
            clock.tick()
            t_start = time.ticks_ms()
            try:
                try:
                    os.exitpoint()
                    time.sleep_ms(0)
                except AttributeError:
                    pass

                img = self.pl.get_frame()
                if img is None:
                    time.sleep_ms(10)
                    continue

                frame_cnt += 1

                # 动态转换字节流并进行共享内存交互
                try:
                    raw_bytes = img.to_bytearray() if hasattr(img, "to_bytearray") else memoryview(img)
                except:
                    raw_bytes = bytearray(img)

                ai_shm.send_frame(frame_cnt, raw_bytes)
                result = ai_shm.get_result()

                # 每帧清空 OSD，开始全新一轮渲染
                self.pl.osd_img.clear()
                trick_color = (1, 255, 255, 255) if (frame_cnt % 2 == 0) else (2, 255, 255, 255)
                self.pl.osd_img.draw_circle(0, 0, 1, color=trick_color, fill=True)
                time.sleep_ms(0)
                
                # 👈 【核心修改】彻底删去左上角 CONDITIONS 静态显示，保持左侧干净
                this_frame_fall = False

                if result:
                    res_frame_id, person_list = result

                    if res_frame_id > 0 and person_list:
                        for idx, person in enumerate(person_list):
                            # 全面切断旧判定，改由决策模型进行单帧博弈白盒计算
                            decision_label, actual = self.fall_detector.is_fallen(person)

                            if decision_label == "FALLEN":
                                this_frame_fall = True

                            # 根据单帧博弈标签自适应确定框与文字的渲染色彩
                            color = self.COLOR_FALL if decision_label == "FALLEN" else self.COLOR_NORMAL
                            label = f"[{decision_label}] Conf:{person['score']:.2f}"

                            # 2. 渲染人体边界框与头部置信度标签
                            x1 = int(person['x1'] * self.scale_w)
                            y1 = int(person['y1'] * self.scale_h)
                            x2 = int(person['x2'] * self.scale_w)
                            y2 = int(person['y2'] * self.scale_h)

                            self.pl.osd_img.draw_rectangle(x1, y1, x2 - x1, y2 - y1, color=color, thickness=4)
                            self.pl.osd_img.draw_string_advanced(x1 + 4, y1 - 22 if y1 - 22 > 0 else y1 + 5, 18, label, self.COLOR_TEXT)

                            # 3. 映射并绘制关键点与骨骼拓扑
                            mapped_kps = []
                            for m in range(17):
                                pt_x, pt_y, pt_s = person['kps'][m][0], person['kps'][m][1], person['kps'][m][2]
                                if pt_s > 0.4:
                                    px, py = int(pt_x * self.scale_w), int(pt_y * self.scale_h)
                                    self.pl.osd_img.draw_circle(px, py, 4, color, fill=True)
                                    mapped_kps.append((px, py, True))
                                else:
                                    mapped_kps.append((0, 0, False))

                            for start_pt, end_pt in self.SKELETON_LINES:
                                s_kp = mapped_kps[start_pt]
                                e_kp = mapped_kps[end_pt]
                                if s_kp[2] and e_kp[2]:
                                    self.pl.osd_img.draw_line(s_kp[0], s_kp[1], e_kp[0], e_kp[1], self.COLOR_LINE, thickness=3)

                            # 4. ================= 右侧高级白盒诊断区 (彻底移除了左侧参数栏) =================
                            if idx == 0 and self.cfg.DEBUG_MODE:
                                dx = 620
                                self.pl.osd_img.draw_string_advanced(dx, 10, 18, "DEBUG DIAGNOSTICS PANEL", (255, 0, 255, 255))
                                
                                # 双端博弈实时得分线显示
                                score_color = self.COLOR_FALL if decision_label == "FALLEN" else self.COLOR_NORMAL
                                scores_text = "SCORES -> FALLEN: {:.1f} | STANDING: {:.1f}".format(actual["score_fallen"], actual["score_standing"])
                                self.pl.osd_img.draw_string_advanced(dx, 35, 16, scores_text, score_color)
                                
                                self.pl.osd_img.draw_string_advanced(dx, 60, 16, "--- 12+ Topo Intermediate Values ---", self.COLOR_TEXT)
                                
                                def fmt_ang(val): return "{:.1f} deg".format(val) if val >= 0 else "Loss"
                                
                                # 纵向全量绘制右侧 12+ 种解耦出的独立几何拓扑核心特征值
                                self.pl.osd_img.draw_string_advanced(dx, 85,  14, "T1 L-KneeHipAnkle: {}".format(fmt_ang(actual["ang_l_kha"])), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 105, 14, "T2 R-KneeHipAnkle: {}".format(fmt_ang(actual["ang_r_kha"])), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 125, 14, "T3 L-ShldHipKnee:  {}".format(fmt_ang(actual["ang_l_shk"])), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 145, 14, "T4 R-ShldHipKnee:  {}".format(fmt_ang(actual["ang_r_shk"])), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 165, 14, "T5 L/R Thigh-Horiz: {} | {}".format(fmt_ang(actual["ang_l_kh_hz"]), fmt_ang(actual["ang_r_kh_hz"])), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 185, 14, "T6 L/R Leg-Horiz:   {} | {}".format(fmt_ang(actual["ang_l_ha_hz"]), fmt_ang(actual["ang_r_ha_hz"])), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 205, 14, "T7 Box AspectRatio: {:.2f}".format(actual["ratio"]), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 225, 14, "T8 Torso AxisAngle: {:.1f} deg".format(actual["angle"]), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 245, 14, "T9 Head-Hip V-Diff: {:.1f} px".format(actual["v_diff_head_hip"]), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 265, 14, "T10 Shld H-Span:    {:.1f} px".format(actual["h_diff_shoulders"]), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 285, 14, "T11 Hips V-Asym:    {:.1f} px".format(actual["v_diff_hips"]), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 305, 14, "T12 Knees H-Span:   {:.1f} px".format(actual["h_diff_knees"]), self.COLOR_TEXT)
                                self.pl.osd_img.draw_string_advanced(dx, 325, 14, "Global ATHERPOSE:   {:.2f}".format(actual["ather"]), self.COLOR_PARAM)
                                self.pl.osd_img.draw_string_advanced(dx, 350, 14, "fps:{}".format(clock.fps()))
                                print("fps:{}".format(clock.fps()))

                # D. 报警时序消抖状态机核心控制 (保留多帧滑动确认阻尼系统)
                if not fall_start[0]:
                    if this_frame_fall:
                        self.fall_frame_counter += 1
                        if self.fall_frame_counter >= self.cfg.FALL_TRIGGER_FRAMES:
                            # 触发 C 拓展硬件驱动、蜂鸣器中断打桩系统
                            fall_start[0] = True
                            
                            k230_alert.log_event("FALL_DETECTED")
                            
                            k230_alert.trigger()
                            
                            print("[ALARM] 核心模型时序确认完成，锁定跌倒。等待 GPIO 物理引脚复位...")
                    else:
                        # 正常帧出现时，按照定义的阻尼降阶系数扣减
                        self.fall_frame_counter = max(0, self.fall_frame_counter - self.cfg.FALL_DAMPING_RATE)
                else:
                    self.fall_frame_counter = 0

                # 👈【核心修改】报警发生时，在屏幕左上角(10, 10)以超大字号(48)渲染 FALL_DETECTED
                if fall_start[0]:
                    self.pl.osd_img.draw_string_advanced(10, 10, 48, "FALL_DETECTED", color=self.COLOR_FALL)

                # 渲染右上方时序计数器实时雷达
                if self.cfg.DEBUG_MODE:
                    self.pl.osd_img.draw_string_advanced(700, 360, 20, f"[Counter] Frame: {self.fall_frame_counter}/{self.cfg.FALL_TRIGGER_FRAMES}", color=(0, 255, 0))

                self.pl.show_image()

                # F. 平滑周期性垃圾回收
                if frame_cnt % self.cfg.GC_INTERVAL_FRAMES == 0:
                    now = time.ticks_ms()
                    if time.ticks_diff(now, self.last_gc_time) > self.cfg.GC_INTERVAL_MS:
                        gc.collect()
                        self.last_gc_time = now

                # G. 硬件时间片平滑让出机制
                elapsed = time.ticks_diff(time.ticks_ms(), t_start)
                if elapsed < frame_target_ms:
                    time.sleep_ms(frame_target_ms - elapsed)

            except Exception as e:
                print(f"[AI线程异常] {e}")
                time.sleep_ms(100)

    def deinit(self):
        self.is_running = False
        time.sleep_ms(200)
        try:
            ai_shm.deinit()
        except:
            pass
        gc.collect()
        print("[AI] 业务资源安全卸载完毕。")