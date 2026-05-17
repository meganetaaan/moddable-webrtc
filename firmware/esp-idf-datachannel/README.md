# ESP-IDF native DataChannel + audio probe

This is the native-only CoreS3 slice for the current goal: M5Stack CoreS3 ↔ PC WebRTC DataChannel ping/pong plus one media path. It intentionally uses the repository AppRTC signaling server, keeps the control plane to tiny DataChannel ping/pong, and adds a generated send-only PCMA audio source as the first media proof.

Audio is the first media path because `esp_peer` 1.4.1 exposes `audio_dir`, `on_audio_data`, and `esp_peer_send_audio()`, supports G.711 A-law/PCMA, and the browser can receive that codec without camera or H.264 encoder work. CoreS3 microphone capture is not wired yet; the generated source proves SDP audio negotiation, RTP/SRTP send, browser `ontrack`, and receive counters before adding I2S peripheral risk.

## Minimum next milestone

The next hardware run is successful if it reaches one of these boundaries:

- DataChannel opens and the browser probe receives `{"type":"pong","from":"cores3"}` after its ping.
- Browser `media=audio` produces an audio `ontrack` event and inbound RTP stats while firmware logs `audio tx frames=...`.
- `/debug/events` plus serial logs prove the exact stop point: offer received, media section missing, `esp_peer_send_msg` failed, no local answer callback, answer relayed but ICE stalled, DataChannel never opened, or media counters never move.

## Configure

From this directory:

```sh
idf.py set-target esp32s3
idf.py menuconfig
```

Set:

- `Stack-chan ESP-IDF DataChannel probe -> Wi-Fi SSID`
- `Stack-chan ESP-IDF DataChannel probe -> Wi-Fi password`
- `Stack-chan ESP-IDF DataChannel probe -> AppRTC signaling base URL`
- `Stack-chan ESP-IDF DataChannel probe -> AppRTC signaling room`
- `Stack-chan ESP-IDF DataChannel probe -> WebRTC media mode`
  - `Audio: generated PCMA test source` for the native media proof.
  - `None: DataChannel only` if you need to bisect a DataChannel regression.
  - `Video placeholder` only documents the future camera path and leaves media disabled.

Do not commit generated `sdkconfig` with real credentials.

## Partition layout

This project tracks `partitions.csv` and selects it from `sdkconfig.defaults` with `CONFIG_PARTITION_TABLE_CUSTOM=y`. CoreS3 is documented by M5Stack with 16 MB flash, so `sdkconfig.defaults` also selects `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`.

Current layout:

| Name | Type | Subtype | Offset | Size | Purpose |
|---|---|---|---:|---:|---|
| `nvs` | `data` | `nvs` | `0x9000` | `0x6000` | Wi-Fi and ESP-IDF key-value storage |
| `phy_init` | `data` | `phy` | `0xf000` | `0x1000` | RF calibration data |
| `factory` | `app` | `factory` | `0x10000` | `0x700000` | Native WebRTC DataChannel + audio probe |

The factory app slot is 7 MiB instead of the ESP-IDF default 1 MiB. The previous clean ESP-IDF build produced `stackchan_espidf_datachannel.bin binary size 0xfb5f0 bytes`, leaving only `0x4a10 bytes (2%) free` in the default partition. With this custom table, the same binary reports `Smallest app partition is 0x700000 bytes` and `0x604a10 bytes (86%) free`. The default layout was too close to the limit for the next transport work, especially microphone, camera, display, speaker, and peripheral measurements. Keep this layout single-factory for now; OTA partitions would reduce the immediate app headroom and are not needed to prove CoreS3 to PC DataChannel plus one media path.

## Run

Terminal 1, from the repository root:

```sh
PUBLIC_BASE_URL=http://<lan-host-ip>:18090 PORT=18090 npm run start:signaling
```

Or:

```sh
PORT=18090 ROOM=stackchan npm run start:lan-signaling
```

The helper prints the browser probe URL and the firmware signaling base URL. Override `LAN_IP=<lan-host-ip>` if the first detected non-loopback IPv4 address is not reachable from the CoreS3.

Terminal 2, from this directory:

```sh
idf.py build
idf.py -p <serial-port> flash monitor
```

Browser:

```text
http://<lan-host-ip>:18090/probe?signal=http://<lan-host-ip>:18090&room=stackchan&role=offerer&icePolicy=all&media=audio
```

Server inspection:

```sh
curl http://<lan-host-ip>:18090/ping
curl http://<lan-host-ip>:18090/debug/rooms
curl http://<lan-host-ip>:18090/debug/events
```

## Expected serial evidence

Capture these lines with timestamps:

- Build output: `stackchan_espidf_datachannel.bin binary size ...` and the app partition size/free percentage line.
- Wi-Fi connected state and IP address.
- `/ping` and `/join` HTTP status, byte count, room, assigned client id, and `wss_url`.
- WebSocket open, close, error, incoming message type, and bounded payload prefix.
- Heap at boot, before and after `esp_peer_open`, and before and after `esp_peer_send_msg`.
- `esp_peer_open` return code.
- `esp_peer_send_msg` return code for offer and candidates.
- Local `peer on_msg(answer)` and `peer on_msg(candidate)` callbacks.
- Peer state changes.
- DataChannel message callback and pong send return code.
- Media mode, negotiated audio info, audio task start, `audio tx frames`, `audio tx bytes`, `audio tx drops`, and heap while audio is running.
- Browser `remote answer media ... m=audio ... a=sendonly`, `ontrack kind=audio`, `track unmute kind=audio`, and `stats audio packets=... bytes=...`.

## Boundary Table

| Layer | Result | Evidence |
|---|---|---|
| Wi-Fi/TCP/HTTP | pending | CoreS3 IP, `GET /ping`, HTTP status |
| Signaling room | pending | `/debug/rooms`, `/debug/events` join and ws-open |
| Peer open | pending | Heap before/after + `esp_peer_open ret=` |
| Offer/answer | pending | SDP summaries, serial `peer on_msg(answer)`, `/debug/events` answer summary |
| ICE/DTLS/SCTP | pending | Peer state logs and candidate callbacks/events |
| DataChannel | pending | DataChannel callback + browser ping/pong log |
| Media negotiation | pending | Browser offer/answer media summary, firmware `audio info`, browser `ontrack` |
| Media samples/frames | pending | Firmware `audio tx frames/bytes/drops`, browser inbound RTP stats, heap/CPU notes |

Update the Result column to pass/fail during each run and paste the exact evidence beside it.

## Native-only data path

Current slice:

```text
Browser probe RTCPeerConnection
  -> AppRTC JSON over /ws
  -> CoreS3 ESP-IDF app
  -> esp_peer
  -> SCTP DataChannel stackchan-control ping/pong
  -> SRTP audio track using generated 8 kHz mono G.711 A-law frames
```

The audio source currently sends generated PCMA silence/test frames. To replace it with CoreS3 capture, wire an I2S microphone driver that produces 8 kHz mono G.711 A-law frames or add a narrow encoder step before `esp_peer_send_audio()`. Keep the existing generated source as a known-good transport baseline until microphone capture is independently measured.

Future Moddable boundary:

```text
Moddable JS control surface
  -> native bridge
  -> ESP-IDF WebRTC/DataChannel transport
```

Do not add the future bridge in this slice. First prove whether native ESP-IDF WebRTC/DataChannel is feasible on CoreS3 with measurable resource headroom.

## Resource constraints to measure

- Internal heap and minimum heap before/after peer open and after ICE starts.
- PSRAM free space on CoreS3.
- CPU pressure during ICE/DTLS/SCTP setup and while ping/pong plus audio RTP are active.
- Latency from browser ping timestamp to CoreS3 callback and browser pong receipt.
- Audio packet cadence, `audio tx drops`, browser inbound RTP packet/byte growth, and whether browser audio track mutes.
- Contention with CoreS3 peripherals that future Stack-chan features need: display, speaker, microphone, servos, and camera if attached.

## Fallback plan

If WebRTC/DataChannel proves too heavy, keep this AppRTC/WebRTC run as evidence and evaluate a smaller transport separately:

- WebSocket-only control channel for LAN diagnostics.
- MQTT/WebSocket command bridge for tiny servo/personality messages.
- WebRTC only on a host gateway, with CoreS3 using a simpler local protocol.

These are fallbacks, not replacements for this issue's native WebRTC feasibility check.
