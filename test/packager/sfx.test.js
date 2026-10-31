import {generateSfxExecutable, generateSfxMeta, SFX_TRAILER_SIZE} from '../../src/packager/sfx';
import crc32 from '../../src/packager/crc32';

test('single file executable trailer matches what src/sfx/stub.c expects', () => {
  const stub = new Uint8Array([0x4D, 0x5A, 0x90, 0x00]); // "MZ" header of a fake exe
  const zipData = new Uint8Array(1000);
  for (let i = 0; i < zipData.length; i++) {
    zipData[i] = Math.random() * 256;
  }

  const output = generateSfxExecutable(stub, zipData);

  expect(output.length).toBe(stub.length + zipData.length + SFX_TRAILER_SIZE);
  expect(output.subarray(0, stub.length)).toEqual(stub);
  expect(output.subarray(stub.length, stub.length + zipData.length)).toEqual(zipData);

  const trailer = output.subarray(stub.length + zipData.length);
  expect(Array.from(trailer.subarray(0, 4))).toEqual([0x54, 0x53, 0x46, 0x58]); // "TSFX"
  const view = new DataView(trailer.buffer, trailer.byteOffset, trailer.byteLength);
  expect(view.getUint32(4, true)).toBe(1); // version
  expect(view.getUint32(8, true)).toBe(0); // reserved
  expect(view.getUint32(12, true)).toBe(stub.length); // zip offset (low)
  expect(view.getUint32(16, true)).toBe(0); // zip offset (high)
  expect(view.getUint32(20, true)).toBe(zipData.length); // zip size (low)
  expect(view.getUint32(24, true)).toBe(0); // zip size (high)
  expect(view.getUint32(28, true)).toBe(crc32(zipData)); // zip crc32
});

test('single file executable metadata', () => {
  expect(generateSfxMeta('my-game')).toBe('packageName=my-game\nmainExe=my-game/my-game.exe\n');
});
