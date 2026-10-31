// Utilities for the "single file executable" packaging mode.
//
// The generated file is: <stub exe> + <zip archive> + <32 byte trailer>.
// See src/sfx/stub.c for the program that reads this format. Changes to one file must be
// mirrored in the other.
//
// Trailer layout (all integers little endian):
//
//   offset  size  field
//   0       4     magic, "TSFX"
//   4       4     format version
//   8       4     reserved
//   12      8     offset of the zip archive from the start of the file
//   20      8     size of the zip archive in bytes
//   28      4     CRC-32 (the one used by zlib) of the zip archive

import crc32 from './crc32';

// Maps packaging targets to the asset that contains the stub executable for that architecture.
export const SFX_STUB_ASSETS = {
  'electron-win32': 'sfx-stub-x86',
  'electron-win64': 'sfx-stub-x64',
  'electron-win-arm': 'sfx-stub-arm64',
  'nwjs-win32': 'sfx-stub-x86',
  'nwjs-win64': 'sfx-stub-x64'
};

export const SFX_VERSION = 1;
export const SFX_TRAILER_SIZE = 32;
export const SFX_META_NAME = '__sfx_meta.txt';

/**
 * @param {string} packageName
 * @returns {string} contents of __sfx_meta.txt, which the stub uses to find the application
 */
export const generateSfxMeta = (packageName) => [
  `packageName=${packageName}`,
  `mainExe=${packageName}/${packageName}.exe`,
  ''
].join('\n');

/**
 * @param {Uint8Array} stub compiled stub executable
 * @param {Uint8Array} zipData zip archive containing the application
 * @returns {Uint8Array} the single file executable
 */
export const generateSfxExecutable = (stub, zipData) => {
  const zipOffset = stub.length;
  const zipSize = zipData.length;
  const output = new Uint8Array(zipOffset + zipSize + SFX_TRAILER_SIZE);
  output.set(stub, 0);
  output.set(zipData, zipOffset);
  const view = new DataView(output.buffer, output.byteOffset + zipOffset + zipSize, SFX_TRAILER_SIZE);
  view.setUint32(0, 0x58465354, true); // "TSFX" as little endian bytes
  view.setUint32(4, SFX_VERSION, true);
  view.setUint32(8, 0, true); // reserved
  view.setUint32(12, zipOffset >>> 0, true);
  view.setUint32(16, Math.floor(zipOffset / 0x100000000), true);
  view.setUint32(20, zipSize >>> 0, true);
  view.setUint32(24, Math.floor(zipSize / 0x100000000), true);
  view.setUint32(28, crc32(zipData), true);
  return output;
};
