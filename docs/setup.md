# Set up Physical GrokBot

Physical GrokBot runs as two cooperating components:

1. **Brain (`brain/`):** A Rust daemon on your desktop or LAN server that holds
   your `XAI_API_KEY` and bridges xAI realtime voice.
2. **Body (`firmware/`):** Thin ESP-IDF 5.5 C firmware on the ESP-VoCat that
   runs local WakeNet9 + ESP-SR AFE, streams binary PCM over LAN, and renders
   the 3D spherical GrokBot face.

See [architecture.md](architecture.md) for the wire protocol and audio design.

## Prerequisites

- Espressif ESP-VoCat (ESP32-S3-WROOM-1, 16 MB flash, 16 MB Octal PSRAM) and a
  USB-C data cable
- Rust toolchain (for `brain/`)
- ESP-IDF 5.5.1 or newer (for building and flashing `firmware/`)
- An [xAI API key](https://console.x.ai) with realtime voice access

## Step 1: Configure and start the Rust Brain

Create a local `secrets.env` file (gitignored) from the example template:

```bash
cp secrets.env.example secrets.env
```

Edit `secrets.env` and set your `XAI_API_KEY` plus a random pairing secret for
`GROKBOT_AUTH_TOKEN` (for example, `openssl rand -hex 24`). Then build and run
`grokbot-brain`:

```bash
./scripts/run_brain.sh
```

By default, `grokbot-brain` listens on `0.0.0.0:8765`, enforces mutual
HMAC-SHA256 authentication, suppresses voice transcripts from `INFO` logs, and
advertises `_grokbot._tcp.local.` via mDNS so the ESP-VoCat discovers it
automatically.

You can pass extra CLI flags directly to `./scripts/run_brain.sh`:

```bash
./scripts/run_brain.sh \
  --voice eve \
  --model grok-voice-latest \
  --reasoning-effort none \
  --log-transcripts \
  --web-search \
  --x-search
```

## Step 2: Grant USB serial permissions on Linux

ESP-VoCat exposes an ESP32-S3 USB-Serial/JTAG interface at `/dev/ttyACM0`
(vendor ID `303a`). On Linux, add a udev rule once so you can flash without
`sudo`:

```bash
echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE:="0666", TAG+="uaccess"' \
  | sudo tee /etc/udev/rules.d/99-esp32.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=tty --action=change
```

## Step 3: Install ESP-IDF and flash the Body firmware

Install ESP-IDF 5.5.1+ if you have not already:

```bash
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 --branch v5.5.1 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
```

From the root of this repository, build and flash the ESP-VoCat:

```bash
./scripts/build_flash.sh
```

To open the serial console right after flashing, run
`./scripts/build_flash.sh --monitor`. Press **`Ctrl+]`** to exit
`idf.py monitor` (not `Ctrl+C`, which `idf_monitor` forwards to the serial
port).

To bake Wi-Fi credentials or a static Brain IP into the build instead of using
SoftAP or mDNS, run `idf.py menuconfig` inside `firmware/` and open
**GrokBot Body Configuration**.

## Step 4: Connect Wi-Fi and pair

If Wi-Fi credentials are not stored in NVS or `menuconfig`:

1. Join the `GrokBot-Setup` WPA2 access point using the random 8-digit PIN
   displayed on the GrokBot screen (and printed to serial).
2. Open `http://192.168.4.1`.
3. Enter your Wi-Fi SSID, Wi-Fi password, and `GROKBOT_AUTH_TOKEN` (leave Brain
   IP blank to use mDNS auto-discovery, or enter your Brain host's LAN IP).
4. Save and wait for GrokBot to reboot.

No xAI API keys are entered on or stored on the ESP-VoCat.

## Controls

- **Say "Hi, ESP" or tap the screen / capacitive side pads:** Wake GrokBot to
  talk (plays a short ascending chime), or interrupt (barge in) while GrokBot
  is speaking.
- **Long-press the screen or side pads:** Put GrokBot to sleep.
- **Swipe up or down on the screen:** Adjust speaker volume in `10%` steps
  (plays a short confirmation tick tone).
- **Swipe right or left on the screen:** Adjust display brightness in `10%`
  steps.

## Face-only demo mode

To inspect the 3D face animations on hardware without Wi-Fi or the Brain
daemon, enable **GrokBot Body Configuration > Face-only demo** in
`idf.py menuconfig` and reflash.
