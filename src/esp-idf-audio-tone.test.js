import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { describe, it } from 'node:test';

const mainCPath = new URL('../firmware/esp-idf-datachannel/main/main.c', import.meta.url);

function extractFunction(source, name) {
  const start = source.indexOf(`static void ${name}(`);
  assert.notEqual(start, -1, `${name} function should exist`);
  const nextFunction = source.indexOf('\nstatic ', start + 1);
  return source.slice(start, nextFunction === -1 ? undefined : nextFunction);
}

describe('ESP-IDF generated audio test source', () => {
  it('generates an audible 440 Hz PCMA tone instead of a near-silent constant frame', async () => {
    const source = await readFile(mainCPath, 'utf8');
    const audioTask = extractFunction(source, 'audio_test_task');

    assert.match(source, /#define\s+AUDIO_TEST_TONE_HZ\s+440/);
    assert.match(source, /static\s+uint8_t\s+linear16_to_alaw\(int16_t\s+sample\)/);
    assert.match(source, /static\s+void\s+generate_pcma_tone_frame\(uint8_t \*frame, size_t frame_size\)/);
    assert.match(audioTask, /generate_pcma_tone_frame\(frame, sizeof\(frame\)\)/);
    assert.doesNotMatch(audioTask, /memset\(frame,\s*0xd5,\s*sizeof\(frame\)\)/);
  });
});
