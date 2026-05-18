# Moddable CoreS3 AudioOut tone sample

This is a WebRTC-free, ESP-IDF-raw-free speaker boundary sample that goes through
Moddable's `esp32/m5stack_cores3` target and its `M5StackCoreS3AudioOut` wrapper.

Why not use `examples/pins/audioout/resource-stream` directly?

- The stock sample streams `bflatmajor.maud` as `11025 Hz` audio.
- Moddable's CoreS3 `AW88298` wrapper notes that `11025 Hz` and multiples are not
  available on ESP32-S3/AW88298 due to PLL clock mismatch.
- This sample therefore uses `AudioOut.Tone` at `24000 Hz`, matching the
  `m5stack_cores3` target default.

Build / flash:

```sh
export MODDABLE=/home/openclaw/.local/share/moddable
export PATH="$MODDABLE/build/bin/lin/release:$PATH"
source /home/openclaw/.local/share/esp32/esp-idf-release-v6/export.sh
cd firmware/moddable-audioout-tone
mcconfig -i -m -p esp32/m5stack_cores3 -t deploy ./manifest.json
```

Expected serial evidence:

```text
[moddable-audioout-tone] start sampleRate=24000
[moddable-audioout-tone] constructed actualSampleRate=24000
[moddable-audioout-tone] audio.start done
[moddable-audioout-tone] enqueue tone hz=440
```
