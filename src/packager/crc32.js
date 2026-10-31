// CRC-32 as used by zip files and zlib.
// This is the same algorithm as mz_crc32() in miniz, which the single file executable stub uses.

let table = null;

const getTable = () => {
  if (!table) {
    table = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) {
        c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
      }
      table[n] = c;
    }
  }
  return table;
};

/**
 * @param {Uint8Array} data
 * @param {number} [previous] result of a previous crc32 call for incremental hashing
 * @returns {number} unsigned 32 bit checksum
 */
const crc32 = (data, previous = 0) => {
  const lookup = getTable();
  let crc = ~previous >>> 0;
  for (let i = 0; i < data.length; i++) {
    crc = (lookup[(crc ^ data[i]) & 0xFF] ^ (crc >>> 8)) >>> 0;
  }
  return (~crc) >>> 0;
};

export default crc32;
