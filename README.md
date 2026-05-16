# moddable-webrtc

ESP32-S3 / Moddable WebRTC remote-control experiments for Stack-chan.

This repository keeps reusable signaling, browser, and firmware spike assets out of `/tmp` so WebRTC boundaries can be reproduced across WSL restarts.

## Development AppRTC signaling server

The first reusable component is a small AppRTC-compatible signaling server for LAN debugging.

It intentionally proves only the signaling layer:

- `POST /join/:roomId` returns AppRTC-style room metadata
- `GET /ice` returns AppRTC-style ICE metadata
- WebSocket `/ws?roomId=<room>&clientId=<id>` relays JSON messages to peers in the same room
- `POST /message/:roomId/:clientId` provides the AppRTC HTTP fallback relay advertised as `wss_post_url`
- `GET /debug/rooms` shows current room/client state

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
curl http://127.0.0.1:18090/debug/rooms
```
