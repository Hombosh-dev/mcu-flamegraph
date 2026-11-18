<script lang="ts">
  import { onMount } from 'svelte';
  import { startSidecar, type Sidecar } from './lib/sidecar';
  import { installMenu } from './lib/menu';
  import type { Hello, ProjectMeta, RunMeta } from './lib/types';
  import RemoveAgent from './components/RemoveAgent.svelte';
  import NewProject from './screens/NewProject.svelte';
  import Profile from './screens/Profile.svelte';
  import Project from './screens/Project.svelte';
  import Run from './screens/Run.svelte';
  import Start from './screens/Start.svelte';

  // Where the user is. A run remembers the project it was opened from, so "close" goes back to it.
  type Route =
    | { name: 'start' }
    | { name: 'newproject' }
    | { name: 'project'; id: string }
    | { name: 'profile'; project: ProjectMeta }
    | { name: 'run'; run: RunMeta; live: boolean };

  let sidecar = $state<Sidecar | null>(null);
  let hello = $state<Hello | null>(null);
  let fatal = $state('');
  let route = $state<Route>({ name: 'start' });
  let removingDevice = $state<string | null>(null);

  // `recording`: attached or built just now; otherwise opened from the history
  const show = (run: RunMeta, recording: boolean) => (route = { name: 'run', run, live: recording });
  const home = (run?: RunMeta): Route => (run?.project_id ? { name: 'project', id: run.project_id } : { name: 'start' });

  onMount(async () => {
    try {
      sidecar = await startSidecar();
      hello = await sidecar.call<Hello>('hello');
      await installMenu({
        // a run being recorded (or shown) is closed first: the engine refuses a new run while one is live
        newProject: async () => {
          if (route.name === 'newproject') return;
          await sidecar?.call('stop').catch(() => {});
          route = { name: 'newproject' };
        },
        stop: () => {
          if (route.name !== 'run') return;
          const back = home(route.run);
          sidecar?.call('stop').finally(() => (route = back));
        },
        removeAgent: () =>
          (removingDevice = route.name === 'run' && route.live ? (route.run.device_id || route.run.port || '') : ''), // '' = the dialog picks the device
      });
    } catch (e) {
      fatal = String(e instanceof Error ? e.message : e);
    }
  });
</script>

{#if fatal}
  <main class="fatal">
    <h1>malgrind could not start its engine</h1>
    <p class="muted">{fatal}</p>
  </main>
{:else if !sidecar || !hello}
  <main class="fatal"><p class="muted">Starting…</p></main>
{:else if route.name === 'start'}
  <Start {sidecar} onNewProject={() => (route = { name: 'newproject' })} onProject={(id) => (route = { name: 'project', id })} onRun={(run) => show(run, false)} />
{:else if route.name === 'newproject'}
  <NewProject {sidecar} onCancel={() => (route = { name: 'start' })} onCreated={(project) => (route = { name: 'project', id: project.id })} />
{:else if route.name === 'project'}
  {#key route.id}
    <Project {sidecar} id={route.id} onBack={() => (route = { name: 'start' })} onProfile={(project) => (route = { name: 'profile', project })} onRun={(run) => show(run, false)} />
  {/key}
{:else if route.name === 'profile'}
  {@const project = route.project}
  <Profile {sidecar} {project} onBack={() => (route = { name: 'project', id: project.id })} onRun={show} />
{:else}
  {#key route.run.id}
    <Run
      {sidecar}
      run={route.run}
      live={route.live}
      onClose={() => (route = home(route.name === 'run' ? route.run : undefined))}
      onRemoveAgent={(device) => (removingDevice = device)}
    />
  {/key}
{/if}

{#if sidecar && removingDevice !== null}
  <RemoveAgent
    {sidecar}
    device={removingDevice}
    onCancel={() => (removingDevice = null)}
    onDone={() => ((removingDevice = null), (route = { name: 'start' }))}
  />
{/if}

<style>
  .fatal { padding: 48px 24px; text-align: center; }
</style>
