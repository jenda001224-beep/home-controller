# Apple Home Controller — T-Display S3 Pro

## What you need

| Item | Where to get it |
|---|---|
| LilyGO T-Display S3 Pro | LilyGO store / AliExpress |
| Home Assistant running on your network | homeassistant.io (free) |
| PlatformIO IDE | platformio.org (VS Code extension) |

---

## Step 1 — Create a Home Assistant token

1. Open Home Assistant → click your **profile picture** (bottom-left)
2. Scroll to **Long-Lived Access Tokens** → **Create Token**
3. Name it `tdisplay` → copy the token

---

## Step 2 — Edit config.h

Open `src/config.h` and fill in:

```c
#define WIFI_SSID   "YourWiFiName"
#define WIFI_PASS   "YourWiFiPassword"
#define HA_HOST     "192.168.1.XX"      // your HA IP, or homeassistant.local
#define HA_TOKEN    "eyJ0eXAi..."        // your long-lived token
```

---

## Step 3 — Verify display pins

The default pins in `config.h` target the standard T-Display S3 Pro.
If your board has a different revision, compare with the official pinout:
https://github.com/Xinyuan-LilyGO/T-Display-S3-Pro

---

## Step 4 — Flash

```bash
pio run --target upload
pio device monitor     # watch serial output
```

---

## How it works

```
Apple Home ↔ Home Assistant ↔ [WiFi] ↔ T-Display S3 Pro
```

- HA syncs all your Apple HomeKit devices natively
- The display reads room layout and entity states from HA over WebSocket
- Changes you make on the display instantly appear in Apple Home (and vice versa)
- Live updates — if you use Siri or the Apple Home app, the display updates within 1 second

---

## Using the controller

| Action | Result |
|---|---|
| **Tap** a device tile | Toggle on/off |
| **Long-press** a tile | Open detail panel |
| Detail → brightness slider | Dim/brighten the light |
| Detail → colour wheel | Change LED strip colour |
| Tap backdrop / X | Close detail |

---

## Assigning rooms in Home Assistant

1. HA → Settings → Areas & Zones → **Create Area** (Living Room, Bedroom, etc.)
2. Settings → Devices & Services → click each device → assign to an area
3. The controller will show a tab per area automatically

If no areas are set up, all devices appear in one "All" tab.
