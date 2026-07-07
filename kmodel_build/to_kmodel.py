import os
import argparse
import numpy as np
from PIL import Image
import onnxsim
import onnx
import nncase
import shutil
import math

def parse_model_input_output(model_file, input_shape):
    onnx_model = onnx.load(model_file)
    input_all = [node.name for node in onnx_model.graph.input]
    input_initializer = [node.name for node in onnx_model.graph.initializer]
    input_names = list(set(input_all) - set(input_initializer))
    input_tensors = [
        node for node in onnx_model.graph.input if node.name in input_names]

    TENSOR_TYPE_TO_NP_TYPE = {
        1: np.float32,
        2: np.uint8,
        3: np.int8,
        4: np.uint16,
        5: np.int16,
        6: np.int32,
        7: np.int64,
        9: np.bool_,
        10: np.float16,
        11: np.double,
        12: np.uint32,
        13: np.uint64
    }

    inputs = []
    for _, e in enumerate(input_tensors):
        onnx_type = e.type.tensor_type
        input_dict = {}
        input_dict['name'] = e.name
        
        elem_type = onnx_type.elem_type
        input_dict['dtype'] = TENSOR_TYPE_TO_NP_TYPE.get(elem_type, np.float32)
        
        input_dict['shape'] = [(i.dim_value if i.dim_value != 0 else d) for i, d in zip(
            onnx_type.shape.dim, input_shape)]
        inputs.append(input_dict)

    return onnx_model, inputs


def onnx_simplify(model_file, dump_dir, input_shape):
    onnx_model, inputs = parse_model_input_output(model_file, input_shape)
    onnx_model = onnx.shape_inference.infer_shapes(onnx_model)
    input_shapes = {}
    for input in inputs:
        input_shapes[input['name']] = input['shape']

    onnx_model, check = onnxsim.simplify(onnx_model, overwrite_input_shapes=input_shapes)
    assert check, "Simplified ONNX model could not be validated"

    model_file = os.path.join(dump_dir, 'simplified.onnx')
    onnx.save_model(onnx_model, model_file)
    return model_file


def read_model_file(model_file):
    with open(model_file, 'rb') as f:
        model_content = f.read()
    return model_content


# 融合【懒加载迭代】与【无畸变 Letterbox 灰边对账】的数据驱动类
class CloudCalibDataset:
    def __init__(self, shape, batch, calib_dir):
        self.shape = shape  # [1, 3, 320, 320]
        self.batch = batch
        self.img_paths = [os.path.join(calib_dir, p) for p in os.listdir(calib_dir) if p.lower().endswith(('.png', '.jpg', '.jpeg'))]
        total_found = len(self.img_paths)
        print(f"🔍 全量对账：在目录 '{calib_dir}' 中共找到了 {total_found} 张校准图片。")
        
        if total_found < self.batch:
            print(f"⚠️ 警告：设定的样本数为 {self.batch}，实际只有 {total_found} 张，已自动修正校准规模。")
            self.batch = total_found
            
        self.index = 0

    def __iter__(self):
        self.index = 0
        return self

    def __next__(self):
        if self.index >= self.batch:
            raise StopIteration
            
        img_path = self.img_paths[self.index]
        img = Image.open(img_path).convert('RGB')
        
        # 标准无畸变 Letterbox 缩放
        target_w = self.shape[3]
        target_h = self.shape[2]
        orig_w, orig_h = img.size
        
        scale = min(target_w / orig_w, target_h / orig_h)
        new_w = int(orig_w * scale)
        new_h = int(orig_h * scale)
        
        img_resized = img.resize((new_w, new_h), Image.BILINEAR)
        
        # 完美匹配 YOLOv8 训练生态的 114 灰边底图
        letterbox_img = Image.new("RGB", (target_w, target_h), (114, 114, 114))
        letterbox_img.paste(img_resized, ((target_w - new_w) // 2, (target_h - new_h) // 2))
        
        # 【核心调优点】：为了完美配合 nncase 的 compile_options.preprocess 硬件级自动归一化，
        # 此处校准图片矩阵必须保留 uint8（0~255），绝对不能提前在 Python 里除以 255.0！
        img_data = np.asarray(letterbox_img, dtype=np.uint8)
        img_data = np.transpose(img_data, (2, 0, 1))  # HWC -> CHW
        img_data = np.expand_dims(img_data, axis=0)    # CHW -> NCHW
        
        self.index += 1
        
        if self.index % 100 == 0 or self.index == self.batch:
            print(f"⏳ 正在执行 Letterbox 级别的全量量化拟合：进度 [{self.index}/{self.batch}] ...")
            
        return [img_data]


def main():
    parser = argparse.ArgumentParser(prog="nncase")
    parser.add_argument("--target", default="k230", type=str, help='target to run, k230/cpu')
    parser.add_argument("--model", type=str, required=True, help='model file')
    parser.add_argument("--dataset", type=str, default="../calibration_data", help='calibration_dataset')
    parser.add_argument("--input_width", type=int, default=320, help='input_width')
    parser.add_argument("--input_height", type=int, default=320, help='input_height')
    parser.add_argument("--ptq_option", type=int, default=3, help='ptq_option:0..5')
    parser.add_argument("--cls_node", type=str, default="", help='onnx node name for classification head to use int16')

    args = parser.parse_args()

    input_width = int(math.ceil(args.input_width / 32.0)) * 32
    input_height = int(math.ceil(args.input_height / 32.0)) * 32
    input_shape = [1, 3, input_height, input_width]

    dump_dir = 'tmp'
    if not os.path.exists(dump_dir):
        os.makedirs(dump_dir)

    # ONNX 拓扑剪枝简化
    model_file = onnx_simplify(args.model, dump_dir, input_shape)                                       

    # 1. 硬件级预处理基础全局配置
    compile_options = nncase.CompileOptions()
    compile_options.target = args.target
    compile_options.preprocess = True
    compile_options.swapRB = True  # YOLOv8 内部需要 RGB，硬件级自动换通道
    compile_options.input_shape = input_shape
    compile_options.input_type = 'uint8'
    compile_options.input_layout = "NCHW"

    compile_options.input_range = [0, 255]
    compile_options.mean = [0.0, 0.0, 0.0] 
    compile_options.std = [255.0, 255.0, 255.0] # 硬件在底层自动执行：(X - mean) / std，即自动 /255.0

    # 默认全网使用速度最快的纯 uint8 大盘跑满 KPU 流水线
    compile_options.quant_type = 'uint8'
    
    # 🟩【核心解密修复】：强行配置输出端保留为高精度 Float32！
    # 这一行能阻止 NNCASE 量化最后一层的 Sigmoid 置信度，彻底打破“最高只有0.5”的魔咒
    compile_options.output_type = 'float32'

    # 初始化编译器并导入拓扑结构
    compiler = nncase.Compiler(compile_options)
    model_content = read_model_file(model_file)
    import_options = nncase.ImportOptions()
    compiler.import_onnx(model_content, import_options)

    # 2. 配置后量化 PTQ 参数与【局部混合精度安全覆盖】
    ptq_options = nncase.PTQTensorOptions()
    ptq_options.samples_count = 2300  # 建议校准集图片设为 100~200 张左右，2000张太庞大会导致量化编译极慢且边际效应递减

    if args.ptq_option == 0:
        ptq_options.calibrate_method = 'NoClip'
        ptq_options.w_quant_type = 'uint8'
        ptq_options.quant_type = 'uint8'
    elif args.ptq_option == 1:
        ptq_options.calibrate_method = 'NoClip'
        ptq_options.w_quant_type = 'int16'
    elif args.ptq_option == 2:
        ptq_options.calibrate_method = 'NoClip'
        ptq_options.quant_type = 'int16'
    elif args.ptq_option == 3:
        ptq_options.calibrate_method = 'Kld'
        ptq_options.w_quant_type = 'uint8'
        ptq_options.quant_type = 'uint8'
    elif args.ptq_option == 4:
        ptq_options.calibrate_method = 'Kld'
        ptq_options.w_quant_type = 'int16'
    elif args.ptq_option == 5:
        ptq_options.calibrate_method = 'Kld'
        ptq_options.quant_type = 'int16'
    else:
        raise ValueError("ptq_option must be 0,1,2,3,4,5")

    # 局部混精层锁定
    if args.cls_node:
        print(f"🎯 局部混精对账：检测到敏感分类节点 '{args.cls_node}'，开始安全重写底层配置...")
        try:
            quant_config = compiler.get_quantization_config()
            quant_config.set_layer_quant_type(args.cls_node, "int16")
            compiler.set_quantization_config(quant_config)
            print("💪 成功通过 quantization_config 将目标节点锁死提升为 int16 精度！")
        except AttributeError:
            try:
                compiler.set_layer_quant_type(args.cls_node, "int16")
                print("💪 成功通过 compiler 直接调用将目标节点锁死提升为 int16 精度！")
            except AttributeError:
                print(f"⚠️ 警告：当前版本未完全暴露混精接口，输出层 float32 策略将作为主代偿支撑。")

    # 实例化无畸变流式懒加载数据集
    calib_dataset = CloudCalibDataset(input_shape, ptq_options.samples_count, args.dataset)
    ptq_options.samples_count = calib_dataset.batch
    
    # 将数据集安全喂入底层分析引擎
    ptq_options.set_tensor_data(calib_dataset)
    compiler.use_ptq(ptq_options)

    # 3. 编译生成面向 K230 KPU 的最终定点模型
    print("🚀 正在为 K230 KPU 执行多维度优化（无畸变Letterbox + 混精流式保护）全量校准编译...")
    compiler.compile()

    kmodel = compiler.gencode_tobytes()
    base, _ = os.path.splitext(args.model)
    kmodel_name = base + ".kmodel"
    with open(kmodel_name, 'wb') as f:
        f.write(kmodel)
        
    print(f"🎉 编译大功告成！高精度复合优化模型已安全导出至: {kmodel_name}")

    if os.path.exists("./tmp"):
        shutil.rmtree("./tmp")

if __name__ == '__main__':
    main()
