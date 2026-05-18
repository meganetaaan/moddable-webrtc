# Agent instructions

This repository is an ESP32-S3 / CoreS3 WebRTC remote-control spike for Stack-chan. Prefer small, measurable slices and keep every claim tied to logs, browser stats, or source inspection.

## When a WebRTC run gets stuck

Do not summarize the failure as "ESP32-S3 is too weak" until each boundary below has evidence. Move one layer at a time and record the current boundary in the relevant issue.

1. **Host/device reachability**
   - Prove CoreS3 can reach the signaling host with `/ping` before debugging WebRTC.
   - On WSL/Windows, distinguish host-local `curl` success from CoreS3-to-host reachability.

2. **Signaling correctness**
   - Check `/debug/rooms` for exactly the expected connected clients.
   - Check `/debug/events` for `join`, `ws-open`, `offer`, raw `candidate`, `answer`, and close events.
   - Do not rely on room presence alone; it does not prove SDP/candidate relay.

3. **Firmware instrumentation**
   - Capture serial logs with timestamps.
   - Include heap before/after `esp_peer_open`, return codes from `esp_peer_open` and `esp_peer_send_msg`, SDP media summaries, peer state changes, DataChannel callbacks, and media counters.

4. **ICE / TURN / DTLS boundaries**
   - If TURN settings change, force a fresh firmware `/join` and confirm the serial log shows the expected first ICE URL. Browser `/ice` alone is not enough.
   - Redact TURN credentials and Wi-Fi secrets from logs and issue comments.
   - If ICE connects but DataChannel/media do not, inspect CoreS3 DTLS return codes before changing signaling again.

5. **Media proof discipline**
   - For audio, pair firmware frame counters with browser RTP stats. `ontrack` alone is not success.
   - For audible checks, verify the audio element, browser autoplay result, volume/mute state, and OS mixer. Constant PCMA near-silence is not an audible proof.
   - For microphone, speaker, camera, and display work, first prove hardware-only counters, then transport counters, then user-visible audio/video.

6. **Issue handoff**
   - Post a concise checkpoint comment when pausing: current slice, boundary table, exact evidence, and next recommended command or hardware action.
   - Use the detailed guide in `docs/guide/webrtc-stuck-debugging.md` for the full checklist and boundary table.

## Implementation style

- Keep generated audio/DataChannel as a known-good transport baseline while adding device I/O.
- Vary one axis at a time: offerer role, media shape, ICE policy, signaling lifecycle, or device peripheral.
- Add focused tests for signaling/browser changes before hardware runs when possible.
- Keep reusable assets in this repository, not under `/tmp`.
