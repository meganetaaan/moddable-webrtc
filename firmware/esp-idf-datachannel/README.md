# ESP-IDF native DataChannel probe

This is the native-only CoreS3 slice for issue #8. It intentionally uses the repository AppRTC signaling server and keeps the control plane to DataChannel ping/pong.

## Minimum next milestone

The next hardware run is successful if it reaches one of these boundaries:

- DataChannel opens and the browser probe receives `{"type":"pong","from":"cores3"}` after its ping.
- `/debug/events` plus serial logs prove the exact stop point: offer received, `esp_peer_send_msg` failed, no local answer callback, answer relayed but ICE stalled, or DataChannel never opened.

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

Do not commit generated `sdkconfig` with real credentials.

## Run

Terminal 1, from the repository root:

```sh
PUBLIC_BASE_URL=http://<lan-host-ip>:18090 PORT=18090 npm run start:signaling
```

Terminal 2, from this directory:

```sh
idf.py build
idf.py -p <serial-port> flash monitor
```

Browser:

```text
http://<lan-host-ip>:18090/probe?signal=http://<lan-host-ip>:18090&room=stackchan&role=offerer&icePolicy=all
```

Server inspection:

```sh
curl http://<lan-host-ip>:18090/ping
curl http://<lan-host-ip>:18090/debug/rooms
curl http://<lan-host-ip>:18090/debug/events
```

## Expected serial evidence

Capture these lines with timestamps:

- Wi-Fi connected state and IP address.
- `/ping` and `/join` HTTP status, byte count, room, assigned client id, and `wss_url`.
- WebSocket open, close, error, incoming message type, and bounded payload prefix.
- Heap at boot, before and after `esp_peer_open`, and before and after `esp_peer_send_msg`.
- `esp_peer_open` return code.
- `esp_peer_send_msg` return code for offer and candidates.
- Local `peer on_msg(answer)` and `peer on_msg(candidate)` callbacks.
- Peer state changes.
- DataChannel message callback and pong send return code.

## Boundary Table

| Layer | Result | Evidence |
|---|---|---|
| Wi-Fi/TCP/HTTP reachability | pending | Serial IP log, `GET /ping`, signaling server log |
| Signaling room | pending | `/debug/rooms`, `/debug/events` join and ws-open |
| Peer allocation/open | pending | Heap before/after + `esp_peer_open ret=` |
| Offer delivery | pending | Serial `signaling message type=offer` + `esp_peer_send_msg type=offer ret=` |
| Answer callback | pending | Serial `peer on_msg(answer)` and `/debug/events` answer summary |
| ICE/DTLS/SCTP | pending | Peer state logs and candidate callbacks/events |
| DataChannel | pending | DataChannel callback + browser ping/pong log |

Update the Result column to pass/fail during each run and paste the exact evidence beside it.

## Native-only data path

Current slice:

```text
Browser probe RTCPeerConnection
  -> AppRTC JSON over /ws
  -> CoreS3 ESP-IDF app
  -> esp_peer
  -> SCTP DataChannel stackchan-control ping/pong
```

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
- CPU pressure during ICE/DTLS/SCTP setup and while ping/pong is active.
- Latency from browser ping timestamp to CoreS3 callback and browser pong receipt.
- Contention with CoreS3 peripherals that future Stack-chan features need: display, speaker, microphone, servos, and camera if attached.

## Fallback plan

If WebRTC/DataChannel proves too heavy, keep this AppRTC/WebRTC run as evidence and evaluate a smaller transport separately:

- WebSocket-only control channel for LAN diagnostics.
- MQTT/WebSocket command bridge for tiny servo/personality messages.
- WebRTC only on a host gateway, with CoreS3 using a simpler local protocol.

These are fallbacks, not replacements for this issue's native WebRTC feasibility check.
