<script lang="ts">
  import { onMount } from 'svelte';
  import type { Sidecar } from '../lib/sidecar';
  import type { SetupInfo } from '../lib/types';
  import BuildLog from './BuildLog.svelte';

  interface Props {
    sidecar: Sidecar;
    onChanged?: () => void; // PlatformIO was installed: the caller should look at the boards again
  }
  let { sidecar, onChanged }: Props = $props();

  let info = $state<SetupInfo | null>(null);
  let installing = $state(false);
  let lines = $state<string[]>([]);
  let error = $state('');

  async function load() {
    try {
      info = await sidecar.call<SetupInfo>('setup');
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
  }
  onMount(load);

  // the engine reports every long job as `build` events; only the install's own lines are kept here
  $effect(() =>
    sidecar.on('build', (e) => {
      if (!installing) return;
      lines.push(e.line);
      if (lines.length > 200) lines.splice(0, lines.length - 200);
    })
  );

  async function install() {
    installing = true;
    error = '';
    lines = [];
    try {
      await sidecar.call('install_pio');
      await load();
      onChanged?.();
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
    installing = false;
  }
</script>

{#if info && (!info.pio || !info.git)}
  <section class="setup" aria-label="Setup">
    <h3>Before you can build firmware</h3>
    <ul>
      <li class:bad={!info.pio}>
        <span class="mark" aria-hidden="true">{info.pio ? '✔' : '✘'}</span>
        <div class="grow">
          <b>PlatformIO</b>
          {#if info.pio}
            <span class="muted">found</span>
          {:else if info.python}
            <span class="muted">builds and flashes the firmware. malgrind downloads it from pypi.org into <code>~/.platformio</code>, no administrator rights needed.
              The first build then downloads the toolchain for your board, about 1 GB.</span>
          {:else}
            <span class="muted">is not installed, and Python 3 is needed to install it ({info.python_hint}).</span>
          {/if}
        </div>
        {#if !info.pio}
          <button class="btn small" onclick={install} disabled={installing || !info.python}>{installing ? 'Installing…' : 'Install PlatformIO'}</button>
        {/if}
      </li>
      {#if !info.git}
        <li class="soft">
          <span class="mark" aria-hidden="true">–</span>
          <div class="grow"><b>git</b> <span class="muted">is only needed to open projects from GitHub. Install it with your package manager if you want that.</span></div>
        </li>
      {/if}
    </ul>
    {#if installing || lines.length > 0}<BuildLog {lines} busy={installing} />{/if}
    {#if error}<p class="error" role="alert">{error}</p>{/if}
  </section>
{/if}

<style>
  .setup { margin: 16px 18px; padding: 14px 18px; border: 1px solid var(--accent); border-radius: 12px; background: color-mix(in srgb, var(--accent) 7%, var(--panel)); }
  h3 { margin: 0 0 10px; font-size: 15px; }
  ul { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 10px; }
  li { display: flex; align-items: center; gap: 12px; }
  .grow { flex: 1; font-size: 13px; }
  .mark { width: 1.2em; color: var(--live); }
  li.bad .mark { color: var(--danger); }
  li.soft .mark { color: var(--muted); }
  .error { color: var(--danger); margin: 10px 0 0; white-space: pre-wrap; }
  code { font-family: var(--mono); background: var(--bg); padding: 1px 5px; border-radius: 4px; }
</style>
