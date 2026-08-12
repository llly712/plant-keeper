"""224x224 修复版 - softmax已加，去除BN避免数值问题"""
import os, sys
os.environ['TF_CPP_MIN_LOG_LEVEL'] = '2'

data_path = 'data'
if not os.path.isdir('data'): data_path = '../data'

import tensorflow as tf
import numpy as np

# CPU全核
tf.config.threading.set_intra_op_parallelism_threads(0)
tf.config.threading.set_inter_op_parallelism_threads(0)

AUTOTUNE = tf.data.AUTOTUNE
IMG_W, IMG_H = 96, 96
BATCH = 32
EPOCHS = 30
CLASSES = ['健康', '白粉病', '叶斑病', '锈病', '虫害']

train_ds = tf.keras.utils.image_dataset_from_directory(
    data_path, validation_split=0.2, subset='training', seed=42,
    image_size=(IMG_H, IMG_W), batch_size=BATCH, class_names=CLASSES
)
val_ds = tf.keras.utils.image_dataset_from_directory(
    data_path, validation_split=0.2, subset='validation', seed=42,
    image_size=(IMG_H, IMG_W), batch_size=BATCH, class_names=CLASSES
)

data_aug = tf.keras.Sequential([
    tf.keras.layers.RandomFlip('horizontal'),
    tf.keras.layers.RandomRotation(0.1),
    tf.keras.layers.RandomZoom(0.1),
])

def preprocess(x, y):
    x = tf.cast(x, tf.float32) / 255.0 - 0.5
    return x, y

def augment(x, y):
    x = data_aug(x, training=True)
    x = tf.clip_by_value(x, 0.0, 1.0)
    return x, y

train_ds = train_ds.map(augment, num_parallel_calls=AUTOTUNE).map(preprocess, num_parallel_calls=AUTOTUNE).cache().prefetch(AUTOTUNE)
val_ds = val_ds.map(preprocess, num_parallel_calls=AUTOTUNE).cache().prefetch(AUTOTUNE)

# ========== 简洁CNN，不用BN，稳定训练 ==========
from tensorflow.keras import layers, models

model = models.Sequential([
    layers.InputLayer(shape=(IMG_H, IMG_W, 3)),

    layers.Conv2D(16, 3, padding='same', activation='relu'),
    layers.MaxPooling2D(),

    layers.Conv2D(32, 3, padding='same', activation='relu'),
    layers.MaxPooling2D(),

    layers.Conv2D(64, 3, padding='same', activation='relu'),
    layers.MaxPooling2D(),

    layers.GlobalAveragePooling2D(),
    layers.Dropout(0.3),
    layers.Dense(64, activation='relu'),
    layers.Dropout(0.3),
    layers.Dense(5, activation='softmax')
])

model.compile(
    optimizer=tf.keras.optimizers.Adam(0.001),
    loss='sparse_categorical_crossentropy',
    metrics=['accuracy']
)
model.summary()

# 权重
class_weight = {0: 2.7, 1: 0.7, 2: 1.0, 3: 1.0, 4: 1.0}
print(f"健康x2.7 白粉x0.7 | Batch={BATCH} Epochs={EPOCHS}\n")

# 训练 (不加EarlyStopping，让它跑满)
model.fit(
    train_ds, validation_data=val_ds,
    epochs=EPOCHS, class_weight=class_weight,
    verbose=2
)

model.save('plant_model_224.h5')

# ========== 转TFLite ==========
print("\nTFLite int8...")
def rep():
    for _ in range(100):
        yield [np.random.rand(1, IMG_H, IMG_W, 3).astype(np.float32)]

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = rep
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.uint8
converter.inference_output_type = tf.float32
converter._experimental_disable_per_channel_quantization_for_dense_layers = True
tflite = converter.convert()

with open('plant_model_224.tflite', 'wb') as f: f.write(tflite)
with open('plant_model_224.h', 'w') as f:
    f.write(f'const unsigned char plant_model[] = {{\n')
    for i in range(0, len(tflite), 12):
        f.write('  ' + ', '.join(f'0x{b:02x}' for b in tflite[i:i+12]) + ',\n')
    f.write('};\n')
    f.write(f'const unsigned int plant_model_len = {len(tflite)};\n')

print(f"Done! {len(tflite)} bytes")
