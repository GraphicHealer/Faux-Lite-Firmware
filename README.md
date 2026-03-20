# Faux-Lite Firmware

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange.svg)](https://platformio.org/)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

WiFi-to-Serial bridge firmware for Pro-Lite LED displays. Turn any ESP32 into a WiFi controller for your Pro-Lite ASCII-Series display with a modern web interface and REST API.

## Features

- 🌐 **Web Interface** - Beautiful, mobile-responsive control panel
- 📡 **REST API** - Full automation support
- 🔄 **OTA Updates** - Upload new firmware wirelessly
- 📱 **WiFi Manager** - Easy WiFi setup with captive portal
- 💾 **Message Queue** - Store and loop multiple messages
- 🎨 **Full Protocol Support** - All Pro-Lite colors, fonts, and animations
- 🔊 **Beep Support** - Audible notifications
- 📄 **26 Pages** - Store messages on pages A-Z

## Hardware Requirements

- **ESP32 Board** (tested on Lolin S2 Mini)
- **Pro-Lite ASCII-Series Display** (Version 6.00 protocol)
- **3 wires** for serial connection

📖 **New to building hardware projects?** Check out the [Complete Build Guide](docs/BUILD_GUIDE.md) for step-by-step instructions!

### Wiring

```
ESP32          Pro-Lite Display
─────────────  ────────────────
GPIO 18 (TX) → RX
GPIO 21 (RX) → TX
GND          → GND
```

## Installation

### Prerequisites

- [PlatformIO](https://platformio.org/) installed
- USB cable to connect ESP32

### Build and Flash

1. Clone this repository:
   ```bash
   git clone https://github.com/GraphicHealer/Faux-Lite-Firmware.git
   cd Faux-Lite-Firmware
   ```

2. Build and upload:
   ```bash
   pio run --target upload
   ```

3. Monitor serial output (optional):
   ```bash
   pio device monitor
   ```

## First-Time Setup

1. **Power on** the ESP32
2. **Connect** to WiFi network `Faux-Lite_XXXX` (XXXX = chip ID)
3. **Captive portal** opens automatically
4. **Select** your WiFi network and enter password
5. **Device restarts** and connects to your network
6. **Find IP** in serial monitor or check your router
7. **Open browser** to `http://<device-ip>`

## Web Interface

### Main Page

- **Send Message**: Choose page, text, color, font, and animation
- **Switch to Page**: Checkbox to display message immediately (checked by default)
- **Send Raw Command**: Direct Pro-Lite protocol commands
- **Protocol Guide**: Built-in reference for all commands

### Message Queue (`/messages`)

- Store unlimited messages with timing
- Loop messages continuously
- Run on boot option
- Test individual messages
- Edit and delete queue items

### Settings (`/settings`)

- **Sign ID**: Change device ID (01-99, default: 01)
- **Baud Rate**: Adjust serial speed (default: 9600)
- **Start Blank**: Clear display on boot
- **AP-Only Mode**: Run without WiFi connection
- **OTA Update**: Upload new firmware wirelessly
- **WiFi Reset**: Clear saved WiFi credentials

## REST API

### Send Command

```bash
curl -X POST http://<device-ip>/api/send \
  -H "Content-Type: application/json" \
  -d '{"command":"<PA><CH><SA>Hello World "}'
```

### Get Device Info

```bash
curl http://<device-ip>/api/info
```

Response:
```json
{
  "device": "faux-lite",
  "version": "1.0",
  "chip_id": "XXXX",
  "sign_id": "01",
  "baud_rate": 9600,
  "ip": "192.168.1.100",
  "mac": "AA:BB:CC:DD:EE:FF"
}
```

### Message Queue API

**Get Queue:**
```bash
curl http://<device-ip>/api/messages
```

**Update Queue:**
```bash
curl -X POST http://<device-ip>/api/messages \
  -H "Content-Type: application/json" \
  -d '{"messages":[{"command":"<PA>Test","duration":5000}],"runOnBoot":false}'
```

**Start/Stop Loop:**
```bash
curl -X POST http://<device-ip>/api/loop/start
curl -X POST http://<device-ip>/api/loop/stop
```

## Pro-Lite Protocol Quick Reference

### Basic Structure
```
<ID##><Command>
```
Where `##` is sign ID (default: 01)

### Common Commands

- **Pages**: `<PA>` to `<PZ>` - Select page A-Z
- **Colors**: `<CA>` Dim Red, `<CB>` Red, `<CH>` Bright Yellow, etc.
- **Fonts**: `<SA>` Normal, `<SB>` Bold, `<SC>` Italic, etc.
- **Animations**: `<FA>` Auto, `<FB>` Open, `<FQ>` Appear, etc.
- **Run Page**: `<RPx>` - Display page x
- **Beep**: `<FB>` - Audible beep

Full protocol reference available in the web interface.

### Complete Protocol Documentation

For the complete Pro-Lite ASCII-Series Version 6.00 protocol specification, see the [official protocol PDF](docs/Protocols_TruColorII_6.0.pdf) included in this repository. This comprehensive document covers:

- All color codes and combinations
- Font styles and sizes
- Animation and transition effects
- Page management and chaining
- Time/date display functions
- Graphics and special characters
- Advanced features like counters and timers
- Serial communication specifications

The PDF is the authoritative reference for all available commands and their exact syntax.

## OTA Updates

1. Build new firmware:
   ```bash
   pio run
   ```

2. Locate firmware file:
   ```
   .pio/build/esp32dev/firmware.bin
   ```

3. Upload via web interface:
   - Navigate to `http://<device-ip>/settings`
   - Scroll to "OTA Firmware Update"
   - Select `firmware.bin`
   - Click "Upload Firmware"
   - Wait for upload and automatic restart

## Home Assistant Integration

Looking for Home Assistant integration? Check out the [Faux-Lite Display HACS Integration](https://github.com/GraphicHealer/Faux-Lite-Addon) for full Home Assistant support with service calls, entities, and automations.

## Troubleshooting

**Device won't connect to WiFi:**
- Reset WiFi via settings page or hold reset button
- Check WiFi credentials
- Ensure 2.4GHz network (ESP32 doesn't support 5GHz)

**Display not responding:**
- Check wiring connections
- Verify baud rate matches display (default: 9600)
- Check sign ID matches display configuration

**OTA upload fails:**
- Ensure stable WiFi connection
- Try smaller firmware file
- Power cycle device and retry

## License

MIT License - See LICENSE file for details

## Credits

Built with:
- [PlatformIO](https://platformio.org/)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)
- [ArduinoJson](https://arduinojson.org/)

## Acknowledgments

This project was developed with the assistance of Cascade AI (Windsurf IDE).
