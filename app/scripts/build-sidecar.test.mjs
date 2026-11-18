import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { findBuiltSidecar, sidecarBinaryName } from './build-sidecar.mjs';

test('sidecarBinaryName appends .exe only on Windows target triples', () => {
  assert.equal(sidecarBinaryName('x86_64-pc-windows-msvc'), 'malgrind-cli-x86_64-pc-windows-msvc.exe');
  assert.equal(sidecarBinaryName('aarch64-pc-windows-msvc'), 'malgrind-cli-aarch64-pc-windows-msvc.exe');
  assert.equal(sidecarBinaryName('x86_64-unknown-linux-gnu'), 'malgrind-cli-x86_64-unknown-linux-gnu');
  assert.equal(sidecarBinaryName('aarch64-apple-darwin'), 'malgrind-cli-aarch64-apple-darwin');
});

test('findBuiltSidecar resolves multi-config and single-config paths in priority order', () => {
  const repo = join('/tmp', 'repo');
  const host = join(repo, 'build-app', 'host');
  const winRelease = join(host, 'Release', 'malgrind-cli.exe');
  const winSingle = join(host, 'malgrind-cli.exe');
  const unixRelease = join(host, 'Release', 'malgrind-cli');
  const unixSingle = join(host, 'malgrind-cli');

  assert.equal(findBuiltSidecar(repo, (p) => p === winRelease || p === unixSingle), winRelease);
  assert.equal(findBuiltSidecar(repo, (p) => p === winSingle), winSingle);
  assert.equal(findBuiltSidecar(repo, (p) => p === unixRelease), unixRelease);
  assert.equal(findBuiltSidecar(repo, (p) => p === unixSingle), unixSingle);
  assert.throws(() => findBuiltSidecar(repo, () => false), /Built sidecar binary not found/);
});
