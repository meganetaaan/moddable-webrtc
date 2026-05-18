import os from 'node:os';

import { createSignalingServer } from './apprtc-signal.js';

function firstLanIpv4() {
  const interfaces = os.networkInterfaces();
  for (const entries of Object.values(interfaces)) {
    for (const entry of entries ?? []) {
      if (entry.family === 'IPv4' && !entry.internal) {
        return entry.address;
      }
    }
  }
  return '127.0.0.1';
}

const port = Number.parseInt(process.env.PORT || '18091', 10);
const host = process.env.HOST || '0.0.0.0';
const lanIp = process.env.LAN_IP || firstLanIpv4();
const publicBaseUrl = process.env.PUBLIC_BASE_URL || `http://${lanIp}:${port}`;
const room = process.env.ROOM || 'stackchan';
const probeUrl = `${publicBaseUrl}/probe?signal=${encodeURIComponent(publicBaseUrl)}&room=${encodeURIComponent(room)}&role=offerer&icePolicy=all&media=audio`;

const app = createSignalingServer({ publicBaseUrl });
const keepAlive = setInterval(() => {}, 1 << 30);
app.server.on('close', () => clearInterval(keepAlive));

app.server.listen(port, host, () => {
  console.log(`AppRTC signaling server listening on ${host}:${port}`);
  console.log(`PUBLIC_BASE_URL=${publicBaseUrl}`);
  console.log(`Browser probe URL: ${probeUrl}`);
  console.log(`Firmware signaling base URL: ${publicBaseUrl}`);
});

async function shutdown() {
  await app.close();
}

process.once('SIGINT', () => {
  shutdown().finally(() => process.exit(0));
});
process.once('SIGTERM', () => {
  shutdown().finally(() => process.exit(0));
});
