# moddable-webrtc

ESP32-S3 / Moddable WebRTC remote-control experiments for Stack-chan.

Current goal: prove M5Stack CoreS3 ↔ PC WebRTC DataChannel ping/pong plus one media path, then use the evidence to decide the next native firmware step.

This repository keeps reusable signaling, browser, and firmware spike assets out of `/tmp` so WebRTC boundaries can be reproduced across WSL restarts.

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
PUBLIC_BASE_URL=http://<windows-or-lan-host-ip>:18091 \
PORT=18091 \
npm run start:signaling
```

For external smartphone checks, expose the same signaling server through a Cloudflare Tunnel and configure TURN relay credentials on the server. `TURN_URLS` is comma-separated and may contain `turn:` and `turns:` URLs. If `TURN_URLS` is unset, `/ice` keeps the default STUN-only response. Use `TURN_USERNAME`/`TURN_CREDENTIAL` for long-term credentials, or `TURN_SECRET` for coturn REST-style time-limited credentials.

```bash
PUBLIC_BASE_URL=https://stackchan.example.trycloudflare.com \
TURN_URLS='turn:turn.example.com:3478?transport=udp,turns:turn.example.com:5349?transport=tcp' \
TURN_USERNAME='example-user' \
TURN_CREDENTIAL='example-secret' \
npm run start:signaling
```

For coturn static-auth-secret / TURN REST API style credentials:

```bash
PUBLIC_BASE_URL=https://stackchan.example.trycloudflare.com \
TURN_URLS='turn:turn.example.com:3478?transport=udp,turns:turn.example.com:5349?transport=tcp' \
TURN_SECRET='example-shared-secret' \
TURN_TTL_SECONDS=86400 \
npm run start:signaling
```

Or let the helper choose the first non-loopback IPv4 address and print the exact audio probe URL:

```bash
PORT=18091 ROOM=stackchan npm run start:lan-signaling
```

For the CoreS3/Stack-chan firmware, use the same host and port in the AppRTC signaling URL, for example:

```text
http://192.168.7.135:18091
```

On WSL2, keep the boundary explicit:

1. prove device → host TCP/HTTP reachability with a tiny `/ping` or `curl`/`tcpdump` probe,
2. prove `/join/:roomId`,
3. prove `/ws` relay,
4. only then debug SDP/ICE/DataChannel.

### Debug endpoints

```bash
curl -X POST http://127.0.0.1:18091/join/stackchan
curl http://127.0.0.1:18091/ice
curl http://127.0.0.1:18091/ping
curl http://127.0.0.1:18091/debug/rooms
curl http://127.0.0.1:18091/debug/events
```

`/debug/events` keeps a bounded in-memory trace of recent signaling activity. It records joins, WebSocket connects/closes, and compact message summaries such as `offer` SDP length or a truncated ICE candidate prefix. Use it before debugging ESP WebRTC internals:

1. confirm both browser and CoreS3 appear in `/debug/rooms`,
2. confirm the browser `offer` appears in `/debug/events`,
3. confirm raw `candidate` messages appear after the offer,
4. confirm a CoreS3 `answer` appears before investigating ICE/DataChannel failures.

## Browser signaling probe

After the signaling server is running, open the browser probe from the same server:

```text
http://127.0.0.1:18091/probe?room=stackchan&role=offerer&icePolicy=all&media=audio
```

For CoreS3/Stack-chan LAN checks, use the Windows/LAN host address that the device can reach:

```text
http://192.168.7.135:18091/probe?signal=http://192.168.7.135:18091&room=stackchan&role=offerer&icePolicy=all&media=audio
```

For smartphone relay checks through the tunnel, force relay candidates and point the probe at the public signaling URL:

```text
https://stackchan.example.trycloudflare.com/probe?signal=https://stackchan.example.trycloudflare.com&room=stackchan&role=answerer&icePolicy=relay&media=audio
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

For the ESP-offerer firmware mode, open the same URL with `role=answerer`; the browser will answer the CoreS3 SDP offer, open a browser-originated DataChannel, send a ping, and log the CoreS3 pong.

When `/ice` includes TURN credentials, the browser probe logs only the ICE policy plus URL schemes and hosts. It does not print TURN usernames or credentials.

This keeps the next boundary narrow:

1. Start the signaling server with `PUBLIC_BASE_URL` set to the LAN-reachable URL.
2. Start the CoreS3 firmware pointed at the same base URL and room.
3. Open `/probe` in a browser with the same room.
4. Watch `/debug/rooms`, `/debug/events`, browser logs, and CoreS3 serial logs.
5. Only interpret WebRTC feasibility after confirming `/join`, `/ws`, offer, and raw candidate delivery.

## ESP-IDF native CoreS3 DataChannel probe

The native-only issue #8/#9 scaffold lives in `firmware/esp-idf-datachannel/`.

It joins the same AppRTC signaling server, opens the advertised `/ws`, can either answer a browser offer or create an ESP offer, forwards raw `candidate` messages into `esp_peer`, logs `esp_peer_open` / `esp_peer_send_msg` return codes and heap boundaries, replies to DataChannel ping with a tiny pong if SCTP opens, and can negotiate a send-only generated PCMA audio test source for browser `ontrack`/RTP counter evidence.

Audio was chosen before video because `esp_peer` directly supports G.711 A-law audio frames and browsers can receive PCMA without CoreS3 camera/H.264 plumbing. See `firmware/esp-idf-datachannel/README.md` for menuconfig fields, hardware run steps, resource metrics, fallback plan, and the boundary table for the next CoreS3 run.
