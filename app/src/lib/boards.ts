// Small pure helpers of the board tiles.
import type { Device, Probe } from './types';

export interface FamilyLabel {
  text: string;
  tentative: boolean;
}

// Why this user cannot use the device, or null when they can.
export function accessProblem(device: Pick<Device, 'access'>): string | null {
  if (device.access.ok) return null;
  return device.access.group
    ? `No permission to use this port: you are not in the group "${device.access.group}".`
    : 'No permission to use this port.';
}

// What tells two boards of the same kind apart: short name, where they are plugged in, and the adapter's serial number.
export function boardDetail(device: Pick<Device, 'name' | 'usb'>): string {
  return [
    device.name,
    device.usb?.location && `USB ${device.usb.location}`,
    device.usb?.serial && `serial ${device.usb.serial}`
  ]
    .filter(Boolean)
    .join(' · ');
}

// Probed family (from a live frame) is shown as fact; a USB family hint is shown as "probably <name>".
export function boardFamily(
  device: Pick<Device, 'family_hint' | 'family_hint_name'>,
  probe: Pick<Probe, 'family' | 'family_name'> | null
): FamilyLabel | null {
  const probed = probe?.family_name || probe?.family;
  if (probed) return { text: probed, tentative: false };
  const hinted = device.family_hint_name || device.family_hint;
  if (hinted) return { text: `probably ${hinted}`, tentative: true };
  return null;
}
