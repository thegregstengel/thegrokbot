# Architecture

Physical GrokBot splits responsibilities between a thin **Body** running on the
Espressif ESP-VoCat (ESP32-S3-WROOM-1, 240 MHz dual-core, 16 MB flash, 16 MB
Octal PSRAM) and a companion **Brain** daemon (`grokbot-brain`) written in Rust
that runs on your desktop or LAN server.

## Design principles

- **Keep the microcontroller thin:** The ESP-VoCat handles real-time sensory
  I/O: dual-mic capture, local ESP-SR audio front-end processing, wake word
  detection, ring-buffered speaker playback, touch input, and the 3D face
  renderer. It stores no API keys, TLS certificates, prompts, or tool logic.
- **Keep the intelligence fast to iterate:** The Rust Brain daemon holds
  `XAI_API_KEY`, maintains the xAI realtime voice WebSocket
  (`wss://api.x.ai/v1/realtime` using binary PCM transport and session
  resumption), paces playback frames in real time, calculates speech energy for
  lip-synced eye animation, and lets you tune voices, prompts, or search tools
  without reflashing firmware.
- **Transport-agnostic framing:** Body and Brain communicate over a compact
  length-prefixed binary protocol (`GB` v1). The current transport uses LAN TCP
  with mDNS discovery (`_grokbot._tcp.local.`), and a USB serial transport can
  reuse the same framing layer.

## Component overview

```text
+--------------------------------------------+    LAN TCP (binary GB v1)     +-------------------------------------------+
| ESP-VoCat Body (ESP-IDF 5.5 C, 240 MHz)    | <---------------------------> | grokbot-brain (Rust / Tokio)              |
|                                            |                               |                                           |
|  ES7210 stereo (MIC1 + MIC2) + playback    |  0x02 AUDIO_UP (640 B / 20ms) |  Binary WS <-> wss://api.x.ai/v1/realtime |
|   -> ESP-SR AFE (MMR: AEC+SE+VAD+WakeNet9) |  0x03 WAKE / 0x04 BARGE_IN -> |   (audio/pcm 16 kHz, transport: binary,   |
|  256 KB PSRAM playback ring -> ES8311 DAC  | <- 0x82 AUDIO_DOWN (paced)    |    server_vad, resumption, optional tools)|
|  360x360 3D SDF face (25 fps, CPU 1)       | <- 0x83 FACE_STATE (20 Hz)    |  20 ms playback pacer (120 ms pre-fill)   |
|  CST816S touch + capacitive pads           | <- 0x84 STOP_AUDIO (flush)    |  Perceptual PCM energy -> eye bounce      |
|  SoftAP Wi-Fi setup + mDNS discovery       |                               |  mDNS advertiser (_grokbot._tcp.local.)   |
+--------------------------------------------+                               +-------------------------------------------+
```

## Audio pipeline and pacing

### ESP-VoCat Body (`firmware/main/audio/audio_pipeline.c`)

- **Capture and ESP-SR AFE:** The official `espressif/esp_vocat` BSP configures
  standard 2-channel full-duplex I2S for ES8311 (mono speaker DAC) and ES7210
  (dual-mic ADC). `board_init()` opens ES7210 in 2-channel stereo mode (`MIC1`
  and `MIC2`), and `audio_pipeline.c` interleaves live speaker samples as a
  third software reference channel (`"MMR"` format) into ESP-SR AFE
  (`AEC + SE(BSS) + WebRTC VAD + WakeNet9 wn9_hiesp`).
- **Echo-safe uplink gating:** While the speaker is actively playing (plus a
  250 ms post-playback echo tail), `body_fsm.c` pauses `AUDIO_UP` streaming so
  residual speaker echo cannot falsely trigger xAI's `server_vad`. Local
  WakeNet9 (`"Hi, ESP"`) and touch taps remain active at all times for instant
  barge-in.
- **Speaker playback:** Incoming 16 kHz mono PCM16 frames write into a 256 KB
  (8-second) PSRAM ring buffer with an 80 ms (4-frame) pre-buffer. The
  `spk_play` task runs at FreeRTOS priority 7 and writes a clean silent tail
  frame when draining to eliminate end-of-utterance pops.

### Rust Brain (`brain/src/session.rs`, `brain/src/xai.rs`)

- **Binary xAI transport:** Connects with `rustls` (`ring` crypto provider) to
  `wss://api.x.ai/v1/realtime?model=grok-voice-latest`, requests `audio/pcm` at
  16 kHz with `"transport": "binary"` on both input and output, enables session
  resumption (`resumption.enabled = true`), and sets `server_vad` threshold to
  `0.45`.
- **Real-time playback pacer:** Because xAI bursts audio much faster than real
  time (15-20 KB chunks), `session.rs` queues 20 ms (640-byte) frames, sends an
  initial 6-frame (120 ms) pre-fill burst for low first-word latency, and paces
  subsequent frames every 19.8 ms. It computes perceptual PCM energy (`0..255`)
  at ~20 Hz directly from the currently paced frame so eye motion stays synced
  with audible speech.

## Display and 3D spherical SDF face

The circular 360x360 ST77916 QSPI display runs an analytic 3D unit-sphere SDF
renderer (`firmware/main/ui/face/face_render.c`) writing into a PSRAM
`lv_canvas` (`LV_COLOR_FORMAT_RGB565`) at 25 fps on CPU 1:

- **Geometry:** Each eye is evaluated as a capsule SDF in angular spherical
  coordinates (`theta`, `phi`), naturally wrapping and foreshortening around a
  curved glossy sphere as GrokBot looks around.
- **Visual polish:** Cached obsidian sphere background with subtle specular
  sheen, luminous eye halo, 4x4 Bayer ordered dithering to eliminate RGB565
  banding, underdamped spring interpolation, organic blinking, micro-saccades,
  happy `^ ^` arches, concerned brows, and sleepy eyelids.
- **Performance:** Zero per-pixel `libm` calls (4 KB internal-SRAM `sqrt` LUT,
  fast FPU `asin` polynomial, tight per-eye dirty bounding boxes, and a single
  fixed 20-line internal DMA LVGL flush buffer so SPI transfers never contend
  for per-frame DMA allocations).

## Memory and core layout (ESP32-S3)

- **CPU 0:** Wi-Fi, lwIP, mDNS, `brain_rx`, `brain_tx`, and `afe_feed`.
- **CPU 1:** `spk_play` (priority 7), `afe_fetch` (priority 5), and `taskLVGL`.
- **PSRAM (16 MB Octal):** 360x360 face canvas and background cache, 256 KB
  playback ring buffer, AEC reference ring buffer, `brain_link` TX queue and RX
  buffer, ESP-SR models and ring buffers, and Wi-Fi/lwIP dynamic buffers.

## Binary wire protocol (`GB` v1)

Every frame starts with a 6-byte little-endian header followed by `len` payload
bytes (maximum 4096 bytes):

| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | 2 | `magic` | ASCII `0x47, 0x42` (`"GB"`) |
| 2 | 1 | `msg_type` | Message identifier |
| 3 | 1 | `flags` | Bit flags (`0x01` = urgent control) |
| 4 | 2 | `len` | Payload length in bytes (little-endian `u16`) |

### Body to Brain messages

| Type | Name | Payload |
| :--- | :--- | :--- |
| `0x01` | `HELLO` | `[proto_ver: u8, rate_khz: u8, channels: u8, caps: u8, device_id_utf8...]` |
| `0x02` | `AUDIO_UP` | Raw 16 kHz mono 16-bit signed little-endian PCM (640 bytes / 20 ms) |
| `0x03` | `WAKE` | `[source: u8]` (`1` = wake word, `2` = touch or capacitive pad) |
| `0x04` | `BARGE_IN` | `[source: u8]` (interrupts active response and cancels xAI turn) |
| `0x05` | `SLEEP` | Empty (pauses mic streaming; Brain keeps `conversation_id` for resumption) |
| `0x06` | `PING` | `[seq: u32 LE]` |
| `0x07` | `AUTH_RESPONSE` | `[nonce_body: 16 B][HMAC-SHA256(token, "GB1-BODY" \|\| nonce_brain \|\| nonce_body): 32 B]` |

### Brain to Body messages

| Type | Name | Payload |
| :--- | :--- | :--- |
| `0x81` | `WELCOME` | `[proto_ver: u8, rate_khz: u8]` + optional `[HMAC-SHA256(token, "GB1-BRAIN" \|\| nonce_brain \|\| nonce_body): 32 B]` |
| `0x82` | `AUDIO_DOWN` | Raw 16 kHz mono 16-bit signed little-endian PCM (640 bytes / 20 ms) |
| `0x83` | `FACE_STATE` | `[emotion: u8, energy: u8, yaw_nudge: i8, pitch_nudge: i8]` |
| `0x84` | `STOP_AUDIO` | Empty (immediately flushes Body speaker ring buffer) |
| `0x85` | `CONFIG` | `[volume: u8, brightness: u8]` (`0xFF` leaves a field unchanged) |
| `0x86` | `PONG` | `[seq: u32 LE]` |
| `0x87` | `AUTH_CHALLENGE` | `[nonce_brain: 16 B]` (fresh CSPRNG nonce per connection) |

## Security model

- **Mutual authentication:** When `GROKBOT_AUTH_TOKEN` is set (required by
  default unless `--allow-unauthenticated` is passed), every connection
  completes a 5-second-bounded mutual `HMAC-SHA256` handshake with fresh
  16-byte CSPRNG nonces from both peers. Unauthenticated LAN clients cannot
  trigger xAI usage, and a rogue mDNS advertiser cannot impersonate the Brain
  or receive microphone audio.
- **Connection hygiene:** Up to 4 concurrent handshakes are accepted to prevent
  slowloris floods, and each newly authenticated Body cleanly supersedes any
  stale previous connection.
- **WPA2 SoftAP provisioning:** First-boot setup creates a WPA2-PSK access
  point (`GrokBot-Setup`) protected by a random per-boot 8-digit PIN shown on
  the display, plus a per-boot CSRF token validated on `POST /save`.
- **Transcript privacy:** Voice transcripts are suppressed from `INFO` logs by
  default and require `--log-transcripts` (`GROKBOT_LOG_TRANSCRIPTS=1`).
- **Transport scope:** Body-to-Brain TCP traffic is mutually authenticated but
  not TLS-encrypted; run on a trusted private LAN or VPN.

### Emotion values

| Value | Emotion | Meaning |
| :--- | :--- | :--- |
| `0` | `Idle` | Resting gaze with subtle drift and blinks |
| `1` | `Listening` | Upright, attentive eyes waiting for speech |
| `2` | `Focusing` | Enlarged upright eyes while user is speaking |
| `3` | `Thinking` | Glancing aside while composing a reply |
| `4` | `Speaking` | Voice-reactive eyes animated by `energy` (`0..255`) |
| `5` | `Happy` | Squinted `^ ^` happy arches |
| `6` | `Curious` | Wide glancing eyes (for example, Wi-Fi setup mode) |
| `7` | `Concerned` | Angled brows (network or upstream error) |
| `8` | `Sleepy` | Heavy-lidded, nearly closed eyes |
