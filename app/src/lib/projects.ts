// Small pure helpers of the project screens (the engine stays the authority on every one of them).
import type { ProjectMeta } from './types';

export type Drop = { kind: 'folder'; path: string } | { kind: 'elf'; path: string } | { kind: 'unsupported'; why: string };

// What was dropped on the window: a firmware .elf, an archive (not supported), or else a project folder. The webview cannot tell
// a folder from a file, so anything else is taken for a folder (a name may contain dots) and the engine says if it is no project.
export function classifyDrop(paths: string[]): Drop {
  if (paths.length !== 1) return { kind: 'unsupported', why: 'drop one project folder or one .elf file' };
  const path = paths[0];
  if (/\.elf$/i.test(path)) return { kind: 'elf', path };
  if (/\.(zip|tar|tgz|gz|7z|rar)$/i.test(path)) return { kind: 'unsupported', why: 'unpack the archive first, then drop its folder' };
  return { kind: 'folder', path };
}

// The repository name of a link like https://github.com/owner/repo (also with .git or a trailing /), else null.
export function parseGithub(text: string): string | null {
  const m = /^https:\/\/github\.com\/([A-Za-z0-9-]+)\/([A-Za-z0-9._-]+?)(?:\.git)?\/?$/.exec(text.trim());
  return m && !m[2].startsWith('.') && !m[2].includes('..') ? m[2] : null;
}

export function sourceLabel(project: Pick<ProjectMeta, 'source'>): string {
  return project.source === 'folder' ? 'Folder' : project.source === 'git' ? 'GitHub' : 'No code';
}

// The title of a board tile. A USB adapter is not the chip, so it never claims one.
export function boardTitle(board: { title?: string; name?: string }): string {
  return board.title || board.name || 'Serial device';
}
