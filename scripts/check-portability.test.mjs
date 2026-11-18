// node --test scripts/check-portability.test.mjs
import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { checkPortability, formatViolation, scanLines } from './check-portability.mjs';

function withTempTree(files, fn) {
  const root = mkdtempSync(join(tmpdir(), 'malgrind-portability-'));
  try {
    for (const [relPath, content] of Object.entries(files)) {
      const fullPath = join(root, relPath);
      mkdirSync(join(fullPath, '..'), { recursive: true });
      writeFileSync(fullPath, content);
    }
    return fn(root);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

test('os rules match all forbidden patterns including comments', () => {
  const samples = [
    '#include <unistd.h>',
    '#include <windows.h>',
    '#include <sys/socket.h>',
    '# include <IOKit/IOKitLib.h>',
    '#ifdef __APPLE__',
    '#if defined(_WIN32) || defined(__linux__)',
    'const char* p = "/dev/tty";',
    'const char* s = "/sys";',
    'const char* proc = "/proc/self/exe";',
    'const char* sh = "/bin/sh";',
    '// path: ~/.local/share/malgrind',
    'auto d = "Library/Application Support/malgrind";',
    'std::getenv("HOME");',
    'setenv("FOO", "1", 1);',
    'unsetenv("FOO");',
    'if (errno == EINTR) {}',
  ];
  for (const line of samples) {
    const hits = scanLines(line, 'os');
    assert.equal(hits.length, 1, `expected os rule to match: ${line}`);
    assert.equal(hits[0].rule, 'os');
    assert.equal(hits[0].line, 1);
  }
});

test('os rules ignore clean portable C++ lines', () => {
  const clean = [
    '#include <cstdint>',
    '#include <filesystem>',
    '#include "malgrind/profiler.hpp"',
    'int error_number = 0;',
    'void get_environment() {}',
    'const char* note = "local/share without dot";',
  ].join('\n');
  assert.deepEqual(scanLines(clean, 'os'), []);
});

test('chip rules match all forbidden patterns and preserve COM<digit> case', () => {
  const samples = [
    '// ESP32 target',
    'run("esptool");',
    'platform = "Espressif32";',
    'framework = "ESPIDF";',
    'xtensa-esp32-elf-addr2line',
    'merge_sdkconfig(a, b);',
    'if (name.startsWith("ttyUSB")) {}',
    'if (name.startsWith("ttyacm")) {}',
    'const port = "/dev/ttyS0";',
    'const port = "COM3";',
    'const cmd = "pkexec";',
    'run("SUDO apt update");',
    '// Ubuntu: apt install python3-venv',
  ];
  for (const line of samples) {
    const hits = scanLines(line, 'chip');
    assert.equal(hits.length, 1, `expected chip rule to match: ${line}`);
    assert.equal(hits[0].rule, 'chip');
  }

  assert.deepEqual(scanLines('const common = "com3"; // recommendation', 'chip'), []);
});

test('host/src/os/** is excluded from os checks', () => {
  withTempTree(
    {
      'host/src/os/posix.cpp': '#include <unistd.h>\nint e = errno;\n',
      'host/src/plain.cpp': '#include <unistd.h>\n',
    },
    (root) => {
      const violations = checkPortability(root, { os: [], chip: [] });
      assert.deepEqual(
        violations.map((v) => `${v.file}:${v.line}:${v.rule}`),
        ['host/src/plain.cpp:1:os'],
      );
    },
  );
});

test('host/src/families/** and app *.test.ts/*.d.ts are excluded from chip checks', () => {
  withTempTree(
    {
      'host/src/families/esp32.cpp': '// ESP32 xtensa- sdkconfig\n',
      'host/src/plain.cpp': '// ESP32\n',
      'app/src/lib/boards.test.ts': 'const port = "/dev/ttyUSB0";\n',
      'app/src/types.d.ts': '// esp32 declaration\n',
      'app/src/components/Board.svelte': '<span>ESP32</span>\n',
      'core/src/profiler.cpp': '#ifdef __linux__\n#endif\n',
      'core/include/malgrind/info.hpp': '#include <fcntl.h>\n',
      'core/tests/test_core.cpp': 'int x = errno;\n',
    },
    (root) => {
      const violations = checkPortability(root, { os: [], chip: [] });
      assert.deepEqual(
        violations.map((v) => `${v.file}:${v.line}:${v.rule}`),
        [
          'core/include/malgrind/info.hpp:1:os',
          'core/src/profiler.cpp:1:os',
          'core/tests/test_core.cpp:1:os',
          'app/src/components/Board.svelte:1:chip',
          'host/src/plain.cpp:1:chip',
        ],
      );
    },
  );
});

test('allow-listed files are skipped only for their listed rule set', () => {
  withTempTree(
    {
      'host/src/both.cpp': '#include <unistd.h>\n// ESP32\n',
      'host/src/only_os.cpp': '#include <unistd.h>\n',
    },
    (root) => {
      const violations = checkPortability(root, {
        os: ['host/src/both.cpp', 'host/src/only_os.cpp'],
        chip: [],
      });
      assert.equal(violations.length, 1);
      assert.equal(formatViolation(violations[0]), 'host/src/both.cpp:2: [chip] // ESP32');
    },
  );
});

test('host/src/os/** allows OS port/privilege terms while still forbidding MCU chip terms', () => {
  withTempTree(
    {
      'host/src/os/ports.cpp': 'const char* p = "/dev/ttyUSB0"; // COM5, pkexec, sudo, apt install\n',
      'host/src/os/bad.cpp': '// ESP32 sdkconfig\n',
    },
    (root) => {
      const violations = checkPortability(root, { os: [], chip: [] });
      assert.deepEqual(
        violations.map((v) => `${v.file}:${v.line}:${v.rule}`),
        ['host/src/os/bad.cpp:1:chip'],
      );
    },
  );
});
