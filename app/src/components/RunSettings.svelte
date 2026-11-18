<script lang="ts">
  import { open } from '@tauri-apps/plugin-dialog';
  import { onMount, untrack } from 'svelte';
  import { defaultRunName } from '../lib/format';
  import { changes, defaultRate, hasOverrides, parseRate, rateProblem } from '../lib/settings';
  import type { Sidecar } from '../lib/sidecar';
  import type { ProjectMeta, RunPlan, RunSettings } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    project: ProjectMeta;
    device: string;
    mode: 'attach' | 'new_run';
    boardLine: string; // "Silicon Labs CP210x · USB 3-3", for the sentence that names the board
    onStart(args: { projectName: string; runName: string; settings: RunSettings }): void;
    onCancel(): void;
  }
  let { sidecar, project, device, mode, boardLine, onStart, onCancel }: Props = $props();

  // The panel is shown before every run: it starts from what the project used last time (else what was detected) and every
  // field can be changed. What it sends is stored with the run.
  let plan = $state<RunPlan | null>(null);
  let loadError = $state('');
  let projectName = $state(untrack(() => project.name));
  let runName = $state(untrack(() => defaultRunName(project.name)));
  let env = $state('');
  let rateText = $state('');
  let elf = $state('');
  let extraArgs = $state('');
  let buildFlags = $state('');
  let buildCommand = $state(''); // empty: the detected command (typing one makes it an override)
  let uploadCommand = $state('');
  let autoBuild = $state(''); // the commands for the fields as they are now, when no override replaces them
  let autoUpload = $state('');
  let envVars = $state<string[]>([]);
  let engineError = $state('');
  let warning = $state('');
  let asked = 0;

  const detectedRate = $derived(plan?.detected.rate_hz || defaultRate);
  const rate = $derived(parseRate(rateText));
  const rateError = $derived(rateProblem(rate));

  const settings = $derived<RunSettings>({
    env: env === (plan?.detected.env ?? '') ? '' : env,
    extra_args: extraArgs,
    build_flags: buildFlags,
    rate_hz: Number.isNaN(rate) || rate === detectedRate ? 0 : rate, // the default is sent as "not set"
    elf,
    build_command: buildCommand,
    upload_command: uploadCommand
  });

  function fill(s: RunSettings, detected: RunSettings) {
    env = s.env || detected.env;
    rateText = String(s.rate_hz || detected.rate_hz || defaultRate);
    elf = s.elf;
    extraArgs = s.extra_args;
    buildFlags = s.build_flags;
    buildCommand = s.build_command;
    uploadCommand = s.upload_command;
  }

  onMount(async () => {
    try {
      plan = await sidecar.call<RunPlan>('run_plan', { project: project.id, device, mode });
      fill(plan.last ?? plan.detected, plan.detected);
      if (mode === 'attach' && !elf) elf = plan.detected.elf;
    } catch (e) {
      loadError = String(e instanceof Error ? e.message : e);
    }
  });

  // The commands follow the fields: the engine builds the preview with the same function the run uses.
  $effect(() => {
    if (!plan || mode === 'attach') return;
    const sent = settings;
    const timer = setTimeout(async () => {
      const mine = ++asked;
      try {
        const answer = await sidecar.call<RunPlan>('run_plan', { project: project.id, device, mode, settings: sent });
        if (mine !== asked) return; // an older answer
        engineError = answer.error;
        warning = answer.warning ?? '';
        envVars = answer.commands?.env ?? [];
        if (answer.commands) {
          if (!sent.build_command) autoBuild = answer.commands.build;
          if (!sent.upload_command) autoUpload = answer.commands.upload;
        }
      } catch (e) {
        engineError = String(e instanceof Error ? e.message : e);
      }
    }, 250);
    return () => clearTimeout(timer);
  });

  async function chooseElf() {
    const picked = await open({ multiple: false, title: 'Firmware ELF (gives function names)', filters: [{ name: 'ELF', extensions: ['elf'] }] });
    if (typeof picked === 'string') elf = picked;
  }

  function reset() {
    if (!plan) return;
    fill({ env: '', extra_args: '', build_flags: '', rate_hz: 0, elf: '', build_command: '', upload_command: '' }, plan.detected);
    if (mode === 'attach') elf = plan.detected.elf;
  }

  const changed = $derived(plan ? changes(settings, plan.detected) : []);
  const canStart = $derived(!!plan && !rateError && !engineError && runName.trim() !== '' && projectName.trim() !== '');
</script>

<div class="panel" role="group" aria-label={mode === 'attach' ? 'Attach settings' : 'Run settings'}>
  <h2>{mode === 'attach' ? 'Attach to' : 'Build & flash on'} {boardLine}</h2>

  {#if loadError}
    <p class="error" role="alert">{loadError}</p>
    <div class="actions"><button class="btn ghost" onclick={onCancel}>Back</button></div>
  {:else if !plan}
    <p class="muted">Looking at the project…</p>
  {:else}
    <div class="grid">
      <label>Project name <input bind:value={projectName} aria-label="Project name" /></label>
      <label>Run name <input bind:value={runName} aria-label="Run name" /></label>

      {#if mode === 'new_run'}
        <label>Environment
          <select bind:value={env}>{#each plan.envs ?? [] as e}<option>{e}</option>{/each}</select>
          <span class="hint">detected: {plan.detected.env}</span>
        </label>
        <label>Sampling rate (Hz per core)
          <input bind:value={rateText} inputmode="numeric" aria-label="Sampling rate" aria-invalid={!!rateError} />
          <span class="hint" class:bad={!!rateError}>{rateError ?? `detected: ${detectedRate}. About 1.5 % of a core per 1000 Hz.`}</span>
        </label>
      {/if}

      <label class="wide">Firmware ELF (for function names)
        <span class="row">
          <input bind:value={elf} placeholder={mode === 'attach' ? 'found automatically' : `found after the build: ${plan.elf_hint ?? ''}`} aria-label="Firmware ELF" />
          <button class="btn ghost small" onclick={chooseElf}>Choose…</button>
        </span>
      </label>

      {#if mode === 'new_run'}
        <label>Extra pio arguments <input bind:value={extraArgs} placeholder="--disable-auto-clean" aria-label="Extra pio arguments" /></label>
        <label>Extra build flags <input bind:value={buildFlags} placeholder="-DMY_FLAG=1" aria-label="Extra build flags" /></label>
      {/if}
    </div>

    {#if mode === 'new_run'}
      <details class="advanced">
        <summary>Advanced: the exact commands{hasOverrides({ build_command: buildCommand, upload_command: uploadCommand }) ? ' (edited)' : ''}</summary>
        <label>Build
          <textarea rows="2" spellcheck="false" aria-label="Build command" class:custom={!!buildCommand}
            value={buildCommand || autoBuild}
            oninput={(e) => (buildCommand = e.currentTarget.value === autoBuild ? '' : e.currentTarget.value)}></textarea>
          {#if buildCommand}<button class="link" onclick={() => (buildCommand = '')}>Use detected</button>{/if}
        </label>
        <label>Upload (after the backup)
          <textarea rows="2" spellcheck="false" aria-label="Upload command" class:custom={!!uploadCommand}
            value={uploadCommand || autoUpload}
            oninput={(e) => (uploadCommand = e.currentTarget.value === autoUpload ? '' : e.currentTarget.value)}></textarea>
          {#if uploadCommand}<button class="link" onclick={() => (uploadCommand = '')}>Use detected</button>{/if}
        </label>
        <p class="hint">No shell is used: the line is split into words and run as it is. These are also set for the run (they carry the agent):</p>
        <pre class="env">{envVars.join('\n')}</pre>
      </details>
    {/if}

    {#if engineError}<p class="error" role="alert">{engineError}</p>{/if}
    {#if warning}<p class="warning" role="status">{warning}</p>{/if}
    {#if changed.length > 0}<p class="hint">Changed from detected: {changed.join(', ')}.</p>{/if}
    {#if mode === 'new_run'}<p class="hint">The firmware on the board is saved first and can be restored.</p>{/if}

    <div class="actions">
      <button class="btn" disabled={!canStart} onclick={() => onStart({ projectName: projectName.trim(), runName: runName.trim(), settings })}>
        {mode === 'attach' ? 'Attach' : 'Flash this board'}
      </button>
      <button class="btn ghost" onclick={reset}>Reset to detected</button>
      <button class="btn ghost" onclick={onCancel}>Cancel</button>
    </div>
  {/if}
</div>

<style>
  .panel { margin-top: 18px; padding: 16px 18px; border: 1.5px solid var(--accent); border-radius: 12px; background: color-mix(in srgb, var(--accent) 6%, var(--panel)); }
  h2 { margin: 0 0 12px; font-size: 15px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 12px 16px; }
  label { display: flex; flex-direction: column; gap: 4px; font-size: 12.5px; color: var(--muted); }
  .wide { grid-column: 1 / -1; }
  .row { display: flex; gap: 8px; }
  input, select, textarea { min-width: 0; padding: 7px 10px; font: 13px var(--mono); color: var(--fg); background: var(--bg); border: 1px solid var(--line); border-radius: 6px; }
  .row input { flex: 1; }
  input:focus, textarea:focus, select:focus { outline: none; border-color: var(--accent); }
  input[aria-invalid='true'] { border-color: var(--danger); }
  textarea { resize: vertical; width: 100%; }
  textarea.custom { border-color: var(--accent); }
  .hint { font: 12px var(--font); color: var(--muted); }
  .hint.bad, .error { color: var(--danger); }
  .warning { margin: 10px 0 0; color: var(--accent); font-size: 13px; }
  .advanced { margin-top: 14px; }
  .advanced summary { cursor: pointer; color: var(--muted); font-size: 13px; }
  .advanced label { margin-top: 10px; }
  .env { margin: 0; padding: 8px 10px; font: 11.5px var(--mono); color: var(--muted); background: var(--bg); border-radius: 6px; white-space: pre-wrap; word-break: break-all; }
  .link { align-self: flex-start; background: none; border: 0; padding: 0; color: var(--accent); font-size: 12px; text-decoration: underline; }
  .actions { display: flex; gap: 10px; margin-top: 14px; }
  .error { white-space: pre-wrap; margin: 10px 0 0; }
</style>
