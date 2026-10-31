# Single file executable stub

This directory contains the source code of the small native Windows program (the "stub") that
makes the "Single file (one .exe)" packaging mode possible. `src/packager/packager.js` appends a
zip archive to a compiled stub to create the final executable. The stub is what actually runs
when the user double clicks the generated `.exe`.

## How the generated file is laid out

```
+--------------------------------------+
| stub (this program, stub-*.exe)      |
+--------------------------------------+
| zip archive (the entire application) |
+--------------------------------------+
| 32 byte trailer (see below)          |
+--------------------------------------+
```

Trailer layout (all integers little endian):

| offset | size | field                                          |
|--------|------|------------------------------------------------|
| 0      | 4    | magic, `TSFX`                                  |
| 4      | 4    | format version (1)                             |
| 8      | 4    | reserved (0)                                   |
| 12     | 8    | offset of the zip archive from the start       |
| 20     | 8    | size of the zip archive in bytes               |
| 28     | 4    | CRC-32 (the one used by zlib) of the zip archive |

`generateSfxExecutable()` in `src/packager/packager.js` writes this structure and
`WinMain()` in `stub.c` reads it. **The two implementations must be kept in sync.**

## What the stub does when it runs

1. Opens its own executable and verifies the trailer (magic, version, bounds, and the CRC-32
   the first time a build is used).
2. Reads `__sfx_meta.txt` from the root of the embedded zip archive. It contains the package
   name and the path of the application executable inside the archive.
3. Extracts the archive into
   `%LOCALAPPDATA%\TurboWarpPackager-SFX\<packageName>-<crc32 as hex>\` if it has not already
   been extracted. A named mutex makes sure that multiple instances of the same application
   that are started at the same time don't corrupt each other's files.
4. Creates a `.sfx-ok` marker file to remember that the extraction is complete, then launches
   the real application executable with the cache directory as the working directory. Later
   launches skip extraction and start immediately.
5. Exits. The launched application keeps running.

## Rebuilding the stubs

The compiled stubs are checked into the repository as `static/sfx/stub-{x64,x86,arm64}.exe` so
that neither `npm ci` nor `npm run build-prod` require a C compiler. Only run this when
`stub.c` changes. It requires an internet connection the first time to download zig.

```
powershell -ExecutionPolicy Bypass -File src/sfx/build.ps1
```

This compiles the stubs with [zig](https://ziglang.org/), copies them to `static/sfx/`, and
prints their SHA-256 checksums. Update the checksums in `src/packager/large-assets.js` after
rebuilding, otherwise the packager will refuse to use the new files.

## Files

- `stub.c`: the stub itself.
- `miniz.c`, `miniz.h`: [miniz](https://github.com/richgel999/miniz) 3.0.2, used to read the
  embedded zip archive. Licensed under MIT, see `miniz-LICENSE`.
- `build.ps1`: builds the stubs with zig.

## Limitations

- Windows only. macOS and Linux have different executable formats and conventions.
- The first launch extracts the entire application, which takes a few seconds.
- Applications are extracted to `%LOCALAPPDATA%`, so they use disk space until that folder is
  deleted manually.
- Unsigned self-extracting executables are sometimes falsely flagged by antivirus software.
- The outer executable is the stub, so the icon that the user selects in the packager is only
  used by the application itself, not by the file that is shown in Explorer.
