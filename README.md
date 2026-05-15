# DAWKnob IDF 01

ESP-IDF conversion of the [DawKnob_03](../DawKnob_03) Arduino project.

A BLE HID mouse device built on ESP32 that maps a rotary encoder to mouse scroll movement, useful as a DAW parameter knob. Includes WiFi + OTA firmware update capability.

## Hardware

| Function | GPIO |
|---|---|
| Rotary encoder CLK (A) | 33 |
| Rotary encoder DT (B) | 32 |
| RE push button | 25 |
| WiFi/OTA trigger button | 27 |
| Right-mode button | 26 |
| Sensitivity potentiometer | 35 (ADC1_CH7) |
| Internal LED | 2 |
| Bluetooth status LED | 21 |
| WiFi status LED | 18 |
| Active mode LED | 19 |

## Framework

- **ESP-IDF v5.5.2** (Bluedroid BLE stack)
- Replaces Arduino `BleMouse` library with ESP-IDF BLE HID profile
- Replaces `AsyncWebServer` + `AsyncElegantOTA` with `esp_http_server` + `esp_ota_ops`

## First-time Setup

### 1. Activate the ESP-IDF environment

This must be done in every new terminal session before using `idf.py`:

```bash
. /Volumes/M2_2TB_Thunder/esp/v5.5.2/esp-idf/export.sh
```

To avoid repeating this, add a shortcut alias to `~/.zshrc`:

```bash
echo 'alias get_idf=". /Volumes/M2_2TB_Thunder/esp/v5.5.2/esp-idf/export.sh"' >> ~/.zshrc
```

Then just run `get_idf` at the start of each session.

### 2. Configure WiFi credentials

```bash
cd /Volumes/M2_2TB_Thunder/Dev/MCs/ESP32/DAWKnob_IDF_01
idf.py menuconfig
# → DAWKnob Configuration → set SSID, password, static IP
# Press S to save, Q to quit
```

### 3. Build

```bash
idf.py build
```

The compiled binary will be at `build/DAWKnob_IDF_01.bin`.

### 4. Find your serial port

Plug in the ESP32 via USB, then:

```bash
ls /dev/cu.usb*
# or
ls /dev/cu.wchusbserial*
```

Use the port name shown in the next step.

### 5. Flash and monitor

```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

Replace `/dev/cu.usbserial-XXXX` with your actual port.  
Press `Ctrl+]` to exit the monitor.

To flash and monitor in separate steps:

```bash
idf.py -p /dev/cu.usbserial-XXXX flash
idf.py -p /dev/cu.usbserial-XXXX monitor
```

### 6. VS Code

Open this folder in VS Code. The ESP-IDF extension will pick up `settings.json` automatically.
Update `idf.port` in `.vscode/settings.json` to match your serial port.  
Use `Ctrl+E B` to build, `Ctrl+E F` to flash, `Ctrl+E M` to open the monitor.

## OTA Updates

OTA lets you flash new firmware over WiFi without a USB cable.

### 1. Build the new firmware

```bash
. /Volumes/M2_2TB_Thunder/esp/v5.5.2/esp-idf/export.sh   # if not already active
cd /Volumes/M2_2TB_Thunder/Dev/MCs/ESP32/DAWKnob_IDF_01
idf.py build
```

The binary to upload is `build/DAWKnob_IDF_01.bin`.

### 2. Switch the device to WiFi mode

With the device powered on and BLE running, press and hold the **WiFi button (GPIO27)**.  
The **WiFi LED (GPIO18)** will light up once the device has connected to your network.

### 3. Open the update page http://192.168.1.103/update

In a browser on the same network, navigate to:

```
http://192.168.1.103
```

Replace `192.168.1.103` with whatever static IP you set in menuconfig if you changed it.

### 4. Upload the firmware

1. Click **Choose File** and select `build/DAWKnob_IDF_01.bin`
2. Click **Upload Firmware**
3. Wait for the "Update successful! Rebooting..." message
4. The device will restart automatically and return to BLE mode

### Notes

- The device must be on the same WiFi network as your computer
- Upload only `.bin` files built for this project — flashing an incompatible binary will cause a boot loop (recover via USB flash as normal)
- If the page doesn't load, check the WiFi LED is on and verify the static IP in menuconfig matches your network's subnet

## BLE Behaviour

- Advertises as **"DAWKnob"** and bonds with the host
- Rotating the encoder presses a mouse button and moves the cursor vertically (scroll emulation)
- RE push button toggles between left-button and right-button scroll mode
- After `KNOB_SENS_MS` (1 second) of no rotation the button is released and movement is reversed
