import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import {
  buildCandidateMessage,
  buildOfferMessage,
  buildWsUrl,
  describeDataChannelMessage,
  formatMediaElementState,
  normalizeRemoteCandidate,
  parseProbeConfig,

  summarizeCandidate,
  summarizeIceCandidate,
  summarizeIceServers,
  summarizeInboundRtpReport,

  summarizeSdpMedia,
  summarizeSelectedCandidatePair,
} from './browser-probe.js';

describe('browser probe helpers', () => {
  it('parses signaling, room, role, ICE policy, and media from query parameters', () => {
    const config = parseProbeConfig('?signal=http%3A%2F%2F192.168.7.135%3A18091&room=stackchan&role=answerer&icePolicy=relay&media=audio', 'http://localhost:18091');

    assert.deepEqual(config, {
      signalBaseUrl: 'http://192.168.7.135:18091',
      roomId: 'stackchan',
      role: 'answerer',
      iceTransportPolicy: 'relay',
      media: 'audio',
    });
  });

  it('defaults to browser-offerer in the current origin and stackchan room', () => {
    assert.deepEqual(parseProbeConfig('', 'http://127.0.0.1:18091'), {
      signalBaseUrl: 'http://127.0.0.1:18091',
      roomId: 'stackchan',
      role: 'offerer',
      iceTransportPolicy: 'all',
      media: 'none',
    });
  });

  it('ignores unknown media modes', () => {
    assert.equal(parseProbeConfig('?media=screen', 'http://127.0.0.1:18091').media, 'none');
  });

  it('treats media=mic as a browser audio recvonly request', () => {
    assert.equal(parseProbeConfig('?media=mic', 'http://127.0.0.1:18090').media, 'audio');
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

  it('summarizes SDP media sections for logs', () => {
    assert.equal(
      summarizeSdpMedia('v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 8\r\na=mid:0\r\na=recvonly\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:1\r\n'),
      'm=audio 9 UDP/TLS/RTP/SAVPF 8 | a=mid:0 | a=recvonly | m=application 9 UDP/DTLS/SCTP webrtc-datachannel | a=mid:1',
    );
  });

  it('builds a room/client websocket URL from the AppRTC join response URL', () => {
    assert.equal(
      buildWsUrl('ws://192.168.7.135:18091/ws', 'stackchan', 'device-1234'),
      'ws://192.168.7.135:18091/ws?roomId=stackchan&clientId=device-1234',
    );
  });

  it('marks answerer WebSocket URLs as replay-capable without changing firmware URLs', () => {
    assert.equal(
      buildWsUrl('wss://stackchan.example/ws', 'stackchan', 'device-phone', { role: 'answerer' }),
      'wss://stackchan.example/ws?roomId=stackchan&clientId=device-phone&role=answerer',
    );
  });

  it('summarizes ICE servers for logs without exposing TURN credentials', () => {
    const summary = summarizeIceServers([
      { urls: ['stun:stun.l.google.com:19302'], username: 'unused', credential: 'unused' },
      {
        urls: ['turn:turn.example.com:3478?transport=udp', 'turns:secret-user:secret-pass@turn.example.com:5349?transport=tcp'],
        username: 'turn-user',
        credential: 'turn-secret',
      },
    ]);

    assert.equal(summary.count, 2);
    assert.deepEqual(summary.urls, [
      { scheme: 'stun', host: 'stun.l.google.com:19302' },
      { scheme: 'turn', host: 'turn.example.com:3478' },
      { scheme: 'turns', host: 'turn.example.com:5349' },
    ]);
    assert.equal(JSON.stringify(summary).includes('turn-user'), false);
    assert.equal(JSON.stringify(summary).includes('turn-secret'), false);
    assert.equal(JSON.stringify(summary).includes('secret-pass'), false);
  });

  it('summarizes candidates by type without needing full verbose browser logs', () => {
    assert.deepEqual(
      summarizeCandidate('candidate:842163049 1 udp 1677729535 203.0.113.10 59902 typ relay raddr 0.0.0.0 rport 0'),
      { type: 'relay', protocol: 'udp', address: '203.0.113.10:59902' },
    );
  });

  it('labels DataChannel pong payloads as decisive evidence', () => {
    assert.equal(
      describeDataChannelMessage('{"type":"pong","from":"cores3"}'),
      'datachannel pong from=cores3 payload={"type":"pong","from":"cores3"}',
    );
  });


  it('summarizes browser ICE candidates so LAN reachability failures are obvious', () => {
    assert.equal(
      summarizeIceCandidate('candidate:2365990239 1 udp 2113937151 e864669a-b16f-4dd3-9f0c-8eb2c2ea7009.local 52455 typ host'),
      'candidate type=host protocol=udp address=e864669a-b16f-4dd3-9f0c-8eb2c2ea7009.local mdns=true port=52455',
    );
    assert.equal(
      summarizeIceCandidate('candidate:2140150961 1 udp 1677729535 153.169.14.35 47597 typ srflx raddr 0.0.0.0 rport 0'),
      'candidate type=srflx protocol=udp address=153.169.14.35 port=47597',
    );
  });

  it('summarizes selected ICE candidate-pair stats when transport is established', () => {
    const stats = new Map([
      ['transport-1', { type: 'transport', selectedCandidatePairId: 'pair-1' }],
      ['pair-1', { type: 'candidate-pair', state: 'succeeded', nominated: true, localCandidateId: 'local-1', remoteCandidateId: 'remote-1', bytesSent: 42, bytesReceived: 84 }],
      ['local-1', { type: 'local-candidate', candidateType: 'host', protocol: 'udp', address: '192.168.7.10', port: 50000 }],
      ['remote-1', { type: 'remote-candidate', candidateType: 'host', protocol: 'udp', address: '192.168.7.125', port: 50712 }],
    ]);
    assert.equal(
      summarizeSelectedCandidatePair(stats),
      'ice selected-pair state=succeeded nominated=true local=host/udp/192.168.7.10:50000 remote=host/udp/192.168.7.125:50712 bytesSent=42 bytesReceived=84',
    );
  });

  it('summarizes missing selected ICE candidate-pair stats before connectivity succeeds', () => {
    assert.equal(summarizeSelectedCandidatePair(new Map()), 'ice selected-pair none');
  });

  it('summarizes audio inbound RTP with packet, byte, and sample evidence', () => {
    assert.equal(
      summarizeInboundRtpReport({
        type: 'inbound-rtp',
        kind: 'audio',
        packetsReceived: 12,
        bytesReceived: 1920,
        totalSamplesReceived: 960,
      }),
      'stats audio packets=12 bytes=1920 evidence=960',
    );
  });

  it('formats audio element playback state for audible-proof logs', () => {
    assert.equal(
      formatMediaElementState('audio', 'play-resolved', {
        paused: false,
        muted: false,
        volume: 1,
        readyState: 4,
        currentTime: 0.25,
      }),
      'audio element play-resolved paused=false muted=false volume=1 readyState=4 currentTime=0.250',
    );
  });
});
