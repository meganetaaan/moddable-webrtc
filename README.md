# moddable-webrtc

ESP32-S3 / Moddable WebRTC remote-control experiments for Stack-chan.

Current goal: prove M5Stack CoreS3 ↔ PC WebRTC DataChannel ping/pong plus one media path, then use the evidence to decide the next native firmware step.

This repository keeps reusable signaling, browser, and firmware spike assets out of `/tmp` so WebRTC boundaries can be reproduced across WSL restarts.

When a run gets stuck, use `AGENTS.md` for the short operational rules and `docs/guide/webrtc-stuck-debugging.md` for the detailed boundary checklist.

## Development AppRTC signaling server

The first reusable component is a small AppRTC-compatible signaling server for LAN debugging.

It intentionally proves only the signaling layer:

- `POST /join/:roomId` returns AppRTC-style room metadata
- `GET /ice` returns AppRTC-style ICE metadata
- WebSocket `/ws?roomId=<room>&clientId=<id>` relays JSON messages to peers in the same room
- `POST /message/:roomId/:clientId` provides the AppRTC HTTP fallback relay advertised as `wss_post_url`
- `GET /debug/rooms` shows current room/client state
- `GET /ping` returns a tiny JSON reachability response for firmware TCP/HTTP checks

### Install and test

```bash
npm install
npm test
```

### Run locally

```bash
PUBLIC_BASE_URL=http://<windows-or-lan-host-ip>:18090 \
PORT=18090 \
npm run start:signaling
```

Or let the helper choose the first non-loopback IPv4 address and print the exact audio probe URL:

```bash
PORT=18090 ROOM=stackchan npm run start:lan-signaling
```

For the CoreS3/Stack-chan firmware, use the same host and port in the AppRTC signaling URL, for example:

```text
http://192.168.7.135:18090
```

On WSL2, keep the boundary explicit:

1. prove device → host TCP/HTTP reachability with a tiny `/ping` or `curl`/`tcpdump` probe,
2. prove `/join/:roomId`,
3. prove `/ws` relay,
4. only then debug SDP/ICE/DataChannel.

### Debug endpoints

```bash
curl -X POST http://127.0.0.1:18090/join/stackchan
curl http://127.0.0.1:18090/ice
curl http://127.0.0.1:18090/ping
curl http://127.0.0.1:18090/debug/rooms
curl http://127.0.0.1:18090/debug/events
```

`/debug/events` keeps a bounded in-memory trace of recent signaling activity. It records joins, WebSocket connects/closes, and compact message summaries such as `offer` SDP length or a truncated ICE candidate prefix. Use it before debugging ESP WebRTC internals:

1. confirm both browser and CoreS3 appear in `/debug/rooms`,
2. confirm the browser `offer` appears in `/debug/events`,
3. confirm raw `candidate` messages appear after the offer,
4. confirm a CoreS3 `answer` appears before investigating ICE/DataChannel failures.

## Browser signaling probe

After the signaling server is running, open the browser probe from the same server:

```text
http://127.0.0.1:18090/probe?room=stackchan&role=offerer&icePolicy=all&media=audio
```

For CoreS3/Stack-chan LAN checks, use the Windows/LAN host address that the device can reach:

```text
http://192.168.7.135:18090/probe?signal=http://192.168.7.135:18090&room=stackchan&role=offerer&icePolicy=all&media=audio
```

The probe joins the room, opens `/ws`, creates a DataChannel in browser-offerer mode, optionally adds one `recvonly` media transceiver with `media=audio` or `media=video`, sends an SDP offer, and sends ICE candidates as raw candidate lines in AppRTC-style messages:

```json
{
  "type": "candidate",
  "candidate": "candidate:...",
  "id": "0",
  "label": 0
}
```

This keeps the next boundary narrow:

1. Start the signaling server with `PUBLIC_BASE_URL` set to the LAN-reachable URL.
2. Start the CoreS3 firmware pointed at the same base URL and room.
3. Open `/probe` in a browser with the same room.
4. Watch `/debug/rooms`, `/debug/events`, browser logs, and CoreS3 serial logs.
5. Only interpret WebRTC feasibility after confirming `/join`, `/ws`, offer, and raw candidate delivery.

## ESP-IDF native CoreS3 DataChannel probe

The native-only issue #8/#9 scaffold lives in `firmware/esp-idf-datachannel/`.

It joins the same AppRTC signaling server, opens the advertised `/ws`, forwards browser-offerer `offer` and raw `candidate` messages into `esp_peer`, logs `esp_peer_open` / `esp_peer_send_msg` return codes and heap boundaries, replies to DataChannel ping with a tiny pong if SCTP opens, and can negotiate a send-only generated PCMA 440 Hz tone test source or guarded CoreS3 ES7210 mic source for browser `ontrack`/RTP counter evidence.

Audio was chosen before video because `esp_peer` directly supports G.711 A-law audio frames and browsers can receive PCMA without CoreS3 camera/H.264 plumbing. The mic source is currently a narrow raw I2S bridge aligned to Espressif's `esp_capture` contract where possible; the intended robust follow-up is adopting `esp_capture` and passing acquired frame `pts/data/size` directly into `esp_peer_send_audio()`. See `firmware/esp-idf-datachannel/README.md` for menuconfig fields, hardware run steps, resource metrics, fallback plan, and the boundary table for the next CoreS3 run.
