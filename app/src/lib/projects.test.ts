import { describe, expect, it } from 'vitest';
import { boardTitle, classifyDrop, parseGithub, sourceLabel } from './projects';

describe('classifyDrop', () => {
  it('takes a plain path for a project folder', () => {
    expect(classifyDrop(['/home/me/firmware'])).toEqual({ kind: 'folder', path: '/home/me/firmware' });
    expect(classifyDrop(['/home/me/esp32.v2'])).toEqual({ kind: 'folder', path: '/home/me/esp32.v2' }); // dots are fine in a folder name
  });
  it('recognises a firmware ELF in any case', () => {
    expect(classifyDrop(['/x/firmware.elf'])).toEqual({ kind: 'elf', path: '/x/firmware.elf' });
    expect(classifyDrop(['/x/FIRMWARE.ELF']).kind).toBe('elf');
  });
  it('asks to unpack an archive, and refuses several items or none', () => {
    expect(classifyDrop(['/x/proj.zip'])).toMatchObject({ kind: 'unsupported', why: expect.stringContaining('unpack') });
    expect(classifyDrop(['/a', '/b']).kind).toBe('unsupported');
    expect(classifyDrop([]).kind).toBe('unsupported');
  });
});

describe('parseGithub', () => {
  it('reads the repository name from the three usual forms', () => {
    expect(parseGithub('https://github.com/Hombosh-dev/mcu-flamegraph')).toBe('mcu-flamegraph');
    expect(parseGithub('https://github.com/o/r.git')).toBe('r');
    expect(parseGithub('  https://github.com/o/r/  ')).toBe('r');
  });
  it('refuses everything that is not a repository link', () => {
    for (const bad of ['', 'github.com/o/r', 'http://github.com/o/r', 'https://gitlab.com/o/r', 'https://github.com/o',
      'https://github.com/o/r/tree/main', 'file:///etc', 'ext::sh -c id', 'https://github.com/o/..', 'git@github.com:o/r.git'])
      expect(parseGithub(bad), bad).toBeNull();
  });
});

describe('labels', () => {
  it('names where a project comes from', () => {
    expect(sourceLabel({ source: 'folder' })).toBe('Folder');
    expect(sourceLabel({ source: 'git' })).toBe('GitHub');
    expect(sourceLabel({ source: 'none' })).toBe('No code');
  });
  it('titles a tile by its device title or name, never by a chip it cannot know', () => {
    expect(boardTitle({ title: 'WCH CH340/CH343 USB-UART', name: 'usbserial-1' })).toBe('WCH CH340/CH343 USB-UART');
    expect(boardTitle({ title: '', name: 'usbserial-1' })).toBe('usbserial-1');
    expect(boardTitle({ title: '', name: '' })).toBe('Serial device');
  });
});
