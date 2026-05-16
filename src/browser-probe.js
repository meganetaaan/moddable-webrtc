export function parseProbeConfig(search = globalThis.location?.search ?? '', origin = globalThis.location?.origin ?? 'http://127.0.0.1:18090') {
  const params = new URLSearchParams(search.startsWith('?') ? search.slice(1) : search);
  const role = params.get('role') === 'answerer' ? 'answerer' : 'offerer';
  const iceTransportPolicy = params.get('icePolicy') === 'relay' ? 'relay' : 'all';

  return {
    signalBaseUrl: params.get('signal') || origin,
    roomId: params.get('room') || 'stackchan',
    role,
    iceTransportPolicy,
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
  const peer = new RTCPeerConnection({
    iceServers: ice.iceServers ?? [],
    iceTransportPolicy: config.iceTransportPolicy,
  });

  peer.onicecandidate = (event) => {
    const message = buildCandidateMessage(event);
    if (message) {
      appendLog(`send candidate ${message.candidate.slice(0, 80)}`);
      sendMessage(message);
    }
  };
  peer.onconnectionstatechange = () => appendLog(`connectionState=${peer.connectionState}`);
  peer.oniceconnectionstatechange = () => appendLog(`iceConnectionState=${peer.iceConnectionState}`);
  peer.ondatachannel = (event) => {
    const channel = event.channel;
    appendLog(`datachannel received label=${channel.label}`);
    channel.onopen = () => appendLog(`datachannel open label=${channel.label}`);
    channel.onmessage = (messageEvent) => appendLog(`datachannel message ${messageEvent.data}`);
  };

  return peer;
}

async function handleRemoteMessage(peer, payload, sendMessage) {
  const message = payload.message ?? payload;
  appendLog(`recv ${message.type ?? 'raw'} from ${payload.from ?? 'unknown'}`);

  if (message.type === 'offer') {
    await peer.setRemoteDescription({ type: 'offer', sdp: message.sdp });
    const answer = await peer.createAnswer();
    await peer.setLocalDescription(answer);
    sendMessage(buildAnswerMessage(answer));
    return;
  }

  if (message.type === 'answer') {
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

  const ws = new WebSocket(buildWsUrl(joined.wss_url, config.roomId, joined.client_id));
  const sendMessage = (message) => {
    ws.send(JSON.stringify(message));
  };

  const peer = await createPeerConnection(config, sendMessage);
  let dataChannel;
  if (config.role === 'offerer') {
    dataChannel = peer.createDataChannel('stackchan-control');
    dataChannel.onopen = () => {
      appendLog('datachannel open label=stackchan-control');
      dataChannel.send(JSON.stringify({ type: 'ping', t: Date.now() }));
    };
    dataChannel.onmessage = (event) => appendLog(`datachannel message ${event.data}`);
  }

  ws.onopen = async () => {
    appendLog('websocket open');
    setStatus('signaling connected');
    if (config.role === 'offerer') {
      const offer = await peer.createOffer();
      await peer.setLocalDescription(offer);
      sendMessage(buildOfferMessage(offer));
      appendLog('offer sent');
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
