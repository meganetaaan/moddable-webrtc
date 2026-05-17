import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHmac, randomBytes } from 'node:crypto';
import { WebSocketServer } from 'ws';

const ROOT_DIR = dirname(dirname(fileURLToPath(import.meta.url)));

export const DEFAULT_ICE_SERVERS = [
  { urls: ['stun:stun.l.google.com:19302'], username: 'unused', credential: 'unused' },
];

function parseTurnUrls(rawUrls) {
  if (!rawUrls) return [];
  return rawUrls
    .split(',')
    .map((url) => url.trim())
    .filter(Boolean)
    .map((url) => {
      const match = url.match(/^(turns?):([^/?#]+)(\?[^#]*)?$/i);
      if (!match) {
        throw new Error(`Invalid TURN URL: ${url}`);
      }
      const host = match[2].includes('@') ? match[2].slice(match[2].lastIndexOf('@') + 1) : match[2];
      if (!host) {
        throw new Error(`Invalid TURN URL host: ${url}`);
      }
      return url;
    });
}

function turnCredentialsFromEnv(env, now = Date.now()) {
  if (env.TURN_SECRET) {
    const ttlSeconds = Number.parseInt(env.TURN_TTL_SECONDS || '86400', 10);
    const expiresAt = Math.floor(now / 1000) + (Number.isFinite(ttlSeconds) && ttlSeconds > 0 ? ttlSeconds : 86400);
    const username = String(expiresAt);
    const credential = createHmac('sha1', env.TURN_SECRET).update(username).digest('base64');
    return { username, credential };
  }

  if (!env.TURN_USERNAME || !env.TURN_CREDENTIAL) {
    throw new Error('TURN_USERNAME and TURN_CREDENTIAL, or TURN_SECRET, are required when TURN_URLS is set');
  }
  return { username: env.TURN_USERNAME, credential: env.TURN_CREDENTIAL };
}

export function iceServersFromEnv(env = process.env, now = Date.now()) {
  const turnUrls = parseTurnUrls(env.TURN_URLS);
  if (turnUrls.length === 0) {
    return DEFAULT_ICE_SERVERS;
  }
  const turnCredentials = turnCredentialsFromEnv(env, now);
  return [
    {
      urls: turnUrls,
      ...turnCredentials,
    },
    ...DEFAULT_ICE_SERVERS,
  ];
}

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
    rooms.set(roomId, { clients: new Map(), latestOffer: null });
  }
  return rooms.get(roomId);
}

function removeClient(rooms, roomId, clientId) {
  const room = rooms.get(roomId);
  if (!room) return;
  room.clients.delete(clientId);
  if (room.latestOffer?.from === clientId) {
    room.latestOffer = null;
  }
  if (room.clients.size === 0) {
    rooms.delete(roomId);
  }
}

function summarizeMessage(message) {
  if (!message || typeof message !== 'object') {
    return { type: typeof message };
  }
  if (message.type === 'offer' || message.type === 'answer') {
    const sdp = typeof message.sdp === 'string' ? message.sdp : '';
    const media = sdp
      .split(/\r?\n/)
      .filter((line) => line.startsWith('m=') || line.startsWith('a=mid:') || line === 'a=sendonly' || line === 'a=recvonly' || line === 'a=sendrecv' || line === 'a=inactive');
    return { type: message.type, sdpLength: sdp.length, media };
  }
  if (message.type === 'candidate') {
    const candidate = typeof message.candidate === 'string' ? message.candidate : '';
    return { type: 'candidate', candidate: candidate.length > 64 ? `${candidate.slice(0, 64)}...` : candidate };
  }
  return { type: message.type ?? 'unknown' };
}

function relayMessage(room, senderId, message) {
  let delivered = 0;
  const outbound = JSON.stringify({ from: senderId, message });
  for (const peer of room.clients.values()) {
    if (peer.clientId !== senderId && peer.socket && peer.socket.readyState === peer.socket.OPEN) {
      peer.socket.send(outbound);
      delivered += 1;
    }
  }
  return delivered;
}

function rememberReplayableMessage(room, senderId, message) {
  if (message?.type === 'offer') {
    room.latestOffer = { from: senderId, message };
  }
}

function replayLatestOffer(room, clientId, socket, role) {
  if (role !== 'answerer') return null;
  const latestOffer = room.latestOffer;
  if (!latestOffer || latestOffer.from === clientId) return null;
  const offerer = room.clients.get(latestOffer.from);
  if (!offerer?.socket || offerer.socket.readyState !== offerer.socket.OPEN) {
    room.latestOffer = null;
    return null;
  }
  socket.send(JSON.stringify({ from: latestOffer.from, message: latestOffer.message }));
  return latestOffer;
}

export function isDirectRun(metaUrl = import.meta.url, argv1 = process.argv[1]) {
  if (!argv1) return false;
  const modulePath = fileURLToPath(metaUrl);
  const resolvedArgv = resolve(argv1);
  return modulePath === resolvedArgv;
}

export function createSignalingServer(options = {}) {
  const publicBaseUrl = options.publicBaseUrl ?? `http://127.0.0.1:${process.env.PORT || 18091}`;
  const iceServers = options.iceServers ?? iceServersFromEnv(options.env ?? process.env);
  const rooms = new Map();
  const events = [];

  function recordEvent(event) {
    events.push({ timestamp: new Date().toISOString(), ...event });
    if (events.length > 200) events.splice(0, events.length - 200);
  }

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
      recordEvent({ event: 'join', roomId, clientId: id, initiator: isInitiator });

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

    if (request.method === 'GET' && url.pathname === '/ping') {
      json(response, 200, { result: 'SUCCESS', pong: true });
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
        const message = JSON.parse(body);
        recordEvent({ event: 'message', roomId, clientId, transport: 'http', message: summarizeMessage(message) });
        rememberReplayableMessage(room, clientId, message);
        const delivered = relayMessage(room, clientId, message);
        recordEvent({ event: 'relay', roomId, clientId, transport: 'http', delivered });
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

    if (request.method === 'GET' && url.pathname === '/debug/events') {
      json(response, 200, { events });
      return;
    }

    json(response, 404, { result: 'ERROR', error: 'not found' });
  });

  const websocketServer = new WebSocketServer({ server, path: '/ws' });
  websocketServer.on('error', (error) => {
    recordEvent({ event: 'ws-server-error', error: error.message });
  });
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
    recordEvent({ event: 'ws-open', roomId, clientId: id });
    const replayRole = url.searchParams.get('role');
    const replayedOffer = replayLatestOffer(room, id, socket, replayRole);
    if (replayedOffer) {
      recordEvent({ event: 'replay', roomId, clientId: id, from: replayedOffer.from, message: summarizeMessage(replayedOffer.message) });
    }

    socket.on('message', (data) => {
      let message;
      try {
        message = JSON.parse(data.toString());
      } catch {
        socket.send(JSON.stringify({ result: 'ERROR', error: 'invalid json' }));
        return;
      }

      recordEvent({ event: 'message', roomId, clientId: id, transport: 'websocket', message: summarizeMessage(message) });
      rememberReplayableMessage(room, id, message);
      const delivered = relayMessage(room, id, message);
      recordEvent({ event: 'relay', roomId, clientId: id, transport: 'websocket', delivered });
    });

    socket.on('close', () => {
      const current = room.clients.get(id);
      if (current?.socket === socket) {
        recordEvent({ event: 'close', roomId, clientId: id });
        removeClient(rooms, roomId, id);
      }
    });
  });

  return {
    server,
    close: async () => {
      for (const socket of websocketServer.clients) {
        socket.close();
      }
      await new Promise((resolve, reject) => {
        server.close((error) => {
          if (error) reject(error);
          else resolve();
        });
      });
      await new Promise((resolve, reject) => {
        websocketServer.close((error) => {
          if (error) reject(error);
          else resolve();
        });
      });
    },
  };
}

if (isDirectRun()) {
  const port = Number.parseInt(process.env.PORT || '18091', 10);
  const host = process.env.HOST || '0.0.0.0';
  const publicBaseUrl = process.env.PUBLIC_BASE_URL || `http://127.0.0.1:${port}`;
  const app = createSignalingServer({ publicBaseUrl });
  const keepAlive = setInterval(() => {}, 1 << 30);
  app.server.on('close', () => clearInterval(keepAlive));
  app.server.listen(port, host, () => {
    console.log(`AppRTC signaling server listening on ${host}:${port}`);
    console.log(`PUBLIC_BASE_URL=${publicBaseUrl}`);
  });
}
