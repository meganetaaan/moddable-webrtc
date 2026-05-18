# CoreS3 speaker-only ESP-IDF sample

This sample intentionally avoids Wi-Fi and WebRTC. It only initializes the CoreS3
speaker power path and writes a generated stereo tone to the speaker I2S pins.

Use it to separate "WebRTC/browser audio arrived" from "CoreS3 built-in speaker
can make sound".

## Build and flash

```bash
cd firmware/esp-idf-speaker-tone
source /home/openclaw/.local/share/esp32/esp-idf-release-v6/export.sh
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

Expected serial evidence:

```text
speaker-tone: i2c scan found ...
speaker-tone: power rails ret=ESP_OK
speaker-tone: aw9523 reg02_before=...
speaker-tone: amp enable ret=...
speaker-tone: i2s start sample_rate=48000 bclk=34 ws=33 dout=13
speaker-tone: wrote frames=...
```

If `wrote frames` increases but no sound is audible, the current boundary is the
CoreS3 speaker power/amp/output path rather than WebRTC, RTP, or PCMA decode.
