export function parseProbeConfig(search = globalThis.location?.search ?? '', origin = globalThis.location?.origin ?? 'http://127.0.0.1:18091') {
  const params = new URLSearchParams(search.startsWith('?') ? search.slice(1) : search);
  const role = params.get('role') === 'answerer' ? 'answerer' : 'offerer';
  const iceTransportPolicy = params.get('icePolicy') === 'relay' ? 'relay' : 'all';
  const media = ['audio', 'video'].includes(params.get('media')) ? params.get('media') : 'none';

  return {
    signalBaseUrl: params.get('signal') || origin,
    roomId: params.get('room') || 'stackchan',
    role,
    iceTransportPolicy,
    media,
  };
}

export function buildWsUrl(wssUrl, roomId, clientId) {
  const url = new URL(wssUrl);
  url.searchParams.set('roomId', roomId);
  url.searchParams.set('clientId', clientId);
  return url.toString();
}

export function buildOfferMessage(description) {
  return { type: 'offer', sdp: description.sdp };
}

export function buildAnswerMessage(description) {
  return { type: 'answer', sdp: description.sdp };
}

export function buildCandidateMessage(event) {
  if (!event.candidate) return null;
  return {
    type: 'candidate',
    candidate: event.candidate.candidate,
    id: event.candidate.sdpMid ?? '0',
    label: event.candidate.sdpMLineIndex ?? 0,
  };
}

export function normalizeRemoteCandidate(message) {
  if (typeof message === 'string') {
    return { candidate: message };
  }
  if (!message || typeof message.candidate !== 'string') {
    return null;
  }
  const candidate = { candidate: message.candidate };
  if (message.id !== undefined) candidate.sdpMid = message.id;
  if (message.label !== undefined) candidate.sdpMLineIndex = message.label;
  return candidate;
}

export function summarizeCandidate(candidateLine = '') {
  const value = String(candidateLine);
  const type = value.match(/ typ ([^ ]+)/)?.[1] ?? 'unknown';
  const protocol = value.match(/^candidate:[^ ]+ [^ ]+ ([^ ]+)/)?.[1]?.toLowerCase() ?? 'unknown';
  const address = value.match(/^candidate:[^ ]+ [^ ]+ [^ ]+ [^ ]+ ([^ ]+) ([^ ]+)/);
  return {
    type,
    protocol,
    address: address ? `${address[1]}:${address[2]}` : 'unknown',
  };
}

export function summarizeSdpMedia(sdp = '') {
  return sdp
    .split(/\r?\n/)
    .filter((line) => line.startsWith('m=') || line.startsWith('a=mid:') || line === 'a=sendonly' || line === 'a=recvonly' || line === 'a=sendrecv' || line === 'a=inactive')
    .join(' | ');
}

function summarizeIceUrl(url) {
  const value = String(url);
  const match = value.match(/^([^:]+):(.+)$/);
  if (!match) {
    return { scheme: 'unknown', host: 'unknown' };
  }

  const scheme = match[1].toLowerCase();
  const withoutQuery = match[2].split('?')[0];
  const host = withoutQuery.includes('@') ? withoutQuery.slice(withoutQuery.lastIndexOf('@') + 1) : withoutQuery;
  return { scheme, host: host || 'unknown' };
}

export function summarizeIceServers(iceServers = []) {
  const servers = Array.isArray(iceServers) ? iceServers : [];
  const urls = [];
  for (const server of servers) {
    const serverUrls = Array.isArray(server?.urls) ? server.urls : [server?.urls];
    for (const url of serverUrls) {
      if (url) urls.push(summarizeIceUrl(url));
    }
  }
  return { count: servers.length, urls };
}

export function describeDataChannelMessage(data) {
  if (typeof data !== 'string') {
    return `datachannel message bytes=${data?.byteLength ?? data?.size ?? 'unknown'}`;
  }
  try {
    const message = JSON.parse(data);
    if (message?.type === 'pong') {
      return `datachannel pong from=${message.from ?? 'unknown'} payload=${data}`;
    }
    if (message?.type === 'ping') {
      return `datachannel ping received payload=${data}`;
    }
  } catch {
    // Fall through to the raw bounded browser-console evidence below.
  }
  return `datachannel message ${data}`;
}

function appendLog(line) {
  const output = document.querySelector('#log');
  const timestamp = new Date().toISOString();
  const text = `[${timestamp}] ${line}`;
  if (output) output.textContent += `${text}\n`;
  console.log(text);
}

function setStatus(status) {
  const target = document.querySelector('#status');
  if (target) target.textContent = status;
}

function attachRemoteTrack(track, streams) {
  appendLog(`ontrack kind=${track.kind} id=${track.id} state=${track.readyState} muted=${track.muted}`);
  track.onunmute = () => appendLog(`track unmute kind=${track.kind}`);
  track.onmute = () => appendLog(`track mute kind=${track.kind}`);
  track.onended = () => appendLog(`track ended kind=${track.kind}`);

  const media = track.kind === 'video' ? document.querySelector('#remoteVideo') : document.querySelector('#remoteAudio');
  if (media && streams[0]) {
    media.srcObject = streams[0];
    media.play?.().catch((error) => appendLog(`${track.kind} autoplay blocked ${error.message}`));
  }

  if (track.kind === 'video' && media?.requestVideoFrameCallback) {
    let frames = 0;
    const countFrame = () => {
      frames += 1;
      if (frames === 1 || frames % 30 === 0) appendLog(`video frames=${frames}`);
      media.requestVideoFrameCallback(countFrame);
    };
    media.requestVideoFrameCallback(countFrame);
  }
}

function configureMedia(peer, config) {
  if (config.media === 'audio') {
    peer.addTransceiver('audio', { direction: 'recvonly' });
    appendLog('media requested audio recvonly');
  } else if (config.media === 'video') {
    peer.addTransceiver('video', { direction: 'recvonly' });
    appendLog('media requested video recvonly');
  } else {
    appendLog('media disabled');
  }
}

function startStatsLog(peer, config) {
  if (config.media === 'none') return;
  setInterval(async () => {
    try {
      const stats = await peer.getStats();
      for (const report of stats.values()) {
        if (report.type === 'inbound-rtp' && !report.isRemote && report.kind === config.media) {
          const frames = report.framesDecoded ?? report.totalSamplesReceived ?? report.packetsReceived ?? 0;
          appendLog(`stats ${report.kind} packets=${report.packetsReceived ?? 0} bytes=${report.bytesReceived ?? 0} evidence=${frames}`);
        }
      }
    } catch (error) {
      appendLog(`stats error ${error.message}`);
    }
  }, 3000);
}

function sendDataChannelPing(channel) {
  const ping = JSON.stringify({ type: 'ping', t: Date.now() });
  channel.send(ping);
  appendLog(`datachannel ping sent payload=${ping}`);
}

function bindDataChannel(channel, { sendPingOnOpen = false } = {}) {
  channel.onopen = () => {
    appendLog(`datachannel open label=${channel.label}`);
    if (sendPingOnOpen) sendDataChannelPing(channel);
  };
  channel.onmessage = (messageEvent) => appendLog(describeDataChannelMessage(messageEvent.data));
}

async function joinRoom(config) {
  const response = await fetch(`${config.signalBaseUrl}/join/${encodeURIComponent(config.roomId)}`, { method: 'POST' });
  const joined = await response.json();
  if (joined.result !== 'SUCCESS') {
    throw new Error(`join failed: ${JSON.stringify(joined)}`);
  }
  return joined.params;
}

async function createPeerConnection(config, sendMessage) {
  const iceResponse = await fetch(`${config.signalBaseUrl}/ice`);
  const ice = await iceResponse.json();
  const iceServers = ice.iceServers ?? [];
  appendLog(`ice policy=${config.iceTransportPolicy} servers=${JSON.stringify(summarizeIceServers(iceServers))}`);
  const peer = new RTCPeerConnection({
    iceServers,
    iceTransportPolicy: config.iceTransportPolicy,
  });

  peer.onicecandidate = (event) => {
    const message = buildCandidateMessage(event);
    if (message) {
      appendLog(`send candidate ${JSON.stringify(summarizeCandidate(message.candidate))} ${message.candidate.slice(0, 120)}`);
      sendMessage(message);
    } else {
      appendLog('icecandidate end-of-candidates');
    }
  };
  peer.onconnectionstatechange = () => appendLog(`connectionState=${peer.connectionState}`);
  peer.oniceconnectionstatechange = () => appendLog(`iceConnectionState=${peer.iceConnectionState}`);
  peer.onicegatheringstatechange = () => appendLog(`iceGatheringState=${peer.iceGatheringState}`);
  peer.onsignalingstatechange = () => appendLog(`signalingState=${peer.signalingState}`);
  peer.onicecandidateerror = (event) => appendLog(`icecandidateerror url=${event.url ?? 'unknown'} code=${event.errorCode ?? 'unknown'} text=${event.errorText ?? 'unknown'}`);
  peer.ontrack = (event) => attachRemoteTrack(event.track, event.streams);
  peer.ondatachannel = (event) => {
    const channel = event.channel;
    appendLog(`datachannel received label=${channel.label}`);
    bindDataChannel(channel, { sendPingOnOpen: true });
  };

  configureMedia(peer, config);
  startStatsLog(peer, config);
  return peer;
}

async function handleRemoteMessage(peer, payload, sendMessage) {
  const message = payload.message ?? payload;
  appendLog(`recv ${message.type ?? 'raw'} from ${payload.from ?? 'unknown'}`);

  if (message.type === 'offer') {
    appendLog(`remote offer media ${summarizeSdpMedia(message.sdp)}`);
    await peer.setRemoteDescription({ type: 'offer', sdp: message.sdp });
    const answer = await peer.createAnswer();
    await peer.setLocalDescription(answer);
    sendMessage(buildAnswerMessage(answer));
    appendLog(`answer media ${summarizeSdpMedia(answer.sdp)}`);
    return;
  }

  if (message.type === 'answer') {
    appendLog(`remote answer media ${summarizeSdpMedia(message.sdp)}`);
    await peer.setRemoteDescription({ type: 'answer', sdp: message.sdp });
    return;
  }

  if (message.type === 'candidate' || typeof message === 'string') {
    const candidate = normalizeRemoteCandidate(message);
    if (candidate) await peer.addIceCandidate(candidate);
  }
}

export async function startBrowserProbe() {
  const config = parseProbeConfig();
  setStatus(`joining ${config.roomId} as ${config.role}`);
  appendLog(`config ${JSON.stringify(config)}`);

  const joined = await joinRoom(config);
  appendLog(`joined clientId=${joined.client_id} initiator=${joined.is_initiator}`);

  let ws;
  const sendMessage = (message) => {
    ws.send(JSON.stringify(message));
  };

  const peer = await createPeerConnection(config, sendMessage);
  // Create a browser-originated DataChannel for both roles. ESP-peer's default
  // SCTP server role waits for the remote peer's DCEP open, so answerer mode
  // must not rely only on `ondatachannel`.
  const dataChannel = peer.createDataChannel('stackchan-control');
  bindDataChannel(dataChannel, { sendPingOnOpen: true });

  // Use the URL the browser successfully fetched for signaling, not necessarily
  // the AppRTC-advertised URL. In WSL mirrored/portproxy setups the ESP can use
  // the LAN address while Windows Chrome must use localhost to reach the same
  // server.
  const browserWsBaseUrl = new URL('/ws', config.signalBaseUrl);
  browserWsBaseUrl.protocol = browserWsBaseUrl.protocol === 'https:' ? 'wss:' : 'ws:';
  appendLog(`websocket endpoint ${browserWsBaseUrl.toString()}`);
  ws = new WebSocket(buildWsUrl(browserWsBaseUrl.toString(), config.roomId, joined.client_id));
  ws.onopen = async () => {
    appendLog('websocket open');
    setStatus('signaling connected');
    if (config.role === 'offerer') {
      const offer = await peer.createOffer();
      await peer.setLocalDescription(offer);
      sendMessage(buildOfferMessage(offer));
      appendLog(`offer sent media ${summarizeSdpMedia(offer.sdp)}`);
    }
  };
  ws.onmessage = async (event) => {
    await handleRemoteMessage(peer, JSON.parse(event.data), sendMessage);
  };
  ws.onerror = () => appendLog('websocket error');
  ws.onclose = () => {
    appendLog('websocket close');
    setStatus('signaling closed');
  };

  return { config, joined, peer, ws };
}

if (typeof document !== 'undefined') {
  document.querySelector('#start')?.addEventListener('click', () => {
    startBrowserProbe().catch((error) => {
      appendLog(`error ${error.stack || error.message}`);
      setStatus('error');
    });
  });
}
