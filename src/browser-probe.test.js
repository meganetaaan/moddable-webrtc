import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import {
  buildCandidateMessage,
  buildOfferMessage,
  buildWsUrl,
  normalizeRemoteCandidate,
  parseProbeConfig,
} from './browser-probe.js';

describe('browser probe helpers', () => {
  it('parses signaling, room, role, and ICE policy from query parameters', () => {
    const config = parseProbeConfig('?signal=http%3A%2F%2F192.168.7.135%3A18090&room=stackchan&role=answerer&icePolicy=relay', 'http://localhost:18090');

    assert.deepEqual(config, {
      signalBaseUrl: 'http://192.168.7.135:18090',
      roomId: 'stackchan',
      role: 'answerer',
      iceTransportPolicy: 'relay',
    });
  });

  it('defaults to browser-offerer in the current origin and stackchan room', () => {
    assert.deepEqual(parseProbeConfig('', 'http://127.0.0.1:18090'), {
      signalBaseUrl: 'http://127.0.0.1:18090',
      roomId: 'stackchan',
      role: 'offerer',
      iceTransportPolicy: 'all',
    });
  });

  it('builds raw-candidate AppRTC messages instead of serializing the full RTCIceCandidate object', () => {
    const message = buildCandidateMessage({
      candidate: {
        candidate: 'candidate:842163049 1 udp 1677729535 192.168.7.10 59902 typ srflx',
        sdpMid: '0',
        sdpMLineIndex: 0,
        usernameFragment: 'ignored-by-apprtc',
        toJSON() {
          return { candidate: this.candidate, usernameFragment: this.usernameFragment };
        },
      },
    });

    assert.deepEqual(message, {
      type: 'candidate',
      candidate: 'candidate:842163049 1 udp 1677729535 192.168.7.10 59902 typ srflx',
      id: '0',
      label: 0,
    });
  });

  it('skips empty end-of-candidates events', () => {
    assert.equal(buildCandidateMessage({ candidate: null }), null);
  });

  it('normalizes raw ESP candidate strings for browser addIceCandidate', () => {
    assert.deepEqual(normalizeRemoteCandidate('candidate:1 1 udp 1 192.168.7.125 9 typ host'), {
      candidate: 'candidate:1 1 udp 1 192.168.7.125 9 typ host',
    });
  });

  it('normalizes AppRTC candidate messages for browser addIceCandidate', () => {
    assert.deepEqual(
      normalizeRemoteCandidate({
        type: 'candidate',
        candidate: 'candidate:1 1 udp 1 192.168.7.125 9 typ host',
        id: 'data',
        label: 1,
      }),
      {
        candidate: 'candidate:1 1 udp 1 192.168.7.125 9 typ host',
        sdpMid: 'data',
        sdpMLineIndex: 1,
      },
    );
  });

  it('builds offer messages with only the SDP fields the signaling peer needs', () => {
    assert.deepEqual(buildOfferMessage({ type: 'offer', sdp: 'v=0\r\n...' }), {
      type: 'offer',
      sdp: 'v=0\r\n...',
    });
  });

  it('builds a room/client websocket URL from the AppRTC join response URL', () => {
    assert.equal(
      buildWsUrl('ws://192.168.7.135:18090/ws', 'stackchan', 'device-1234'),
      'ws://192.168.7.135:18090/ws?roomId=stackchan&clientId=device-1234',
    );
  });
});
