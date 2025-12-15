"""
Outline training script for autoencoder on time-series windows.

- This script expects prepared CSV/NumPy windows (N x feature_len) from historical data.
- Trains a small dense autoencoder and saves as Keras HDF5. Then use convert_to_tflite.py to quantize and produce .tflite
"""
import numpy as np
from tensorflow.keras.models import Model
from tensorflow.keras.layers import Input, Dense
from tensorflow.keras.optimizers import Adam
from sklearn.model_selection import train_test_split

# Load your prepared features array: shape (n_windows, feature_dim)
# Example: data = np.load("windows_features.npy")
data = np.load("data/windows_features.npy")
X_train, X_val = train_test_split(data, test_size=0.2, random_state=42)

input_dim = X_train.shape[1]
inp = Input(shape=(input_dim,))
h = Dense(64, activation='relu')(inp)
h = Dense(32, activation='relu')(h)
z = Dense(16, activation='relu')(h)
h2 = Dense(32, activation='relu')(z)
h2 = Dense(64, activation='relu')(h2)
out = Dense(input_dim, activation='linear')(h2)
model = Model(inputs=inp, outputs=out)
model.compile(optimizer=Adam(1e-3), loss='mse')
model.fit(X_train, X_train, validation_data=(X_val, X_val), epochs=50, batch_size=64)
model.save("models/autoencoder.h5")
print("Saved Keras model -> models/autoencoder.h5")
# After this, run convert_to_tflite.py to produce tflite
