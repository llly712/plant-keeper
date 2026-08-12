import os
import numpy as np
import tensorflow as tf


model_file = "plant_model_224.h5"
data_dir = os.path.join("..", "data")
out_file = "plant_model_96_esp32.tflite"
head_file = "plant_model.h"

model = tf.keras.models.load_model(model_file, compile=False)
h, w = model.input_shape[1:3]


def rep_data():
    files = []
    for root, _, names in os.walk(data_dir):
        for name in names:
            if name.lower().endswith((".jpg", ".jpeg", ".png", ".bmp")):
                files.append(os.path.join(root, name))
            if len(files) >= 150:
                break
        if len(files) >= 150:
            break

    for path in files:
        img = tf.keras.utils.load_img(path, target_size=(h, w))
        img = tf.keras.utils.img_to_array(img).astype(np.float32) / 255.0 - 0.5
        yield [img[np.newaxis, ...]]


converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = rep_data
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.uint8
converter.inference_output_type = tf.float32

# ESP TFLM 目前不支持 Dense 的逐通道量化
converter._experimental_disable_per_channel_quantization_for_dense_layers = True

data = converter.convert()
with open(out_file, "wb") as f:
    f.write(data)

with open(head_file, "w", encoding="ascii") as f:
    f.write("// 96x96 uint8 model for ESP32 TFLite Micro\n")
    f.write("const unsigned char plant_model[] = {\n")
    for i in range(0, len(data), 12):
        part = ", ".join(f"0x{x:02x}" for x in data[i:i + 12])
        f.write(f"  {part},\n")
    f.write("};\n")
    f.write(f"const unsigned int plant_model_len = {len(data)};\n")

print(f"input={h}x{w}, model={len(data)} bytes")
