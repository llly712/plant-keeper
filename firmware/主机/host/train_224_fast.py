"""
224x224 快速训练版
健康权重2.7x  白粉病权重0.7x  榨干性能
"""
import os, sys
os.environ['TF_CPP_MIN_LOG_LEVEL'] = '2'
os.environ['TF_ENABLE_ONEDNN_OPTS'] = '0'

data_path = 'data'
if not os.path.isdir('data'): data_path = '../data'

import tensorflow as tf
import numpy as np

# ========== 性能拉满 ==========
gpus = tf.config.list_physical_devices('GPU')
if gpus:
    for gpu in gpus:
        tf.config.experimental.set_memory_growth(gpu, True)
    tf.keras.mixed_precision.set_global_policy('mixed_float16')
    print(f"GPU模式: {len(gpus)}张卡, mixed_float16")
else:
    tf.config.threading.set_intra_op_parallelism_threads(0)
    tf.config.threading.set_inter_op_parallelism_threads(0)
    print("CPU模式: 全核")

AUTOTUNE = tf.data.AUTOTUNE
IMG_W, IMG_H = 160, 160
BATCH = 64
EPOCHS = 30
CLASSES = ['健康', '白粉病', '叶斑病', '锈病', '虫害']

# ========== tf.data 高性能输入管道 ==========
train_ds = tf.keras.utils.image_dataset_from_directory(
    data_path, validation_split=0.2, subset='training',
    seed=42, image_size=(IMG_H, IMG_W), batch_size=BATCH,
    labels='inferred', class_names=CLASSES
)
val_ds = tf.keras.utils.image_dataset_from_directory(
    data_path, validation_split=0.2, subset='validation',
    seed=42, image_size=(IMG_H, IMG_W), batch_size=BATCH,
    labels='inferred', class_names=CLASSES
)

# 数据增强 + 预处理
data_aug = tf.keras.Sequential([
    tf.keras.layers.RandomFlip('horizontal'),
    tf.keras.layers.RandomRotation(0.15),
    tf.keras.layers.RandomZoom(0.15),
    tf.keras.layers.RandomBrightness(0.15),
    tf.keras.layers.RandomContrast(0.15),
])

def preprocess(x, y):
    x = tf.cast(x, tf.float32) / 255.0
    return x, y

def augment(x, y):
    x = data_aug(x, training=True)
    x = tf.clip_by_value(x, 0.0, 1.0)
    return x, y

train_ds = train_ds.map(augment, num_parallel_calls=AUTOTUNE).map(preprocess, num_parallel_calls=AUTOTUNE).prefetch(AUTOTUNE)
val_ds = val_ds.map(preprocess, num_parallel_calls=AUTOTUNE).prefetch(AUTOTUNE)

num_classes = len(CLASSES)

# ========== 模型 ==========
from tensorflow.keras import layers, models, regularizers

inputs = layers.Input(shape=(IMG_H, IMG_W, 3))

# Block 1
x = layers.Conv2D(24, 3, padding='same')(inputs)
x = layers.BatchNormalization()(x)
x = layers.Activation('relu')(x)
x = layers.Conv2D(24, 3, padding='same')(x)
x = layers.BatchNormalization()(x)
x = layers.Activation('relu')(x)
x = layers.MaxPooling2D()(x)  # 112

# Block 2
x = layers.Conv2D(48, 3, padding='same')(x)
x = layers.BatchNormalization()(x)
x = layers.Activation('relu')(x)
x = layers.Conv2D(48, 3, padding='same')(x)
x = layers.BatchNormalization()(x)
x = layers.Activation('relu')(x)
x = layers.MaxPooling2D()(x)  # 56

# Block 3
x = layers.Conv2D(96, 3, padding='same')(x)
x = layers.BatchNormalization()(x)
x = layers.Activation('relu')(x)
x = layers.Conv2D(96, 3, padding='same')(x)
x = layers.BatchNormalization()(x)
x = layers.Activation('relu')(x)
x = layers.MaxPooling2D()(x)  # 28

# Block 4
x = layers.Conv2D(160, 3, padding='same')(x)
x = layers.BatchNormalization()(x)
x = layers.Activation('relu')(x)
x = layers.MaxPooling2D()(x)  # 14

# Head
x = layers.GlobalAveragePooling2D()(x)
x = layers.Dropout(0.4)(x)
x = layers.Dense(128)(x)
x = layers.BatchNormalization()(x)
x = layers.Activation('relu')(x)
x = layers.Dropout(0.3)(x)
outputs = layers.Dense(num_classes, activation='softmax', dtype='float32')(x)

model = models.Model(inputs, outputs)

# 学习率调度
lr_schedule = tf.keras.optimizers.schedules.CosineDecayRestarts(
    initial_learning_rate=0.002,
    first_decay_steps=500,
    t_mul=2.0,
    m_mul=0.8,
    alpha=0.0001
)

model.compile(
    optimizer=tf.keras.optimizers.AdamW(learning_rate=lr_schedule, weight_decay=0.0005),
    loss=tf.keras.losses.SparseCategoricalCrossentropy(from_logits=False),
    metrics=['accuracy']
)

model.summary()

# ========== 类权重 ==========
class_weight = {
    0: 2.7,   # 健康 - 拉满，误报扣重
    1: 0.7,   # 白粉病 - 拉低，别太敏感
    2: 1.0,   # 叶斑病
    3: 1.0,   # 锈病
    4: 1.0,   # 虫害
}
print(f"权重: 健康x{class_weight[0]} 白粉病x{class_weight[1]} 其他x1.0")
print(f"Batch={BATCH} Epochs={EPOCHS}\n")

# ========== 训练 ==========
callbacks = [
    tf.keras.callbacks.ReduceLROnPlateau(monitor='val_loss', factor=0.5, patience=5, min_lr=1e-6),
    tf.keras.callbacks.EarlyStopping(monitor='val_accuracy', patience=10, restore_best_weights=True),
]

model.fit(
    train_ds, validation_data=val_ds,
    epochs=EPOCHS, class_weight=class_weight,
    callbacks=callbacks, verbose=2
)

# 保存
model.save('plant_model_224.h5')

# ========== 转TFLite ==========
print("\n转换 TFLite int8...")

def rep():
    for _ in range(100):
        yield [np.random.rand(1, IMG_H, IMG_W, 3).astype(np.float32)]

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = rep
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.uint8
converter.inference_output_type = tf.float32

tflite_model = converter.convert()
with open('plant_model_224.tflite', 'wb') as f:
    f.write(tflite_model)
print(f"保存 plant_model_224.tflite ({len(tflite_model)} bytes)")

# C头文件
with open('plant_model_224.h', 'w') as f:
    f.write('// 植物病害AI 224x224 健康2.7x 白粉0.7x\n')
    f.write(f'const unsigned char plant_model[] = {{\n')
    for i in range(0, len(tflite_model), 12):
        f.write('  ' + ', '.join(f'0x{b:02x}' for b in tflite_model[i:i+12]) + ',\n')
    f.write('};\n')
    f.write(f'const unsigned int plant_model_len = {len(tflite_model)};\n')

print("完成!")
