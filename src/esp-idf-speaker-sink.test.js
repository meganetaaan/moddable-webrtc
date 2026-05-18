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
