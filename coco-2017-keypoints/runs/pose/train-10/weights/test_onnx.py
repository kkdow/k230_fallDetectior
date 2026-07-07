import cv2
import numpy as np
import math
import onnxruntime as ort

# --- 配置 ---
ONNX_MODEL_PATH = "best.onnx"  # 导出的 ONNX 模型文件
CONF_THRESH = 0.45
IOU_THRESH = 0.45
INPUT_SIZE = 640  # ONNX 模型的输入尺寸

class YOLOv8PoseONNX:
    def __init__(self, model_path, conf_thres=0.45, iou_thres=0.45):
        self.conf_thres = conf_thres
        self.iou_thres = iou_thres
        self.input_size = INPUT_SIZE
        
        # 加载 ONNX Runtime 会话
        self.session = ort.InferenceSession(model_path, providers=['CPUExecutionProvider'])
        self.input_name = self.session.get_inputs()[0].name
        
        # YOLOv8-Pose ONNX 输出格式: [1, 56, 8400] (4 bbox + 1 cls + 17*3 kpts)
        self.num_kpts = 17
        self.kpt_dim = 3 # (x, y, conf)

    def _preprocess(self, image):
        """预处理：Letterbox 缩放、归一化、维度转换 (NHWC -> NCHW)"""
        h, w = image.shape[:2]
        scale = min(self.input_size / h, self.input_size / w)
        new_h, new_w = int(h * scale), int(w * scale)
        
        # 1. Resize
        resized = cv2.resize(image, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
        
        # 2. Letterbox padding (将图像填充到正方形)
        pad_h = self.input_size - new_h
        pad_w = self.input_size - new_w
        top, bottom = pad_h // 2, pad_h - pad_h // 2
        left, right = pad_w // 2, pad_w - pad_w // 2
        padded = cv2.copyMakeBorder(resized, top, bottom, left, right, cv2.BORDER_CONSTANT, value=(114, 114, 114))
        
        # 3. 归一化并转换为 NCHW
        blob = np.ascontiguousarray(padded.transpose((2, 0, 1))[::-1]) # BGR -> RGB
        blob = blob.astype(np.float32) / 255.0
        blob = np.expand_dims(blob, axis=0) # [1, 3, 640, 640]
        
        return blob, scale, left, top

    def _postprocess(self, outputs, orig_shape, scale, left, top):
        """后处理：提取 BBox、Keypoints 并执行 NMS"""
        # outputs shape: (1, 56, 8400) -> transpose to (1, 8400, 56)
        outputs = np.transpose(np.squeeze(outputs[0]), (1, 0)) # [8400, 56]
        
        rows = outputs.shape[0]
        boxes, confs, kpts = [], [], []
        
        # 1. 过滤低置信度
        for i in range(rows):
            row = outputs[i]
            conf = row[4] # 目标置信度 (YOLOv8只有一个类别)
            if conf < self.conf_thres:
                continue
            
            # bbox (cx, cy, w, h)
            cx, cy, w, h = row[0], row[1], row[2], row[3]
            x1 = (cx - w / 2 - left) / scale
            y1 = (cy - h / 2 - top) / scale
            x2 = (cx + w / 2 - left) / scale
            y2 = (cy + h / 2 - top) / scale
            
            # 保证坐标在图片范围内
            x1 = max(0, min(x1, orig_shape[1]))
            y1 = max(0, min(y1, orig_shape[0]))
            x2 = max(0, min(x2, orig_shape[1]))
            y2 = max(0, min(y2, orig_shape[0]))
            
            if x2 <= x1 or y2 <= y1: continue
            
            boxes.append([x1, y1, x2, y2])
            confs.append(float(conf))
            
            # 关键点处理 (共17个点，每个点 3 个值: x, y, conf)
            kpt_start = 5
            cur_kpts = []
            for k in range(self.num_kpts):
                kx = (row[kpt_start + k * 3] - left) / scale
                ky = (row[kpt_start + k * 3 + 1] - top) / scale
                kconf = row[kpt_start + k * 3 + 2]
                cur_kpts.append([kx, ky, kconf])
            kpts.append(np.array(cur_kpts))

        if not boxes:
            return [], []

        # 2. NMS (非极大值抑制)
        boxes_np = np.array(boxes).astype(np.float32)
        indices = cv2.dnn.NMSBoxes(boxes_np.tolist(), confs, self.conf_thres, self.iou_thres)
        
        final_boxes, final_kpts = [], []
        if len(indices) > 0:
            for i in indices.flatten():
                final_boxes.append(boxes_np[i])
                final_kpts.append(kpts[i])
                
        return final_boxes, final_kpts

    def infer(self, image):
        """执行推理"""
        orig_shape = image.shape[:2]
        blob, scale, left, top = self._preprocess(image)
        
        # 推理
        outputs = self.session.run(None, {self.input_name: blob})
        
        # 后处理
        boxes, kpts = self._postprocess(outputs, orig_shape, scale, left, top)
        
        return boxes, kpts

# --- 主程序逻辑 (与 test_pt.py 保持一致) ---
if __name__ == "__main__":
    # 1. 初始化 ONNX 推理器
    print(">>> 加载 ONNX 模型...")
    pose_detector = YOLOv8PoseONNX(ONNX_MODEL_PATH, conf_thres=CONF_THRESH, iou_thres=IOU_THRESH)
    
    cap = cv2.VideoCapture(0)
    history = []
    is_fallen = False

    print(">>> 启动 ONNX 姿态识别测试，按 'q' 退出...")
    
    while cap.isOpened():
        ret, frame = cap.read()
        if not ret: break

        cur_fall = False
        
        # 1. 运行 ONNX 推理
        boxes, keypoints = pose_detector.infer(frame)

        # 2. 遍历检测到的每一个人
        for idx, kpts in enumerate(keypoints):
            box = boxes[idx]
            
            # COCO 关键点索引: 0:鼻子, 5:左肩, 6:右肩, 11:左臀, 12:右臀
            try:
                l_shoulder, r_shoulder = kpts[5], kpts[6]
                l_hip, r_hip = kpts[11], kpts[12]
                
                # 判断骨架点置信度，必须大于 0.5
                if l_shoulder[2] > 0.5 and r_shoulder[2] > 0.5 and l_hip[2] > 0.5 and r_hip[2] > 0.5:
                    # 计算上半身和下半身中心
                    shoulder_center_x = (l_shoulder[0] + r_shoulder[0]) / 2
                    shoulder_center_y = (l_shoulder[1] + r_shoulder[1]) / 2
                    hip_center_x = (l_hip[0] + r_hip[0]) / 2
                    hip_center_y = (l_hip[1] + r_hip[1]) / 2

                    # 计算躯干向量
                    dx = shoulder_center_x - hip_center_x
                    dy = shoulder_center_y - hip_center_y
                    
                    # 计算与水平线夹角
                    angle = math.degrees(math.atan2(abs(dy), abs(dx)))
                    if angle < 35:
                        cur_fall = True

                    # 策略 B：宽高比兜底
                    x1, y1, x2, y2 = box
                    fw, fh = x2 - x1, y2 - y1
                    if fw / (fh + 1e-5) > 1.35:
                        cur_fall = True

            except Exception as e:
                pass

        # 3. 滑动窗口滤波 (移植原有逻辑)
        history.append(cur_fall)
        if len(history) > 10: history.pop(0)

        if history.count(True) >= 4:
            if not is_fallen: print("[ALERT] ONNX 检测：跌倒！"); is_fallen = True
        else:
            if is_fallen: print("[INFO] ONNX：恢复正常"); is_fallen = False

        # 4. 画面渲染
        status_text = "FALLEN!" if is_fallen else "NORMAL"
        status_color = (0, 0, 255) if is_fallen else (0, 255, 0)
        cv2.putText(frame, status_text, (20, 50), cv2.FONT_HERSHEY_SIMPLEX, 1.2, status_color, 3)

        # 如果检测到人，在画面上画个框
        for box in boxes:
            x1, y1, x2, y2 = map(int, box)
            cv2.rectangle(frame, (x1, y1), (x2, y2), (255, 0, 0), 2)

        cv2.imshow("ONNX Pose Test", frame)
        if cv2.waitKey(1) & 0xFF == ord('q'): break

    cap.release()
    cv2.destroyAllWindows()