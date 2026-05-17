import assert from 'node:assert/strict';
import { afterEach, beforeEach, describe, it } from 'node:test';
import WebSocket from 'ws';

import { createSignalingServer, iceServersFromEnv, isDirectRun } from './apprtc-signal.js';

async function readJson(response) {
  return JSON.parse(await response.text());
}

async function listen(app) {
  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      app.server.off('error', reject);
      reject(new Error('server listen timed out'));
    }, 2000);
    app.server.once('error', reject);
    app.server.listen(0, '127.0.0.1', () => {
      clearTimeout(timeout);
      app.server.off('error', reject);
      resolve();
    });
  });
  const { port } = app.server.address();
  return `http://127.0.0.1:${port}`;
}

function wsUrl(baseUrl, roomId, clientId) {
  const url = new URL('/ws', baseUrl);
  url.protocol = 'ws:';
  url.searchParams.set('roomId', roomId);
  url.searchParams.set('clientId', clientId);
  return url.toString();
}

function waitForOpen(socket) {
  return new Promise((resolve, reject) => {
    socket.once('open', resolve);
    socket.once('error', reject);
  });
}

function waitForMessage(socket) {
  return new Promise((resolve, reject) => {
    socket.once('message', (data) => resolve(JSON.parse(data.toString())));
    socket.once('error', reject);
  });
}

function waitForMessageWithin(socket, milliseconds = 300) {
  return Promise.race([
    waitForMessage(socket),
    new Promise((_, reject) => setTimeout(() => reject(new Error(`timed out waiting for websocket message after ${milliseconds}ms`)), milliseconds)),
  ]);
}

describe('AppRTC-compatible signaling server', () => {
  it('detects direct CLI execution when Node receives a relative script path', () => {
    assert.equal(isDirectRun(new URL('./apprtc-signal.js', import.meta.url).href, 'src/apprtc-signal.js'), true);
    assert.equal(isDirectRun(new URL('./apprtc-signal.js', import.meta.url).href, 'src/other.js'), false);
    assert.equal(isDirectRun(new URL('./apprtc-signal.js', import.meta.url).href, undefined), false);
  });

  let app;
  let baseUrl;

  beforeEach(async () => {
    app = createSignalingServer({ publicBaseUrl: 'http://device-host.test:18091' });
    baseUrl = await listen(app);
  });

  afterEach(async () => {
    await app.close();
  });

  it('returns join metadata and marks the second client as non-initiator', async () => {
    const first = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const second = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));

    assert.equal(first.result, 'SUCCESS');
    assert.equal(first.params.room_id, 'stackchan');
    assert.match(first.params.client_id, /^device-/);
    assert.equal(first.params.is_initiator, 'true');
    assert.equal(first.params.wss_url, 'ws://device-host.test:18091/ws');
    assert.equal(first.params.wss_post_url, `http://device-host.test:18091/message/stackchan/${first.params.client_id}`);
    assert.equal(first.params.ice_server_url, 'http://device-host.test:18091/ice');
    assert.deepEqual(first.params.pc_config, {
      iceServers: [{ urls: ['stun:stun.l.google.com:19302'], username: 'unused', credential: 'unused' }],
    });

    assert.equal(second.result, 'SUCCESS');
    assert.notEqual(second.params.client_id, first.params.client_id);
    assert.equal(second.params.is_initiator, 'false');
  });

  it('serves configurable ICE metadata in AppRTC shape', async () => {
    const ice = await readJson(await fetch(`${baseUrl}/ice`));

    assert.equal(ice.result, 'SUCCESS');
    assert.deepEqual(ice.iceServers, [
      { urls: ['stun:stun.l.google.com:19302'], username: 'unused', credential: 'unused' },
    ]);
  });

  it('adds TURN servers from environment config while preserving the default STUN server', async () => {
    await app.close();
    app = createSignalingServer({
      publicBaseUrl: 'http://device-host.test:18091',
      env: {
        TURN_URLS: 'turn:turn.example.com:3478?transport=udp, turns:turn.example.com:5349?transport=tcp',
        TURN_USERNAME: 'test-user',
        TURN_CREDENTIAL: 'test-secret',
      },
    });
    baseUrl = await listen(app);

    const ice = await readJson(await fetch(`${baseUrl}/ice`));

    assert.deepEqual(ice, {
      result: 'SUCCESS',
      iceServers: [
        { urls: ['stun:stun.l.google.com:19302'], username: 'unused', credential: 'unused' },
        {
          urls: ['turn:turn.example.com:3478?transport=udp', 'turns:turn.example.com:5349?transport=tcp'],
          username: 'test-user',
          credential: 'test-secret',
        },
      ],
    });
  });

  it('uses the same TURN ICE config in join pc_config and /ice', async () => {
    await app.close();
    app = createSignalingServer({
      publicBaseUrl: 'http://device-host.test:18091',
      env: {
        TURN_URLS: 'turn:relay.test:3478?transport=udp',
        TURN_USERNAME: 'join-user',
        TURN_CREDENTIAL: 'join-secret',
      },
    });
    baseUrl = await listen(app);

    const ice = await readJson(await fetch(`${baseUrl}/ice`));
    const joined = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));

    assert.deepEqual(joined.params.pc_config.iceServers, ice.iceServers);
  });

  it('can derive coturn REST-style time-limited credentials from TURN_SECRET', () => {
    const iceServers = iceServersFromEnv(
      {
        TURN_URLS: 'turn:staticauth.openrelay.metered.ca:80?transport=udp',
        TURN_SECRET: 'openrelayprojectsecret',
        TURN_TTL_SECONDS: '60',
      },
      1_770_000_000_000,
    );

    assert.deepEqual(iceServers[1], {
      urls: ['turn:staticauth.openrelay.metered.ca:80?transport=udp'],
      username: '1770000060',
      credential: 'ayJEUjsOg9J0J39G4Qncq7CqZlY=',
    });
  });

  it('serves a tiny reachability ping for firmware TCP/HTTP checks', async () => {
    const ping = await readJson(await fetch(`${baseUrl}/ping`));

    assert.deepEqual(ping, { result: 'SUCCESS', pong: true });
  });

  it('relays WebSocket messages only to peers in the same room', async () => {
    const a = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const b = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const other = await readJson(await fetch(`${baseUrl}/join/other-room`, { method: 'POST' }));

    const socketA = new WebSocket(wsUrl(baseUrl, 'stackchan', a.params.client_id));
    const socketB = new WebSocket(wsUrl(baseUrl, 'stackchan', b.params.client_id));
    const socketOther = new WebSocket(wsUrl(baseUrl, 'other-room', other.params.client_id));
    await Promise.all([waitForOpen(socketA), waitForOpen(socketB), waitForOpen(socketOther)]);

    const receivedByB = waitForMessage(socketB);
    let leaked = false;
    socketOther.once('message', () => {
      leaked = true;
    });

    socketA.send(JSON.stringify({ type: 'offer', sdp: 'v=0...' }));

    assert.deepEqual(await receivedByB, {
      from: a.params.client_id,
      message: { type: 'offer', sdp: 'v=0...' },
    });

    await new Promise((resolve) => setTimeout(resolve, 30));
    assert.equal(leaked, false);

    socketA.close();
    socketB.close();
    socketOther.close();
  });

  it('replays the latest offer to an answerer that joins after the ESP offerer is already waiting', async () => {
    const esp = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const socketEsp = new WebSocket(wsUrl(baseUrl, 'stackchan', esp.params.client_id));
    await waitForOpen(socketEsp);

    const offer = { type: 'offer', sdp: 'v=0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n' };
    socketEsp.send(JSON.stringify(offer));
    await new Promise((resolve) => setTimeout(resolve, 30));

    const phone = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const socketPhone = new WebSocket(wsUrl(baseUrl, 'stackchan', phone.params.client_id));
    await waitForOpen(socketPhone);

    assert.deepEqual(await waitForMessageWithin(socketPhone), {
      from: esp.params.client_id,
      message: offer,
    });

    const receivedAnswer = waitForMessage(socketEsp);
    socketPhone.send(JSON.stringify({ type: 'answer', sdp: 'v=0\r\na=setup:active\r\n' }));
    assert.deepEqual(await receivedAnswer, {
      from: phone.params.client_id,
      message: { type: 'answer', sdp: 'v=0\r\na=setup:active\r\n' },
    });

    const { events } = await readJson(await fetch(`${baseUrl}/debug/events`));
    assert.ok(events.some((event) => event.event === 'replay' && event.clientId === phone.params.client_id && event.from === esp.params.client_id));

    socketEsp.close();
    socketPhone.close();
  });

  it('relays messages posted to the advertised HTTP fallback URL', async () => {
    const a = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const b = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const socketB = new WebSocket(wsUrl(baseUrl, 'stackchan', b.params.client_id));
    await waitForOpen(socketB);

    const receivedByB = waitForMessage(socketB);
    const response = await fetch(a.params.wss_post_url.replace('http://device-host.test:18091', baseUrl), {
      method: 'POST',
      body: JSON.stringify({ type: 'candidate', candidate: 'candidate:1 1 udp ...' }),
    });

    assert.deepEqual(await readJson(response), { result: 'SUCCESS' });
    assert.deepEqual(await receivedByB, {
      from: a.params.client_id,
      message: { type: 'candidate', candidate: 'candidate:1 1 udp ...' },
    });

    socketB.close();
  });

  it('removes disconnected WebSocket clients so a later single client becomes initiator', async () => {
    const first = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const socket = new WebSocket(wsUrl(baseUrl, 'stackchan', first.params.client_id));
    await waitForOpen(socket);

    await new Promise((resolve) => {
      socket.once('close', resolve);
      socket.close();
    });

    const second = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));

    assert.equal(second.params.is_initiator, 'true');
  });

  it('serves the browser probe page and module', async () => {
    const page = await fetch(`${baseUrl}/probe`);
    const script = await fetch(`${baseUrl}/browser-probe.js`);

    assert.equal(page.status, 200);
    assert.equal(page.headers.get('content-type'), 'text/html; charset=utf-8');
    assert.match(await page.text(), /ESP32-S3 WebRTC browser probe/);

    assert.equal(script.status, 200);
    assert.equal(script.headers.get('content-type'), 'text/javascript; charset=utf-8');
    assert.match(await script.text(), /function parseProbeConfig/);
  });

  it('exposes debug room/client state', async () => {
    const joined = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));

    const debug = await readJson(await fetch(`${baseUrl}/debug/rooms`));

    assert.deepEqual(debug, {
      stackchan: {
        clients: [
          {
            clientId: joined.params.client_id,
            connected: false,
          },
        ],
      },
    });
  });

  it('records compact debug events for joins, websocket relay, HTTP fallback relay, and close', async () => {
    const a = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const b = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const socketA = new WebSocket(wsUrl(baseUrl, 'stackchan', a.params.client_id));
    const socketB = new WebSocket(wsUrl(baseUrl, 'stackchan', b.params.client_id));
    await Promise.all([waitForOpen(socketA), waitForOpen(socketB)]);

    const receivedOffer = waitForMessage(socketB);
    socketA.send(JSON.stringify({ type: 'offer', sdp: 'v=0\r\na=setup:actpass\r\n' }));
    await receivedOffer;

    const receivedCandidate = waitForMessage(socketB);
    await fetch(a.params.wss_post_url.replace('http://device-host.test:18091', baseUrl), {
      method: 'POST',
      body: JSON.stringify({ type: 'candidate', candidate: 'candidate:1 1 udp 2122260223 192.0.2.1 54545 typ host generation 0' }),
    });
    await receivedCandidate;

    const receivedAnswer = waitForMessage(socketA);
    socketB.send(JSON.stringify({ type: 'answer', sdp: 'v=0\r\na=setup:active\r\n' }));
    await receivedAnswer;

    await new Promise((resolve) => {
      socketA.once('close', resolve);
      socketA.close();
    });

    const response = await fetch(`${baseUrl}/debug/events`);
    assert.equal(response.status, 200);
    const { events } = await readJson(response);
    await new Promise((resolve) => {
      socketB.once('close', resolve);
      socketB.close();
    });

    const eventSummaries = events.map((event) => ({ event: event.event, roomId: event.roomId, clientId: event.clientId, transport: event.transport }));
    assert.deepEqual(eventSummaries.slice(0, 2), [
      { event: 'join', roomId: 'stackchan', clientId: a.params.client_id, transport: undefined },
      { event: 'join', roomId: 'stackchan', clientId: b.params.client_id, transport: undefined },
    ]);
    assert.deepEqual(
      eventSummaries
        .slice(2, 4)
        .sort((left, right) => left.clientId.localeCompare(right.clientId)),
      [
        { event: 'ws-open', roomId: 'stackchan', clientId: a.params.client_id, transport: undefined },
        { event: 'ws-open', roomId: 'stackchan', clientId: b.params.client_id, transport: undefined },
      ].sort((left, right) => left.clientId.localeCompare(right.clientId)),
    );
    assert.deepEqual(eventSummaries.slice(4), [
      { event: 'message', roomId: 'stackchan', clientId: a.params.client_id, transport: 'websocket' },
      { event: 'relay', roomId: 'stackchan', clientId: a.params.client_id, transport: 'websocket' },
      { event: 'message', roomId: 'stackchan', clientId: a.params.client_id, transport: 'http' },
      { event: 'relay', roomId: 'stackchan', clientId: a.params.client_id, transport: 'http' },
      { event: 'message', roomId: 'stackchan', clientId: b.params.client_id, transport: 'websocket' },
      { event: 'relay', roomId: 'stackchan', clientId: b.params.client_id, transport: 'websocket' },
      { event: 'close', roomId: 'stackchan', clientId: a.params.client_id, transport: undefined },
    ]);

    const messageEvents = events.filter((event) => event.event === 'message');
    assert.deepEqual(messageEvents.map((event) => event.message), [
      { type: 'offer', sdpLength: 22, media: [] },
      { type: 'candidate', candidate: 'candidate:1 1 udp 2122260223 192.0.2.1 54545 typ host generation...' },
      { type: 'answer', sdpLength: 21, media: [] },
    ]);
  });

  it('records SDP media lines in debug event summaries', async () => {
    const a = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const b = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const socketA = new WebSocket(wsUrl(baseUrl, 'stackchan', a.params.client_id));
    const socketB = new WebSocket(wsUrl(baseUrl, 'stackchan', b.params.client_id));
    await Promise.all([waitForOpen(socketA), waitForOpen(socketB)]);

    const receivedOffer = waitForMessage(socketB);
    socketA.send(JSON.stringify({
      type: 'offer',
      sdp: 'v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 8\r\na=mid:0\r\na=recvonly\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:1\r\n',
    }));
    await receivedOffer;

    const { events } = await readJson(await fetch(`${baseUrl}/debug/events`));
    const offerEvent = events.find((event) => event.event === 'message' && event.message.type === 'offer');
    assert.deepEqual(offerEvent.message.media, [
      'm=audio 9 UDP/TLS/RTP/SAVPF 8',
      'a=mid:0',
      'a=recvonly',
      'm=application 9 UDP/DTLS/SCTP webrtc-datachannel',
      'a=mid:1',
    ]);

    socketA.close();
    socketB.close();
  });
});
