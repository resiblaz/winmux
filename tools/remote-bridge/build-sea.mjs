#!/usr/bin/env node
/**
 * Builds a zero-dependency standalone `winmux-bridge.exe` using Node.js 25+ native `--build-sea`.
 *
 * Output:
 *   tools/remote-bridge/dist/winmux-bridge.exe
 *
 * Usage:
 *   node tools/remote-bridge/build-sea.mjs [--out <path>]
 */

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const rootDir = path.dirname(fileURLToPath(import.meta.url));
const srcDir = path.join(rootDir, 'src');
const hooksDir = path.join(rootDir, 'hooks');
const distDir = path.join(rootDir, 'dist');

let outputExe = path.join(distDir, 'winmux-bridge.exe');
for (let i = 2; i < process.argv.length; i++) {
  if (process.argv[i] === '--out' && process.argv[i + 1]) {
    outputExe = path.resolve(process.argv[++i]);
  }
}

const MODULE_ORDER = [
  'qr-encoder.mjs',
  'tls-cert.mjs',
  'auth-store.mjs',
  'decision-store.mjs',
  'agent-inspector.mjs',
  'system-monitor.mjs',
  'hook-installer.mjs',
  'tunnel-manager.mjs',
  'web-ui.mjs',
  'websocket.mjs',
  'winmux-client.mjs',
  'server.mjs',
];

function transformEsmToCjs(source) {
  const exportedNames = [];
  const lines = source.replace(/\r\n/g, '\n').split('\n');
  const out = [];

  for (const line of lines) {
    // Strip shebang
    if (line.startsWith('#!')) continue;

    // import { a, b } from 'node:...' or './...'
    const namedImport = line.match(/^import\s+\{([^}]+)\}\s+from\s+['"]([^'"]+)['"];?\s*$/);
    if (namedImport) {
      const spec = namedImport[2];
      const loader = spec.startsWith('./') ? `__req(${JSON.stringify(spec)})` : `require(${JSON.stringify(spec)})`;
      out.push(`const {${namedImport[1]}} = ${loader};`);
      continue;
    }

    // import foo from 'node:...' or './...'
    const defaultImport = line.match(/^import\s+([A-Za-z0-9_$]+)\s+from\s+['"]([^'"]+)['"];?\s*$/);
    if (defaultImport) {
      const spec = defaultImport[2];
      const loader = spec.startsWith('./') ? `__req(${JSON.stringify(spec)})` : `require(${JSON.stringify(spec)})`;
      out.push(`const ${defaultImport[1]} = ${loader};`);
      continue;
    }

    // export function foo(
    const expFn = line.match(/^export\s+(async\s+)?function\s+([A-Za-z0-9_$]+)\b(.*)$/);
    if (expFn) {
      exportedNames.push(expFn[2]);
      out.push(`${expFn[1] || ''}function ${expFn[2]}${expFn[3]}`);
      continue;
    }

    // export class Foo
    const expClass = line.match(/^export\s+class\s+([A-Za-z0-9_$]+)\b(.*)$/);
    if (expClass) {
      exportedNames.push(expClass[1]);
      out.push(`class ${expClass[1]}${expClass[2]}`);
      continue;
    }

    // export const FOO = ...
    const expConst = line.match(/^export\s+const\s+([A-Za-z0-9_$]+)\b(.*)$/);
    if (expConst) {
      exportedNames.push(expConst[1]);
      out.push(`const ${expConst[1]}${expConst[2]}`);
      continue;
    }

    out.push(line.replaceAll('import.meta.url', JSON.stringify('file:///winmux-bridge.exe')));
  }

  for (const name of exportedNames) {
    out.push(`exports.${name} = ${name};`);
  }
  return out.join('\n');
}

function buildHookBody(source) {
  const transformed = transformEsmToCjs(source)
    .replaceAll('process.argv.slice(2)', 'hookArgv');
  return `async function __runHook(hookArgv) {\n${transformed}\n}`;
}

fs.mkdirSync(path.dirname(outputExe), { recursive: true });
fs.mkdirSync(distDir, { recursive: true });

const chunks = [
  `'use strict';`,
  `globalThis.__WINMUX_SEA__ = true;`,
  `const __factories = Object.create(null);`,
  `const __cache = Object.create(null);`,
  `function __req(id) {`,
  `  if (__cache[id]) return __cache[id].exports;`,
  `  const mod = { exports: {} };`,
  `  __cache[id] = mod;`,
  `  __factories[id](mod.exports, __req);`,
  `  return mod.exports;`,
  `}`,
];

for (const fileName of MODULE_ORDER) {
  const fullPath = path.join(srcDir, fileName);
  const src = fs.readFileSync(fullPath, 'utf8');
  const body = transformEsmToCjs(src);
  chunks.push(`__factories[${JSON.stringify('./' + fileName)}] = function(exports, __req) {\n${body}\n};`);
}

const hookSrc = fs.readFileSync(path.join(hooksDir, 'winmux-hook.mjs'), 'utf8');
chunks.push(buildHookBody(hookSrc));

chunks.push(`
async function __main() {
  if (process.argv[2] === '--version') {
    console.log('winmux-bridge 1.0.0 (SEA)');
    process.exit(0);
  }
  if (process.argv[2] === 'hook') {
    await __runHook(process.argv.slice(3));
    return;
  }

  const { WinmuxBridgeServer } = __req('./server.mjs');
  const port = Number(process.env.WINMUX_BRIDGE_PORT || 47710);
  const host = process.env.WINMUX_BRIDGE_HOST || '::';
  const tls = process.env.WINMUX_BRIDGE_TLS !== '0';
  const server = new WinmuxBridgeServer({ host, port, tls });
  const pairing = await server.start();

  console.log('============================================================');
  console.log(' Winmux Remote Bridge (Standalone SEA, ' + (tls ? 'HTTPS/WSS TLS' : 'HTTP/WS') + ')');
  console.log('------------------------------------------------------------');
  console.log(' Hostname      : ' + pairing.hostname);
  console.log(' LAN URL       : ' + pairing.lanUrl);
  if (pairing.remoteUrl) console.log(' Remote URL    : ' + pairing.remoteUrl);
  if (pairing.certFingerprint) console.log(' TLS SHA-256   : ' + pairing.certFingerprint);
  console.log(' Pairing Code  : ' + pairing.code);
  console.log(' Pair URI      : ' + pairing.pairUri);
  console.log(' Web Console   : ' + pairing.lanUrl + '/');
  console.log('============================================================');

  const shutdown = async () => {
    await server.stop();
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

__main().catch((err) => {
  console.error(err);
  process.exit(1);
});
`);

const bundlePath = path.join(distDir, 'winmux-bridge.bundle.cjs');
const seaConfigPath = path.join(distDir, 'sea-config.json');
fs.writeFileSync(bundlePath, chunks.join('\n\n'), 'utf8');

const seaConfig = {
  main: bundlePath,
  output: outputExe,
  disableExperimentalSEAWarning: true,
  useCodeCache: false,
};
fs.writeFileSync(seaConfigPath, JSON.stringify(seaConfig, null, 2), 'utf8');

console.log(`Building standalone SEA binary: ${outputExe}`);
const result = spawnSync(process.execPath, ['--build-sea', seaConfigPath], {
  stdio: 'inherit',
  cwd: rootDir,
});

try { fs.rmSync(bundlePath, { force: true }); } catch {}
try { fs.rmSync(seaConfigPath, { force: true }); } catch {}

if (result.status !== 0) {
  process.exit(result.status || 1);
}

const stat = fs.statSync(outputExe);
console.log(`Built standalone binary: ${outputExe} (${(stat.size / (1024 * 1024)).toFixed(1)} MB)`);
