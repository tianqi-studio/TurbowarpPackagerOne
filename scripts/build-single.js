'use strict';

// Builds a single file executable (one .exe) without using the website.
// This is a thin wrapper around the Node.js API; see node-api-docs/README.md for details.
//
// Usage:
//   npm run build-electron-single
//   npm run build-nwjs-single
//   node scripts/build-single.js --target nwjs-win64 --input static/example.sb3 --output output
//
// Targets that support single file output: electron-win32, electron-win64, electron-win-arm,
// nwjs-win32, nwjs-win64

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');

const getArgument = (name, fallback) => {
  const index = process.argv.indexOf(name);
  if (index !== -1 && process.argv[index + 1]) {
    return process.argv[index + 1];
  }
  return fallback;
};

const target = getArgument('--target', 'electron-win64');
const input = path.resolve(root, getArgument('--input', 'static/example.sb3'));
const outputDirectory = path.resolve(root, getArgument('--output', 'output'));

const Packager = require('../dist/packager');

const run = async () => {
  if (!fs.existsSync(input)) {
    throw new Error(`Input file does not exist: ${input}`);
  }

  const fileName = path.basename(input);
  const projectData = fs.readFileSync(input);
  const loadedProject = await Packager.loadProject(projectData);

  const packager = new Packager.Packager();
  packager.project = loadedProject;
  packager.options.target = target;
  packager.options.app.singleFile = true;
  packager.options.app.packageName = Packager.Packager.getDefaultPackageNameFromFileName(fileName);
  packager.options.app.windowTitle = Packager.Packager.getWindowTitleFromFileName(fileName);

  if (process.stdout.isTTY) {
    packager.addEventListener('large-asset-fetch', ({detail}) => {
      process.stdout.write(`\rLoading ${detail.asset} (${Math.round(detail.progress * 100)}%)        `);
    });
    packager.addEventListener('zip-progress', ({detail}) => {
      process.stdout.write(`\rCompressing project (${Math.round(detail.progress * 100)}%)        `);
    });
  }

  const result = await packager.package();
  process.stdout.write('\r');

  fs.mkdirSync(outputDirectory, {recursive: true});
  const outputPath = path.join(outputDirectory, result.filename);
  fs.writeFileSync(outputPath, Buffer.from(result.data));
  console.log(`Target: ${target}`);
  console.log(`Wrote ${outputPath} (${(result.data.length / 1024 / 1024).toFixed(1)} MB)`);
};

run().catch((error) => {
  console.error(error);
  process.exit(1);
});
