# K230 kmodel 模型转换工具

本目录包含将 YOLOv8-pose 的 `.onnx` 模型转换为 K230 平台可执行的 `.kmodel` 文件的完整工具与配置。

- **转换脚本**：`to_kmodel.py` 基于 nncase 工具链实现 ONNX → kmodel 的量化编译。
- **输入适配**：自动处理 Letterbox 无畸变缩放与 114 灰边填充，匹配 YOLOv8 训练生态。
- **精度优化**：采用 **KLD 校准 + int16 量化** (`ptq_option=5`)，在保证推理速度的同时最大限度保留模型精度。
- **硬件加速**：针对 K230 KPU 的 RISC-V 向量指令集优化。

---

## 📂 目录结构

```text
kmodel_build/
├── best.onnx               # 训练导出的 ONNX 模型 (输入: 3×320×320)
├── best.kmodel             # 转换好的 K230 模型 (可直接部署)
├── to_kmodel.py            # 转换脚本 (基于 nncase)
├── images/                 # 校准图片目录 (需自行准备)
└── README.md
🚀 快速开始
1. 安装 nncase
bash
pip install nncase
2. 准备校准数据集
在 images/ 目录下放置 100~300 张代表性图片 (建议从 COCO 验证集中抽取)，用于 PTQ 校准。

3. 运行转换脚本
bash
python to_kmodel.py \
    --model best.onnx \
    --dataset ./images \
    --input_width 320 \
    --input_height 320 \
    --ptq_option 5 \
    --target k230
4. 获取转换后的模型
转换成功后，在当前目录生成 best.kmodel 文件。

🔧 参数说明
参数	类型	默认值	说明
--model	str	必填	输入的 ONNX 模型路径
--dataset	str	../calibration_data	校准图片目录
--input_width	int	320	模型输入宽度 (自动对齐到 32 的倍数)
--input_height	int	320	模型输入高度 (自动对齐到 32 的倍数)
--ptq_option	int	5	量化方式: 5-KLD + int16 (推荐)
--target	str	k230	目标平台 (k230/cpu)
--cls_node	str	""	指定分类节点使用 int16 精度 (局部混精)
PTQ 量化选项说明
选项	校准方法	量化类型	说明
0	NoClip	uint8	最快，精度最低
1	NoClip	int16	较快，精度中等
2	NoClip	int16	同上
3	KLD	uint8	精度较好，速度较快
4	KLD	int16	精度高，速度中等
5	KLD	int16	精度最高，推荐用于姿态估计任务
为什么选择 ptq_option=5？ 姿态估计任务对关键点坐标精度要求较高，int16 量化相比 uint8 能更好地保留小数精度，KLD 校准则能更准确地拟合数据分布，减少量化误差。

🔧 关键配置解析
配置项	值	说明
input_type	uint8	输入图像数据类型
input_shape	[1, 3, 320, 320]	输入张量形状 (NCHW)
mean	[0, 0, 0]	归一化均值
std	[255, 255, 255]	归一化标准差 (将 0~255 映射到 0~1)
output_type	float32	输出层保留浮点精度，避免置信度截断
swapRB	True	RGB 通道交换 (适配 YOLO)
preprocess	True	启用硬件预处理
局部混精配置
若模型输出置信度异常，可尝试指定关键节点使用 int16：

bash
python to_kmodel.py --model best.onnx --dataset ./images --cls_node "/model.22/Conv"
📝 常见问题 (FAQ)
Q: 转换后模型推理置信度最高只有 0.5？
A: 检查 compile_options.output_type = 'float32' 是否生效。若仍异常，尝试在 to_kmodel.py 中指定 --cls_node 参数锁定关键节点为 int16。

Q: 校准图片需要多少张？
A: 建议 100~300 张，过多会显著增加编译时间 (2000 张以上边际效应递减)。

Q: 转换后的 kmodel 如何部署？
A: 将 best.kmodel 拷贝到 K230 的 /data/ 目录，Python 代码中通过 /data/best.kmodel 路径加载。

Q: 运行时报错 ImportError: No module named 'nncase'？
A: 在 PC 端安装 nncase: pip install nncase。注意 nncase 有版本兼容性要求，建议使用与 K230 SDK 匹配的版本。

Q: 转换过程报错 Killed 或内存不足？
A: 减少校准图片数量 (ptq_options.samples_count) 或使用 --ptq_option 较低选项。
📜 参考文献
[1] 嘉楠科技. K230 YOLO 大作战[EB/OL]. (2024)[2026-06-19]. https://www.kendryte.com/k230_canmv/zh/main/example/ai/YOLO大作战.html.
[2] nncase: Neural Network Compiler for K230[CP/OL]. (2024)[2026-06-19]. https://github.com/kendryte/nncase.
