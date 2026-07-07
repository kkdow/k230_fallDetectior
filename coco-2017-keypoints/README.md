# COCO-2017-Keypoints 数据集处理与 YOLOv8-pose 模型训练

本目录包含从 COCO-2017 关键点数据集到 YOLOv8-pose 模型训练的完整工具链。

- **数据集转换**：`cocotoyolo.py` 将 COCO 格式的关键点标注转换为 YOLO 格式。
- **训练配置**：`data.yaml` 定义数据集路径与关键点配置 (17 点，3 属性)；`args.yaml` 为训练超参数存档。
- **训练产出**：`runs/pose/train-10/` 包含训练权重、训练曲线与评估结果。
- **测试验证**：`weights/test_pt.py` 提供 PyTorch 实时推理验证脚本；`weights/test_onnx.py` 提供 ONNX 实时推理验证脚本。

---

## 📂 目录结构

```text
coco-2017-keypoints/
├── cocotoyolo.py               # COCO → YOLO 格式转换脚本
├── yolo_pose_dataset/          # YOLO 格式数据集
│   ├── data.yaml               # 数据集配置文件
│   ├── images/                 # 训练/验证图片 (需自行下载)
│   │   ├── train2017/          # 训练集图片
│   │   └── val2017/            # 验证集图片
│   ├── labels/                 # 转换后的 YOLO 标签文件
│   │   ├── train2017/          # 训练集标签
│   │   └── val2017/            # 验证集标签
│   ├── train2017.txt           # 训练集图片路径索引
│   └── val2017.txt             # 验证集图片路径索引
├── runs/pose/train-10/         # 训练产出
│   ├── args.yaml               # 训练超参数存档
│   ├── results.csv             # 逐轮训练指标
│   ├── results.png             # 训练曲线总览
│   ├── weights/
│   │   ├── best.pt             # 最佳模型权重
│   │   ├── last.pt             # 最后一轮权重
│   │   ├── best.onnx           # 导出的 ONNX 模型
│   │   ├── test_pt.py          # PyTorch 实时推理测试脚本
│   │   └── test_onnx.py        # ONNX Runtime 实时推理测试脚本
│   └── *.jpg                   # 训练/验证可视化样本
└── README.md
```

---

## 🚀 快速开始

### 1. 下载 COCO-2017 关键点数据集
从 [Kaggle](https://www.kaggle.com/datasets/asad11914/coco-2017-keypoints) 下载数据集 (约 10GB)，解压后得到 `annotations/` 和 `images/` 目录。

### 2. 转换数据集格式
修改 `cocotoyolo.py` 中的路径配置：

```python
json_path = r'你的路径/person_keypoints_train2017.json'
save_path = r'./yolo_pose_dataset/labels/train2017'
txt_path = r'./yolo_pose_dataset/train2017.txt'
```

运行转换：

```bash
python cocotoyolo.py
```

### 3. 配置 data.yaml
确认 `path`、`train`、`val` 路径正确。关键点配置已预设为 17 点 (COCO 格式)：

```yaml
kpt_shape: [17, 3]
names: {0: person}
flip_idx: [0, 2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11, 14, 13, 16, 15]
```

### 4. 训练 YOLOv8-pose 模型
```bash
yolo pose train data=./yolo_pose_dataset/data.yaml model=yolov8n-pose.pt epochs=300 imgsz=320 batch=32 device=0
```

训练完成后，权重保存在 `runs/pose/train-10/weights/`。

### 5. 验证模型 (PyTorch)
```bash
cd runs/pose/train-10/weights
python test_pt.py   # 使用摄像头实时验证 (需配置摄像头)
```

### 6. 导出 ONNX 模型
```bash
yolo export model=runs/pose/train-10/weights/best.pt format=onnx imgsz=320
```

### 7. 验证 ONNX 模型
```bash
cd runs/pose/train-10/weights
python test_onnx.py   # 使用 ONNX Runtime 实时验证
```

---

## 📊 训练指标 (部分)

| Epoch | Box Loss | Pose Loss | mAP50(P) | mAP50-95(P) |
| --- | --- | --- | --- | --- |
| 1 | 1.204 | 4.826 | 0.541 | 0.240 |
| 50 | 1.227 | 4.939 | 0.629 | 0.317 |
| 100 | 1.207 | 4.794 | 0.644 | 0.329 |
| 200 | 1.118 | 4.296 | 0.658 | 0.351 |
| 300 | 0.975 | 3.286 | 0.670 | 0.358 |

*(完整指标见 `results.csv`)*

---

## 📝 常见问题 (FAQ)

**Q: 数据集下载后 images 目录为空？**
A: COCO 官方数据集需要从官网下载图片，Kaggle 上的版本可能只包含标注文件。请从 COCO 官网下载 train2017 和 val2017 图片。

**Q: 转换脚本报错 `KeyError: 'num_keypoints'`？**
A: 确认标注文件路径正确，且使用的是 `person_keypoints_train2017.json` 而非 `instances_train2017.json`。

**Q: 训练时显存不足 (OOM)？**
A: 减小 `batch` 参数 (如 `batch=16`) 或 `imgsz` 参数。

**Q: ONNX 导出后 `test_onnx.py` 推理结果与 PyTorch 不一致？**
A: 确认 ONNX 导出的 `imgsz` 与训练时一致，且 `test_onnx.py` 中的 `INPUT_SIZE` 参数匹配。

---

## 📜 参考文献

[1] Jocher G, Chaurasia A, Qiu J. Ultralytics YOLOv8[CP/OL]. (2023)[2026-06-19]. https://github.com/ultralytics/ultralytics.

[2] Kaggle. COCO-2017 Keypoints Dataset[DB/OL]. (2017)[2026-06-19]. https://www.kaggle.com/datasets/asad11914/coco-2017-keypoints.