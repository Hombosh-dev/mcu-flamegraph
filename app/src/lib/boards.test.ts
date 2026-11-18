import { describe, expect, it } from 'vitest';
import { accessProblem, boardDetail, boardFamily } from './boards';
import type { UsbInfo } from './types';

const usb = (location: string, serial: string): UsbInfo => ({
  vid: '1a86',
  pid: '7523',
  location,
  serial,
  product: '',
  manufacturer: ''
});

describe('accessProblem', () => {
  it('is null when the port can be opened', () => {
    expect(accessProblem({ access: { ok: true, group: 'dialout', in_group: true, fixable: false } })).toBeNull();
  });
  it('names the group the user is missing', () => {
    expect(accessProblem({ access: { ok: false, group: 'dialout', in_group: false, fixable: true } })).toContain('"dialout"');
  });
  it('still says something when the group is unknown or not fixable', () => {
    expect(accessProblem({ access: { ok: false, group: '', in_group: false, fixable: false } })).toMatch(/^No permission/);
  });
});

describe('boardDetail', () => {
  it('tells identical boards apart by name, socket and serial number', () => {
    expect(boardDetail({ name: 'usbserial-1410', usb: usb('3-3', '0001') })).toBe('usbserial-1410 · USB 3-3 · serial 0001');
    expect(boardDetail({ name: 'usbserial-1420', usb: usb('3-4', '') })).toBe('usbserial-1420 · USB 3-4');
    expect(boardDetail({ name: 'usbserial-1430', usb: usb('', '') })).toBe('usbserial-1430');
    expect(boardDetail({ name: 'usbserial-1440', usb: null })).toBe('usbserial-1440');
    expect(boardDetail({ name: '', usb: null })).toBe('');
  });
});

describe('boardFamily', () => {
  it('returns null for generic bridges without a hint or probed family', () => {
    expect(boardFamily({ family_hint: '', family_hint_name: '' }, null)).toBeNull();
    expect(boardFamily({ family_hint: '', family_hint_name: '' }, { family: '', family_name: '' })).toBeNull();
  });

  it('marks a USB family hint as tentative before and after a non-agent probe', () => {
    expect(boardFamily({ family_hint: 'acme', family_hint_name: 'Acme MCU' }, null)).toEqual({
      text: 'probably Acme MCU',
      tentative: true
    });
    expect(boardFamily({ family_hint: 'acme', family_hint_name: 'Acme MCU' }, { family: '', family_name: '' })).toEqual({
      text: 'probably Acme MCU',
      tentative: true
    });
    expect(boardFamily({ family_hint: 'acme', family_hint_name: '' }, null)).toEqual({
      text: 'probably acme',
      tentative: true
    });
  });

  it('shows the probed family as fact even when a hint existed', () => {
    expect(
      boardFamily({ family_hint: 'acme', family_hint_name: 'Acme MCU' }, { family: 'acme', family_name: 'Acme MCU' })
    ).toEqual({
      text: 'Acme MCU',
      tentative: false
    });
    expect(boardFamily({ family_hint: '', family_hint_name: '' }, { family: 'acme', family_name: '' })).toEqual({
      text: 'acme',
      tentative: false
    });
  });
});
