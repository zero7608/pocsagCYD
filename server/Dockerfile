FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    rtl-sdr \
    multimon-ng \
    python3 \
    python3-pip \
    python3-yaml \
    udev \
    && pip3 install --no-cache-dir --break-system-packages paho-mqtt \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY pocsag_publisher.py config.yaml ./

CMD ["python3", "pocsag_publisher.py"]
