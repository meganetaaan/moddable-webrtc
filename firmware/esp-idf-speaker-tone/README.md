# CoreS3 speaker-only ESP-IDF sample

This sample intentionally avoids Wi-Fi and WebRTC. It initializes the CoreS3
speaker control path, then writes a generated mono tone to the built-in speaker
I2S pins.

Use it to separate "WebRTC/browser audio arrived" from "CoreS3 built-in speaker
can make sound".

## Moddable facts reflected here

Checked against Moddable `m5stack_cores3` target/provider and AudioOut source:

- CoreS3 internal I2C is `SDA=GPIO12`, `SCL=GPIO11`.
- Moddable opens that internal I2C bus on `I2C_NUM_0` when no explicit port is
  provided.
- CoreS3 speaker pins are `BCLK=GPIO34`, `WS=GPIO33`, `DOUT=GPIO13`.
- Moddable AudioOut uses `I2S_NUM_1`, `I2S_CLK_SRC_DEFAULT`,
  `I2S_MCLK_MULTIPLE_256`, 16-bit mono, `slot_mask=I2S_STD_SLOT_LEFT`,
  `bit_shift=true`, and `left_align=false`.
- The proven raw tone uses `24000 Hz`; the WebRTC speaker sink still uses 8 kHz
  because received PCMA/G.711A frames decode to 8 kHz PCM.
- Moddable CoreS3 setup writes the AXP2101 power rails, then AW9523 output
  registers, then AW88298 registers (`0x05`, `0x61`, `0x04`, `0x0c`, `0x06`).
- `i2c_master_probe()` times out on this board even when direct register
  read/write succeeds, so the sample uses an AXP2101 `reg 0x90` read as the
  stronger bus-reachability boundary.

## Build and flash

```bash
cd firmware/esp-idf-speaker-tone
source /home/openclaw/.local/share/esp32/esp-idf-release-v6/export.sh
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

Expected serial evidence:

```text
speaker-tone: trying I2C candidate moddable-internal
speaker-tone: i2c axp-read-test port=0 sda=12 scl=11 reg90=0xbf ret=ESP_OK
speaker-tone: selected I2C candidate moddable-internal
speaker-tone: power init boundary ret=ESP_OK
speaker-tone: aw9523 init boundary ret=ESP_OK
speaker-tone: amp enable aw88298_ret=ESP_OK rate_reg=0x14c5 sample_rate=24000
speaker-tone: amp init boundary ret=ESP_OK
speaker-tone: i2s start ret=ESP_OK sample_rate=24000 bclk=34 ws=33 dout=13
speaker-tone: wrote writes=... frames=... bytes=... last_ret=ESP_OK
```

If the counters above pass but no tone is audible, the remaining boundary is
physical/audibility rather than WebRTC: confirm CoreS3 volume/speaker hardware,
then compare with the Moddable tone sample that is known to produce sound on the
same unit.

## Next integration point

`firmware/esp-idf-datachannel` has a `STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK`
mode. In that mode `peer_audio_data_callback()` decodes received PCMA/G.711A
frames to `int16_t` PCM and writes them via the same CoreS3 speaker init/I2S
shape as this sample. Serial success should include:

```text
core-s3 speaker amp enable=1 ret=ESP_OK ... aw88298_ret=ESP_OK
core-s3 speaker i2s start ret=ESP_OK sample_rate=8000 channels=1 ...
audio rx frames=... bytes=... rms=... peak=...
audio rx speaker writes=... samples=... bytes=... drops=0 ...
```
