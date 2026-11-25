# Physical GrokBot (`thegrokbot`)

Physical GrokBot turns an Espressif **ESP-VoCat** (ESP32-S3-WROOM-1) into a
desk-top voice companion powered by xAI's realtime voice API
(`grok-voice-latest`).

It pairs a thin **Body** firmware on the ESP-VoCat with a **Brain** daemon
written in Rust on your desktop or LAN server:

- **Voice-reactive 3D spherical face:** A 25 fps analytic 3D unit-sphere signed
  distance field (SDF) renderer draws GrokBot's white pill eyes onto the
  360x360 circular display with RGB565 Bayer dithering, subtle sphere sheen,
  luminous eye halo, underdamped spring physics, natural blinks, saccades, and
  lip-synced speech bounce.
- **Local wake word and audio pipeline:** ESP-SR WakeNet9 (`"Hi, ESP"`),
  dual-mic ES7210 beamforming, AEC, VAD, and a 256 KB PSRAM playback ring
  buffer run locally on the ESP32-S3 at 240 MHz.
- **Zero secrets on the microcontroller:** Your `XAI_API_KEY` stays strictly on
  the Rust Brain host, never in flash or NVS on the ESP-VoCat.
- **Low-latency binary streaming:** Body and Brain exchange 16 kHz mono PCM16
  audio and face cues over a lightweight framed binary protocol (`GB` v1) with
  automatic mDNS discovery (`_grokbot._tcp.local.`) and real-time 20 ms
  playback pacing.

## How it works

```text
+--------------------------------------------+    LAN TCP (binary GB v1)     +-------------------------------------------+
| ESP-VoCat Body (ESP-IDF 5.5 C, 240 MHz)    | <---------------------------> | grokbot-brain (Rust / Tokio)              |
|                                            |                               |                                           |
|  ES7210 dual-mic (MIC1 + MIC2) + ref       |  0x02 AUDIO_UP (20 ms PCM) -> |  Binary WS <-> wss://api.x.ai/v1/realtime |
|   -> ESP-SR AFE (AEC + SE + VAD + WakeNet) |  0x03 WAKE / 0x04 BARGE_IN -> |   (audio/pcm 16 kHz, transport: binary,   |
|  256 KB PSRAM ring buffer -> ES8311 DAC    | <- 0x82 AUDIO_DOWN (paced)    |    server_vad, session resumption, tools) |
|  360x360 3D spherical SDF face (25 fps)    | <- 0x83 FACE_STATE (20 Hz)    |  20 ms real-time pacer + speech energy    |
|  CST816S touch + capacitive side pads      | <- 0x84 STOP_AUDIO            |  mDNS advertiser (_grokbot._tcp.local.)   |
+--------------------------------------------+                               +-------------------------------------------+
```

1. **Idle:** GrokBot rests on your desk with gentle gaze drift and blinks while
   ESP-SR WakeNet9 listens locally for **"Hi, ESP"**.
2. **Wake:** Saying **"Hi, ESP"** or tapping the screen plays a soft ascending
   chime, switches the eyes to listening, and notifies `grokbot-brain`.
3. **Talk:** The Body streams 16 kHz PCM16 frames over LAN TCP to the Brain,
   which forwards them as raw binary WebSocket frames to xAI realtime voice.
4. **Respond:** xAI streams binary PCM16 audio back. The Brain paces frames at
   20 ms intervals (with a 120 ms initial pre-fill burst) and sends synchronized
   speech energy (`0..255`) so GrokBot's eyes bounce in step with every
   syllable.
5. **Barge-in and follow-up:** Tap the screen or say **"Hi, ESP"** mid-sentence
   to cut playback immediately and interrupt Grok. When Grok finishes speaking,
   mic streaming reopens automatically for hands-free follow-up until the
   inactivity timeout returns GrokBot to idle.

See [docs/architecture.md](docs/architecture.md) for the complete protocol and
audio design, and [docs/setup.md](docs/setup.md) for full installation and
troubleshooting steps.

## Quick start

### 1. Configure and start the Rust Brain

Copy `secrets.env.example` to `secrets.env` (gitignored) and add your xAI API
key:

```bash
cp secrets.env.example secrets.env
# Edit secrets.env and set XAI_API_KEY
./scripts/run_brain.sh
```

`grokbot-brain` listens on `0.0.0.0:8765` and advertises
`_grokbot._tcp.local.` via mDNS.

### 2. Build and flash the ESP-VoCat firmware

With ESP-IDF 5.5.1+ installed at `~/esp/esp-idf` and the ESP-VoCat connected
over USB-C (`/dev/ttyACM0`):

```bash
./scripts/build_flash.sh
```

Pass `--monitor` to attach the serial console after flashing (press `Ctrl+]` to
exit `idf.py monitor`).

### 3. Join Wi-Fi (first boot)

If Wi-Fi credentials are not saved in NVS or `menuconfig`:

1. Connect to the `GrokBot-Setup` Wi-Fi access point.
2. Open `http://192.168.4.1` and enter your Wi-Fi SSID and password (leave
   Brain IP blank for mDNS auto-discovery).
3. Save and wait for GrokBot to reboot. The eyes flash happy once the Body
   completes the handshake with `grokbot-brain`.

## Controls

| Action | Behavior |
| :--- | :--- |
| Say **"Hi, ESP"** or tap screen / side pad | Wake GrokBot, or interrupt (barge in) while speaking |
| Long-press screen or side pad | Put GrokBot to sleep (returns to idle) |
| Swipe up / down on screen | Increase or decrease speaker volume (`10%` steps with tick tone) |
| Swipe right / left on screen | Increase or decrease display brightness (`10%` steps) |

## Repository layout

| Path | Description |
| :--- | :--- |
| `brain/` | Rust companion Brain daemon (TCP server, mDNS, xAI realtime binary client, 20 ms playback pacer) |
| `firmware/` | ESP-IDF 5.5 C Body firmware (ESP-VoCat BSP, ESP-SR AFE, 256 KB PSRAM audio pipeline, 3D SDF face, touch controls) |
| `docs/` | Architecture specification, setup guide, and GrokBot visual reference crops |
| `scripts/` | Build/flash helper, Brain launcher, and host PNG face previewer (`preview_face.c`) |
| `secrets.env.example` | Template for local `secrets.env` (actual `secrets.env` is gitignored) |

## Development utilities

- **Run Brain tests:**

  ```bash
  cd brain && cargo test
  ```

- **Preview all face poses on desktop without flashing:**

  ```bash
  gcc -O2 -I firmware/main/ui/face scripts/preview_face.c firmware/main/ui/face/face_render.c -lm -o /tmp/preview_face
  /tmp/preview_face /tmp/face_sheet.ppm
  ```
