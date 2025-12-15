"""
Convert Keras model to TFLite (float or quantized).
Run on a machine with TensorFlow installed (training machine), then copy tflite to Pi.
"""
import tensorflow as tf
model = tf.keras.models.load_model("models/autoencoder.h5")
converter = tf.lite.TFLiteConverter.from_keras_model(model)
# For size/speed, enable post-training quantization
converter.optimizations = [tf.lite.Optimize.DEFAULT]
# Use representative dataset if you want full integer quantization:
def representative_data_gen():
    import numpy as np
    data = np.load("data/windows_features.npy")
    for i in range(100):
        yield [data[i:i+1].astype("float32")]
converter.representative_dataset = representative_data_gen
tflite_model = converter.convert()
open("models/autoencoder.tflite", "wb").write(tflite_model)
print("Wrote models/autoencoder.tflite")
