import assert from 'node:assert/strict';
import { afterEach, beforeEach, describe, it } from 'node:test';
import WebSocket from 'ws';

import { createSignalingServer } from './apprtc-signal.js';

async function readJson(response) {
  return JSON.parse(await response.text());
}

async function listen(app) {
  await new Promise((resolve) => app.server.listen(0, '127.0.0.1', resolve));
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

describe('AppRTC-compatible signaling server', () => {
  let app;
  let baseUrl;

  beforeEach(async () => {
    app = createSignalingServer({ publicBaseUrl: 'http://device-host.test:18090' });
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
    assert.equal(first.params.wss_url, 'ws://device-host.test:18090/ws');

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

  it('relays messages posted to the advertised HTTP fallback URL', async () => {
    const a = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const b = await readJson(await fetch(`${baseUrl}/join/stackchan`, { method: 'POST' }));
    const socketB = new WebSocket(wsUrl(baseUrl, 'stackchan', b.params.client_id));
    await waitForOpen(socketB);

    const receivedByB = waitForMessage(socketB);
    const response = await fetch(a.params.wss_post_url.replace('http://device-host.test:18090', baseUrl), {
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
    await fetch(a.params.wss_post_url.replace('http://device-host.test:18090', baseUrl), {
      method: 'POST',
      body: JSON.stringify({ type: 'candidate', candidate: 'candidate:1 1 udp 2122260223 192.0.2.1 54545 typ host generation 0' }),
    });
    await receivedCandidate;

    await new Promise((resolve) => {
      socketA.once('close', resolve);
      socketA.close();
    });
    socketB.close();

    const response = await fetch(`${baseUrl}/debug/events`);
    assert.equal(response.status, 200);
    const { events } = await readJson(response);

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
      { event: 'close', roomId: 'stackchan', clientId: a.params.client_id, transport: undefined },
    ]);

    const messageEvents = events.filter((event) => event.event === 'message');
    assert.deepEqual(messageEvents.map((event) => event.message), [
      { type: 'offer', sdpLength: 22 },
      { type: 'candidate', candidate: 'candidate:1 1 udp 2122260223 192.0.2.1 54545 typ host generation...' },
    ]);
  });
});
