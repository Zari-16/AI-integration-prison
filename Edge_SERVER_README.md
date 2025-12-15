# Edge Server for Smart Prison (Raspberry Pi)
This repository contains the Edge ingestion & AI inference service that:
- Receives JSON telemetry from Arduino MKR1010 devices (/ingest).
- Buffers per-device windows, extracts features.
- Runs inference (TFLite autoencoder or fallback IsolationForest).
- Writes sensor windows and ai_alerts to InfluxDB v2.
- Publishes alerts to MQTT (optional).
- Exposes minimal admin endpoints.

Security:
- Configure an API key (EDGE_API_KEY) and set full Influx token in config.
- Run behind TLS (nginx reverse proxy or uvicorn with TLS).

See config.example.yaml for configuration options.
```


```text name=EDGE_SERVER/requirements.txt
fastapi
uvicorn[standard]
tflite-runtime    # or 'tensorflow' if using full TF on dev machine; on Pi use prebuilt tflite-runtime wheel
numpy
pandas
scikit-learn
influxdb-client
paho-mqtt
pyyaml
```

````python name=EDGE_SERVER/config.example.yaml
# Example Edge server configuration
server:
  host: "0.0.0.0"
  port: 5000
  api_key: "edge_api_key_here"

influx:
  url: "http://10.197.56.165:8086"   # change to your Influx host
  token: "qjY1cFVn4YcnpI3nmYZT454K9HHjDFaEKJ3ftgQqbg3sCkZcHKv8dD7ZHGhiOxm4cmYQ4viEEbNqQ_l8yiv5OA=="
  org: "MDX"
  bucket: "Prison_Data"

mqtt:
  enabled: false
  host: "localhost"
  port: 1883
  topic_prefix: "smartprison"

ml:
  window_size: 12        # number of samples in a window (12*5s = 60s)
  sample_interval_s: 5   # seconds between samples from devices
  tflite_model_path: "models/autoencoder.tflite" # set to empty if not present
  anomaly_threshold: 0.5 # threshold on reconstruction error or model-specific score
  fallback_isolationforest: true
```


```python name=EDGE_SERVER/app/influx_client.py
# Simple InfluxDB v2 helper using influxdb-client
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

class InfluxWriter:
    def __init__(self, url, token, org, bucket):
        self.client = InfluxDBClient(url=url, token=token, org=org)
        self.write_api = self.client.write_api(write_options=SYNCHRONOUS)
        self.bucket = bucket
        self.org = org

    def write_sensor_window(self, tags: dict, fields: dict, timestamp=None, measurement="sensor_windows"):
        p = Point(measurement)
        for k,v in tags.items(): p.tag(k, v)
        for k,v in fields.items():
            if isinstance(v, (int,)) : p.field(k, int(v))
            elif isinstance(v, (float,)) : p.field(k, float(v))
            else: p.field(k, str(v))
        if timestamp: p.time(timestamp)
        self.write_api.write(bucket=self.bucket, org=self.org, record=p)

    def write_ai_alert(self, tags: dict, fields: dict, timestamp=None, measurement="ai_alerts"):
        self.write_sensor_window(tags, fields, timestamp, measurement)
```

```python name=EDGE_SERVER/app/ml_engine.py
# ML engine: loads tflite model if available, else provides a fallback IsolationForest instance
import os
import numpy as np
from sklearn.ensemble import IsolationForest
import joblib

try:
    import tflite_runtime.interpreter as tflite
    TFLITE_AVAILABLE = True
except Exception as e:
    TFLITE_AVAILABLE = False

class MLEngine:
    def __init__(self, config):
        self.config = config
        self.window_size = config['ml']['window_size']
        self.threshold = config['ml']['anomaly_threshold']
        self.tflite_path = config['ml'].get('tflite_model_path', '')
        self.tflite = None
        self.iforest = None
        self.model_version = "none"
        if self.tflite_path and os.path.exists(self.tflite_path) and TFLITE_AVAILABLE:
            self._load_tflite(self.tflite_path)
        elif config['ml'].get('fallback_isolationforest', True):
            # Try to load a persisted isolation forest model (trained offline)
            if os.path.exists("models/iforest.joblib"):
                self.iforest = joblib.load("models/iforest.joblib")
                self.model_version = "iforest-loaded"
            else:
                # create a default random iforest (not ideal; better to train offline)
                self.iforest = IsolationForest(contamination=0.01, random_state=42)
                self.model_version = "iforest-default"

    def _load_tflite(self, path):
        try:
            self.tflite = tflite.Interpreter(model_path=path)
            self.tflite.allocate_tensors()
            self.model_version = "tflite:" + os.path.basename(path)
            print("[ML] Loaded TFLite model:", self.model_version)
        except Exception as e:
            print("[ML] Failed to load TFLite:", e)
            self.tflite = None

    def run_tflite(self, input_array: np.ndarray):
        # input_array shape must match model input
        input_details = self.tflite.get_input_details()
        output_details = self.tflite.get_output_details()
        # ensure dtype and shape
        arr = input_array.astype(np.float32)
        self.tflite.set_tensor(input_details[0]['index'], arr)
        self.tflite.invoke()
        out = self.tflite.get_tensor(output_details[0]['index'])
        return out

    def score_window(self, feature_vec: np.ndarray):
        # feature_vec is 1D or 2D (1, n)
        if self.tflite:
            try:
                out = self.run_tflite(feature_vec.reshape(1, -1))
                # for autoencoder: reconstruction error or returned score
                score = float(np.mean(out))
                return score, self.model_version
            except Exception as e:
                print("[ML] tflite inference error:", e)
        if self.iforest is not None:
            try:
                # IsolationForest: decision_function (higher => normal), invert for anomaly
                s = self.iforest.decision_function(feature_vec.reshape(1, -1))[0]
                # map to anomaly score range 0..1
                score = float(1.0 - (s + 0.5))  # rough scaling
                return score, self.model_version
            except Exception as e:
                print("[ML] iforest scoring error:", e)
        # If no model, return 0 (normal)
        return 0.0, self.model_version
```

```python name=EDGE_SERVER/app/main.py
# FastAPI Edge ingestion + AI inference server
import uvicorn
import yaml
import time
import asyncio
from fastapi import FastAPI, Request, HTTPException, Header
from collections import deque, defaultdict
import numpy as np
import threading
import paho.mqtt.client as mqtt

from .influx_client import InfluxWriter
from .ml_engine import MLEngine

app = FastAPI()
config = yaml.safe_load(open("config.yaml"))
API_KEY = config['server']['api_key']
INFLUX = config['influx']
MQTT_CFG = config.get('mqtt', {})
ML_CFG = config.get('ml', {})

# initialize clients
influx = InfluxWriter(INFLUX['url'], INFLUX['token'], INFLUX['org'], INFLUX['bucket'])
ml = MLEngine(config)

# MQTT optional
mqtt_client = None
if MQTT_CFG.get('enabled', False):
    mqtt_client = mqtt.Client()
    if MQTT_CFG.get('username'):
        mqtt_client.username_pw_set(MQTT_CFG.get('username'), MQTT_CFG.get('password'))
    mqtt_client.connect(MQTT_CFG['host'], MQTT_CFG.get('port', 1883))

# device buffers
WINDOW = ML_CFG.get('window_size', 12)
buffers = defaultdict(lambda: deque(maxlen=WINDOW))
lock = threading.Lock()

def extract_features(window_list):
    # window_list: list of telemetry dicts length WINDOW
    import numpy as np
    vib = np.array([int(x.get('vib',0)) for x in window_list])
    gas = np.array([int(x.get('gas',0)) for x in window_list])
    pir = np.array([int(x.get('pir',0)) for x in window_list])
    temp = np.array([float(x.get('temp',0)) for x in window_list])
    people = np.array([int(x.get('people',0)) for x in window_list])
    feat = [
        float(vib.mean()), float(vib.std()), float(vib.max()), float(vib.min()),
        float(gas.mean()), float(gas.std()), float(gas.max()),
        float(pir.sum()), float(temp.mean()), float(people.max())
    ]
    return np.array(feat, dtype=np.float32)

@app.post("/ingest")
async def ingest(request: Request, authorization: str = Header(None)):
    # simple bearer auth
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Missing Authorization")
    token = authorization.split(" ",1)[1]
    if token != API_KEY:
        raise HTTPException(status_code=403, detail="Invalid API key")
    payload = await request.json()
    device = payload.get('device','unknown')
    timestamp = payload.get('ts', int(time.time()))
    # append to buffer
    with lock:
        buffers[device].append(payload)
        if len(buffers[device]) == WINDOW:
            window = list(buffers[device])
            feat = extract_features(window)
            score, model_version = ml.score_window(feat)
            tags = {"device": device}
            fields = {
                "vib_mean": float(feat[0]), "vib_std": float(feat[1]),
                "gas_mean": float(feat[4]), "pir_sum": int(feat[7]),
                "temp_mean": float(feat[8]), "people_max": int(feat[9]),
                "ai_score": float(score), "model_version": model_version
            }
            # write full window summary to Influx
            influx.write_sensor_window(tags, fields, timestamp=timestamp)
            # check threshold
            if score >= ML_CFG.get('anomaly_threshold', 0.5):
                alert_fields = {"score": float(score), "model_version": model_version, "confirmed": 0}
                influx.write_ai_alert(tags, alert_fields, timestamp=timestamp)
                # publish via MQTT if enabled
                if mqtt_client:
                    mqtt_topic = f"{MQTT_CFG.get('topic_prefix','smartprison')}/alerts/{device}"
                    payload_alert = {"device": device, "ts": timestamp, "score": float(score), "model_version": model_version}
                    mqtt_client.publish(mqtt_topic, str(payload_alert))
    return {"status":"ok"}

@app.get("/model")
def model_info():
    return {"model_version": ml.model_version, "window_size": WINDOW}

@app.post("/alert/confirm")
async def confirm_alert(info: Request, authorization: str = Header(None)):
    if not authorization or not authorization.startswith("Bearer "): raise HTTPException(status_code=401)
    token = authorization.split(" ",1)[1]
    if token != API_KEY: raise HTTPException(status_code=403)
    payload = await info.json()
    device = payload.get('device'); ts = payload.get('ts'); confirmed = payload.get('confirmed',1)
    # write confirmation back to influx as field
    tags = {"device": device}
    fields = {"confirmed": int(confirmed), "confirmer":"operator"}
    influx.write_ai_alert(tags, fields, timestamp=ts)
    return {"status":"confirmed"}

if __name__ == "__main__":
    # run with uvicorn
    uvicorn.run("app.main:app", host=config['server']['host'], port=config['server']['port'], reload=False)
