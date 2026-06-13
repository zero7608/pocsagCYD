# POCSAG on a Cheap Yellow Display

### Someone is still paging. That someone is your county EMS.

---

Pagers are not dead. Your local fire department, EMS, and probably a dozen other agencies you've never thought about are broadcasting their dispatches in plaintext over radio frequencies anyone can receive. No encryption. No subscription. No login required.

I bought two of these displays. The first one runs ESP32 WiFi Marauder. The second was too good a deal to pass up and sat in a drawer until now. It has a job.

---

## What This Does

It decodes live POCSAG pager traffic off the air using an RTL-SDR, formats it, and pushes it over MQTT to a small display that looks like a prop from a 1994 police drama. New pages come in at the top. The RGB LED flashes. It sits on your desk and quietly reports everything happening in your county.

---

## What Is POCSAG?

POCSAG is a pager protocol from 1978. Most people forgot pagers existed sometime around the year 2000, but emergency services never stopped using them. They work when cell towers don't, the coverage is excellent, and replacing a statewide pager network is expensive enough that nobody in local government wants to be the one to sign that check.

So they're still out there, broadcasting, in plaintext, at known frequencies. A $25 RTL-SDR dongle and a Raspberry Pi is all you need to receive every fire dispatch, EMS call, and hospital page in your area. This project puts that feed on a screen.

---

## Hardware

**Server side (your homelab, a Pi, whatever):**
- RTL-SDR v5 or any RTL2832U-based dongle
- Docker and Docker Compose

**Display side:**
- AITRIP ESP32-2432S024, also known as the Cheap Yellow Display (CYD)
  - 2.4" ILI9341 TFT with resistive touch
  - Built-in ESP32-WROOM-32 and RGB LED
  - About $15 on Amazon
- USB-C cable

---

## How It Works

```
RTL-SDR -> rtl_fm -> multimon-ng -> Python parser -> MQTT -> CYD
```

`rtl_fm` tunes to a configured frequency and pipes raw audio. `multimon-ng` decodes POCSAG 512, 1200, and 2400 baud from that audio. The Python publisher parses the output, formats it as JSON, and pushes it to your MQTT broker. The CYD subscribes to that topic and displays each page as it arrives.

The publisher cycles through multiple frequencies with a configurable dwell time, so you can cover your whole county without needing multiple dongles.

---

## Setup

### 1. Find Your Frequencies

Look up your county on [RadioReference](https://www.radioreference.com) and find the POCSAG dispatch frequencies. They are usually in the 150 to 170 MHz range. Drop them into `server/config.yaml`.

### 2. Configure Credentials

```bash
cp server/.env.example server/.env
```

Fill in your MQTT broker details. The `.env` file is gitignored so your credentials stay on your machine.

```bash
cp firmware/src/secrets.h.example firmware/src/secrets.h
```

Same deal for the firmware. Add your WiFi credentials and MQTT broker info.

### 3. Start the Server

Two things to sort out on the host before the container will work.

**Blacklist the kernel DVB driver.**
Linux ships a DVB driver that claims the RTL-SDR the moment it's plugged in. Since we're running rtl-sdr inside Docker rather than installing it on the host, nothing sets up the blacklist automatically. This applies to any Linux — not just Raspberry Pi:

```bash
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-rtl.conf
sudo reboot
```

Skip this and rtl_fm will fail with "kernel driver active" regardless of what else you do.

**Set up the udev rule.**
Linux assigns USB devices restrictive permissions by default, which means only root can talk to the dongle. A udev rule fixes that when the RTL-SDR is plugged in so the container can reach it without running as root:

```bash
sudo sh -c 'echo SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"0bda\", MODE=\"0664\", GROUP=\"plugdev\" > /etc/udev/rules.d/rtl-sdr.rules'
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG plugdev $USER
```

The vendor ID `0bda` is Realtek, which makes the RTL2832U chip inside every RTL-SDR dongle.

Then plug in the RTL-SDR and start the container:

```bash
cd server
docker compose up -d --build
```

Check the logs after it starts:

```bash
docker compose logs -f
```

`[MQTT] connected` and `[SDR] tuning X MHz` means it is working. Pages show up as `[PAGE]` lines when traffic hits the frequency.

### 4. Flash the Display

```bash
cd firmware
pio run --target upload
```

The display boots, connects to WiFi, and starts listening. The status bar at the bottom shows WiFi and MQTT state. When a page comes in the LED flashes and the entry appears at the top of the list.

---

## The Display

Portrait, 240x320, green on black because it felt right.

- **Header**: frequency being monitored and a live clock
- **Page list**: newest at the top, shows time, capcode, and message text
- **Status bar**: WiFi and MQTT connection state plus an unread count
- **Touch**: upper half scrolls up, lower half scrolls down

The last 10 pages are kept in a ring buffer. The newest entry shows in amber until a new page arrives or you scroll.

---

## Filtering

If you only want specific capcodes, like just your local fire district, add them to `capcode_filter` in `config.yaml`. Leave it empty to receive everything.

---

## Running on a Raspberry Pi

A Pi 4 works well for this. Raspberry Pi OS Lite 64-bit is the right image — no desktop needed.

The DVB blacklist and udev rule from the setup section above apply here too. Do those first, before the dongle touches the Pi.

**Install Docker from the official script, not apt.**
The version in the Pi OS repos is usually too old:

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
```

Log out and back in after.

**Verify the dongle works on the host before starting Docker.**

```bash
sudo apt install rtl-sdr
rtl_test -t
```

If it finds the device and reports samples, Docker will be able to reach it. If it fails here it will fail in the container too.

**The Docker image builds natively on arm64.** No changes needed to the Dockerfile.

---

*thinkleet.net*
