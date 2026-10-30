# Buddy Tracker

LoRa position sharing for Heltec WiFi LoRa 32 V3 boards with an ATGM336H GPS
(GPS TX → GPIO41, GPS RX → GPIO42).

- **Board on its own:** north-right OLED map of the other boards, Morse messaging on the PRG
  button (`SOS` and `MARK` are commands), delivered ticks, leash alerts, track logging.
- **With a phone:** the companion website in [`docs/`](docs/) connects over Bluetooth for a
  real map with compass heading, sky plot, chat, remote Wi-Fi/BLE scans, range test, waypoints,
  GPX export and settings. Published with GitHub Pages from `/docs`; try `?demo` without a board.

## PRG button

| Press | Action |
|---|---|
| tap / ~0.5 s | Morse dot / dash; message sends after 4 s idle |
| release after 1.2 s | next page (or cancel draft) |
| release after 3 s | Bluetooth on/off |
| release after 6 s | forget paired phones |

## Building

PlatformIO, two environments:

```
pio run -e tracker -t upload   # the tracker (default)
pio run -e probe -t upload     # GPS wiring/baud probe: listens on GPIO41 and GPIO42
```
