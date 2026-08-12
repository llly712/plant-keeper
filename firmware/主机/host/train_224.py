"""
植物病害AI模型训练 - 224x224版
加大健康类权重，减少误报
用法: python train_224.py
"""
import os, sys

# 检查数据
data_path = 'data'
if not os.path.isdir('data'):
    data_path = '../data'

try:
    import tensorflow as tf
    import numpy as np
    from tensorflow.keras.preprocessing.image import ImageDataGenerator
    from tensorflow.keras import layers, models, regularizers
except ImportError:
    print("请先安装: pip install tensorflow")
    sys.exit(1)

IMG_W, IMG_H = 224, 224
BATCH = 16
EPOCHS = 40
CLASSES = ['健康', '白粉病', '叶斑病', '锈病', '虫害']

# ========== 数据加载 ==========
datagen = ImageDataGenerator(
    rescale=1./255,
    rotation_range=30,
    width_shift_range=0.2,
    height_shift_range=0.2,
    shear_range=0.2,
    zoom_range=0.2,
    horizontal_flip=True,
    brightness_range=[0.8, 1.2],
    validation_split=0.2
)

train_gen = datagen.flow_from_directory(
    data_path, target_size=(IMG_H, IMG_W), batch_size=BATCH,
    class_mode='categorical', subset='training', classes=CLASSES
)
val_gen = datagen.flow_from_directory(
    data_path, target_size=(IMG_H, IMG_W), batch_size=BATCH,
    class_mode='categorical', subset='validation', classes=CLASSES
)

num_classes = len(CLASSES)

# ========== 模型 ==========
# 比96版深一些，224输入需要更多下采样
model = models.Sequential([
    layers.InputLayer(input_shape=(IMG_H, IMG_W, 3)),

    # Block 1
    layers.Conv2D(16, 3, padding='same', activation='relu'),
    layers.Conv2D(16, 3, padding='same', activation='relu'),
    layers.MaxPooling2D(),  # 112

    # Block 2
    layers.Conv2D(32, 3, padding='same', activation='relu'),
    layers.Conv2D(32, 3, padding='same', activation='relu'),
    layers.MaxPooling2D(),  # 56

    # Block 3
    layers.Conv2D(64, 3, padding='same', activation='relu'),
    layers.Conv2D(64, 3, padding='same', activation='relu'),
    layers.MaxPooling2D(),  # 28

    # Block 4
    layers.Conv2D(128, 3, padding='same', activation='relu'),
    layers.MaxPooling2D(),  # 14

    # Head
    layers.GlobalAveragePooling2D(),
    layers.Dropout(0.4),
    layers.Dense(64, activation='relu', kernel_regularizer=regularizers.l2(0.001)),
    layers.Dropout(0.3),
    layers.Dense(num_classes, activation='softmax')
])

model.compile(
    optimizer=tf.keras.optimizers.Adam(learning_rate=0.001),
    loss='categorical_crossentropy',
    metrics=['accuracy']
)

model.summary()

# ========== 类权重 (重点！健康拉大，减少误报) ==========
# class 0=健康 权重提到3倍，病害类保持1
# 这样模型更"谨慎"，不会随便判病
class_weight = {
    0: 2.5,   # 健康 - 权重拉大，误报扣分重
    1: 1.0,   # 白粉病
    2: 1.0,   # 叶斑病
    3: 1.0,   # 锈病
    4: 1.0,   # 虫害
}

print(f"\n类权重: 健康x{class_weight[0]}, 病害x1.0")
print(f"训练 {EPOCHS} 轮...\n")

# 回调
callbacks = [
    tf.keras.callbacks.ReduceLROnPlateau(monitor='val_loss', factor=0.5, patience=4, min_lr=1e-6),
    tf.keras.callbacks.EarlyStopping(monitor='val_accuracy', patience=8, restore_best_weights=True),
]

model.fit(
    train_gen,
    validation_data=val_gen,
    epochs=EPOCHS,
    class_weight=class_weight,
    callbacks=callbacks
)

# 保存
model.save('plant_model_224.h5')
print("保存 plant_model_224.h5")

# ========== 转TFLite (量化) ==========
print("\n转换 TFLite int8 量化...")

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

with open('plant_model_224.tflite', 'wb') as f:
    f.write(tflite_model)
print(f"保存 plant_model_224.tflite ({len(tflite_model)} bytes)")

# 生成C头文件
with open('plant_model_224.h', 'w', encoding='utf-8') as f:
    f.write('// 植物病害AI模型 - 224x224 健康加权版\n')
    f.write('// 输入: [1,224,224,3] uint8\n')
    f.write('// 输出: [1,5] float32 (softmax概率)\n')
    f.write('// 类别: [健康, 白粉病, 叶斑病, 锈病, 虫害]\n')
    f.write('// 健康类权重2.5x，减少误报\n\n')
    f.write('#ifndef PLANT_MODEL_H\n#define PLANT_MODEL_H\n\n')
    f.write(f'const unsigned char plant_model[] = {{\n')
    for i in range(0, len(tflite_model), 12):
        chunk = tflite_model[i:i+12]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
        f.write(f'  {hex_str},\n')
    f.write('};\n')
    f.write(f'const unsigned int plant_model_len = {len(tflite_model)};\n\n')
    f.write('#endif\n')

print("生成 plant_model_224.h")
print("\n完成! 模型已生成，健康类权重2.5倍")
print("把 plant_model_224.tflite 替换到 demo_app 下即可")
