# ESP-NOW Framework (UART to Pi)

Reusable framework for ESP32 devices talking to a Raspberry Pi via **ESP-NOW → Master Gateway → UART**. No WiFi SSID/password needed on slaves.

## Why ESP-NOW here?

- **No SSID**: ESP-NOW uses WiFi in STA mode but does **not** connect to a router. Slaves never call `WiFi.begin(SSID, password)`, so there is nothing to hardcode for production.
- **Stream + stop**: Slaves stream data (e.g. RFID EPC) to the Pi; when the Pi receives a tag it can send "stop all readers" over the same link.

## Architecture

| Component | Role |
|----------|------|
| **Master Gateway** | ESP32 connected to Pi via USB. Forwards ESP-NOW ↔ Serial hex. Sends HEARTBEAT. |
| **Pi** | Runs logic, decodes packets, sends TX commands (e.g. stop/start). |
| **Slaves** | ESP32s (sensors, RFID readers). ESP-NOW only; no MQTT, no WiFi login. |

## Directory structure

```
esp_now_framework_uart/
├── master_gateway/          # Flash to Master ESP32 (USB to Pi)
│   └── master_gateway.ino
├── rfid_slave/              # UHF RFID reader over ESP-NOW (no SSID)
│   └── uhf_esp_now_slave.ino
├── slave_template/           # Generic sensor template
│   └── slave_template.ino
├── pi_controller/
│   ├── controller.py        # Standalone CLI for testing
│   └── esp_now_serial_bridge.py  # Used by main app handler
└── README.md
```

## Connecting ESP Master to Pi (no extra GPIO pins if using USB)

The Master talks to the Pi over **serial (UART)**. You do **not** need any of the Pi’s GPIO pins if you use USB.

### Option 1: USB (recommended) — no Pi GPIO used

- Connect the ESP Master board to the Pi with a **USB cable** (most ESP32 dev boards have built‑in USB‑serial, e.g. CP2102/CH340).
- The Pi sees it as a serial device: **`/dev/ttyUSB0`** (or `ttyUSB1` if another USB serial device is already plugged in) or **`/dev/ttyACM0`** on some boards.
- Set `esp_now_serial_port` in `config.json` to that device (e.g. `"/dev/ttyUSB0"`).
- **No Pi GPIO pins are used**; your existing pins (button 4, lights 17/27, indicators 5/6/23/24, buzzer, etc.) stay as they are.

### Option 2: Pi hardware UART (GPIO 14 & 15)

Use this only if you cannot use USB (e.g. no free USB port or a bare ESP with only UART pins).

| Pi (BCM) | Function | Connect to ESP Master |
|----------|----------|------------------------|
| **GPIO 14** | UART Tx  | → ESP **Rx** (receive) |
| **GPIO 15** | UART Rx  | ← ESP **Tx** (transmit) |
| **GND**      | Ground   | → ESP **GND**          |

- These are the **primary UART** pins and are typically free (not used by your current relays/button/indicators).
- On the Pi, the device is usually **`/dev/ttyS0`** (Pi 3/4/5) or **`/dev/ttyAMA0`** on older Pi. Enable serial in `raspi-config`: Interface Options → Serial Port → enable serial hardware, and disable login shell over serial if you want the port for the Master only.
- Set `esp_now_serial_port` in `config.json` to `"/dev/ttyS0"` (or the device you get after enabling UART).
- Use 3.3 V logic only; do not connect the ESP to 5 V logic.

**Summary:** Prefer **USB** and `esp_now_serial_port: "/dev/ttyUSB0"` so no Pi pins are used. Use GPIO 14/15 only when USB is not an option.

---

## RFID flow (no SSID)

1. **Master**: Flash `master_gateway.ino`. Connect to Pi via USB (e.g. `/dev/ttyUSB0`) or UART (GPIO 14/15 → `/dev/ttyS0`).
2. **RFID readers**: Flash `rfid_slave/uhf_esp_now_slave.ino` to each ESP32. Set `masterMAC[]` to the Master’s MAC. No SSID or MQTT.
3. **Pi**: In `config.json` (in project root) set:
   - `rfid_transport`: `"esp_now"` **or** set `esp_now_reader_macs`
   - `esp_now_serial_port`: e.g. `"/dev/ttyUSB0"` (Master’s USB serial)
   - `esp_now_reader_macs`: list of reader MACs for "stop all", e.g. `["F0:24:F9:0D:90:A4"]`
   - Optional: `esp_now_tag_timeout` (default 30s), `esp_now_inactivity_timeout` (default 1200s)

   Example snippet:
   ```json
   "rfid_transport": "esp_now",
   "esp_now_serial_port": "/dev/ttyUSB0",
   "esp_now_reader_macs": ["F0:24:F9:0D:90:A4"]
   ```

Flow: Readers stream EPC to Pi. On first tag received, Pi sends STOP to all readers (no ACK required). START is sent when a new read is triggered; inactivity timeout sends STOP again.

## New (non-RFID) project

1. Flash **Master** and connect to Pi.
2. Copy `slave_template` to a new folder, change the payload struct, flash.
3. In `controller.py` (or your app) handle the new hex format and add any commands (e.g. TX to slaves).

---

## If you stay on MQTT: avoiding hardcoded SSID

If you keep using WiFi + MQTT (e.g. `esp_rfid/uhf.ino`) and want to avoid hardcoding SSID in production:

1. **WiFiManager (ESP32)**  
   First boot: ESP32 starts in AP mode; you connect with a phone/laptop, open the captive portal, enter SSID/password. They are saved in NVS and used on later boots. No reflash for new networks.

2. **NVS / provisioning**  
   Store SSID and password in NVS (e.g. via a one-time BLE or serial provisioning tool). Your `.ino` reads from NVS instead of `const char *WIFI_SSID`.

3. **BLE provisioning (Espressif)**  
   Use Espressif’s WiFi provisioning over BLE so users configure the device from a phone app; credentials are written to NVS.

ESP-NOW avoids this entirely because it does not join an AP.
