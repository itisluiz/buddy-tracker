# Buddy Tracker website

Companion page for the Heltec V3 LoRa buddy trackers. It connects to one board over Bluetooth LE
(Web Bluetooth) and shows everything the 128x64 OLED can't: a real map with your phone's compass,
peer cards with "turn left/right" arrows, a GPS sky plot, chat, remote Wi-Fi/BLE scans, a range-test
heatmap, waypoints, GPX export and board settings.

## Hosting on GitHub Pages

1. Push this repository to GitHub.
2. Repository **Settings → Pages → Build and deployment**: Source *Deploy from a branch*,
   branch `main`, folder `/docs`.
3. Open `https://<user>.github.io/<repo>/` in **Chrome on Android**.

Plain HTML/CSS/JS modules, no build step. Leaflet comes from cdnjs, map tiles from OpenStreetMap.

## Using it

1. On the board, hold **PRG** until the bottom bar says `release: BT ON` (~3 s) and let go.
   A Bluetooth icon appears top-right on the OLED.
2. Tap **Connect**, pick `Buddy-XXXX`.
3. The first time, Android asks for a PIN: type the 6-digit code the board shows.
4. On the Map tab, tap **Enable compass** so arrows and the heading cone follow where you face.

Holding PRG ~6 s forgets every paired phone (also available under Tools → Bluetooth). If you do,
remove the board from Android's Bluetooth settings too before pairing again.

## Installing as an app

It's a PWA: in Chrome use **Tools → App → Install app** (or the browser menu's *Install app*). The installed
app opens full screen, starts offline, and keeps up to 2000 map tiles you've viewed for use without data.
Bluetooth works the same as in the browser.

## Demo mode

Open `index.html?demo` to preview the UI with made-up data, no board needed.

## Testing locally

Web Bluetooth needs a secure context; `localhost` counts:

```
cd docs
python -m http.server 8000
```

then open `http://localhost:8000` in desktop Chrome (uses the PC's Bluetooth adapter).

## Limits

- No iPhone support: no iOS browser implements Web Bluetooth.
- One phone per board at a time.
