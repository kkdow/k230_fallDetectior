import cv2
import numpy as np
import math
from ultralytics import YOLO

# --- 配置 ---
MODEL_PATH = "best.pt"
CONF_THRESH = 0.45

# 加载 YOLOv8-Pose 模型
model = YOLO(MODEL_PATH)
cap = cv2.VideoCapture(0)

history = []
is_fallen = False

print(">>> 启动 PyTorch 姿态识别测试，按 'q' 退出...")

while cap.isOpened():
    ret, frame = cap.read()
    if not ret: break

    # 1. 运行姿态推理
    results = model(frame, verbose=False)[0]
    
    cur_fall = False

    # 检查是否有检测到人
    if results.boxes is not None and len(results.boxes) > 0:
        boxes = results.boxes.xyxy.cpu().numpy()
        confs = results.boxes.conf.cpu().numpy()
        
        # 获取关键点数据形状通常为: [检测到的人数, 17, 3] (x, y, visible_conf)
        if results.keypoints is not None:
            keypoints = results.keypoints.data.cpu().numpy()

            for idx, kpts in enumerate(keypoints):
                if confs[idx] < CONF_THRESH: continue
                
                # COCO 关键点索引: 0: 鼻子, 5: 左肩, 6: 右肩, 11: 左臀, 12: 右臀
                # 我们可以通过计算躯干（肩膀中点 到 臀部中点）的倾斜角度来判断是否跌倒
                try:
                    # 鼻子和左右肩膀
                    nose = kpts[0]
                    l_shoulder, r_shoulder = kpts[5], kpts[6]
                    l_hip, r_hip = kpts[11], kpts[12]

                    # 只要这些核心骨架点的置信度大于 0.5 才进行计算
                    if l_shoulder[2] > 0.5 and r_shoulder[2] > 0.5 and l_hip[2] > 0.5 and r_hip[2] > 0.5:
                        # 计算上半身中心和下半身中心
                        shoulder_center_x = (l_shoulder[0] + r_shoulder[0]) / 2
                        shoulder_center_y = (l_shoulder[1] + r_shoulder[1]) / 2
                        hip_center_x = (l_hip[0] + r_hip[0]) / 2
                        hip_center_y = (l_hip[1] + r_hip[1]) / 2

                        # 计算躯干向量
                        dx = shoulder_center_x - hip_center_x
                        dy = shoulder_center_y - hip_center_y
                        
                        # 计算与水平线的夹角 (角度在 0~90 之间，越接近 0 越倾向于躺下)
                        angle = math.degrees(math.atan2(abs(dy), abs(dx)))
                        
                        # 策略 A：如果人体躯干夹角小于 35 度，判定为躺下/跌倒
                        if angle < 35:
                            cur_fall = True

                    # 策略 B：传统宽高比兜底
                    x1, y1, x2, y2 = boxes[idx]
                    fw, fh = x2 - x1, y2 - y1
                    if fw / (fh + 1e-5) > 1.35:
                        cur_fall = True

                except Exception as e:
                    pass

    # 2. 滑动窗口滤波
    history.append(cur_fall)
    if len(history) > 10: history.pop(0)

    if history.count(True) >= 4:
        if not is_fallen: print("[ALERT] Pose 检测：跌倒！"); is_fallen = True
    else:
        if is_fallen: print("[INFO] Pose：恢复正常"); is_fallen = False

    # 3. 画面渲染
    status_text = "FALLEN!" if is_fallen else "NORMAL"
    status_color = (0, 0, 255) if is_fallen else (0, 255, 0)
    cv2.putText(frame, status_text, (20, 50), cv2.FONT_HERSHEY_SIMPLEX, 1.2, status_color, 3)

    # 借用官方的 plot() 方法直接画出漂亮的骨骼线，省去手动绘制逻辑
    annotated_frame = results.plot()
    
    # 将状态字写回带有骨骼线的画面上
    cv2.putText(annotated_frame, status_text, (20, 50), cv2.FONT_HERSHEY_SIMPLEX, 1.2, status_color, 3)

    cv2.imshow("PyTorch Pose Test", annotated_frame)
    if cv2.waitKey(1) & 0xFF == ord('q'): break

cap.release()
cv2.destroyAllWindows()