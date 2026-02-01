# Example run script (on Raspberry Pi)
# 1) pip install -r requirements.txt (use virtualenv)
# 2) edit config.yaml with real Influx + API key
# 3) run:
uvicorn app.main:app --host 0.0.0.0 --port 5000
# For production, run behind nginx with TLS or use systemd to manage uvicorn.
