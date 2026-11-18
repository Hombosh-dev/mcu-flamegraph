#!/usr/bin/env node
// Portability guardrails: flags OS-specific code outside `host/src/os/**` and
// chip/target-specific code outside `host/src/families/**`.
//
//   node scripts/check-portability.mjs
//
// Uses only Node 18 APIs.
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { dirname, join, relative, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export const OS_PATTERNS = [
  /#\s*include\s*<(unistd|termios|fcntl|spawn|poll|pwd|grp|dlfcn|windows|winsock2|ws2tcpip|setupapi|cfgmgr32|shlobj|io)\.h>/,
  /#\s*include\s*<(sys|IOKit|CoreFoundation|mach-o|arpa|netinet)\//,
  /\b(_WIN32|__APPLE__|__linux__)\b/,
  /"\/(dev|sys|proc)(?:\/|")/,
  /\/bin\/sh/,
  /\.local\/share/,
  /Library\/Application Support/,
  /\b(std::)?(getenv|setenv|unsetenv)\s*\(/,
  /\berrno\b/,
];

export const CHIP_ONLY_PATTERNS = [
  /\besp(32|tool|ressif|idf)\w*/i,
  /xtensa-/i,
  /sdkconfig/i,
];

export const CHIP_OS_PORT_PATTERNS = [
  /ttyUSB|ttyACM|\/dev\//i,
  /\bCOM\d/,
  /pkexec/i,
  /\bsudo\b/i,
  /apt install/i,
];

export const CHIP_PATTERNS = [...CHIP_ONLY_PATTERNS, ...CHIP_OS_PORT_PATTERNS];

function toPosix(p) {
  return sep === '/' ? p : p.split(sep).join('/');
}

// Recursive directory walker compatible with Node 18 (no { recursive: true }).
export function walkFiles(dir, root = dir) {
  if (!existsSync(dir)) return [];
  const out = [];
  const entries = readdirSync(dir, { withFileTypes: true });
  entries.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  for (const entry of entries) {
    const fullPath = join(dir, entry.name);
    if (entry.isDirectory()) {
      out.push(...walkFiles(fullPath, root));
    } else if (entry.isFile()) {
      out.push(toPosix(relative(root, fullPath)));
    }
  }
  return out;
}

function isCppFile(relPath) {
  return relPath.endsWith('.cpp') || relPath.endsWith('.hpp');
}

function isAppSourceFile(relPath) {
  if (relPath.endsWith('.test.ts') || relPath.endsWith('.d.ts')) return false;
  return relPath.endsWith('.ts') || relPath.endsWith('.svelte');
}

export function getFilesForRule(root, rule) {
  if (rule === 'os') {
    const hostFiles = walkFiles(join(root, 'host', 'src'), root).filter(
      (f) => isCppFile(f) && !f.startsWith('host/src/os/'),
    );
    const coreFiles = ['include', 'src', 'tests'].flatMap((sub) =>
      walkFiles(join(root, 'core', sub), root).filter(isCppFile),
    );
    return [...hostFiles, ...coreFiles].sort();
  }
  if (rule === 'chip') {
    const hostFiles = walkFiles(join(root, 'host', 'src'), root).filter(
      (f) => isCppFile(f) && !f.startsWith('host/src/families/'),
    );
    const appFiles = walkFiles(join(root, 'app', 'src'), root).filter(isAppSourceFile);
    return [...hostFiles, ...appFiles].sort();
  }
  throw new Error(`Unknown portability rule: ${rule}`);
}

export function scanLines(text, rule, { osLayer = false } = {}) {
  const patterns = rule === 'os'
    ? OS_PATTERNS
    : rule === 'chip'
      ? (osLayer ? CHIP_ONLY_PATTERNS : CHIP_PATTERNS)
      : null;
  if (!patterns) throw new Error(`Unknown portability rule: ${rule}`);
  const lines = text.split(/\r?\n/);
  const hits = [];
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    if (patterns.some((re) => re.test(line))) {
      hits.push({ line: i + 1, rule, text: line.trim() });
    }
  }
  return hits;
}

export function loadAllowList(allowPath) {
  if (!existsSync(allowPath)) return { os: [], chip: [] };
  const parsed = JSON.parse(readFileSync(allowPath, 'utf8'));
  return {
    os: Array.isArray(parsed.os) ? parsed.os : [],
    chip: Array.isArray(parsed.chip) ? parsed.chip : [],
  };
}

export function checkPortability(root, allow = loadAllowList(join(root, 'scripts', 'portability-allow.json'))) {
  const violations = [];
  for (const rule of ['os', 'chip']) {
    const allowed = new Set(allow?.[rule] ?? []);
    for (const file of getFilesForRule(root, rule)) {
      if (allowed.has(file)) continue;
      const content = readFileSync(join(root, file), 'utf8');
      const osLayer = rule === 'chip' && file.startsWith('host/src/os/');
      for (const hit of scanLines(content, rule, { osLayer })) {
        violations.push({ file, line: hit.line, rule, text: hit.text });
      }
    }
  }
  return violations;
}

export function formatViolation(v) {
  return `${v.file}:${v.line}: [${v.rule}] ${v.text}`;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const root = join(dirname(fileURLToPath(import.meta.url)), '..');
  const violations = checkPortability(root);
  for (const v of violations) {
    console.error(formatViolation(v));
  }
  if (violations.length > 0) {
    process.exit(1);
  }
}
