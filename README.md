# Common ESP-NOW Framework

This is a reusable, scalable framework for ESP32-based Home Automation using a Transparent Gateway architecture.

## Architecture
- **Master Gateway**: A "dumb" Hex bridge. Receives *any* ESP-NOW packet and forwards it to Serial as Hex. Sends Serial Hex commands to ESP-NOW. **No code changes required for new devices.**
- **Pi Controller**: Python script that manages the logic, decodes Hex packets, and handles the Watchdog.
- **Slave Template**: A starting point for any new sensor/actuator.

## Directory Structure
```
common_esp_now_framework/
├── master_gateway/       # Flash this to your Master ESP32 (Hub)
│   └── master_gateway.ino
├── slave_template/       # Copy this for each new sensor (Remote, Temp, etc.)
│   └── slave_template.ino
└── pi_controller/        # Run this on your Raspberry Pi
    └── controller.py
```

## How to use for a New Project
1.  **Master**: Flash `master_gateway.ino` to one ESP32. Connect it to Pi via USB.
2.  **Slave**: Copy `slave_template` to a new folder (e.g., `my_new_sensor`).
    - Modify the struct to fit your data.
    - Flash it.
3.  **Pi**: Update `controller.py` to handle the new device's Hex structure (if different from default).
