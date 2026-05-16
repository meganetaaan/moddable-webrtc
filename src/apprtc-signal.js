import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomBytes } from 'node:crypto';
import { WebSocketServer } from 'ws';

const ROOT_DIR = dirname(dirname(fileURLToPath(import.meta.url)));

const DEFAULT_ICE_SERVERS = [
  { urls: ['stun:stun.l.google.com:19302'], username: 'unused', credential: 'unused' },
];

function json(response, statusCode, body) {
  const payload = JSON.stringify(body);
  response.writeHead(statusCode, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': Buffer.byteLength(payload),
  });
  response.end(payload);
}

async function textFile(response, contentType, path) {
  const payload = await readFile(path, 'utf8');
  response.writeHead(200, {
    'content-type': `${contentType}; charset=utf-8`,
    'content-length': Buffer.byteLength(payload),
  });
  response.end(payload);
}

function readRequestBody(request) {
  return new Promise((resolve, reject) => {
    let body = '';
    request.setEncoding('utf8');
    request.on('data', (chunk) => {
      body += chunk;
    });
    request.on('end', () => resolve(body));
    request.on('error', reject);
  });
}

function clientId() {
  return `device-${randomBytes(4).toString('hex')}`;
}

function wsUrlFromBase(publicBaseUrl) {
  const url = new URL('/ws', publicBaseUrl);
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
  return url.toString();
}

function roomSnapshot(rooms) {
  const snapshot = {};
  for (const [roomId, room] of rooms) {
    snapshot[roomId] = {
      clients: Array.from(room.clients.values(), (client) => ({
        clientId: client.clientId,
        connected: Boolean(client.socket && client.socket.readyState === client.socket.OPEN),
      })),
    };
  }
  return snapshot;
}

function ensureRoom(rooms, roomId) {
  if (!rooms.has(roomId)) {
    rooms.set(roomId, { clients: new Map() });
  }
  return rooms.get(roomId);
}

function removeClient(rooms, roomId, clientId) {
  const room = rooms.get(roomId);
  if (!room) return;
  room.clients.delete(clientId);
  if (room.clients.size === 0) {
    rooms.delete(roomId);
  }
}

function relayMessage(room, senderId, message) {
  const outbound = JSON.stringify({ from: senderId, message });
  for (const peer of room.clients.values()) {
    if (peer.clientId !== senderId && peer.socket && peer.socket.readyState === peer.socket.OPEN) {
      peer.socket.send(outbound);
    }
  }
}

export function createSignalingServer(options = {}) {
  const {
    publicBaseUrl = `http://127.0.0.1:${process.env.PORT || 18090}`,
    iceServers = DEFAULT_ICE_SERVERS,
  } = options;
  const rooms = new Map();

  const server = http.createServer(async (request, response) => {
    const url = new URL(request.url, publicBaseUrl);

    if (request.method === 'POST' && url.pathname.startsWith('/join/')) {
      await readRequestBody(request);
      const roomId = decodeURIComponent(url.pathname.slice('/join/'.length));
      if (!roomId) {
        json(response, 400, { result: 'ERROR', error: 'room id is required' });
        return;
      }

      const room = ensureRoom(rooms, roomId);
      const id = clientId();
      const isInitiator = room.clients.size === 0;
      room.clients.set(id, { clientId: id, socket: null });

      json(response, 200, {
        result: 'SUCCESS',
        params: {
          room_id: roomId,
          client_id: id,
          is_initiator: String(isInitiator),
          messages: [],
          wss_url: wsUrlFromBase(publicBaseUrl),
          wss_post_url: `${publicBaseUrl}/message/${encodeURIComponent(roomId)}/${id}`,
          ice_server_url: `${publicBaseUrl}/ice`,
          pc_config: { iceServers },
        },
      });
      return;
    }

    if (request.method === 'GET' && url.pathname === '/ice') {
      json(response, 200, { result: 'SUCCESS', iceServers });
      return;
    }

    if (request.method === 'GET' && url.pathname === '/probe') {
      await textFile(response, 'text/html', join(ROOT_DIR, 'public/probe.html'));
      return;
    }

    if (request.method === 'GET' && url.pathname === '/browser-probe.js') {
      await textFile(response, 'text/javascript', join(ROOT_DIR, 'src/browser-probe.js'));
      return;
    }

    if (request.method === 'POST' && url.pathname.startsWith('/message/')) {
      const [, , encodedRoomId, clientId] = url.pathname.split('/');
      const roomId = decodeURIComponent(encodedRoomId || '');
      const body = await readRequestBody(request);
      const room = rooms.get(roomId);
      if (!room || !room.clients.has(clientId)) {
        json(response, 404, { result: 'ERROR', error: 'unknown room or client' });
        return;
      }

      try {
        relayMessage(room, clientId, JSON.parse(body));
      } catch {
        json(response, 400, { result: 'ERROR', error: 'invalid json' });
        return;
      }

      json(response, 200, { result: 'SUCCESS' });
      return;
    }

    if (request.method === 'GET' && url.pathname === '/debug/rooms') {
      json(response, 200, roomSnapshot(rooms));
      return;
    }

    json(response, 404, { result: 'ERROR', error: 'not found' });
  });

  const websocketServer = new WebSocketServer({ server, path: '/ws' });
  websocketServer.on('connection', (socket, request) => {
    const url = new URL(request.url, publicBaseUrl);
    const roomId = url.searchParams.get('roomId');
    const id = url.searchParams.get('clientId');

    if (!roomId || !id) {
      socket.close(1008, 'roomId and clientId are required');
      return;
    }

    const room = ensureRoom(rooms, roomId);
    const client = room.clients.get(id) ?? { clientId: id, socket: null };
    client.socket = socket;
    room.clients.set(id, client);

    socket.on('message', (data) => {
      let message;
      try {
        message = JSON.parse(data.toString());
      } catch {
        socket.send(JSON.stringify({ result: 'ERROR', error: 'invalid json' }));
        return;
      }

      relayMessage(room, id, message);
    });

    socket.on('close', () => {
      const current = room.clients.get(id);
      if (current?.socket === socket) {
        removeClient(rooms, roomId, id);
      }
    });
  });

  return {
    server,
    close: () =>
      new Promise((resolve, reject) => {
        websocketServer.close((wsError) => {
          server.close((serverError) => {
            const error = wsError || serverError;
            if (error) reject(error);
            else resolve();
          });
        });
      }),
  };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const port = Number.parseInt(process.env.PORT || '18090', 10);
  const host = process.env.HOST || '0.0.0.0';
  const publicBaseUrl = process.env.PUBLIC_BASE_URL || `http://127.0.0.1:${port}`;
  const app = createSignalingServer({ publicBaseUrl });
  app.server.listen(port, host, () => {
    console.log(`AppRTC signaling server listening on ${host}:${port}`);
    console.log(`PUBLIC_BASE_URL=${publicBaseUrl}`);
  });
}
