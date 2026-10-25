# ⚡ Lightning Detector Unit

**Detects lightning, sounds the alarm, and tells everyone nearby when it's safe to return. No internet required.**

A low-cost lightning warning station for beaches, pools, sports fields, and outdoor events. An ESP32 reads an AS3935 lightning sensor, flashes a beacon and sounds a siren when a strike is close, alerts phones through a dashboard it hosts on its own Wi-Fi network, and runs an all-clear countdown after the last strike.

Built for a hardware hackathon at Texas A&M (October 2026).

---

## Features

- **Lightning detection** with the AS3935 sensor, with an estimated distance to the storm (1–40 km)
- **False-alarm rejection:** man-made electrical noise is logged as a *disturber* and never triggers the alarm
- **Local alarm:** flashing LED beacon and buzzer siren for 10 seconds when a strike is within the alarm distance
- **All-clear countdown:** every strike restarts it; when it ends, the station plays an all-clear chime (15 s for demos; the real safety rule is 30 minutes)
- **Offline phone alerts:** phones on the station's Wi-Fi get a full-screen warning, a siren sound, and vibration (Android)
- **Live dashboard:** station and sensor health, uptime, event log, alarm distance setting, test button
- **Simulation mode (clearly labeled):** a handheld wand triggers *simulated* strikes for demos. Its distance is estimated from its radio signal strength (RSSI), and strike strength from how long its button is held. Simulated strikes are shown in purple and counted separately from real detections

> **Note:** The station detects strikes that have already happened. It does not predict the first strike of a storm.

---

## How it works

Lightning produces bursts of radio waves. The AS3935 listens near **500 kHz** using an LC antenna that resonates at

$$f = \frac{1}{2\pi\sqrt{LC}}$$

The chip has 16 internal tuning-capacitor settings; `as3935_antenna_tune` measures the antenna frequency at each one and picks the closest to 500 kHz. The chip's built-in algorithm then classifies each signal as **lightning**, a **disturber** (man-made), or a **noise** warning (too much background interference to listen reliably).

The station ESP32 reads those events over SPI, runs the alarm and countdown, and serves the dashboard from its own Wi-Fi access point. The wand is a second ESP32 that communicates with the station over **ESP-NOW**.

---

## Repository layout

```
firmware/
  lightning_station/      Main station program (detector, alarm, dashboard, simulation mode)
  lightning_wand/         Simulation wand (ESP-NOW beacons + strike button; optional 500 kHz coil)
  tools/
    as3935_event_monitor/ Wiring test (SPI write/read-back) + prints every sensor event
    as3935_antenna_tune/  Antenna calibration: finds the TUNE_CAP value closest to 500 kHz
    int_pin_test/         Checks the sensor's INT connection without a multimeter
cad/
  lightning_mast.py       Parametric CadQuery model (edit the numbers at the top, rerun)
  stl/                    Print-ready parts
  step/                   Editable parts + full assembly (Fusion 360, FreeCAD, etc.)
```

---

## Hardware

| Part | Qty | Notes |
|---|---|---|
| ESP32 dev board (ESP32-WROOM) | 2 | One station, one wand |
| SparkFun AS3935 Lightning Detector (SEN-15441) | 1 | SPI |
| 5 mm LED + 220 Ω resistor | 1 | Warning beacon |
| Passive buzzer | 1 | Optional 100 Ω series resistor and 100 nF capacitor across it to reduce idle noise |
| Push button | 1 | On the wand |
| USB power bank | 1–2 | |
| 3D-printed enclosure | 1 set | See `cad/` |
| *(Optional)* coil: ~28 turns of 26 AWG on a pen + 220 Ω | 1 | Experimental 500 kHz emulator on the wand |

### Station wiring

| Part | Pin | ESP32 |
|---|---|---|
| AS3935 | 3V3 / GND | 3V3 / GND |
| AS3935 | SCK | GPIO 18 |
| AS3935 | MISO | GPIO 19 |
| AS3935 | MOSI | GPIO 23 |
| AS3935 | CS | GPIO 5 |
| AS3935 | INT | *not used* (events are polled over SPI) |
| LED | long leg | 220 Ω → GPIO 26 |
| LED | short leg | GND |
| Buzzer | + | GPIO 25 |
| Buzzer | − | GND |

### Wand wiring

| Part | Pin | ESP32 |
|---|---|---|
| Button | one corner leg | GPIO 27 |
| Button | diagonally opposite leg | GND |
| Coil *(optional)* | end 1 | 220 Ω → GPIO 18 |
| Coil *(optional)* | end 2 | GND |

---

## Setup

1. **Arduino IDE:** install the *esp32 by Espressif Systems* board package and the **SparkFun AS3935 Lightning Detector Arduino Library** (Library Manager). Board: *ESP32 Dev Module*.
2. **Test the sensor wiring:** upload `tools/as3935_event_monitor`, open the Serial Monitor at 115200, press EN, and look for `SPI TEST: PASS`.
3. **Calibrate (optional but recommended):** upload `tools/as3935_antenna_tune` with the sensor in its final position, and note the `BEST SETTING`. Put it in `TUNE_CAP` at the top of `lightning_station.ino`. Leaving it at `0` uses the factory tuning (SparkFun boards ship within about 1% of 500 kHz). Calibration requires a working INT connection.
4. **Upload** `lightning_station` to the station ESP32 and `lightning_wand` to the wand.
5. **Open the dashboard:** join Wi-Fi **LightningStation** (password `lightning123`; change both at the top of `lightning_station.ino`) and go to **http://192.168.4.1**. Tap *Enable alerts on this phone* to allow sound and vibration.

**Upload tip:** if uploads fail with *"Failed to communicate with the flash chip,"* disconnect the wiring from the ESP32, upload, reconnect, and press EN. Setting *Tools → Upload Speed* to 115200 also helps.

### Settings (top of `lightning_station.ino`)

| Setting | Default | Meaning |
|---|---|---|
| `TUNE_CAP` | 0 | Antenna tuning value from calibration |
| `ALARM_MS` | 10000 | How long the LED/siren alarm runs (ms) |
| `CLEAR_MS` | 15000 | All-clear countdown (ms). The real lightning-safety rule is 30 minutes |
| `RSSI_NEAR` / `RSSI_FAR` | −35 / −85 dBm | Wand signal strengths mapped to 1 km and 40 km in simulation mode |
| `alarmKm` | 20 | Alarm distance (also adjustable on the dashboard) |

---

## Printing the enclosure

Printed in PLA on a Bambu Lab X1 Carbon: 0.2–0.28 mm layers, 10–15% infill, no supports.

- Flip **lid** and **sensor_cap** upside down (open side on the bed); print the others as exported.
- Add a brim to the **mast**.
- Use plastic only near the sensor; metal detunes and blocks the antenna.

---

## Limitations

- **No direction:** a single antenna measures signal strength, not bearing.
- **Distance is an estimate:** the AS3935 reports distance to the front of the storm in coarse steps.
- **Simulation mode is a demo tool:** wand strikes are not lightning and are always labeled as simulated.
- **The coil emulator is experimental:** it couples a 500 kHz near-field signal into the sensor, but its burst pattern has not been validated as lightning by the chip.
- **Phone alerts need the dashboard open** and the phone on the station's Wi-Fi; iPhones don't support web vibration.

## Future work

- Storm direction with a second antenna at a right angle
- Earlier warnings with an electric field mill (charge buildup before the first strike)
- Solar power and a weatherproof enclosure
- Internet push notifications (e.g., ntfy) so phones get alerts with the screen off
- A network of stations sharing alerts across a beach or campus

## License

MIT. See [LICENSE](LICENSE).
