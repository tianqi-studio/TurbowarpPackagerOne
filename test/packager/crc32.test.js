import crc32 from '../../src/packager/crc32';

const encode = (string) => new TextEncoder().encode(string);

test('crc32 of known values', () => {
  expect(crc32(encode(''))).toBe(0);
  expect(crc32(encode('123456789'))).toBe(0xCBF43926);
  expect(crc32(encode('The quick brown fox jumps over the lazy dog'))).toBe(0x414FA339);
});

test('crc32 is incremental', () => {
  const data = new Uint8Array(10000);
  for (let i = 0; i < data.length; i++) {
    data[i] = Math.random() * 256;
  }
  const whole = crc32(data);
  let incremental = 0;
  for (let i = 0; i < data.length; i += 1000) {
    incremental = crc32(data.subarray(i, i + 1000), incremental);
  }
  expect(incremental).toBe(whole);
});
