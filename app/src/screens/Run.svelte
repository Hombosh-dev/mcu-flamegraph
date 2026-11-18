<script lang="ts">
  import { untrack } from 'svelte';
  import { formatDuration } from '../lib/format';
  import type { Node } from '../lib/blend';
  import type { Sidecar } from '../lib/sidecar';
  import type { RunMeta } from '../lib/types';
  import Flame from '../views/Flame.svelte';
  import Functions, { type FunctionsData } from '../views/Functions.svelte';

  interface Props {
    sidecar: Sidecar;
    run: RunMeta;
    live: boolean; // recording right now (false: a stored run)
    onClose(): void;
    onRemoveAgent(device: string): void;
  }
  let { sidecar, run, live: startsLive, onClose, onRemoveAgent }: Props = $props();

  interface Stats {
    arch?: string;
    ncores?: number;
    cpu_mhz?: number;
    rate_hz?: number;
    samples_per_s?: number;
    dropped?: number;
    overhead_pct?: number | null;
    duration_ms?: number;
  }

  // The parent remounts this screen for every run ({#key}), so the props are read once on purpose.
  let live = $state(untrack(() => startsLive));
  let view = $state<'flame' | 'functions'>('flame');
  let core = $state<'all' | number>('all');
  let windowS = $state(untrack(() => startsLive) ? 10 : 0); // a stored run opens as the whole run
  let at = $state<number | null>(null); // null: the end of the run (following it, while live)
  let focus = $state('');
  let tree = $state<Node | null>(null);
  let functions = $state<FunctionsData | null>(null);
  let stats = $state<Stats>({});
  let notices = $state<string[]>([]);
  let name = $state(untrack(() => run.name));
  let notes = $state(untrack(() => run.notes));
  let showNotes = $state(false);
  let showSettings = $state(false); // what the run was made with (only runs made with the settings panel have it)
  // the fields the user set; an empty one was the detected value
  const used = $derived(
    run.settings
      ? ([
          ['Environment', run.settings.env],
          ['Sampling rate', run.settings.rate_hz ? `${run.settings.rate_hz} Hz` : ''],
          ['Firmware ELF', run.settings.elf],
          ['Extra arguments', run.settings.extra_args],
          ['Build flags', run.settings.build_flags],
          ['Build command', run.settings.build_command],
          ['Upload command', run.settings.upload_command]
        ].filter(([, value]) => value) as [string, string][])
      : []
  );
  let error = $state('');
  let busy = false;

  const duration = $derived(stats.duration_ms ?? run.duration_ms);
  const position = $derived(at ?? duration);
  const resetKey = $derived(`${core}|${windowS}|${at ?? 'end'}`);

  async function poll() {
    if (busy) return;
    busy = true;
    try {
      const args: Record<string, unknown> = { window_s: windowS };
      if (core !== 'all') args.core = core;
      if (at !== null) args.at_ms = at;
      const snapshot = await sidecar.call<{ tree: Node | null; stats: Stats }>('snapshot', args);
      tree = snapshot.tree;
      stats = snapshot.stats ?? {};
      if (view === 'functions') functions = await sidecar.call<FunctionsData>('functions', { ...args, focus });
      error = '';
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
    busy = false;
  }

  // refresh at once when anything the picture depends on changes ...
  $effect(() => {
    void [view, core, windowS, at, focus];
    poll();
  });
  // ... and twice a second while recording
  $effect(() => {
    if (!live) return;
    const timer = setInterval(poll, 500);
    return () => clearInterval(timer);
  });

  $effect(() => {
    const offs = [
      sidecar.on('notice', (e) => (notices = [...notices, e.text])),
      sidecar.on('ended', () => (live = false)),
    ];
    return () => offs.forEach((off) => off());
  });

  async function close() {
    try {
      await sidecar.call('stop');
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
    onClose();
  }

  async function save(fields: { name?: string; notes?: string }) {
    try {
      await sidecar.call('update', { run: run.id, ...fields });
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
  }

  const clock = (ms: number) => `${Math.floor(ms / 60000)}:${String(Math.floor(ms / 1000) % 60).padStart(2, '0')}`;
  const cores = $derived(Array.from({ length: stats.ncores ?? run.ncores }, (_, i) => i));
</script>

<div class="page">
  <header>
    <span class="logo">🔥 malgrind</span>
    <input class="name" bind:value={name} onblur={() => save({ name })} onkeydown={(e) => e.key === 'Enter' && e.currentTarget.blur()} aria-label="Run name" />
    {#if live}<span class="live"><span class="dot"></span>LIVE</span>{:else}<span class="muted">{formatDuration(duration)}</span>{/if}
    {#if run.port}<span class="muted stat" title="The board this run was recorded from">{run.port}</span>{/if}
    <span class="spacer"></span>
    {#if stats.arch}
      <span class="stat muted">{stats.samples_per_s ?? '–'} samples/s · {stats.dropped ?? 0} dropped · {stats.overhead_pct == null ? '–' : stats.overhead_pct + ' %'} overhead</span>
    {/if}
    {#if run.settings}<button class="btn ghost small" onclick={() => (showSettings = !showSettings)}>Settings</button>{/if}
    <button class="btn ghost small" onclick={() => (showNotes = !showNotes)}>Notes</button>
    <button class="btn small" onclick={close}>{live ? 'Stop' : 'Close'}</button>
  </header>

  {#if showSettings && run.settings}
    <div class="used" aria-label="Settings of this run">
      {#if used.length === 0}<span class="muted">Made with everything as detected.</span>{/if}
      {#each used as [label, value]}<span><span class="muted">{label}</span> <code>{value}</code></span>{/each}
    </div>
  {/if}
  {#if showNotes}
    <textarea class="notes" bind:value={notes} onblur={() => save({ notes })} placeholder="Notes about this run…"></textarea>
  {/if}
  {#each notices as text, i}
    <div class="notice">{text}<button class="x" onclick={() => (notices = notices.filter((_, j) => j !== i))} aria-label="Dismiss">×</button></div>
  {/each}
  {#if !live && startsLive}<div class="notice">The board stopped sending data (unplugged, or reset?). The run is saved.</div>{/if}
  {#if error}<div class="notice bad" role="alert">{error}</div>{/if}

  <div class="body">
    <nav>
      <button class:on={view === 'flame'} onclick={() => (view = 'flame')}>Flamegraph</button>
      <button class:on={view === 'functions'} onclick={() => (view = 'functions')}>Functions</button>
      <div class="filters">
        <label>Core
          <select bind:value={core}><option value="all">all</option>{#each cores as c}<option value={c}>{c}</option>{/each}</select>
        </label>
        <label>Window
          <select bind:value={windowS}>
            <option value={5}>5 s</option><option value={10}>10 s</option><option value={30}>30 s</option>
            <option value={60}>60 s</option><option value={0}>whole run</option>
          </select>
        </label>
      </div>
      {#if live}<button class="remove" onclick={() => onRemoveAgent(run.device_id || run.port)}>Remove agent…</button>{/if}
    </nav>

    <main>
      {#if view === 'flame'}
        <Flame {tree} {resetKey} />
      {:else}
        <Functions data={functions} {focus} onFocus={(f) => (focus = f)} />
      {/if}
    </main>
  </div>

  <footer>
    <span class="clock">{clock(position)}</span>
    <input type="range" min="0" max={Math.max(duration, 1)} value={position} oninput={(e) => (at = Number(e.currentTarget.value))} aria-label="Time in the run" />
    <span class="clock">{clock(duration)}</span>
    {#if at !== null}<button class="btn ghost small" onclick={() => (at = null)}>{live ? 'Back to live' : 'To the end'}</button>{/if}
  </footer>
</div>

<style>
  .page { display: flex; flex-direction: column; height: 100%; }
  header { display: flex; align-items: center; gap: 12px; padding: 8px 16px; border-bottom: 1px solid var(--line); background: var(--panel); }
  .logo { font-weight: 700; color: var(--accent); }
  .name { font: 600 14px var(--font); color: var(--fg); background: none; border: 1px solid transparent; border-radius: 6px; padding: 3px 8px; min-width: 200px; }
  .name:hover, .name:focus { border-color: var(--line); background: var(--bg); outline: none; }
  .live { display: inline-flex; align-items: center; gap: 6px; color: var(--live); font-weight: 600; font-size: 12px; }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--live); box-shadow: 0 0 8px var(--live); }
  .spacer { flex: 1; }
  .stat { font-size: 12px; font-variant-numeric: tabular-nums; }
  .used { display: flex; flex-wrap: wrap; gap: 6px 18px; padding: 8px 16px; font-size: 12.5px; background: var(--panel); border-bottom: 1px solid var(--line); }
  .used code { font-family: var(--mono); font-size: 12px; }
  .notes { margin: 0; width: 100%; min-height: 70px; padding: 10px 16px; font: inherit; color: var(--fg); background: var(--panel); border: 0; border-bottom: 1px solid var(--line); resize: vertical; }
  .notice { display: flex; justify-content: space-between; padding: 8px 16px; background: color-mix(in srgb, var(--accent) 12%, var(--panel)); border-bottom: 1px solid var(--line); font-size: 13px; }
  .notice.bad { background: color-mix(in srgb, var(--danger) 14%, var(--panel)); color: var(--danger); }
  .x { background: none; border: 0; color: var(--muted); font-size: 16px; }
  .body { flex: 1; display: flex; min-height: 0; }
  nav { width: 168px; display: flex; flex-direction: column; border-right: 1px solid var(--line); padding: 8px 0; }
  nav > button { text-align: left; padding: 9px 16px; background: none; border: 0; border-left: 3px solid transparent; color: var(--muted); }
  nav > button.on { color: var(--fg); background: var(--panel); border-left-color: var(--accent); }
  .filters { margin-top: 18px; padding: 0 16px; display: flex; flex-direction: column; gap: 10px; font-size: 12px; color: var(--muted); }
  .filters select { width: 100%; margin-top: 3px; }
  .remove { margin-top: auto; padding: 10px 16px; text-align: left; background: none; border: 0; border-top: 1px solid var(--line); color: var(--danger); }
  main { flex: 1; min-width: 0; overflow: auto; padding: 14px 18px; }
  footer { display: flex; align-items: center; gap: 12px; padding: 8px 16px; border-top: 1px solid var(--line); background: var(--panel); }
  footer input[type='range'] { flex: 1; accent-color: var(--accent); }
  .clock { font: 12px var(--mono); color: var(--muted); }
</style>
