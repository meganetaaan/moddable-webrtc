import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { describe, it } from 'node:test';

const mainCPath = new URL('../firmware/esp-idf-datachannel/main/main.c', import.meta.url);
const kconfigPath = new URL('../firmware/esp-idf-datachannel/main/Kconfig.projbuild', import.meta.url);

describe('ESP-IDF CoreS3 speaker sink', () => {
  it('exposes a browser-to-CoreS3 recvonly media mode', async () => {
    const kconfig = await readFile(kconfigPath, 'utf8');
    const source = await readFile(mainCPath, 'utf8');

    assert.match(kconfig, /config STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK/);
    assert.match(kconfig, /Audio: CoreS3 speaker sink/);
    assert.match(source, /CONFIG_STACKCHAN_MEDIA_CORE_S3_SPEAKER_SINK[\s\S]*ESP_PEER_MEDIA_DIR_RECV_ONLY/);
    assert.match(source, /return "core-s3-speaker-sink";/);
  });

  it('decodes received PCMA frames and writes PCM to the CoreS3 speaker I2S path', async () => {
    const source = await readFile(mainCPath, 'utf8');

    assert.match(source, /#include "driver\/i2s_std\.h"/);
    assert.match(source, /static esp_err_t core_s3_speaker_i2s_start\(void\)/);
    assert.match(source, /i2s_channel_write\(app\.speaker_tx/);
    assert.match(source, /audio_rx_speaker_write_frames/);
    assert.match(source, /audio rx speaker writes=%u samples=%u bytes=%u drops=%u/);
  });

  it('uses a bounded blocking I2S write timeout to avoid speaker underrun pops', async () => {
    const source = await readFile(mainCPath, 'utf8');

    assert.match(source, /#define CORE_S3_SPEAKER_WRITE_TIMEOUT_MS\s+20/);
    assert.match(source, /pdMS_TO_TICKS\(CORE_S3_SPEAKER_WRITE_TIMEOUT_MS\)/);
    assert.doesNotMatch(source, /i2s_channel_write\(app\.speaker_tx,\s*samples,\s*write_bytes,\s*&bytes_written,\s*0\)/);
  });

  it('matches Moddable CoreS3 speaker I2S clock and slot framing', async () => {
    const source = await readFile(mainCPath, 'utf8');
    const sample = await readFile(new URL('../firmware/esp-idf-speaker-tone/main/main.c', import.meta.url), 'utf8');

    for (const candidate of [source, sample]) {
      assert.match(candidate, /#define CORE_S3_SPEAKER_MCLK_GPIO\s+0|#define SPEAKER_MCLK_GPIO\s+0/);
      assert.match(candidate, /\.gpio_cfg\.mclk\s*=\s*(?:CORE_S3_SPEAKER_MCLK_GPIO|SPEAKER_MCLK_GPIO)/);
      assert.match(candidate, /\.slot_cfg\.bit_shift\s*=\s*false/);
      assert.doesNotMatch(candidate, /\.gpio_cfg\.mclk\s*=\s*I2S_GPIO_UNUSED/);
      assert.doesNotMatch(candidate, /\.slot_cfg\.bit_shift\s*=\s*true/);
    }
  });

  it('keeps the standalone speaker tone sample smooth and below clipping-prone levels', async () => {
    const sample = await readFile(new URL('../firmware/esp-idf-speaker-tone/main/main.c', import.meta.url), 'utf8');

    assert.match(sample, /#define SPEAKER_TONE_AMPLITUDE\s+3000/);
    assert.match(sample, /static const int16_t sine_table_64\[64\]/);
    assert.match(sample, /phase_step_q16/);
    assert.match(sample, /pcm\[i\] = sine_table_64\[\(phase_q16 >> 16\) & 63\]/);
    assert.doesNotMatch(sample, /\?\s*12000\s*:\s*-12000/);
    assert.doesNotMatch(sample, /\b12000\b/);
  });

  it('attenuates decoded browser audio before writing to the CoreS3 speaker', async () => {
    const source = await readFile(mainCPath, 'utf8');

    assert.match(source, /#define CORE_S3_SPEAKER_OUTPUT_GAIN_Q15\s+8192/);
    assert.match(source, /static int16_t scale_speaker_pcm_sample\(int16_t sample\)/);
    assert.match(source, /speaker_pcm\[i\]\s*=\s*scale_speaker_pcm_sample\(sample\)/);
  });

  it('enables the CoreS3 speaker amplifier before writing I2S samples', async () => {
    const source = await readFile(mainCPath, 'utf8');

    assert.match(source, /CORE_S3_AW9523_I2C_ADDR 0x58/);
    assert.match(source, /CORE_S3_AW88298_I2C_ADDR 0x36/);
    assert.match(source, /CORE_S3_AXP2101_I2C_ADDR 0x34/);
    assert.match(source, /core_s3_i2c_write_reg8\(power,\s*0x90,\s*0xbf\)/);
    assert.match(source, /core_s3_speaker_enable_amp\(true\)/);
    assert.match(source, /core_s3_aw88298_write_reg\([^,]+,\s*0x04,\s*0x4040\)/);
    assert.match(source, /core_s3_aw88298_write_reg\([^,]+,\s*0x0c,\s*0x0664\)/i);
    assert.match(source, /core-s3 speaker aw88298 register writes did not ACK/);
    assert.match(source, /core-s3 speaker amp enable=%d/);
  });
});
