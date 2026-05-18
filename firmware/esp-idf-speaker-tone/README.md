# CoreS3 speaker-only ESP-IDF sample

This sample intentionally avoids Wi-Fi and WebRTC. It initializes the CoreS3
speaker control path as far as possible, then writes a generated mono tone to the
speaker I2S pins.

Use it to separate "WebRTC/browser audio arrived" from "CoreS3 built-in speaker
can make sound".

## M5Unified facts reflected here

Checked against M5Unified source:

- CoreS3 internal I2C is `SCL=GPIO11`, `SDA=GPIO12`.
- M5Unified opens internal I2C on `I2C_NUM_1` when a separate external I2C bus can
  use `I2C_NUM_0`.
- CoreS3 speaker pins are `BCLK=GPIO34`, `WS=GPIO33`, `DOUT=GPIO13`.
- M5Unified's speaker setup uses `I2S_NUM_0`, `PLL_160M`, 16-bit mono, `slot_mask=BOTH`,
  `ws_width=16`, `bit_shift=true`, and `left_align=true`.
- AW88298 register `0x06` depends on sample rate. This sample currently uses
  `44100 Hz`, matching `M5.Speaker.playRaw()`'s default, and computes the same
  lower-nibble rate value as M5Unified.
- AW88298 `RST` is reported by stackchan-kai's driver work as being wired to
  AW9523 `P0_1`; AW88298 can NACK until AW9523 releases it.
- M5Unified also enables `AW9523 reg 0x03 bit7` for `SY7088 BOOST_EN` during
  CoreS3 power setup.

## Build and flash

```bash
cd firmware/esp-idf-speaker-tone
source /home/openclaw/.local/share/esp32/esp-idf-release-v6/export.sh
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

Expected serial evidence:

```text
speaker-tone: trying I2C candidate m5unified-internal
speaker-tone: i2c probe ... addr=0x34 name=AXP2101 ret=...
speaker-tone: i2c probe ... addr=0x58 name=AW9523 ret=...
speaker-tone: boost enable boundary ret=...
speaker-tone: power init boundary ret=...
speaker-tone: amp enable ... rate_reg=0x14c7 sample_rate=44100
speaker-tone: i2s start ret=ESP_OK sample_rate=44100 bclk=34 ws=33 dout=13
speaker-tone: wrote writes=... frames=... last_ret=ESP_OK
```

If `wrote` increases but no sound is audible, and all I2C probes are still
`ESP_ERR_TIMEOUT`, the current boundary is before sample-rate playback: the
CoreS3 internal I2C/control path is not reachable from this raw ESP-IDF sample.
At that point the fastest cross-check is an M5Unified/Arduino sample using
`M5.Speaker.tone()`, preferably built with an IDF/Arduino stack known to support
M5Unified. The ESP-IDF v6 component build currently fails in M5GFX on legacy
headers/types, so that cross-check likely needs IDF 5.x or Arduino IDE/CLI.
