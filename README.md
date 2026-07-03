# IdleCut

Motion-activated power saver for the ESP32. IdleCut watches a room with a radar motion sensor and automatically cuts power to an AC appliance (fan, lamp, heater, etc.) via a relay once no motion has been seen for a configurable period — no cloud, no app, just a local web dashboard served straight from the board.

## Features

- **Automatic power control** — relay switches on when motion is detected and switches off after a configurable idle timeout
- **Manual override** — force the relay on or off from the web dashboard regardless of motion state
- **Adjustable sensitivity** — tune how long the radar signal must stay confirmed before it's trusted, filtering out EMI/false triggers
- **Live environment readout** — temperature and humidity from an onboard DHT11 sensor
- **Local web dashboard** — responsive control page served over the ESP32's own WiFi access point, no internet required
- **OLED status display** — animated at-a-glance view of date/time, temperature, humidity, and motion status
- **Motion event log** — optional in-memory log of the last 15 motion timestamps, viewable from the dashboard
- **Physical controls** — two onboard buttons for radar enable/disable and manual relay toggle, independent of the web UI

## Hardware

| Component | Purpose |
|---|---|
| ESP32 dev board | Main controller, WiFi AP, web server |
| RCWL-0516 | Doppler radar motion sensor |
| Relay module | Switches the AC appliance |
| DHT11 | Temperature and humidity sensor |
| SSD1306 OLED (128x64, I2C) | Local status display |
| 2x pushbutton | Radar enable/disable, manual relay toggle |
| 1x LED | Motion status indicator |

### Wiring

| ESP32 pin | Connects to |
|---|---|
| D4 | DHT11 data |
| D5 | RCWL-0516 OUT |
| D16 | Button 1 (radar enable/disable) → GND |
| D18 | Button 2 (manual relay toggle) → GND |
| D21 / D22 | OLED SDA / SCL (I2C, address `0x3C`) |
| D23 | Status LED |
| D26 | Relay IN1 |

Buttons use the ESP32's internal pull-ups (`INPUT_PULLUP`) — no external resistors needed. The relay module's own VCC and GND should go to the ESP32's VIN and GND, separate from the D26 signal line.

> **Note:** the RCWL-0516 is a Doppler radar sensor, not a PIR — it can see through thin walls/plastic and is sensitive to noise from shared power rails, WiFi radios, and flickering lights. Give it a clean, separate power supply where possible.

## Getting started

### 1. Configure

Open the sketch and adjust the constants at the top to match your setup:

```cpp
const char* AP_SSID     = "Idle Cut";
const char* AP_PASSWORD = "";
const char* MDNS_HOST   = "project";
const char* TZ_STRING   = "IST-5:30";
```

### 2. Install dependencies

Install the following libraries via the Arduino Library Manager:

- `Adafruit GFX Library`
- `Adafruit SSD1306`
- `DHTesp`
- ESP32 board support (`WiFi`, `WebServer`, `ESPmDNS`, `Wire` are included with it)

### 3. Flash

Select your ESP32 board and port in the Arduino IDE (or `arduino-cli` / PlatformIO equivalent), then upload the sketch.

### 4. Connect

On boot, the ESP32 starts a WiFi access point:

- **SSID:** `Idle Cut` (or whatever you set `AP_SSID` to)
- Connect from your phone or laptop, then open **`http://project.local`** (or the IP address printed to Serial at 115200 baud if mDNS isn't available on your network)

## Using the dashboard

| Control | What it does |
|---|---|
| **Manual override** | Force the relay on or off; automatic control resumes on the next motion detection or after the auto-off timer |
| **Sensitivity** | How long (50–1000ms) the radar signal must stay high before a detection is trusted. Lower = faster response, higher = fewer false triggers |
| **Auto-off timer** | Minutes of no motion before the relay switches off automatically (1–60 min) |
| **Motion log** | Toggle logging on/off and view the last 15 motion timestamps |
| **Resync time** | Syncs the board's clock to your device's local time (no internet/NTP required) |

## Physical controls

- **Button 1 (D16):** enable or disable the radar sensor. Disabling it leaves the relay in whatever state it was last in.
- **Button 2 (D18):** manually toggle the relay — only active while the radar is disabled.

## Web API

The dashboard is a static page that talks to these endpoints; useful if you want to integrate IdleCut with your own scripts or home automation setup.

| Endpoint | Method | Description |
|---|---|---|
| `GET /` | — | Serves the dashboard |
| `GET /data` | — | Current sensor readings, relay/radar state, and config as JSON |
| `GET /relay?state=on\|off` | — | Manually set the relay state |
| `GET /config?confirm=<ms>&autooff=<min>` | — | Update sensitivity and auto-off timer |
| `GET /log?state=on\|off` | — | Enable or disable motion logging |
| `GET /events` | — | Returns the motion event log as a JSON array of timestamps |
| `GET /setTime?epoch=<unix_ts>` | — | Sync the board's clock |

## Troubleshooting

**OLED shows the boot logo and then goes blank**
Usually a loose or marginal I2C connection (SDA/SCL/GND), rather than a wrong pin — if wiring were fundamentally wrong, the logo wouldn't display at all. Check for a flaky ground, and consider powering the OLED from a supply separate from the relay coil if you see it drop out right as the relay clicks.

**Motion is detected when nothing is moving**
Raise the sensitivity value (try 400–600ms) from the dashboard. Also check that the radar isn't sharing a power rail with the relay, and keep it away from fans, curtains, and flickering lights — the RCWL-0516 reacts to all of it.

**Can't reach `project.local`**
Not all networks/devices support mDNS. Check the Serial Monitor (115200 baud) at boot for the board's IP address and use that directly.

## License

MIT — do whatever you'd like with it.
