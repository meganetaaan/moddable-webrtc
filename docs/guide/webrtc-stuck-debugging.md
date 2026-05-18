# WebRTC stuck debugging guide

Use this guide when a CoreS3 / ESP32-S3 WebRTC run stalls. The goal is to identify the narrow boundary, not to guess whether WebRTC is generally feasible on the device.

## First rule: name the current boundary

Every investigation should end with one sentence like:

> Current boundary: signaling relays the CoreS3 offer and browser answer, ICE reaches connected, but CoreS3 DTLS times out before DataChannel open.

Avoid broad conclusions such as "WebRTC is too heavy" unless the resource evidence actually proves that boundary.

## Boundary order

Work down this list in order. Do not skip a layer just because a later symptom is visible.

| Layer | Evidence to collect | Common false conclusion |
|---|---|---|
| Host/device reachability | CoreS3 `/ping` success, host server log, optional packet capture | WSL-local `curl` works, so the device can reach it |
| Signaling room | `/debug/rooms` expected clients | Two clients in a room means offer/answer exchanged |
| Signaling events | `/debug/events` join/ws/offer/candidate/answer sequence | SDP JSON was sent, so WebRTC progressed |
| Peer open | serial heap and `esp_peer_open ret=` | Allocation succeeded, so ICE/DTLS is working |
| SDP/candidate ingestion | `esp_peer_send_msg` return codes and media summary | `ret=0` means answer generation succeeded |
| ICE/TURN | browser candidate types, `iceConnectionState`, serial candidate logs | Browser `/ice` proves firmware is using the same TURN config |
| DTLS/SCTP | serial DTLS return codes, browser `connectionState`, DataChannel open/pong | ICE connected means DataChannel should open |
| Media negotiation | SDP media lines, `ontrack`, codec info | `ontrack` means audio/video is usable |
| Media frames | firmware tx/rx counters plus browser RTP stats | RTP stats alone prove audible audio or visible video |
| Device I/O | microphone/speaker/camera/display counters | Transport success proves peripheral integration |

## Minimal run checklist

1. Start the signaling server from the repository root.

   ```sh
   PORT=18090 ROOM=stackchan npm run start:lan-signaling
   ```

2. Confirm the server endpoints from the host.

   ```sh
   curl http://127.0.0.1:18090/ping
   curl http://127.0.0.1:18090/debug/rooms
   curl http://127.0.0.1:18090/debug/events
   ```

3. Flash/monitor the firmware and confirm the CoreS3 reaches the LAN URL printed by the helper.

4. Open one fresh browser probe tab. Close stale probe tabs before reset/reoffer tests.

5. Capture, at minimum:
   - browser console logs,
   - `/debug/rooms`,
   - `/debug/events`,
   - CoreS3 serial logs,
   - browser RTP stats when media is involved.

## Signaling checks

Use `/debug/events` before changing ESP code. A good ESP-offerer/browser-answerer run should show a compact sequence like:

```text
join CoreS3
ws-open CoreS3
offer media=[audio,application]
join browser
ws-open browser
answer media=[audio,application]
candidate ... typ host/srflx/relay
```

If the browser receives no offer, inspect stale offer lifecycle and room cleanup before touching DTLS or media. If the replayed offer has no media sections, reset/regenerate the CoreS3 offer and fix the server not to retain malformed/media-less offers as the latest offer.

## TURN and ICE checks

- Keep `/join/:room` and `/ice` returning the same ICE server list.
- After changing TURN environment variables or restarting signaling, force a fresh CoreS3 `/join`.
- Require serial evidence that the firmware parsed the intended first ICE URL.
- Redact TURN usernames, credentials, and Wi-Fi secrets in all shared logs.
- For smartphone/public-tunnel checks, use `role=answerer&icePolicy=relay` and require relay candidate evidence before interpreting failures.

## DTLS checks

If ICE reaches `connected` but browser `connectionState`, DataChannel, or RTP stats do not progress, inspect CoreS3 serial logs before changing signaling again.

Useful boundaries from previous runs:

- `DTLS: Server handshake return -0x7880` after `agent_recv timeout`: increasing the agent receive timeout can matter on relay paths.
- `ClientHello fragmentation not supported` / `-0x7080`: inspect the ESP-IDF mbedTLS source or use a branch/tag that handles fragmented initial ClientHello for the ESP DTLS server path. Do not claim a release fixes it without checking the actual mbedTLS submodule.

## Audio checks

For generated audio, microphone, and speaker work, always pair both sides of evidence.

CoreS3 → browser:

- firmware send frames/bytes/drops,
- negotiated codec/sample rate,
- browser `ontrack`, `track unmute`, and inbound RTP packets/bytes,
- audio element `play()` result and mute/volume state for audible checks.

Browser → CoreS3:

- browser outbound RTP packets/bytes,
- CoreS3 receive frames/bytes/drops,
- decode result,
- speaker write count and underrun/overrun logs.

`ontrack` alone is not enough. RTP counters alone are not enough for audible proof.

## Video checks

Start smaller than WebRTC video if needed:

1. camera capture counters only,
2. still image or low-fps JPEG stream to browser,
3. browser test pattern or static frame to CoreS3 display,
4. then evaluate WebRTC video track or a hybrid DataChannel/control + lightweight image stream design.

Every video result should include fps, latency, drops, heap/PSRAM headroom, and display/capture time.

## Checkpoint comment template

Post this to the relevant issue when handing off or pausing:

```md
## Continuity checkpoint

Current boundary: <one sentence>

Completed:
- <evidence-backed checkpoint>
- <evidence-backed checkpoint>

Evidence:
- Signaling: <debug endpoint summary>
- Browser: <console/RTP stats summary>
- CoreS3: <serial summary>

Next recommended step:
1. <single next action>
2. <verification command/log to collect>

Secrets redacted: yes
```
