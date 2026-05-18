export function parseProbeConfig(search = globalThis.location?.search ?? '', origin = globalThis.location?.origin ?? 'http://127.0.0.1:18091') {
  const params = new URLSearchParams(search.startsWith('?') ? search.slice(1) : search);
  const role = params.get('role') === 'answerer' ? 'answerer' : 'offerer';
  const iceTransportPolicy = params.get('icePolicy') === 'relay' ? 'relay' : 'all';
  const requestedMedia = params.get('media') === 'mic' ? 'audio' : params.get('media');
  const media = ['audio', 'audio-duplex', 'audio-tone', 'video'].includes(requestedMedia) ? requestedMedia : 'none';

  return {
    signalBaseUrl: params.get('signal') || origin,
    roomId: params.get('room') || 'stackchan',
    role,
    iceTransportPolicy,
    media,
  };
}


export function buildWsUrl(wssUrl, roomId, clientId, options = {}) {
  const url = options.signalBaseUrl ? new URL('/ws', options.signalBaseUrl) : new URL(wssUrl);
  if (url.protocol === 'https:') url.protocol = 'wss:';
  if (url.protocol === 'http:') url.protocol = 'ws:';
  url.searchParams.set('roomId', roomId);
  url.searchParams.set('clientId', clientId);
  if (options.role === 'answerer') {
    url.searchParams.set('role', 'answerer');
  }
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

export function formatMediaElementState(kind, label, media) {
  return `${kind} element ${label} paused=${media.paused} muted=${media.muted} volume=${media.volume} readyState=${media.readyState} currentTime=${media.currentTime.toFixed(3)}`;
}

export function summarizeInboundRtpReport(report) {
  const evidence = report.kind === 'audio'
    ? (report.totalSamplesReceived ?? report.packetsReceived ?? 0)
    : (report.framesDecoded ?? report.packetsReceived ?? 0);
  return `stats ${report.kind} packets=${report.packetsReceived ?? 0} bytes=${report.bytesReceived ?? 0} evidence=${evidence}`;
}

export function summarizeOutboundRtpReport(report) {
  const evidence = report.kind === 'audio'
    ? (report.totalSamplesSent ?? report.packetsSent ?? 0)
    : (report.framesEncoded ?? report.packetsSent ?? 0);
  return `stats outbound ${report.kind} packets=${report.packetsSent ?? 0} bytes=${report.bytesSent ?? 0} evidence=${evidence}`;
}

export function summarizeIceCandidate(candidate = '') {
  const parts = candidate.trim().split(/\s+/);
  const typIndex = parts.indexOf('typ');
  const type = typIndex >= 0 ? parts[typIndex + 1] : 'unknown';
  const protocol = parts[2] ?? 'unknown';
  const address = parts[4] ?? 'unknown';
  const port = parts[5] ?? 'unknown';
  const mdns = address.endsWith('.local');
  return `candidate type=${type} protocol=${protocol} address=${address}${mdns ? ' mdns=true' : ''} port=${port}`;
}

export function explainCandidateConnectivity(candidate = '') {
  const parts = candidate.trim().split(/\s+/);
  const typIndex = parts.indexOf('typ');
  const type = typIndex >= 0 ? parts[typIndex + 1] : 'unknown';
  const address = parts[4] ?? '';
  if (type === 'host' && address.endsWith('.local')) {
    return 'candidate warning: host candidate uses mDNS .local address; CoreS3/esp_peer usually cannot resolve it, so use a LAN browser with mDNS disabled or TURN relay';
  }
  if (type === 'srflx') {
    return 'candidate warning: srflx candidate is public/NAT-reflexive; a same-LAN CoreS3 may not be able to send back to it without TURN or a usable host candidate';
  }
  return null;
}

export function summarizeSelectedCandidatePair(stats) {
  let selectedPair;
  const reports = Array.from(stats.values());
  for (const report of reports) {
    if (report.type === 'transport' && report.selectedCandidatePairId) {
      selectedPair = stats.get(report.selectedCandidatePairId);
      break;
    }
    if (report.type === 'candidate-pair' && report.selected) {
      selectedPair = report;
      break;
    }
  }
  if (!selectedPair) return 'ice selected-pair none';
  const local = stats.get(selectedPair.localCandidateId);
  const remote = stats.get(selectedPair.remoteCandidateId);
  return `ice selected-pair state=${selectedPair.state ?? 'unknown'} nominated=${selectedPair.nominated ?? false} local=${local?.candidateType ?? 'unknown'}/${local?.protocol ?? 'unknown'}/${local?.address ?? local?.ip ?? 'unknown'}:${local?.port ?? 'unknown'} remote=${remote?.candidateType ?? 'unknown'}/${remote?.protocol ?? 'unknown'}/${remote?.address ?? remote?.ip ?? 'unknown'}:${remote?.port ?? 'unknown'} bytesSent=${selectedPair.bytesSent ?? 0} bytesReceived=${selectedPair.bytesReceived ?? 0}`;
}

export async function ensureAudioDuplexSender(peer, stream) {
  const track = stream?.getAudioTracks?.()[0];
  if (!track) return false;
  const transceivers = peer.getTransceivers?.() ?? [];
  const transceiver = transceivers.find((candidate) => candidate?.receiver?.track?.kind === 'audio')
    ?? transceivers.find((candidate) => candidate?.sender?.track?.kind === 'audio');
  if (!transceiver?.sender) return false;
  transceiver.direction = 'sendrecv';
  if (typeof transceiver.sender.replaceTrack === 'function') {
    await transceiver.sender.replaceTrack(track);
  }
  return true;
}

function createBrowserToneStream({ frequency = 440, gain = 0.08 } = {}) {
  const AudioContextCtor = globalThis.AudioContext ?? globalThis.webkitAudioContext;
  if (!AudioContextCtor) {
    throw new Error('Web Audio API is unavailable; cannot create browser tone track');
  }
  const context = new AudioContextCtor();
  const oscillator = context.createOscillator();
  const gainNode = context.createGain();
  const destination = context.createMediaStreamDestination();
  oscillator.frequency.value = frequency;
  oscillator.type = 'sine';
  gainNode.gain.value = gain;
  oscillator.connect(gainNode).connect(destination);
  oscillator.start();
  const stream = destination.stream;
  stream.__stackchanToneContext = context;
  stream.__stackchanToneOscillator = oscillator;
  return stream;
}

function attachRemoteTrack(track, streams) {
  appendLog(`ontrack kind=${track.kind} id=${track.id} state=${track.readyState} muted=${track.muted}`);
  track.onunmute = () => appendLog(`track unmute kind=${track.kind}`);
  track.onmute = () => appendLog(`track mute kind=${track.kind}`);
  track.onended = () => appendLog(`track ended kind=${track.kind}`);

  const media = track.kind === 'video' ? document.querySelector('#remoteVideo') : document.querySelector('#remoteAudio');
  if (media) {
    media.srcObject = streams[0] ?? new MediaStream([track]);
    const logMediaState = (label) => appendLog(formatMediaElementState(track.kind, label, media));
    logMediaState('attached');
    media.onplay = () => logMediaState('play');
    media.onpause = () => logMediaState('pause');
    media.onvolumechange = () => logMediaState('volumechange');
    media.ontimeupdate = () => logMediaState('timeupdate');
    media.play?.()
      .then(() => logMediaState('play-resolved'))
      .catch((error) => {
        appendLog(`${track.kind} autoplay blocked ${error.message}`);
        logMediaState('play-rejected');
      });
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

async function configureMedia(peer, config) {
  if (config.media === 'audio') {
    peer.addTransceiver('audio', { direction: 'recvonly' });
    appendLog('media requested audio recvonly');
  } else if (config.media === 'audio-duplex' || config.media === 'audio-tone') {
    const useTone = config.media === 'audio-tone';
    appendLog(useTone ? 'media requested audio sendrecv with browser tone source' : 'media requested audio sendrecv');
    try {
      const stream = useTone ? createBrowserToneStream() : await navigator.mediaDevices.getUserMedia({ audio: true });
      const tracks = stream.getAudioTracks();
      appendLog(useTone ? `browser tone acquired tracks=${tracks.length} frequency=440` : `browser mic acquired tracks=${tracks.length}`);
      if (tracks[0]) {
        peer.__stackchanAudioDuplexStream = stream;
        if (config.role === 'answerer') {
          appendLog(useTone ? 'browser tone deferred until remote audio transceiver is available' : 'browser mic deferred until remote audio transceiver is available');
        } else {
          peer.addTransceiver(tracks[0], { streams: [stream], direction: 'sendrecv' });
        }
      } else {
        peer.addTransceiver('audio', { direction: 'recvonly' });
        appendLog(useTone ? 'browser tone acquired no audio track; falling back to recvonly' : 'browser mic acquired no audio track; falling back to recvonly');
      }
    } catch (error) {
      appendLog(`${useTone ? 'browser tone' : 'browser mic'} error ${error.name ?? 'Error'} ${error.message}`);
      throw error;
    }
  } else if (config.media === 'video') {
    peer.addTransceiver('video', { direction: 'recvonly' });
    appendLog('media requested video recvonly');
  } else {
    appendLog('media disabled');
  }
}

function startStatsLog(peer, config) {
  if (config.media === 'none') return;
  const statsKind = (config.media === 'audio-duplex' || config.media === 'audio-tone') ? 'audio' : config.media;
  setInterval(async () => {
    try {
      const stats = await peer.getStats();
      appendLog(summarizeSelectedCandidatePair(stats));
      for (const report of stats.values()) {
        if (report.type === 'inbound-rtp' && !report.isRemote && report.kind === statsKind) {
          appendLog(summarizeInboundRtpReport(report));
        } else if (report.type === 'outbound-rtp' && !report.isRemote && report.kind === statsKind) {
          appendLog(summarizeOutboundRtpReport(report));
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

      appendLog(`send ${summarizeIceCandidate(message.candidate)} ${JSON.stringify(summarizeCandidate(message.candidate))}`);
      const connectivityWarning = explainCandidateConnectivity(message.candidate);
      if (connectivityWarning) appendLog(connectivityWarning);

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

  await configureMedia(peer, config);
  startStatsLog(peer, config);
  return peer;
}

async function handleRemoteMessage(peer, payload, sendMessage) {
  const message = payload.message ?? payload;
  appendLog(`recv ${message.type ?? 'raw'} from ${payload.from ?? 'unknown'}`);

  if (message.type === 'offer') {
    appendLog(`remote offer media ${summarizeSdpMedia(message.sdp)}`);
    await peer.setRemoteDescription({ type: 'offer', sdp: message.sdp });
    if (peer.__stackchanAudioDuplexStream) {
      const attached = await ensureAudioDuplexSender(peer, peer.__stackchanAudioDuplexStream);
      appendLog(`browser mic attached to remote audio transceiver=${attached}`);
    }
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
  // the AppRTC-advertised URL. In split LAN/tunnel setups the ESP can use the
  // LAN address while the browser must use the signal origin it loaded.
  const browserWsBaseUrl = new URL('/ws', config.signalBaseUrl);
  browserWsBaseUrl.protocol = browserWsBaseUrl.protocol === 'https:' ? 'wss:' : 'ws:';
  appendLog(`websocket endpoint ${browserWsBaseUrl.toString()}`);
  ws = new WebSocket(buildWsUrl(browserWsBaseUrl.toString(), config.roomId, joined.client_id, { role: config.role }));

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
