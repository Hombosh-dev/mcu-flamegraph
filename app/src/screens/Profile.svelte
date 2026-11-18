<script lang="ts">
  import { onMount, untrack } from 'svelte';
  import BuildLog from '../components/BuildLog.svelte';
  import RunSettingsPanel from '../components/RunSettings.svelte';
  import Setup from '../components/Setup.svelte';
  import TopBar from '../components/TopBar.svelte';
  import { accessProblem, boardDetail, boardFamily } from '../lib/boards';
  import type { Sidecar } from '../lib/sidecar';
  import type { Device, DevicesInfo, Method, Probe, ProjectMeta, RunMeta, RunSettings } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    project: ProjectMeta;
    onBack(): void;
    onRun(run: RunMeta, recording: boolean): void;
  }
  let { sidecar, project, onBack, onRun }: Props = $props();

  type Tile = Device & {
    probe: Probe | null;
    error: string;
    chip: string;
    identifying: boolean;
    granting: boolean; // the system's password prompt is open
    grantError: string;
  };

  let step = $state<'board' | 'mode' | 'process'>('board');
  let tiles = $state<Tile[]>([]);
  let probes = $state<Device[]>([]);
  let scanned = $state(false);
  let chosen = $state<Tile | null>(null);
  let lines = $state<string[]>([]);
  let building = $state(false);
  let error = $state('');
  let notice = $state('');
  let setting = $state<'attach' | 'new_run' | null>(null); // the settings panel is open for this way of looking at the board
  let projectTitle = $state(untrack(() => project.name)); // follows a rename made in the panel
  let scanId = 0;

  // One tile per serial port, shown at once; each port is then listened to in turn (2 s, read-only) to see whether the agent runs.
  // A port this user may not open is not listened to: its tile offers to grant access instead.
  async function scan() {
    const mine = ++scanId;
    scanned = false;
    error = '';
    try {
      const info = await sidecar.call<DevicesInfo>('devices');
      if (mine !== scanId) return;
      const previousId = chosen?.id;
      probes = info.devices.filter((d) => d.kind === 'probe');
      tiles = info.devices
        .filter((d) => d.kind === 'serial')
        .map((d) => ({ ...d, probe: null, error: '', chip: '', identifying: false, granting: false, grantError: '' }));
      if (previousId) {
        const match = tiles.find((t) => t.id === previousId) ?? null;
        if (match) chosen = match;
        else if (step === 'board') chosen = null;
      }
      scanned = true;
      for (const tile of tiles) {
        if (!tile.access.ok) continue;
        try {
          const probe = await sidecar.call<Probe>('probe', { device: tile.id, project: project.id });
          if (mine === scanId) tile.probe = probe;
        } catch (e) {
          if (mine === scanId) tile.error = String(e instanceof Error ? e.message : e);
        }
      }
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
      scanned = true;
    }
  }
  onMount(scan);

  $effect(() => sidecar.on('build', (e) => {
    lines.push(e.line);
    if (lines.length > 200) lines.splice(0, lines.length - 200);
  }));

  // Asks the engine to grant port access, then rescans the boards.
  async function grant(tile: Tile) {
    tile.granting = true;
    tile.grantError = '';
    try {
      const result = await sidecar.call<{ ok_now: boolean; relogin: boolean }>('grant_access', { device: tile.id });
      // ok_now false: only the group part worked (no `setfacl`), and a group applies from the next login
      notice = !result.ok_now
        ? `You were added to the group "${tile.access.group}". Log out and in, then press Rescan.`
        : result.relogin
          ? 'Access works now. Log out and in once so it stays after the board is replugged.'
          : '';
      await scan();
    } catch (e) {
      tile.grantError = String(e instanceof Error ? e.message : e);
    }
    tile.granting = false;
  }

  async function identify(tile: Tile) {
    tile.identifying = true;
    tile.chip = '';
    try {
      tile.chip = (await sidecar.call<{ chip: string }>('chip_id', { device: tile.id })).chip;
    } catch (e) {
      tile.chip = String(e instanceof Error ? e.message : e);
    }
    tile.identifying = false;
  }

  const labels: Record<Method['id'], { title: string; text: string }> = {
    attach: { title: 'Attach', text: 'Watch the firmware that is already running on the board. Needs the malgrind agent in it. Nothing is flashed.' },
    new_run: {
      title: 'Build & flash',
      text: 'Builds your project with the agent and flashes it. The firmware on the board is backed up first and can be restored.'
    },
    no_flash: { title: 'Debug probe', text: 'Profile without touching the firmware, through a JTAG/SWD probe.' }
  };
  const order: Method['id'][] = ['attach', 'new_run', 'no_flash'];

  const methods = $derived(
    order.map((id) => chosen?.probe?.methods.find((m) => m.id === id) ?? { id, available: false, why: 'unknown' })
  );
  const chosenLine = $derived(chosen ? [chosen.title, boardDetail(chosen) || chosen.name].filter(Boolean).join(' · ') : '');

  function choose(tile: Tile) {
    chosen = tile;
    error = '';
    setting = null;
    step = 'mode';
  }

  // Both ways open the settings panel first (shown before every run, so a run can be made differently from the last one). It
  // names the board, which catches a click on the wrong one of two plugged-in boards; Build & flash replaces what is on the board.
  function pick(method: Method) {
    if (!chosen || !method.available || (method.id !== 'attach' && method.id !== 'new_run')) return;
    error = '';
    setting = method.id;
  }

  async function start(args: { projectName: string; runName: string; settings: RunSettings }) {
    if (!chosen || !setting) return;
    const mode = setting;
    error = '';
    try {
      if (args.projectName !== projectTitle) {
        await sidecar.call('project_update', { project: project.id, name: args.projectName });
        projectTitle = args.projectName;
      }
      if (mode === 'new_run') {
        step = 'process';
        building = true;
        lines = [];
      }
      const { run } = await sidecar.call<{ run: RunMeta }>(mode, { project: project.id, device: chosen.id, name: args.runName, settings: args.settings });
      onRun(run, true);
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
    building = false;
  }
</script>

<div class="page">
  <TopBar
    title={projectTitle}
    onBack={step === 'board' ? onBack : step === 'mode' ? () => (step = 'board') : building ? undefined : () => (step = 'mode')}
    backLabel="Back"
  >
    <ol class="steps">
      {#each ['Board', 'How', 'Run'] as label, i}
        <li class:on={['board', 'mode', 'process'].indexOf(step) === i} class:done={['board', 'mode', 'process'].indexOf(step) > i}>{label}</li>
      {/each}
    </ol>
  </TopBar>

  {#if step === 'board'}
    <Setup {sidecar} onChanged={scan} />
    <section>
      <div class="intro">
        <div>
          <h1>Which board?</h1>
          <p class="muted">Boards that are plugged in. Nothing is sent to a board while you look.</p>
        </div>
        <button class="btn ghost small" onclick={scan}>Rescan</button>
      </div>

      {#if scanned && tiles.length === 0 && probes.length === 0}
        <p class="empty">No board found. Connect it with a USB cable, then press Rescan.</p>
      {/if}

      {#if notice}<p class="notice" role="status">{notice}</p>{/if}

      <div class="tiles">
        {#each tiles as tile (tile.id)}
          {@const family = boardFamily(tile, tile.probe)}
          <div class="tile" class:wait={tile.access.ok && !tile.probe && !tile.error} class:denied={!tile.access.ok}>
            <button class="pick" disabled={!tile.probe || !tile.access.ok} onclick={() => choose(tile)} aria-label={`Use ${tile.title || tile.name} on ${tile.name}`}>
              <span class="chipicon" aria-hidden="true">▦</span>
              <span class="t">
                <b>{tile.title || tile.name}</b>
                {#if family}<span class="family" class:muted={family.tentative}>{family.text}</span>{/if}
                <span class="muted">{tile.name}</span>
                {#if boardDetail(tile)}<span class="muted detail">{boardDetail(tile)}</span>{/if}
              </span>
              <span class="status">
                {#if !tile.access.ok}<span class="bad">{accessProblem(tile)}</span>
                {:else if tile.error}<span class="bad">{tile.error}</span>
                {:else if !tile.probe}Listening for the agent…
                {:else if tile.probe.busy}<span class="bad">Busy: another program uses this port</span>
                {:else if tile.probe.agent}<span class="good">● malgrind agent running · {tile.probe.arch} · {tile.probe.ncores} cores · {tile.probe.cpu_mhz} MHz</span>
                {:else}No malgrind agent detected{/if}
              </span>
            </button>
            {#if !tile.access.ok && tile.access.fixable}
              <div class="grant">
                <button class="btn small" onclick={() => grant(tile)} disabled={tile.granting}>{tile.granting ? 'Waiting for your password…' : 'Grant access…'}</button>
                <span class="muted small">asks for your password once, in the system's own window</span>
                {#if tile.grantError}<pre class="bad">{tile.grantError}</pre>{/if}
              </div>
            {:else}
              <div class="foot">
                {#if tile.chip}<span class="muted chip">{tile.chip}</span>{/if}
                <button class="link" onclick={() => identify(tile)} disabled={tile.identifying || !tile.probe || tile.probe.busy} title="Asks the chip for its name. This resets the board.">
                  {tile.identifying ? 'Asking the chip…' : 'Identify chip (resets the board)'}
                </button>
              </div>
            {/if}
          </div>
        {/each}
        {#each probes as probe (probe.id)}
          <div class="tile off" aria-disabled="true">
            <div class="pick">
              <span class="chipicon" aria-hidden="true">⌁</span>
              <span class="t"><b>{probe.title || probe.name}</b><span class="muted">debug probe</span></span>
              <span class="status">Found. Profiling through a probe is in development.</span>
            </div>
          </div>
        {/each}
      </div>
      {#if error}<p class="error" role="alert">{error}</p>{/if}
    </section>
  {:else if step === 'mode' && chosen}
    <section>
      <h1>How do you want to look at it?</h1>
      <p class="muted">{chosenLine}</p>
      <div class="cards">
        {#each methods as m (m.id)}
          <button class="card" class:off={!m.available} class:picked={m.id === setting} disabled={!m.available} onclick={() => pick(m)}>
            <b>{labels[m.id].title}</b>
            {#if m.id === 'no_flash'}<span class="soon">in development</span>{/if}
            <span class="muted text">{labels[m.id].text}</span>
            {#if !m.available && m.id !== 'no_flash'}<span class="why">{m.why}</span>{/if}
          </button>
        {/each}
      </div>
      {#if setting}
        {#key setting}
          <RunSettingsPanel
            {sidecar}
            {project}
            device={chosen.id}
            mode={setting}
            boardLine={chosenLine}
            onStart={start}
            onCancel={() => (setting = null)}
          />
        {/key}
      {/if}
      {#if error}<p class="error" role="alert">{error}</p>{/if}
    </section>
  {:else}
    <section>
      <h1>{building ? 'Backing up the board, building with the agent, flashing…' : 'It did not finish'}</h1>
      <p class="muted">The board's flash is saved first (about a minute the first time), so "Remove agent" can put your firmware back exactly.</p>
      <BuildLog {lines} busy={building} />
      {#if error}<p class="error" role="alert">{error}</p>{/if}
    </section>
  {/if}
</div>

<style>
  .page { min-height: 100%; }
  section { max-width: 900px; margin: 0 auto; padding: 24px; }
  h1 { margin: 0 0 4px; font-size: 22px; }
  .intro { display: flex; justify-content: space-between; align-items: flex-start; gap: 16px; }
  .empty { padding: 28px; text-align: center; border: 1px dashed var(--line); border-radius: 12px; color: var(--muted); }
  .steps { display: flex; gap: 6px; list-style: none; margin: 0 8px 0 0; padding: 0; }
  .steps li { padding: 2px 10px; border-radius: 999px; background: var(--line); color: var(--muted); font-size: 12px; }
  .steps li.done { color: var(--live); }
  .steps li.on { background: var(--accent); color: var(--accent-fg); font-weight: 600; }
  .tiles { display: grid; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); gap: 16px; margin-top: 18px; }
  .tile { display: flex; flex-direction: column; border: 1.5px solid var(--line); border-radius: 16px; background: var(--panel); overflow: hidden; }
  .tile:not(.off):hover { border-color: var(--accent); box-shadow: 0 6px 24px #ff5a1f22; }
  .tile.off { opacity: 0.55; }
  .tile.wait { opacity: 0.8; }
  .pick { display: flex; flex-direction: column; gap: 8px; padding: 20px; text-align: left; background: none; border: 0; flex: 1; }
  button.pick:disabled { cursor: default; }
  .chipicon { font-size: 34px; color: var(--accent); line-height: 1; }
  .t { display: flex; flex-direction: column; }
  .t b { font-size: 16px; }
  .family { font-size: 12.5px; margin-top: 2px; }
  .status { font-size: 12.5px; color: var(--muted); }
  .good { color: var(--live); }
  .bad { color: var(--danger); }
  .foot { display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 6px 14px 10px; min-height: 36px; }
  .chip { font-size: 12px; }
  .link { background: none; border: 0; padding: 2px 0; color: var(--muted); font-size: 12px; text-decoration: underline; margin-left: auto; }
  .link:hover:not(:disabled) { color: var(--fg); }
  .link:disabled { opacity: 0.5; cursor: default; }
  .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 16px; margin-top: 20px; }
  .card { display: flex; flex-direction: column; gap: 8px; padding: 20px; text-align: left; border: 1.5px solid var(--line); border-radius: 16px; background: var(--panel); }
  .card:not(:disabled):hover { border-color: var(--accent); box-shadow: 0 6px 24px #ff5a1f22; }
  .card b { font-size: 17px; }
  .card.off { opacity: 0.75; cursor: default; }
  .text { font-size: 13px; }
  .why { font-size: 12.5px; color: var(--danger); margin-top: 4px; }
  .soon { align-self: flex-start; font-size: 11px; padding: 2px 8px; border-radius: 999px; background: var(--line); color: var(--muted); }
  .error { color: var(--danger); }
  .detail { font-size: 12px; }
  .tile.denied { border-color: var(--danger); }
  .grant { display: flex; flex-direction: column; align-items: flex-start; gap: 6px; padding: 4px 20px 16px; }
  .grant pre { margin: 4px 0 0; white-space: pre-wrap; font: 12px var(--mono); }
  .small { font-size: 12px; }
  .notice { margin: 14px 0 0; padding: 10px 14px; border: 1px solid var(--live); border-radius: 8px; color: var(--live); }
  .card.picked { border-color: var(--accent); }
</style>
