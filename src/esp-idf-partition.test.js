import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { describe, it } from 'node:test';

const partitionCsvPath = new URL('../firmware/esp-idf-datachannel/partitions.csv', import.meta.url);
const sdkconfigDefaultsPath = new URL('../firmware/esp-idf-datachannel/sdkconfig.defaults', import.meta.url);

function parseSize(value) {
  const trimmed = value.trim().toLowerCase();
  if (trimmed.startsWith('0x')) {
    return Number.parseInt(trimmed, 16);
  }

  const match = trimmed.match(/^(\d+)([km])?$/);
  assert.ok(match, `unsupported partition size: ${value}`);

  const size = Number.parseInt(match[1], 10);
  if (match[2] === 'k') return size * 1024;
  if (match[2] === 'm') return size * 1024 * 1024;
  return size;
}

function parsePartitions(csv) {
  return csv
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line && !line.startsWith('#'))
    .map((line) => {
      const [name, type, subtype, offset, size, flags = ''] = line.split(',').map((field) => field.trim());
      return { name, type, subtype, offset, size: parseSize(size), flags };
    });
}

describe('ESP-IDF DataChannel partition table', () => {
  it('tracks a custom CoreS3 partition table with app headroom above the default 1 MiB factory slot', async () => {
    const [csv, sdkconfigDefaults] = await Promise.all([
      readFile(partitionCsvPath, 'utf8'),
      readFile(sdkconfigDefaultsPath, 'utf8'),
    ]);

    assert.match(sdkconfigDefaults, /^CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y$/m);
    assert.match(sdkconfigDefaults, /^CONFIG_PARTITION_TABLE_CUSTOM=y$/m);
    assert.match(sdkconfigDefaults, /^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions\.csv"$/m);

    const partitions = parsePartitions(csv);
    assert.deepEqual(
      partitions.map(({ name, type, subtype }) => ({ name, type, subtype })),
      [
        { name: 'nvs', type: 'data', subtype: 'nvs' },
        { name: 'phy_init', type: 'data', subtype: 'phy' },
        { name: 'factory', type: 'app', subtype: 'factory' },
      ],
    );

    const factory = partitions.find((partition) => partition.name === 'factory');
    assert.equal(factory.offset, '0x10000');
    assert.ok(factory.size >= 0x400000, `factory app partition is too small: 0x${factory.size.toString(16)}`);
  });
});
