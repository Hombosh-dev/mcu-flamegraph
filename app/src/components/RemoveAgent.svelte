<script lang="ts">
  import { onMount, untrack } from 'svelte';
  import BuildLog from './BuildLog.svelte';
  import type { Sidecar } from '../lib/sidecar';
  import type { Device, DevicesInfo, ProjectMeta } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    device: string; // the board to reflash; empty: choose among the serial devices found
    onDone(): void;
    onCancel(): void;
  }
  let { sidecar, device: given, onDone, onCancel }: Props = $props();

  let device = $state(untrack(() => given));
  let devices = $state<Device[]>([]);
  let devicesKnown = $state(false);

  const selectedLabel = $derived.by(() => {
    const found = devices.find((d) => d.id === device);
    return found ? `${found.title || found.name} (${found.name})` : '';
  });

  onMount(async () => {
    buildable = (await sidecar.call<ProjectMeta[]>('projects')).filter((p) => p.has_source && p.trusted && !p.problem);
    project = buildable[0]?.id ?? '';
    if (!device) {
      const info = await sidecar.call<DevicesInfo>('devices');
      devices = info.devices.filter((d) => d.kind === 'serial');
      device = devices[0]?.id ?? '';
    }
    devicesKnown = true;
  });

  let rebuild = $state(false); // false: write back the backup taken before the agent went on; true: build the user's project
  let buildable = $state<ProjectMeta[]>([]); // projects with source that may be built, for "rebuild my project instead"
  let project = $state('');
  let working = $state(false);
  let started = $state(false);
  let lines = $state<string[]>([]);
  let error = $state('');

  $effect(() => sidecar.on('build', (e) => lines.push(e.line)));

  async function remove() {
    working = started = true;
    error = '';
    lines = [];
    try {
      await sidecar.call('remove_agent', rebuild ? { how: 'rebuild', project, device } : { how: 'restore', device });
      onDone();
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
    working = false;
  }
</script>

<div class="backdrop" role="presentation">
  <div class="dialog" role="dialog" aria-modal="true" aria-labelledby="remove-title">
    <h3 id="remove-title">Remove the agent from the board?</h3>
    <p class="muted">
      {#if rebuild}
        Rebuilds your project without malgrind and flashes it. Your code is not touched. Live profiling stops.
      {:else}
        Restores the firmware that was on the board before malgrind last flashed the agent (a backup taken then), byte for byte. Live
        profiling stops.
      {/if}
    </p>
    {#if devices.length > 1}
      <label class="muted">Device
        <select bind:value={device}>
          {#each devices as d (d.id)}<option value={d.id}>{d.title || d.name} ({d.name})</option>{/each}
        </select>
      </label>
    {:else if device}
      <p class="muted">Device <code>{selectedLabel || device}</code></p>
    {:else if devicesKnown}
      <p class="error" role="alert">No serial device found. Connect the board first.</p>
    {/if}
    {#if rebuild}
      {#if buildable.length === 0}
        <p class="error">No project with source code. Add one first (New project), or restore the backup.</p>
      {:else}
        <label class="muted">Project <select bind:value={project}>{#each buildable as p}<option value={p.id}>{p.name}</option>{/each}</select></label>
      {/if}
    {/if}
    {#if !started}
      <button class="link" onclick={() => (rebuild = !rebuild)}>{rebuild ? 'Restore the backup instead' : 'Rebuild my project instead'}</button>
    {/if}
    {#if started}<BuildLog {lines} busy={working} />{/if}
    {#if error}<p class="error" role="alert">{error}</p>{/if}
    <div class="actions">
      <button class="btn danger" disabled={(rebuild && !project) || !device || working} onclick={remove}>
        {working ? 'Flashing…' : rebuild ? 'Rebuild & flash' : 'Restore & flash'}
      </button>
      <button class="btn ghost" disabled={working} onclick={onCancel}>Cancel</button>
    </div>
  </div>
</div>

<style>
  .backdrop { position: fixed; inset: 0; display: grid; place-items: center; background: #0008; }
  .dialog { width: min(560px, 92vw); padding: 20px 22px; background: var(--panel); border: 1px solid var(--line); border-radius: 12px; }
  h3 { margin: 0 0 8px; }
  .actions { display: flex; gap: 10px; margin-top: 16px; }
  .error { color: var(--danger); }
  .link { background: none; border: 0; padding: 4px 0; color: var(--muted); text-decoration: underline; }
  .link:hover { color: var(--fg); }
  code { font-family: var(--mono); }
</style>
