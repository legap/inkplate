# inkplate

Experiments with an **e-radionica Inkplate 6** (the original Inkplate 6): an ESP32 board with a built-in 6" e-paper display.
Product page: <https://www.crowdsupply.com/soldered/inkplate-6>

## What was detected over USB (2026-10-30)

Probed read-only with `esptool` v5.6. Nothing was written to the board.

| Item | Result |
|---|---|
| USB-serial bridge | WCH **CH340** (`VID_1A86&PID_7523`), enumerated as **COM10** |
| SoC | **ESP32-D0WDQ6, revision v1.0**: dual core, 240 MHz, Wi-Fi + BT, 40 MHz crystal |
| MAC | `fc:f5:c4:1b:4e:88` |
| Flash | **4 MB**, GigaDevice (mfr `0xC8`, device `0x6016`, 1.8 V part), DIO mode. The 1.8 V flash marks this as an original ESP32-WROVER module |
| Partition table | Arduino default: `nvs` 20K · `otadata` 8K · `app0` 1280K · `app1` 1280K (OTA) · `spiffs` 1472K |
| Installed firmware | e-radionica **factory "Peripheral mode" firmware**, built with Arduino-ESP32 1.0.x (old ESP-IDF 3.x; PSRAM support compiled in). It also contains a VCOM-calibration routine (`Panel VCOM Voltage`, `VCOM EEPROM PROGRAMMING`) |
| Serial check | The board answers `OK` to the peripheral-mode ping `#?*` at 115200 baud |

Notes:
- The CH340 runs on USB power even when the **power switch** is off. With the switch off, the port appears but the ESP32 stays silent ("No serial data received").
- The ROM sometimes prints `flash read err, 1000` on the first power-on reset, then reboots and boots normally. This is a known harmless ESP32 quirk.
- **Peripheral mode** lets a PC or another microcontroller drive the display over UART with text commands (`#?*` = ping). You can use the board this way without flashing anything.
- Flashing your own sketch replaces this firmware. A full 4 MB backup is in [`firmware/inkplate6-factory.bin`](firmware/inkplate6-factory.bin). It was verified against the chip with `verify-flash` (SHA-256 `9745c7eedacdcc96f38496ede8b30670fd408a66e759bac0186ac679bb70bafb`). To restore it:
  ```bash
  python -m esptool --port COM10 -b 921600 write-flash 0 firmware/inkplate6-factory.bin
  ```
- Don't run the VCOM programming routine unless you know the panel's VCOM value (printed on the panel's flex cable). It writes the PMIC's EEPROM.

## Hardware summary

| Area | Details |
|---|---|
| Display | 6" e-paper, **800 × 600**, 3-bit grayscale (8 levels: black, white, 6 grays) or 1-bit B/W mode |
| Refresh | Full refresh ≈ 1.26 s. Partial updates are supported in 1-bit mode and are much faster (no full-screen flash) |
| MCU | **ESP32-WROVER**: dual-core Xtensa LX6, 240 MHz |
| Memory | 520 KB SRAM + PSRAM (4 MB usable, holds the framebuffer), **4 MB flash** |
| Wireless | Wi-Fi 2.4 GHz b/g/n, Bluetooth 4.x / BLE |
| Display power | TI **TPS65186** e-paper PMIC (generates the panel's high voltages; also has a built-in temperature sensor) |
| GPIO expander | **MCP23017** at I²C `0x20`: drives the panel and exposes extra GPIOs |
| RTC | None (nothing at `0x51`) |
| Storage | microSD card slot (SPI) |
| Inputs | 3 capacitive touch pads, reset button, power switch |
| Expansion | **easyC** connector (I²C, Qwiic/STEMMA QT-compatible), GPIO headers with I²C and SPI |
| Power | Micro-USB or a 1-cell LiPo (2-pin JST-PH 2.0 connector), **MCP73831** charger (about 400 mA), battery-voltage readout, charge LED |
| Deep sleep | ≈ **25 µA**; the e-paper image stays visible with no power |
| USB | CH340C USB-serial for programming and logging |

## Battery

Requirements for this board:
- 1 cell, 3.7 V nominal (4.2 V full), LiPo or Li-ion, **with a built-in protection circuit**.
- **2-pin JST-PH 2.0** plug (2.0 mm pitch). JST 1.25, JST-XH/SYP 2.54 and Molex plugs don't fit.
- At least about 400 mAh, because the charger charges at about 400 mA.
- To fit inside the case: at most **90 × 50 mm** and **5.5 mm** thick. A LiPo size code such as `505060` means thickness × width × length: 5.0 × 50 × 60 mm.

Ordered (2026-10-31):

| Battery | Size | Plug | Protection | Link |
|---|---|---|---|---|
| XINJ 454261, 1500 mAh (×2) | 4.5 × 42 × 61 mm | JST 2-pin 2.0 mm (variant "454261 2pin2.0") | PCM | [eBay.ch](https://www.ebay.ch/itm/185693084946?var=693621035993) |
| HXJN 505060, 2100 mAh | 5 × 50 × 60 mm | 2-pin, type not stated in the listing (looks like JST-PH 2.0) | Overcharge, over-discharge, overcurrent, short circuit | [Amazon.de](https://www.amazon.de/dp/B0H1WBS3JT?psc=1&smid=A91QXEJF0VMKO) |

**Check the polarity before plugging in.** JST-PH pinouts aren't standardized between battery makers, and reversed polarity can destroy the board. With the connector's notch facing up, **+ is on the left**; compare with the `+` marking on the PCB. Red wire = + on both batteries. If the polarity is reversed, lift the plug's plastic latches with a needle and swap the two pins.

Battery life is dominated by Wi-Fi time. As a rough estimate, waking once an hour for about 10 s of Wi-Fi plus a refresh gives a few months on 1500–2100 mAh.

## Software options

- **Arduino** (recommended): add the Inkplate board package to the Arduino IDE or `arduino-cli`, select *e-radionica Inkplate 6*, and install the `InkplateLibrary`. It is fully Adafruit-GFX compatible (text, fonts, shapes, bitmaps), with helpers for JPEG/PNG/BMP from SD or a URL, partial updates, deep sleep, the touch pads, battery voltage and the temperature sensor. There are many examples to start from.
- **PlatformIO**: the same library, using the `esp32` platform with a WROVER board and PSRAM enabled.
- **MicroPython**: an Inkplate MicroPython module exists, but it is less complete than the Arduino library.
- **ESP-IDF**: possible for full control, but you need your own panel driver or a port of the library.

## Toolchain setup (installed 2026-10-30)

| Component | Version |
|---|---|
| `arduino-cli` (via scoop) | 1.5.1 |
| Board package `soldered-inkplate-boards:esp32` | 4.0.0 |
| Library `InkplateLibrary` | 11.2.1 |

To reproduce on another machine:

```bash
arduino-cli config add board_manager.additional_urls https://github.com/SolderedElectronics/Inkplate-Board-Definitions-for-Arduino-IDE/raw/refs/heads/main/package_Inkplate_Boards_index.json
```

```bash
arduino-cli core update-index
```

```bash
arduino-cli core install soldered-inkplate-boards:esp32
```

```bash
arduino-cli lib install InkplateLibrary
```

The board target for this board is **`soldered-inkplate-boards:esp32:Inkplate6`** ("e-radionica Inkplate 6" in the Arduino IDE).

Compile and upload the starter sketch. Uploading **replaces the factory firmware**; see the backup above.

```bash
arduino-cli compile --fqbn soldered-inkplate-boards:esp32:Inkplate6 sketches/hello_world
```

```bash
arduino-cli upload -p COM10 --fqbn soldered-inkplate-boards:esp32:Inkplate6 sketches/hello_world
```

[`sketches/diag`](sketches/diag/diag.ino) prints board diagnostics to serial at 115200 baud, then draws "Hello World!". Output on this board:

```
psramFound: 1, PSRAM size: 4194304   (4 MB usable)
I2C scan: found 0x20                 (MCP23017 I/O expander; no RTC at 0x51)
Temperature: 27 C                    (from the TPS65186)
Battery: 4.12 V
```

The first upload of `hello_world` didn't change the screen, but `diag` (same drawing code, with a 500 ms delay before `display.begin()`) did. The cause hasn't been confirmed yet.

The library's own Inkplate 6 examples check for `ARDUINO_ESP32_DEV`, but board package 4.x defines `ARDUINO_INKPLATE6` for this board. Those examples therefore fail with "Wrong board selection". Change the `#if` check in the example to `ARDUINO_INKPLATE6` (as in [`sketches/hello_world`](sketches/hello_world/hello_world.ino)). The library itself supports the board.

## What it is good for

E-paper is sharp, readable in sunlight, and uses power only while the image changes. That makes this board a good fit for **slow-changing information on battery**. The usual pattern is: wake up → fetch data over Wi-Fi → draw → deep sleep for minutes or hours. On a typical LiPo, that runs for weeks to months.

Project ideas:

- **Dashboards**: weather, calendar, Home Assistant / MQTT status, public transport departures, stock/crypto tickers.
- **Smart signage**: room and meeting-room door signs, shop price tags, name badges.
- **Picture frame**: grayscale photos dithered to 8 levels, rotated from SD card or a server.
- **E-reader / document viewer**: text or images rendered from SD or a web endpoint.
- **Sensor logger with display**: easyC/Qwiic sensors (temperature, CO₂, air quality) with readings shown on screen and logged to SD.
- **Server-rendered display**: a backend renders a PNG and the Inkplate only downloads and shows it. This keeps the firmware simple (for example a TRMNL-style setup).
- **BLE/Wi-Fi notifier**: a low-power status board that a phone or PC can push to.

### Limitations

- Refreshes are slow: about 1 s with a visible flash for a full refresh, so it is not suited to animation or video.
- Partial updates leave ghosting. Do a full refresh every N partial updates.
- Grayscale only, no color.
- No frontlight, no touchscreen and no RTC (deep-sleep timing relies on the ESP32's internal RTC timer, which drifts).
- Battery life is dominated by Wi-Fi on time, so keep wake windows short.

## Next steps

1. ~~Back up the factory firmware~~ (done: `firmware/inkplate6-factory.bin`).
2. ~~Install the Arduino board package and the Inkplate library~~ (done). ~~Upload `sketches/hello_world`~~ (done).
3. When the batteries arrive: check the polarity, connect, and confirm charging and the battery-voltage readout with `sketches/diag`.
4. Pick a first project, such as a weather + calendar dashboard with deep sleep.
