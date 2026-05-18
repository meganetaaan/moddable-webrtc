# CoreS3 microphone audio runbook

This runbook is the hardware checklist for issue #14, CoreS3 microphone audio received by a PC/browser through WebRTC.

Use it together with `docs/guide/webrtc-stuck-debugging.md`. Do not treat browser `ontrack` alone as success; pair CoreS3 capture/send counters with browser RTP stats and audio element state.

## Modes

| Mode | Firmware Kconfig | Browser probe media | Purpose |
|---|---|---|---|
| Generated audio baseline | `STACKCHAN_MEDIA_AUDIO_TEST_SOURCE=y` | `media=audio` | Known-good PCMA tone transport baseline. |
| CoreS3 mic to browser | `STACKCHAN_MEDIA_CORE_S3_MIC_SOURCE=y` | `media=audio` | Main issue #14 proof: CoreS3 ES7210 mic capture -> PCMA -> `esp_peer_send_audio()` -> browser receive. |
| CoreS3 mic plus browser mic speaker sink | `STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX=y` | `media=audio-duplex` | Bidirectional diagnostic mode. CoreS3 sends mic audio and plays browser->CoreS3 PCMA frames on the CoreS3 speaker while logging receive/decode/write counters. |
| CoreS3 mic plus generated browser tone speaker sink | `STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX=y` | `media=audio-tone` | Same as duplex, but the browser sends a Web Audio 440 Hz tone instead of requiring microphone permission/device availability. Use this in headless or no-mic environments. |

Keep generated audio available as the transport baseline while changing device I/O.

## Build and flash

From the firmware directory:

```sh
cd firmware/esp-idf-datachannel
source /home/openclaw/.local/share/esp32/esp-idf-release-v6/export.sh >/dev/null
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

For the main mic proof, enable:

```text
CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_SOURCE=y
```

For the duplex diagnostic proof, enable:

```text
CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX=y
```

Do not paste Wi-Fi or TURN credentials into issue comments. If `idf.py build` prints local `sdkconfig` default mismatch warnings, redact those values before sharing logs.

## Signaling and browser probe

Start signaling from the repository root. Use a port reachable from the CoreS3; previous CoreS3 runs used `18091`.

```sh
PORT=18091 ROOM=stackchan npm run start:lan-signaling
```

Confirm the host endpoints:

```sh
curl http://127.0.0.1:18091/ping
curl http://127.0.0.1:18091/debug/rooms
curl http://127.0.0.1:18091/debug/events
```

Open one fresh browser probe tab for the CoreS3 mic receive proof:

```text
http://127.0.0.1:18091/probe?room=stackchan&role=answerer&media=audio
```

For duplex diagnostics, use browser mic permission and:

```text
http://127.0.0.1:18091/probe?room=stackchan&role=answerer&media=audio-duplex
```

If the browser machine has no microphone or permission is unavailable, use the generated tone source instead. It still exercises browser outbound RTP -> CoreS3 receive/decode/speaker write boundaries:

```text
http://127.0.0.1:18091/probe?room=stackchan&role=answerer&media=audio-tone
```

Close stale probe tabs before resetting the CoreS3. If a fresh answerer does not receive an offer, inspect `/debug/rooms` and `/debug/events`; do not change media code before proving signaling freshness.

## Expected CoreS3 serial evidence

Mic/I2S capture should log RMS, peak, sample count, short reads, and clips:

```text
core-s3 mic acquire=11250 samples=1800000 rms=67 peak=128 read_failures=11017 short_reads=0 clips=0 bytes_read=1280 frame_samples=160 pts=224980
core-s3 mic acquire=11700 samples=1872000 rms=225 peak=770 read_failures=11112 short_reads=0 clips=0 bytes_read=1280 frame_samples=160 pts=233980
```

RMS/peak should change with voice, tapping, or another external stimulus. If counters are static or near zero, the current boundary is `microphone/I2S capture`.

WebRTC transport should show ICE, DTLS/SRTP, DataChannel, and media send counters:

```text
Channel bind success, connection DONE
DTLS: SRTP connected OK
PEER_DEF: DTLS handshake success
datachannel open label=stackchan-control stream_id=0
datachannel pong ret=0 bytes=31
core-s3-mic audio tx frames=100 bytes=16000 drops=0 ret_ok=100 ret_fail=0 pts_delta=20
core-s3-mic audio tx frames=2850 bytes=456000 drops=0 ret_ok=2850 ret_fail=0 pts_delta=20
```

If mic acquire logs progress but `core-s3-mic audio tx` does not, the current boundary is `RTP send path / esp_peer_send_audio()`. If send counters progress but the browser has no RTP packets, the boundary is `DTLS/SRTP or browser receive`.

## Expected browser evidence

The browser probe should show all of these, not just `ontrack`:

```text
ontrack kind=audio id=a0 state=live muted=true
audio element play-resolved paused=false muted=false volume=1 readyState=4
track unmute kind=audio
datachannel pong from=cores3
stats audio packets=106 bytes=16960 evidence=16800
stats audio packets=2504 bytes=400640 evidence=400800
audio element timeupdate ... currentTime=51.649
```

Interpretation:

- `ontrack` + `track unmute`: media negotiation and remote track became live.
- `stats audio packets/bytes`: inbound RTP is actually increasing.
- `audio element play-resolved` + `timeupdate`: browser playback pipeline is advancing.
- `datachannel pong`: existing DataChannel success path is not broken.

A human still needs to confirm audible sound by ear. If the element plays and RTP increases but no sound is heard, the current boundary is `browser playback / OS audio output`, not CoreS3 mic capture or RTP send.

## Duplex diagnostic evidence

With `CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX=y` and `media=audio-duplex` or `media=audio-tone`, the browser should additionally log outbound RTP stats and the CoreS3 should log browser audio receive/decode/speaker-write counters.

Browser:

```text
media requested audio sendrecv
browser mic acquired tracks=1
stats outbound audio packets=<n> bytes=<n> evidence=<samples>
```

For no-mic environments, the browser generated-tone mode should show:

```text
media requested audio sendrecv with browser tone source
browser tone acquired tracks=1 frequency=440
stats outbound audio packets=<n> bytes=<n> evidence=<samples>
```

CoreS3:

```text
audio rx frames=<n> bytes=<n> empty=<n> pts=<n> pts_delta=<n> pts_discont=<n> last_size=<n>
audio rx decode samples=<n> invalid=<n> rms=<n> peak=<n> min=<n> max=<n>
audio rx speaker writes=<n> samples=<n> bytes=<n> drops=<n> short_writes=<n> last_ret=ESP_OK
```

This mode is for boundary diagnostics. A human still needs to confirm speaker audibility by ear; the serial evidence proves the receive/decode/write pipeline, not acoustic output.

## Boundary table for issue comments

| Boundary | Success evidence | Failure symptom |
|---|---|---|
| Mic/I2S capture | `core-s3 mic acquire` samples increase; RMS/peak react to stimulus | no acquire logs, I2S timeout only, static RMS/peak |
| Conversion | PCMA frame size matches frame samples; clips logged | clips climb or send sizes are wrong |
| RTP send | `core-s3-mic audio tx` frames/bytes increase, `drops=0`, `ret_fail=0` | send counters absent or `ret_fail` increases |
| DTLS/SRTP | `SRTP connected OK`, peer connected state | ICE connected but no DTLS success |
| Browser receive/playback | `track unmute`, inbound RTP packets/bytes, `play-resolved`, `timeupdate` | `ontrack` only, no packets/bytes, autoplay/volume blocked |

## Checkpoint comment template

```md
## CoreS3 mic audio checkpoint

Current boundary: <e.g. CoreS3 mic -> PCMA -> RTP -> browser playback pipeline is passing; audible-by-ear check remains human-only.>

Commit:
- `<commit sha>`

Firmware mode:
- `<CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_SOURCE or CONFIG_STACKCHAN_MEDIA_CORE_S3_MIC_DUPLEX>`

Verification:
- `npm test`: <result>
- `idf.py build`: <result>
- Hardware: <port/device and short result>

CoreS3 serial evidence:
```text
<redacted serial snippets>
```

Browser evidence:
```text
<probe stats snippets>
```

Boundary table:
- Mic/I2S capture: <OK / blocked + evidence>
- Conversion: <OK / blocked + evidence>
- RTP send: <OK / blocked + evidence>
- DTLS/SRTP: <OK / blocked + evidence>
- Browser receive/playback: <OK / blocked + evidence>

Next step:
- <single next command or hardware action>

Secrets redacted: yes
```
