import AudioOut from "pins/audioout";
import Timer from "timer";

const sampleRate = 24_000;
const tones = [440, 660, 880, 1320];

trace(`[moddable-audioout-tone] start sampleRate=${sampleRate}\n`);

const audio = new AudioOut({
  streams: 1,
  sampleRate,
  numChannels: 1,
});

trace(`[moddable-audioout-tone] constructed actualSampleRate=${audio.sampleRate}\n`);
audio.start();
trace(`[moddable-audioout-tone] audio.start done\n`);

let index = 0;
Timer.repeat(() => {
  const hz = tones[index++ % tones.length];
  trace(`[moddable-audioout-tone] enqueue tone hz=${hz}\n`);
  audio.enqueue(0, AudioOut.Flush);
  audio.enqueue(0, AudioOut.Volume, 256);
  audio.enqueue(0, AudioOut.Tone, hz, sampleRate);
}, 1500);
