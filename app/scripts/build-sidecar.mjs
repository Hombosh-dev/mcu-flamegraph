// Builds the C++ sidecar and puts it where Tauri expects an external binary: binaries/malgrind-cli-<target triple>[.exe].
import { execFileSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export function sidecarBinaryName(triple) {
  const ext = triple.includes('windows') ? '.exe' : '';
  return `malgrind-cli-${triple}${ext}`;
}

export function findBuiltSidecar(repo, exists = existsSync) {
  const hostDir = join(repo, 'build-app', 'host');
  const candidates = [
    join(hostDir, 'Release', 'malgrind-cli.exe'),
    join(hostDir, 'malgrind-cli.exe'),
    join(hostDir, 'Release', 'malgrind-cli'),
    join(hostDir, 'malgrind-cli'),
  ];
  for (const candidate of candidates) {
    if (exists(candidate)) return candidate;
  }
  throw new Error(`Built sidecar binary not found; searched: ${candidates.join(', ')}`);
}

export function main() {
  const app = join(dirname(fileURLToPath(import.meta.url)), '..');
  const repo = join(app, '..');
  const run = (cmd, args, cwd = repo) => execFileSync(cmd, args, { cwd, stdio: 'inherit' });

  // A separate build tree for the package: no repository path baked into the binary as the agent directory
  // (it finds the app's resources next to itself at run time).
  run('cmake', ['-B', 'build-app', '-DCMAKE_BUILD_TYPE=Release', '-DMALGRIND_DEFAULT_AGENT_DIR=']);
  run('cmake', ['--build', 'build-app', '--target', 'malgrind-cli', '--config', 'Release', '-j']);

  const triple = execFileSync('rustc', ['--print', 'host-tuple']).toString().trim();
  const src = findBuiltSidecar(repo);
  const name = sidecarBinaryName(triple);
  const dest = join(app, 'src-tauri', 'binaries');
  mkdirSync(dest, { recursive: true });
  copyFileSync(src, join(dest, name));
  console.log(`sidecar: binaries/${name}`);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main();
}
