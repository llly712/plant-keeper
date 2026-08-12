import sys
sys.path.insert(0, '.')
# Patch: add class_weight and tweak to the working train_model.py
import os, tensorflow as tf, numpy as np
os.environ['TF_CPP_MIN_LOG_LEVEL'] = '2'

data_path = 'data'
if not os.path.isdir('data'): data_path = '../data'

IMG_W, IMG_H = 256, 256
BATCH = 16
EPOCHS = 30
CLASSES = ['健康', '白粉病', '叶斑病', '锈病', '虫害']

from tensorflow.keras.preprocessing.image import ImageDataGenerator
datagen = ImageDataGenerator(
    rescale=1./255,
    rotation_range=30,
    width_shift_range=0.2,
    height_shift_range=0.2,
    shear_range=0.15,
    zoom_range=0.2,
    horizontal_flip=True,
    vertical_flip=True,
    brightness_range=[0.8, 1.2],
    validation_split=0.2
)
train_gen = datagen.flow_from_directory(data_path, target_size=(IMG_H,IMG_W), batch_size=BATCH, class_mode='categorical', subset='training', classes=CLASSES)
val_gen = datagen.flow_from_directory(data_path, target_size=(IMG_H,IMG_W), batch_size=BATCH, class_mode='categorical', subset='validation', classes=CLASSES)

model = tf.keras.Sequential([
    tf.keras.layers.InputLayer(shape=(IMG_H,IMG_W,3)),
    tf.keras.layers.Conv2D(16,3,padding='same',activation='relu'), tf.keras.layers.MaxPooling2D(),
    tf.keras.layers.Conv2D(32,3,padding='same',activation='relu'), tf.keras.layers.MaxPooling2D(),
    tf.keras.layers.Conv2D(64,3,padding='same',activation='relu'), tf.keras.layers.MaxPooling2D(),
    tf.keras.layers.Conv2D(128,3,padding='same',activation='relu'), tf.keras.layers.MaxPooling2D(),
    tf.keras.layers.GlobalAveragePooling2D(), tf.keras.layers.Dropout(0.3),
    tf.keras.layers.Dense(64,activation='relu'), tf.keras.layers.Dropout(0.3),
    tf.keras.layers.Dense(5,activation='softmax')
])
model.compile(optimizer='adam',loss='categorical_crossentropy',metrics=['accuracy'])
model.summary()

# class_weight for categorical
cw = {0:4.0, 1:0.3, 2:0.6, 3:0.4, 4:1.0}
print(f'权重: 健康x{cw[0]} 白粉x{cw[1]} 叶斑x{cw[2]} 锈病x{cw[3]} 虫害x{cw[4]}')

model.fit(train_gen, validation_data=val_gen, epochs=EPOCHS, class_weight=cw, verbose=2)
model.save('plant_model_w.h5')

def r():
    for _ in range(80): yield [np.random.rand(1,IMG_H,IMG_W,3).astype(np.float32)]
c=tf.lite.TFLiteConverter.from_keras_model(model);c.optimizations=[tf.lite.Optimize.DEFAULT];c.representative_dataset=r;c.target_spec.supported_ops=[tf.lite.OpsSet.TFLITE_BUILTINS_INT8];c.inference_input_type=tf.uint8;c.inference_output_type=tf.float32
t=c.convert()
open('plant_model_w.tflite','wb').write(t)
print(f'model: {len(t)} bytes')
