# ESP32 Departures

A tiny departure board for the Munich S-Bahn: it shows the next three trains from
**Donnersbergerbrücke** that stop at **Vaterstetten**, with live delays and cancellations.

![Display showing three departures: S4 Zorneding in 4 min, S6 Ebersberg in 12 min with a delay icon, S6 Grafing cancelled](docs/display.png)

The minutes are the real departure time, delays included; cancelled trains are crossed out.

## Hardware

- ESP32-C3 Super Mini
- 0.96" I2C OLED, 128×64, SSD1306 (yellow/blue two-colour version)

| OLED | ESP32-C3 |
|------|----------|
| GND  | G        |
| VCC  | 3.3      |
| SCL  | GPIO1    |
| SDA  | GPIO0    |

## Setup

Everything the build needs is pinned in [`firmware/sketch.yaml`](firmware/sketch.yaml):
the board, the `esp32` core and the library versions. The first build downloads them
automatically (the ESP32 toolchain is a ~2 GB download, ~6 GB on disk), so a new
machine only needs `arduino-cli`.

1. **Install [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/)**

   ```sh
   brew install arduino-cli                    # macOS
   curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh   # Linux
   winget install ArduinoSA.CLI                # Windows
   ```

2. **Add your WiFi:** copy `firmware/secrets.example.h` to `firmware/secrets.h` and fill
   it in. The office network is optional; it's tried when home isn't reachable.
   `secrets.h` is git-ignored.

3. **Build and flash** (from the `firmware` folder):

   ```sh
   arduino-cli compile .
   arduino-cli board list                      # find the port
   arduino-cli upload -p <port> .
   arduino-cli monitor -p <port> -c baudrate=115200   # optional: serial log
   ```

### USB connection

No driver is needed: the ESP32-C3 has USB built in and shows up as a standard serial port.

| OS      | Port looks like         | Notes |
|---------|-------------------------|-------|
| macOS   | `/dev/cu.usbmodem…`     | Allow the accessory if macOS asks. |
| Linux   | `/dev/ttyACM0`          | Add yourself to the `dialout` group (`uucp` on Arch), then log in again. |
| Windows | `COM3`, `COM4`, …       | Built-in driver on Windows 10 and later. |

If no port shows up:

- **Use a data cable.** Many USB-C cables only carry power.
- **Plug the board straight into the computer**, not through a hub.
- **Enter download mode:** hold **BOOT** while plugging the board in, then let go. After
  flashing in this mode, press **RST** to start the new firmware.

## How it works

Every 30 seconds the board asks the [MVG](https://www.mvg.de) departures API for the
station's S-Bahn departures and keeps the trains heading to a stop at or beyond
Vaterstetten (Zorneding, Grafing, Ebersberg, …). Some eastbound S4s end at Trudering,
so filtering by line alone isn't enough.

The station, the destinations and the number of trains are set at the top of
[`firmware/firmware.ino`](firmware/firmware.ino). Set `DEMO_MODE` to `true` to preview
the layout with sample trains.

Inspired by [mvv-display-for-ESP32](https://github.com/florianlederer/mvv-display-for-ESP32).
