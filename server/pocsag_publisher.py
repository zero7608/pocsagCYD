#!/usr/bin/env python3
"""
POCSAG Publisher — RTL-SDR → multimon-ng → MQTT

Cycles through configured frequencies, decodes POCSAG 512/1200/2400,
and publishes each page as JSON to the configured MQTT topic.
"""

import json
import os
import re
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
import yaml

CONFIG_FILE = os.environ.get("CONFIG_FILE", "config.yaml")

POCSAG_RE = re.compile(
    r"POCSAG(?P<baud>\d+):\s+Address:\s*(?P<capcode>\d+)\s+"
    r"Function:\s*(?P<func>\d)\s+"
    r"(?P<type>Alpha|Numeric):\s*(?P<msg>.*)"
)


def load_config():
    with open(CONFIG_FILE) as f:
        cfg = yaml.safe_load(f) or {}

    # MQTT credentials come from environment (set via .env / docker-compose env_file).
    # config.yaml mqtt block is optional and takes lower priority.
    mqtt_defaults = cfg.get("mqtt", {})
    cfg["mqtt"] = {
        "broker":    os.environ.get("MQTT_BROKER",    mqtt_defaults.get("broker",    "localhost")),
        "port":      int(os.environ.get("MQTT_PORT",  mqtt_defaults.get("port",      1883))),
        "username":  os.environ.get("MQTT_USER",      mqtt_defaults.get("username",  "")),
        "password":  os.environ.get("MQTT_PASS",      mqtt_defaults.get("password",  "")),
        "topic":     os.environ.get("MQTT_TOPIC",     mqtt_defaults.get("topic",     "pocsag/rx")),
        "client_id": os.environ.get("MQTT_CLIENT_ID", mqtt_defaults.get("client_id", "pocsag-publisher")),
    }
    return cfg


def make_mqtt_client(cfg):
    mc = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=cfg["mqtt"].get("client_id", "pocsag-publisher"),
    )
    mc.username_pw_set(cfg["mqtt"]["username"], cfg["mqtt"]["password"])

    def on_connect(client, userdata, flags, rc, props):
        if rc == 0:
            print(f"[MQTT] connected to {cfg['mqtt']['broker']}", flush=True)
        else:
            print(f"[MQTT] connection failed rc={rc}", flush=True)

    def on_disconnect(client, userdata, flags, rc, props):
        print(f"[MQTT] disconnected rc={rc}", flush=True)

    mc.on_connect = on_connect
    mc.on_disconnect = on_disconnect
    return mc


def connect_mqtt(mc, cfg):
    while True:
        try:
            mc.connect(cfg["mqtt"]["broker"], cfg["mqtt"]["port"], keepalive=60)
            mc.loop_start()
            return
        except Exception as e:
            print(f"[MQTT] connect error: {e} — retry in 10s", flush=True)
            time.sleep(10)


def parse_line(line):
    m = POCSAG_RE.search(line)
    if not m:
        return None
    msg = m.group("msg").strip()
    # multimon-ng pads short messages with <NUL> — strip control chars
    msg = re.sub(r"[\x00-\x1f\x7f]", "", msg).strip()
    return {
        "baud": int(m.group("baud")),
        "capcode": m.group("capcode").lstrip("0") or "0",
        "func": int(m.group("func")),
        "type": m.group("type").lower(),
        "msg": msg,
    }


def run_frequency(freq_mhz, cfg, mc, stop_event):
    gain = cfg.get("rtl_gain", 40)
    ppm = cfg.get("rtl_ppm", 0)
    dev = cfg.get("rtl_device_index", 0)
    capcode_filter = set(cfg.get("capcode_filter", []))
    topic = cfg["mqtt"]["topic"]

    rtl_cmd = [
        "rtl_fm",
        "-d", str(dev),
        "-f", f"{freq_mhz}M",
        "-s", "22050",
        "-g", str(gain),
        "-p", str(ppm),
        "-E", "dc",
        "-",
    ]
    mmng_cmd = [
        "multimon-ng",
        "-t", "raw",
        "-a", "POCSAG512",
        "-a", "POCSAG1200",
        "-a", "POCSAG2400",
        "-f", "alpha",
        "-",
    ]

    print(f"[SDR] tuning {freq_mhz} MHz  gain={gain}  ppm={ppm}", flush=True)

    try:
        rtl = subprocess.Popen(rtl_cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        mmng = subprocess.Popen(
            mmng_cmd, stdin=rtl.stdout, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True,
        )
        rtl.stdout.close()

        while not stop_event.is_set():
            line = mmng.stdout.readline()
            if not line:
                break
            line = line.rstrip()
            if not line:
                continue

            page = parse_line(line)
            if page is None:
                continue
            if capcode_filter and page["capcode"] not in capcode_filter:
                continue
            if not page["msg"]:
                continue

            page["freq_mhz"] = freq_mhz
            page["ts"] = int(datetime.now(timezone.utc).timestamp())
            page["ts_iso"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

            payload = json.dumps(page)
            mc.publish(topic, payload, qos=0, retain=False)
            print(f"[PAGE] {page['ts_iso']}  cap={page['capcode']}  {page['msg'][:80]}", flush=True)

    except Exception as e:
        print(f"[SDR] error on {freq_mhz} MHz: {e}", flush=True)
    finally:
        try:
            mmng.terminate()
        except Exception:
            pass
        try:
            rtl.terminate()
        except Exception:
            pass


def main():
    cfg = load_config()
    freqs = cfg.get("frequencies", [])
    dwell = cfg.get("dwell_seconds", 30)

    if not freqs:
        print("ERROR: no frequencies configured", flush=True)
        sys.exit(1)

    mc = make_mqtt_client(cfg)
    connect_mqtt(mc, cfg)

    print(f"[POCSAG] monitoring {len(freqs)} freq(s), {dwell}s dwell each", flush=True)

    idx = 0
    while True:
        freq = freqs[idx % len(freqs)]
        stop = threading.Event()
        t = threading.Thread(target=run_frequency, args=(freq, cfg, mc, stop), daemon=True)
        t.start()
        time.sleep(dwell)
        stop.set()
        t.join(timeout=5)
        idx += 1


if __name__ == "__main__":
    main()
