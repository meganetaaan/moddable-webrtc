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
});
