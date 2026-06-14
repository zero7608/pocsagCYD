#!/usr/bin/env python3
"""
POCSAG Publisher — RTL-SDR → multimon-ng → MQTT

Cycles through configured frequencies, decodes POCSAG 512/1200/2400,
and publishes each page as JSON to the configured MQTT topic.
"""

import json
import math
import os
import queue
import re
import struct
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


SAMPLE_RATE = 22050  # rtl_fm output sample rate (Hz)


def audio_rms(data):
    """RMS amplitude of raw signed 16-bit PCM bytes from rtl_fm."""
    n = len(data) // 2
    if n == 0:
        return 0.0
    total = sum(struct.unpack_from("<h", data, i * 2)[0] ** 2 for i in range(n))
    return math.sqrt(total / n)


def rtl_cmd(freq_mhz, cfg):
    return [
        "rtl_fm",
        "-d", str(cfg.get("rtl_device_index", 0)),
        "-f", f"{freq_mhz}M",
        "-s", str(SAMPLE_RATE),
        "-g", str(cfg.get("rtl_gain", 0)),
        "-p", str(cfg.get("rtl_ppm", 0)),
        "-E", "dc",
        "-",
    ]


def scan_level(freq_mhz, cfg, duration=0.4):
    """Tune to freq for `duration` seconds and return audio RMS. 0.0 on error."""
    # Bytes for `duration` seconds of signed 16-bit mono audio, plus a discard
    # prefix for AGC settle time (~0.1s).
    discard = int(SAMPLE_RATE * 2 * 0.1)
    read_len = int(SAMPLE_RATE * 2 * duration)
    try:
        proc = subprocess.Popen(rtl_cmd(freq_mhz, cfg),
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        proc.stdout.read(discard)          # discard AGC settle
        data = proc.stdout.read(read_len)
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
        return audio_rms(data)
    except Exception as e:
        print(f"[SCAN] error {freq_mhz} MHz: {e}", flush=True)
        return 0.0


def decode_locked(freq_mhz, cfg, mc):
    """
    Tune to freq_mhz, decode POCSAG, publish to MQTT.
    Returns when silence_timeout seconds pass without any multimon-ng output.
    """
    capcode_filter = set(cfg.get("capcode_filter", []))
    topic = cfg["mqtt"]["topic"]
    silence_timeout = cfg.get("silence_timeout", 8)

    mmng_args = [
        "multimon-ng", "-t", "raw",
        "-a", "POCSAG512", "-a", "POCSAG1200", "-a", "POCSAG2400",
        "-f", "alpha", "-",
    ]

    print(f"[LOCK] {freq_mhz} MHz — decoding", flush=True)
    try:
        rtl = subprocess.Popen(rtl_cmd(freq_mhz, cfg),
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        mmng = subprocess.Popen(mmng_args, stdin=rtl.stdout,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        rtl.stdout.close()

        # Read multimon-ng output in a background thread; main thread enforces
        # the silence timeout via queue.get(timeout=...).
        line_q = queue.Queue()

        def reader():
            for line in mmng.stdout:
                line_q.put(line)
            line_q.put(None)

        threading.Thread(target=reader, daemon=True).start()

        last_activity = time.monotonic()
        while True:
            try:
                line = line_q.get(timeout=1.0)
            except queue.Empty:
                if time.monotonic() - last_activity >= silence_timeout:
                    print(f"[LOCK] {silence_timeout}s quiet on {freq_mhz} MHz — resuming scan",
                          flush=True)
                    break
                continue

            if line is None:
                break

            page = parse_line(line.rstrip())
            if not page or not page["msg"]:
                continue
            if capcode_filter and page["capcode"] not in capcode_filter:
                continue

            page["freq_mhz"] = freq_mhz
            page["ts"] = int(datetime.now(timezone.utc).timestamp())
            page["ts_iso"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

            mc.publish(topic, json.dumps(page), qos=0, retain=False)
            print(f"[PAGE] {page['ts_iso']}  cap={page['capcode']}  {page['msg'][:80]}",
                  flush=True)
            last_activity = time.monotonic()

    except Exception as e:
        print(f"[LOCK] error on {freq_mhz} MHz: {e}", flush=True)
    finally:
        for proc in (mmng, rtl):
            try:
                proc.terminate()
            except Exception:
                pass


def main():
    cfg = load_config()
    freqs = cfg.get("frequencies", [])
    threshold = cfg.get("signal_threshold", 800)
    scan_dwell = cfg.get("scan_dwell_seconds", 0.4)

    if not freqs:
        print("ERROR: no frequencies configured", flush=True)
        sys.exit(1)

    mc = make_mqtt_client(cfg)
    connect_mqtt(mc, cfg)

    print(f"[POCSAG] scanning {len(freqs)} freq(s)  threshold={threshold}  "
          f"dwell={scan_dwell}s  silence={cfg.get('silence_timeout', 8)}s", flush=True)

    idx = 0
    while True:
        freq = freqs[idx % len(freqs)]
        rms = scan_level(freq, cfg, scan_dwell)
        print(f"[SCAN] {freq} MHz  RMS={rms:.0f}", flush=True)

        if rms >= threshold:
            decode_locked(freq, cfg, mc)
            # Re-scan the same frequency first — back-to-back pages are common
        else:
            idx += 1


if __name__ == "__main__":
    main()
