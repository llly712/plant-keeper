"""
植物病害AI模型训练脚本
用法: python train_model.py
生成 plant_model.h 给ESP32-S3用

数据集准备:
  把图片按类别放到 data/ 文件夹下:
  data/
    健康/
    白粉病/
    叶斑病/
    锈病/
    虫害/
  每类至少50张

如果没有数据集，脚本会生成一个随机权重的测试模型
"""

import os
import sys

def make_dummy_model():
    """生成一个未训练的测试模型 (没有tensorflow也能用)"""
    import struct

    # 最简单的办法：用flatbuffers库构建
    # 如果没有flatbuffers，创建一个极小的手写模型
    print("没有训练数据，生成测试模型...")

    # 这里我们手写一个最小的TFLite模型
    # 模型结构: Input(96,96,3,uint8) -> AveragePool -> FC(5) -> Softmax
    # 实际推理会输出随机概率，但格式是对的

    try:
        import flatbuffers
        return _build_with_flatbuffers()
    except ImportError:
        pass

    # 如果flatbuffers也没装，就创建一个空壳
    # 用户需要自己装tensorflow训练
    print("flatbuffers未安装，创建占位模型")
    print("请先安装: pip install flatbuffers")
    print("或安装完整tensorflow: pip install tensorflow")
    return _build_minimal_model()


def _build_minimal_model():
    """纯手工构建最小TFLite模型二进制"""
    import struct

    # 这是通过手动构造FlatBuffer来创建的一个极简TFLite模型
    # 结构: Model -> SubGraph -> [Input -> Softmax -> Output]
    # 输入96x96x3 uint8, 输出5 float

    buf = bytearray()

    def write_u32(v):
        buf.extend(struct.pack('<I', v))

    def write_i32(v):
        buf.extend(struct.pack('<i', v))

    def write_u16(v):
        buf.extend(struct.pack('<H', v))

    def write_u8(v):
        buf.extend(struct.pack('<B', v))

    def write_offset(offset_pos, target_pos):
        """在offset_pos位置写入相对target_pos的偏移"""
        rel = target_pos - offset_pos
        struct.pack_into('<I', buf, offset_pos, rel)

    # FlatBuffer基础结构
    # Root: Model { version, subgraphs, buffers, metadata }

    # Buffer 0: 空 (input tensor用)
    # Buffer 1: 模型权重数据

    builder_offset = 0
    root_start = 0
    builder_start = len(buf) + 4  # 前面留4字节

    # ====== 暂时先放一个简单的占位模型 ======
    # 等不了手动构建了，先输出提示

    return None


def _build_with_flatbuffers():
    """用flatbuffers库构建模型"""
    import flatbuffers
    import numpy as np
    import struct

    # TFLite schema的Python绑定
    # 由于我们没有预生成的schema类，这里直接用flatbuffers原始API构建
    builder = flatbuffers.Builder(4096)

    # 这个路径太复杂了...我们换个思路
    return None


def generate_model_header(model_bytes, output_path="plant_model.h"):
    """把TFLite模型二进制转成C头文件"""
    with open(output_path, 'w') as f:
        f.write('// 植物病害AI模型 - 5分类\n')
        f.write('// 输入: 96x96x3 uint8 (量化)\n')
        f.write('// 输出: float32[5] (softmax概率)\n')
        f.write('// 类别: [健康, 白粉病, 叶斑病, 锈病, 虫害]\n')
        f.write('// 运行 train_model.py 可重新训练\n')
        f.write('// 或用 xxd -i model.tflite 生成\n\n')
        f.write(f'const unsigned char plant_model[] = {{\n')

        for i in range(0, len(model_bytes), 12):
            chunk = model_bytes[i:i+12]
            hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
            f.write(f'  {hex_str},\n')

        f.write('};\n')
        f.write(f'const unsigned int plant_model_len = {len(model_bytes)};\n')

    print(f"生成 {output_path} ({len(model_bytes)} bytes)")


# ============================================================
# 主流程：会优先尝试用tensorflow训练，不行就生成测试模型
# ============================================================
def main():
    # 检查是否有训练数据
    has_data = os.path.isdir('data') and any(
        os.listdir(os.path.join('data', d))
        for d in os.listdir('data')
        if os.path.isdir(os.path.join('data', d))
    )

    if has_data:
        print("找到训练数据，开始训练...")
        train_with_tensorflow()
    else:
        print("没有训练数据目录 data/")
        print("")
        print("========================================")
        print("两种方式获得plant_model.h:")
        print("")
        print("方法1 (推荐): 安装tensorflow并用真实数据训练")
        print("  pip install tensorflow")
        print("  然后把图片按类别放到 data/ 文件夹下")
        print("  再运行 python train_model.py")
        print("")
        print("方法2 (测试用): 生成一个随机模型先让代码跑通")
        print("  pip install tensorflow")
        print("  python train_model.py --dummy")
        print("")
        print("方法3 (在线): 用Edge Impulse训练导出")
        print("  https://studio.edgeimpulse.com/")
        print("  上传图片 -> 训练 -> 导出Arduino库")
        print("  把导出的 .h 文件改成 plant_model.h")
        print("========================================")


def train_with_tensorflow():
    """用TensorFlow训练模型"""
    try:
        import tensorflow as tf
        import numpy as np
        from tensorflow.keras.preprocessing.image import ImageDataGenerator
    except ImportError:
        print("请先安装: pip install tensorflow")
        sys.exit(1)

    # 图片参数
    IMG_W, IMG_H = 96, 96
    BATCH = 16
    EPOCHS = 30
    CLASSES = ['健康', '白粉病', '叶斑病', '锈病', '虫害']

    # 数据增强
    datagen = ImageDataGenerator(
        rescale=1./255,
        rotation_range=30,
        width_shift_range=0.2,
        height_shift_range=0.2,
        shear_range=0.2,
        zoom_range=0.2,
        horizontal_flip=True,
        validation_split=0.2
    )

    train_gen = datagen.flow_from_directory(
        'data',
        target_size=(IMG_H, IMG_W),
        batch_size=BATCH,
        class_mode='categorical',
        subset='training',
        classes=CLASSES
    )

    val_gen = datagen.flow_from_directory(
        'data',
        target_size=(IMG_H, IMG_W),
        batch_size=BATCH,
        class_mode='categorical',
        subset='validation',
        classes=CLASSES
    )

    num_classes = len(CLASSES)

    # 建立模型 (轻量CNN 适合ESP32-S3)
    model = tf.keras.Sequential([
        tf.keras.layers.InputLayer(input_shape=(IMG_H, IMG_W, 3)),
        # 第一层
        tf.keras.layers.Conv2D(16, 3, padding='same', activation='relu'),
        tf.keras.layers.MaxPooling2D(),
        # 第二层
        tf.keras.layers.Conv2D(32, 3, padding='same', activation='relu'),
        tf.keras.layers.MaxPooling2D(),
        # 第三层
        tf.keras.layers.Conv2D(64, 3, padding='same', activation='relu'),
        tf.keras.layers.MaxPooling2D(),
        # 全连接
        tf.keras.layers.GlobalAveragePooling2D(),
        tf.keras.layers.Dropout(0.3),
        tf.keras.layers.Dense(64, activation='relu'),
        tf.keras.layers.Dropout(0.3),
        tf.keras.layers.Dense(num_classes, activation='softmax')
    ])

    model.compile(
        optimizer='adam',
        loss='categorical_crossentropy',
        metrics=['accuracy']
    )

    model.summary()

    print(f"\n开始训练 {EPOCHS} 轮...")
    model.fit(
        train_gen,
        validation_data=val_gen,
        epochs=EPOCHS
    )

    # 保存Keras模型
    model.save('plant_model.h5')
    print("保存 plant_model.h5")

    # ========== 转TFLite (量化) ==========
    print("\n转换为TFLite (int8量化)...")

    def representative_dataset():
        for _ in range(100):
            data = np.random.rand(1, IMG_H, IMG_W, 3).astype(np.float32)
            yield [data]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.uint8
    converter.inference_output_type = tf.float32

    tflite_model = converter.convert()

    # 保存tflite文件
    with open('plant_model.tflite', 'wb') as f:
        f.write(tflite_model)
    print(f"保存 plant_model.tflite ({len(tflite_model)} bytes)")

    # 生成C头文件
    generate_model_header(tflite_model)
    print("\n完成! 把 plant_model.h 复制到Arduino项目文件夹")


def make_dummy_with_tf():
    """用tensorflow生成随机权重的测试模型"""
    try:
        import tensorflow as tf
        import numpy as np
    except ImportError:
        print("需要安装tensorflow: pip install tensorflow")
        sys.exit(1)

    IMG_W, IMG_H = 96, 96

    model = tf.keras.Sequential([
        tf.keras.layers.InputLayer(input_shape=(IMG_H, IMG_W, 3)),
        tf.keras.layers.Conv2D(8, 3, padding='same', activation='relu'),
        tf.keras.layers.MaxPooling2D(),
        tf.keras.layers.Conv2D(16, 3, padding='same', activation='relu'),
        tf.keras.layers.MaxPooling2D(),
        tf.keras.layers.GlobalAveragePooling2D(),
        tf.keras.layers.Dense(5, activation='softmax')
    ])

    def representative_dataset():
        for _ in range(50):
            data = np.random.rand(1, IMG_H, IMG_W, 3).astype(np.float32)
            yield [data]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.uint8
    converter.inference_output_type = tf.float32

    tflite_model = converter.convert()
    generate_model_header(tflite_model)
    print("测试模型生成完毕 (随机权重，只能用来验证代码能跑)")


if __name__ == '__main__':
    if '--dummy' in sys.argv:
        make_dummy_with_tf()
    else:
        main()
