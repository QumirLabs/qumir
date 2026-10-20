'use strict';

import { DebuggerUI } from './debugger_ui.js';
import { ExecutionStopped } from './runtime/execution_session.js';
import { consumeExample, exampleData } from './example_data.js';

const embedded = document.body.dataset.qumirMode === 'embed';
let activeRun = null;

let debuggerUI = null;
let executionSession = null;
let executionAbort = null;

const $ = sel => document.querySelector(sel);
let currentAbort = null;
// Set to true by the Stop button; cleared at the start of each run.
let __coroStopRequested = false;
let __coroRunning = false;
// Output view selection for the compiler pane (text or turtle)
let __compilerOutputMode = 'text';
let __turtleCanvas = null;
let __turtleToggle = null;
let __turtleModule = null;
let __robotModule = null;
let __robotCanvas = null;
let __robotEditEnabled = false;
let __robotEditTool = 'wall';
let __robotResizeDrag = null;
let __robotSuppressNextClick = false;
let __executorToolbarMode = null;
let __drawerModule = null;
let __drawerCanvas = null;
let __colorsModule = null;
let __keyboardModule = null;
let __painterModule = null;
let __painterUI = null;
let __painterRulerX = null;
let __painterRulerY = null;
let __painterStatus = null;
let __painterCanvas = null;
const IO_PANE_COOKIE = 'q_io_pane';
let __ioFiles = [];
let __ioSelectEl = null;
let __ioTabsEl = null;
let __ioFilesRoot = null;
let __currentIoPane = 'errors';
let __errorsHasBadge = false;
let __ioFileCounter = 0;
let __browserFileManager = null;
const PROJECTS_STORAGE_KEY = 'q_projects';
const ACTIVE_PROJECT_KEY = 'q_active_project';
let __projects = [];
let __activeProjectId = null;
let __projectsDrawer = null;
let __projectsListEl = null;
let __projectsBackdrop = null;
let __projectToggleBtn = null;
let __projectActiveNameEl = null;
let __projectNewBtn = null;
let __projectsRenderPending = false;
let __successfulRunsCount = 0;
const api = async (path, body, asBinary, signal) => {
  const code = body.code || '';
  const O = body.O || '0';
  const syntaxMode = document.getElementById('syntax-mode');
  const headers = { 'Content-Type': 'text/plain', 'X-Qumir-O': String(O) };
  if (syntaxMode && syntaxMode.value === 'core') headers['X-Qumir-Syntax'] = 'core';
  if (body.debug) headers['X-Qumir-Debug-Points'] = '1';
  const r = await fetch(path, { method: 'POST', headers, body: code, signal });
  if (!r.ok) {
    let msg;
    try { const j = await r.json(); msg = j.error || r.statusText; } catch { msg = r.statusText; }
    throw new Error(msg);
  }
  return asBinary ? new Uint8Array(await r.arrayBuffer()) : await r.text();
};

// Simple GET helper for text/json
const apiGet = async (path) => {
  const r = await fetch(path, { method: 'GET' });
  if (!r.ok) throw new Error(await r.text());
  const ct = r.headers.get('Content-Type') || '';
  if (ct.includes('application/json')) return await r.json();
  return await r.text();
};

function requestStopRunningProgram() {
  const stopBtn = document.getElementById('btn-stop');
  signalCoroStop();
  if (stopBtn && stopBtn.style.display !== 'none') {
    stopBtn.click();
  }
}

function resetCoroStopSignal() {
  __coroStopRequested = false;
}

function signalCoroStop() {
  executionSession?.stop();
  executionAbort?.abort();
  __coroStopRequested = true;
}

const sample = document.getElementById('code')?.value || '';

function parseAlgHeader(code) {
  const lines = code.split(/\r?\n/);
  for (const line of lines) {
    if (!/^\s*алг\b/u.test(line)) continue;
    const trimmed = line.trim();
    const tokens = trimmed.split(/\s+/);
    if (tokens.length === 1) {
      // Plain `алг` — anonymous void entry point
      return { type: null, name: null };
    }
    if (tokens.length === 2) {
      return { type: null, name: tokens[1].replace(/\(.*/, '') };
    }
    if (tokens.length >= 3) {
      return { type: tokens[1], name: tokens[2].replace(/\(.*/, '') };
    }
  }
  return { type: null, name: null };
}

// CodeMirror editor (initialized below if library present)
let editor = null;
function getCode() {
  if (editor) return editor.getValue();
  const el = document.getElementById('code');
  return el ? el.value : '';
}
function setCode(text) {
  if (editor) return editor.setValue(text);
  const el = document.getElementById('code');
  if (el) {
    el.value = text;
  } else {
    console.warn('setCode: textarea not found, code will not be set');
  }
}

function setCookie(name, value, days = 365) {
  if (embedded) {
    return;
  }
  const expires = `max-age=${days*24*60*60}`;
  document.cookie = `${encodeURIComponent(name)}=${encodeURIComponent(value)}; ${expires}; path=/`;
}

function getCookie(name) {
  if (embedded) {
    return null;
  }
  const n = encodeURIComponent(name) + '=';
  const parts = document.cookie.split(';');
  for (let p of parts) {
    p = p.trim();
    if (p.startsWith(n)) return decodeURIComponent(p.substring(n.length));
  }
  return null;
}

function readPersistedValue(name) {
  if (embedded) {
    return null;
  }
  try {
    if (typeof window !== 'undefined' && window.localStorage) {
      const stored = window.localStorage.getItem(name);
      if (stored !== null && stored !== undefined) return stored;
    }
  } catch (err) {
    console.warn('localStorage read failed:', err);
  }
  return getCookie(name);
}

function writePersistedValue(name, value) {
  if (embedded) {
    return;
  }
  const payload = value ?? '';
  try {
    if (typeof window !== 'undefined' && window.localStorage) {
      window.localStorage.setItem(name, payload);
    }
  } catch (err) {
    console.warn('localStorage write failed:', err);
  }
  setCookie(name, payload);
}

function generateProjectId() {
  return `project-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`;
}

function normalizeProject(entry, idx) {
  if (!entry || typeof entry !== 'object') return null;
  const id = typeof entry.id === 'string' && entry.id.trim() ? entry.id.trim() : generateProjectId();
  const name = typeof entry.name === 'string' && entry.name.trim() ? entry.name.trim() : `Проект ${idx + 1}`;
  return {
    id,
    name,
    code: typeof entry.code === 'string' ? entry.code : '',
    args: typeof entry.args === 'string' ? entry.args : '',
    stdin: typeof entry.stdin === 'string' ? entry.stdin : '',
    files: Array.isArray(entry.files) ? entry.files.map((f, i) => normalizeIoFile(f, i)).filter(Boolean) : [],
    updatedAt: Number(entry.updatedAt) || Date.now()
  };
}

function bootstrapProjects() {
  if (__projects.length) return;
  let parsed = [];
  const raw = readPersistedValue(PROJECTS_STORAGE_KEY);
  if (raw) {
    try {
      const data = JSON.parse(raw);
      if (Array.isArray(data)) {
        parsed = data.map((entry, idx) => normalizeProject(entry, idx)).filter(Boolean);
      }
    } catch (err) {
      console.warn('projects parse failed:', err);
    }
  }
  if (!parsed.length) {
    const fallbackCode = readPersistedValue('q_code');
    const fallbackArgs = readPersistedValue('q_args');
    parsed = [normalizeProject({
      id: generateProjectId(),
      name: 'Проект 1',
      code: typeof fallbackCode === 'string' ? fallbackCode : sample,
      args: typeof fallbackArgs === 'string' ? fallbackArgs : '',
      stdin: '',
      files: [],
      updatedAt: Date.now()
    }, 0)];
  }
  __projects = parsed;
  const storedActive = readPersistedValue(ACTIVE_PROJECT_KEY);
  if (storedActive && __projects.some(p => p.id === storedActive)) {
    __activeProjectId = storedActive;
  } else {
    __activeProjectId = __projects.length ? __projects[0].id : null;
  }
  persistProjects();
}

function getActiveProject() {
  return __projects.find(p => p.id === __activeProjectId) || null;
}

function persistProjects() {
  if (!Array.isArray(__projects)) __projects = [];
  __projects.sort((a, b) => (b.updatedAt || 0) - (a.updatedAt || 0));
  writePersistedValue(PROJECTS_STORAGE_KEY, JSON.stringify(__projects));
  if (__activeProjectId) {
    writePersistedValue(ACTIVE_PROJECT_KEY, __activeProjectId);
  }
}

function captureCurrentEditorState() {
  const argsEl = $('#args');
  const stdinEl = $('#stdin');
  return {
    code: getCode(),
    args: argsEl ? argsEl.value : '',
    stdin: stdinEl ? stdinEl.value : '',
    files: __ioFiles.map((file, idx) => normalizeIoFile(file, idx)).filter(Boolean),
  };
}

function updateActiveProjectFromInputs() {
  const project = getActiveProject();
  if (!project) return;
  const snapshot = captureCurrentEditorState();
  const prevFiles = JSON.stringify(Array.isArray(project.files) ? project.files : []);
  const nextFiles = JSON.stringify(snapshot.files);
  // console.log('[projects] updateActiveProjectFromInputs before', {
  //   id: project.id,
  //   name: project.name,
  //   prev: { code: project.code, args: project.args, stdin: project.stdin },
  //   next: snapshot
  // });
  if (project.code === snapshot.code && project.args === snapshot.args && project.stdin === snapshot.stdin && prevFiles === nextFiles) {
    // console.log('[projects] updateActiveProjectFromInputs: no changes, skip');
    return;
  }
  project.code = snapshot.code;
  project.args = snapshot.args;
  project.stdin = snapshot.stdin;
  project.files = snapshot.files;
  project.updatedAt = Date.now();
  // console.log('[projects] updateActiveProjectFromInputs after', {
  //   id: project.id,
  //   name: project.name,
  //   stdin: project.stdin
  // });
  persistProjects();
  scheduleProjectsRender();
}

function persistIoFiles() {
  updateActiveProjectFromInputs();
}

function applyProjectToInputs(project, { silent = false } = {}) {
  if (!project) return;
  // Capture project.files before setCode() — CodeMirror fires 'change' synchronously
  // which calls saveState() → updateActiveProjectFromInputs() → project.files = __ioFiles.
  // If we read project.files after setCode(), we'd get the old project's files.
  const _projectFiles = Array.isArray(project.files) ? project.files.slice() : [];
  setCode(typeof project.code === 'string' ? project.code : '');
  const argsEl = $('#args');
  if (argsEl) argsEl.value = project.args || '';
  const stdinEl = $('#stdin');
  if (stdinEl) stdinEl.value = project.stdin || '';
  // Restore per-project IO files. Always clear first — old files must never
  // bleed through even when project.files is missing (legacy data without migration).
  if (__ioFilesRoot) {
    __ioFilesRoot.innerHTML = '';
    ensureErrorsPane();
  }
  __ioFiles.length = 0;
  _projectFiles.forEach(file => {
    const f = normalizeIoFile(file, __ioFiles.length);
    if (!f) return;
    __ioFiles.push(f);
    renderIoFilePane(f);
  });
  refreshIoSelectOptions();
  {
    const savedPane = getCookie(IO_PANE_COOKIE) || 'errors';
    const knownIds = new Set(['stdout', 'stdin', 'errors', ...__ioFiles.map(f => f.id)]);
    const targetPane = knownIds.has(savedPane) ? savedPane : 'errors';
    setActiveIoPane(targetPane, { persistCookie: false });
  }
  if (!silent) {
    saveState();
  }
  // Preview robot field if code uses robot and .fil file exists
  return tryPreviewRobotField(project.code, _projectFiles);
}

// Check if code likely uses robot (by looking for robot keywords)
function codeUsesRobot(code) {
  if (!code) return false;
  // Check for "использовать Робот" or robot function calls
  return /(использовать)\s+Робот/i.test(code) ||
         /\b(вверх|вниз|влево|вправо|закрасить|сверху_свободно|снизу_свободно|слева_свободно|справа_свободно|сверху_стена|снизу_стена|слева_стена|справа_стена|клетка_закрашена|клетка_чистая)\s*[\(\n]/i.test(code);
}

// Check if files contain a .fil file
function hasFilFile(files) {
  if (!Array.isArray(files)) return false;
  return files.some(f => f && typeof f.name === 'string' && f.name.toLowerCase().endsWith('.fil'));
}

function addRobotIoFile(file) {
  const newId = generateIoFileId();
  const f = { id: newId, name: file.name || 'robot.fil', content: file.content || '' };
  __ioFiles.push(f);
  renderIoFilePane(f);
  refreshIoSelectOptions();
  persistIoFiles();
  return f;
}

function updateRobotIoFile(fileId, content) {
  const file = __ioFiles.find(f => f.id === fileId);
  if (!file) return;
  file.content = content;
  if (file.elements && file.elements.editor && file.elements.editor.value !== content) {
    file.elements.editor.value = content;
  }
  persistIoFiles();
}

function setupRobotFilesAccessor() {
  if (!__robotModule || typeof __robotModule.__setRobotFilesAccessor !== 'function') return;
  __robotModule.__setRobotFilesAccessor(
    () => __ioFiles,
    addRobotIoFile,
    updateRobotIoFile
  );
}

// Try to preview robot field if applicable
async function tryPreviewRobotField(code, files) {
  if (!codeUsesRobot(code)) {
    // Hide robot UI if code doesn't use robot
    hideRobotUI();
    return;
  }

  // Ensure robot module is loaded
  if (!__robotModule) {
    try { __robotModule = await import('./runtime/robot.js'); } catch { return; }
  }

  // Setup canvas if needed
  ensureRobotUI();

  // Setup file accessor for robot module
  setupRobotFilesAccessor();

  // Preview field
  if (__robotModule && typeof __robotModule.__previewField === 'function') {
    __robotModule.__previewField();
  }

  // Show robot canvas
  setCompilerOutputMode('robot');
}

// Update robot field preview when .fil file is edited (if robot view is active)
function tryUpdateRobotFieldPreview() {
  // Only update if robot mode is active
  if (__compilerOutputMode !== 'robot') return;
  if (!__robotModule) return;

  setupRobotFilesAccessor();

  // Re-preview the field
  if (typeof __robotModule.__previewField === 'function') {
    __robotModule.__previewField();
  }
  renderRobotField();
}

function setActiveProject(projectId, { silent = false } = {}) {
  if (!projectId || projectId === __activeProjectId) return;
  requestStopRunningProgram();
  // console.log('[projects] setActiveProject start', {
  //   from: __activeProjectId,
  //   to: projectId
  // });
  // Before switching, persist current editor state and IO files into the
  // active project so that each project keeps its own files and stdin.
  updateActiveProjectFromInputs();
  const current = getActiveProject();
  if (current) {
    // console.log('[projects] setActiveProject saving current files', {
    //   id: current.id,
    //   name: current.name,
    //   filesCount: __ioFiles.length,
    //   stdin: current.stdin
    // });
    current.files = __ioFiles.map((file, idx) => normalizeIoFile(file, idx)).filter(Boolean);
    current.updatedAt = Date.now();
    persistProjects();
  }
  if (!__projects.some(p => p.id === projectId)) return;
  __activeProjectId = projectId;
  persistProjects();
  scheduleProjectsRender();
  const target = getActiveProject();
  if (target) {
    // console.log('[projects] setActiveProject applying target', {
    //   id: target.id,
    //   name: target.name,
    //   stdin: target.stdin
    // });
    applyProjectToInputs(target, { silent });
  }
}

function createProject(initial = {}, { activate = true } = {}) {
  if (activate && __activeProjectId) {
    requestStopRunningProgram();
  }
  const name = typeof initial.name === 'string' && initial.name.trim() ? initial.name.trim() : `Проект ${__projects.length + 1}`;
  const project = {
    id: generateProjectId(),
    name,
    code: typeof initial.code === 'string' ? initial.code : '',
    args: typeof initial.args === 'string' ? initial.args : '',
    stdin: typeof initial.stdin === 'string' ? initial.stdin : '',
    files: Array.isArray(initial.files) ? initial.files.map((f, i) => normalizeIoFile(f, i)).filter(Boolean) : [],
    updatedAt: Date.now()
  };
  __projects.push(project);
  if (activate || !__activeProjectId) {
    __activeProjectId = project.id;
  }
  persistProjects();
  scheduleProjectsRender();
  if (activate) {
    // silent: true — the new project already has its correct initial files;
    // saveState() would fire before __ioFiles is cleared and write the
    // previous project's files into the new one.
    applyProjectToInputs(project, { silent: true });
    closeProjectsDrawer();
  }
  return project;
}

function renameProject(projectId, nextName) {
  const project = __projects.find(p => p.id === projectId);
  if (!project) return;
  const trimmed = typeof nextName === 'string' ? nextName.trim() : '';
  if (!trimmed || trimmed === project.name) return;
  project.name = trimmed;
  project.updatedAt = Date.now();
  persistProjects();
  scheduleProjectsRender();
}

function deleteProject(projectId) {
  const idx = __projects.findIndex(p => p.id === projectId);
  if (idx === -1) return;
  __projects.splice(idx, 1);
  if (!__projects.length) {
    createProject({ name: 'Проект 1' }, { activate: true });
    return;
  }
  if (__activeProjectId === projectId) {
    requestStopRunningProgram();
    const fallback = __projects[idx] || __projects[idx - 1] || __projects[0];
    __activeProjectId = fallback.id;
    persistProjects();
    scheduleProjectsRender();
    applyProjectToInputs(fallback);
  } else {
    persistProjects();
    scheduleProjectsRender();
  }
}

function formatProjectTimestamp(ts) {
  if (!ts) return '';
  try {
    const date = new Date(Number(ts));
    if (Number.isNaN(date.getTime())) return '';
    const now = new Date();
    const sameDay = date.toDateString() === now.toDateString();
    if (sameDay) {
      return date.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
    }
    return date.toLocaleDateString('ru-RU', { day: '2-digit', month: 'short' });
  } catch {
    return '';
  }
}

function renderProjectsList() {
  if (!__projectsListEl) {
    if (__projectActiveNameEl) {
      const active = getActiveProject();
      __projectActiveNameEl.textContent = active ? active.name : 'Проект';
    }
    return;
  }
  const activeId = __activeProjectId;
  const fragment = document.createDocumentFragment();
  __projects.forEach(project => {
    const row = document.createElement('div');
    row.className = 'project-row';
    if (project.id === activeId) row.classList.add('active');
    row.dataset.projectId = project.id;

    const main = document.createElement('div');
    main.className = 'project-row-main';
    const nameEl = document.createElement('div');
    nameEl.className = 'project-name';
    nameEl.textContent = project.name || 'Без имени';
    const metaEl = document.createElement('div');
    metaEl.className = 'project-meta';
    const stamp = formatProjectTimestamp(project.updatedAt);
    metaEl.textContent = stamp ? `обновл. ${stamp}` : ' ';
    main.appendChild(nameEl);
    main.appendChild(metaEl);

    const actions = document.createElement('div');
    actions.className = 'project-actions';
    const renameBtn = document.createElement('button');
    renameBtn.type = 'button';
    renameBtn.className = 'project-action';
    renameBtn.dataset.action = 'rename';
    renameBtn.title = 'Переименовать';
    renameBtn.textContent = '✎';
    const deleteBtn = document.createElement('button');
    deleteBtn.type = 'button';
    deleteBtn.className = 'project-action danger';
    deleteBtn.dataset.action = 'delete';
    deleteBtn.title = 'Удалить';
    deleteBtn.textContent = '✕';
    actions.appendChild(renameBtn);
    actions.appendChild(deleteBtn);

    row.appendChild(main);
    row.appendChild(actions);
    fragment.appendChild(row);
  });
  __projectsListEl.replaceChildren(fragment);
  if (__projectActiveNameEl) {
    const active = getActiveProject();
    __projectActiveNameEl.textContent = active ? active.name : 'Проект';
  }
}

function scheduleProjectsRender() {
  if (__projectsRenderPending) return;
  __projectsRenderPending = true;
  const runner = typeof requestAnimationFrame === 'function' ? requestAnimationFrame : (cb) => setTimeout(cb, 0);
  runner(() => {
    __projectsRenderPending = false;
    renderProjectsList();
  });
}

function deriveExampleProjectName(path) {
  if (!path) return 'Пример';
  const parts = path.split('/');
  const last = parts[parts.length - 1] || path;
  const trimmed = last.replace(/\.[^.]+$/, '');
  return trimmed || last || 'Пример';
}

function openProjectsDrawer() {
  document.body.classList.add('projects-open');
  if (__projectToggleBtn) __projectToggleBtn.setAttribute('aria-expanded', 'true');
}

function closeProjectsDrawer() {
  document.body.classList.remove('projects-open');
  if (__projectToggleBtn) __projectToggleBtn.setAttribute('aria-expanded', 'false');
}

function toggleProjectsDrawer() {
  if (document.body.classList.contains('projects-open')) {
    closeProjectsDrawer();
  } else {
    openProjectsDrawer();
  }
}

function generateIoFileId() {
  __ioFileCounter += 1;
  return `file-${Date.now().toString(36)}-${__ioFileCounter}`;
}

function normalizeIoFile(entry, idx) {
  if (!entry || typeof entry !== 'object') return null;
  const id = typeof entry.id === 'string' && entry.id.trim() ? entry.id.trim() : generateIoFileId();
  const defaultName = `file${idx + 1}`;
  const name = typeof entry.name === 'string' && entry.name.trim() ? entry.name.trim() : defaultName;
  const content = typeof entry.content === 'string' ? entry.content : '';
  return { id, name, content };
}

function canonicalIoFileName(name) {
  return (name || '').trim();
}

function createTextTokenStream(text) {
  const raw = String(text || '');
  const WHITESPACE = new Set([9, 10, 11, 12, 13, 32, 160]);
  let cursor = 0;

  const isWhitespace = (code) => WHITESPACE.has(code);

  const skipWhitespace = () => {
    while (cursor < raw.length && isWhitespace(raw.charCodeAt(cursor))) {
      cursor++;
    }
  };

  return {
    readToken() {
      skipWhitespace();
      if (cursor >= raw.length) return '0';
      const start = cursor;
      while (cursor < raw.length && !isWhitespace(raw.charCodeAt(cursor))) {
        cursor++;
      }
      const token = raw.slice(start, cursor);
      return token.length ? token : '0';
    },
    readLine() {
      if (cursor >= raw.length) return '';
      const newlineIndex = raw.indexOf('\n', cursor);
      let line;
      if (newlineIndex === -1) {
        line = raw.slice(cursor);
        cursor = raw.length;
      } else {
        line = raw.slice(cursor, newlineIndex);
        cursor = newlineIndex + 1;
      }
      if (line.endsWith('\r')) {
        line = line.slice(0, -1);
      }
      return line;
    },
    hasMore() {
      return cursor < raw.length;
    },
    reset() {
      cursor = 0;
    }
  };
}

function createBrowserFileManager(filesAccessor, { addFile, updateFile } = {}) {
  const handles = new Map();
  const freeHandles = [];
  let nextHandle = 1;

  const getFiles = () => (typeof filesAccessor === 'function' ? filesAccessor() : []);

  return {
    open(name) {
      const targetName = canonicalIoFileName(name);
      if (!targetName) return -1;
      const files = getFiles();
      const file = files.find(f => canonicalIoFileName(f.name) === targetName);
      if (!file) return -1;
      const handle = freeHandles.length ? freeHandles.pop() : nextHandle++;
      handles.set(handle, {
        name: targetName,
        stream: createTextTokenStream(file.content || ''),
        mode: 'read',
        fileId: file.id
      });
      return handle;
    },
    openForWrite(name) {
      const targetName = canonicalIoFileName(name);
      if (!targetName) return -1;
      const files = getFiles();
      let file = files.find(f => canonicalIoFileName(f.name) === targetName);
      if (file) {
        // Clear existing file
        file.content = '';
        if (typeof updateFile === 'function') updateFile(file.id, '');
      } else {
        // Create new file
        const newId = generateIoFileId();
        file = { id: newId, name: targetName, content: '' };
        if (typeof addFile === 'function') addFile(file);
      }
      const handle = freeHandles.length ? freeHandles.pop() : nextHandle++;
      handles.set(handle, {
        name: targetName,
        mode: 'write',
        fileId: file.id
      });
      return handle;
    },
    openForAppend(name) {
      const targetName = canonicalIoFileName(name);
      if (!targetName) return -1;
      const files = getFiles();
      let file = files.find(f => canonicalIoFileName(f.name) === targetName);
      if (!file) {
        // Create new file if doesn't exist
        const newId = generateIoFileId();
        file = { id: newId, name: targetName, content: '' };
        if (typeof addFile === 'function') addFile(file);
      }
      // Do NOT clear existing content
      const handle = freeHandles.length ? freeHandles.pop() : nextHandle++;
      handles.set(handle, {
        name: targetName,
        mode: 'write',  // same mode as write, just don't clear
        fileId: file.id
      });
      return handle;
    },
    write(handle, text) {
      const h = Number(handle) | 0;
      const slot = handles.get(h);
      if (!slot || slot.mode !== 'write') return;
      const files = getFiles();
      const file = files.find(f => f.id === slot.fileId);
      if (file) {
        file.content = (file.content || '') + String(text);
        if (typeof updateFile === 'function') updateFile(file.id, file.content);
      }
    },
    close(handle) {
      const h = Number(handle) | 0;
      if (handles.delete(h)) {
        freeHandles.push(h);
      }
    },
    hasMore(handle) {
      const slot = handles.get(Number(handle) | 0);
      if (!slot || !slot.stream || typeof slot.stream.hasMore !== 'function') return false;
      return slot.stream.hasMore();
    },
    getStream(handle) {
      const slot = handles.get(Number(handle) | 0);
      return slot ? slot.stream : null;
    },
    reset() {
      handles.clear();
      freeHandles.length = 0;
      nextHandle = 1;
    }
  };
}

function ensureRuntimeFileManager(ioRuntime) {
  if (!ioRuntime || typeof ioRuntime.setFileManager !== 'function') {
    return;
  }
  if (!__browserFileManager) {
    __browserFileManager = createBrowserFileManager(() => __ioFiles, {
      addFile(file) {
        __ioFiles.push(file);
        // Create DOM pane for new file
        renderIoFilePane(file);
        refreshIoSelectOptions();
        persistIoFiles();
        // Show the new file pane
        setActiveIoPane(file.id);
      },
      updateFile(fileId, content) {
        const file = __ioFiles.find(f => f.id === fileId);
        if (file) {
          file.content = content;
          if (file.elements && file.elements.editor) {
            file.elements.editor.value = content;
          }
          // Do not call persistIoFiles() here: fires on every WASM write()
          // call and would save all __ioFiles (including stale files from a
          // previous project) into the current project. Content is already
          // updated in memory and will be saved when switching projects.
        }
      }
    });
  }
  ioRuntime.setFileManager(__browserFileManager);
}

function refreshIoSelectOptions() {
  if (!__ioSelectEl && !__ioTabsEl) return;
  const fragment = document.createDocumentFragment();
  const addOption = (value, label) => {
    const opt = document.createElement('option');
    opt.value = value;
    opt.textContent = label;
    fragment.appendChild(opt);
  };
  addOption('stdout', 'Вывод');
  addOption('stdin', 'Ввод');
  addOption('errors', __errorsHasBadge ? '⚠ Ошибки' : 'Ошибки');
  __ioFiles.forEach(file => {
    const label = file.name && file.name.trim() ? file.name.trim() : 'untitled';
    addOption(file.id, label);
  });
  const knownIds = new Set(['stdout', 'stdin', 'errors', ...__ioFiles.map(f => f.id)]);
  const target = knownIds.has(__currentIoPane) ? __currentIoPane : 'errors';
  if (__ioSelectEl) {
    __ioSelectEl.replaceChildren(fragment);
    __ioSelectEl.value = target;
  }
  refreshIoTabs(target);
}

function refreshIoTabs(activeId = __currentIoPane) {
  if (!__ioTabsEl) return;
  const fragment = document.createDocumentFragment();
  const tabs = [
    { id: 'stdout', label: 'Вывод' },
    { id: 'stdin', label: 'Ввод' },
    { id: 'errors', label: 'Ошибки', hasBadge: __errorsHasBadge },
    ...__ioFiles.map(file => ({
      id: file.id,
      label: file.name && file.name.trim() ? file.name.trim() : 'untitled',
      fileTab: true,
    })),
  ];
  const knownIds = new Set(tabs.map(tab => tab.id));
  const target = knownIds.has(activeId) ? activeId : 'errors';

  tabs.forEach(tab => {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'io-tab';
    btn.dataset.ioTab = tab.id;
    if (tab.fileTab) {
      btn.draggable = true;
      btn.dataset.ioFileTab = 'true';
      btn.setAttribute('aria-grabbed', 'false');
      btn.setAttribute('data-tooltip', 'Перетащите, чтобы изменить порядок файлов');
    }
    btn.setAttribute('role', 'tab');
    btn.setAttribute('aria-selected', tab.id === target ? 'true' : 'false');
    btn.tabIndex = tab.id === target ? 0 : -1;
    btn.textContent = tab.label;
    if (tab.id === target) btn.classList.add('active');
    if (tab.hasBadge) {
      const badge = document.createElement('span');
      badge.id = 'errors-badge';
      badge.className = 'io-tab-badge';
      badge.setAttribute('aria-label', 'Есть ошибки');
      badge.textContent = '!';
      btn.appendChild(badge);
    }
    fragment.appendChild(btn);
  });

  __ioTabsEl.replaceChildren(fragment);
}

function setErrorsBadge(show) {
  __errorsHasBadge = show;
  if (__ioSelectEl) {
    const opt = __ioSelectEl.querySelector('option[value="errors"]');
    if (opt) opt.textContent = show ? '⚠ Ошибки' : 'Ошибки';
  }
  refreshIoTabs();
}

function setActiveIoPane(candidate, { persistCookie = true } = {}) {
  const knownIds = new Set(['stdout', 'stdin', 'errors', ...__ioFiles.map(f => f.id)]);
  const target = knownIds.has(candidate) ? candidate : 'errors';
  __currentIoPane = target;
  if (target === 'errors') setErrorsBadge(false);
  if (__ioSelectEl && __ioSelectEl.value !== target) {
    __ioSelectEl.value = target;
  }
  if (__ioTabsEl) {
    __ioTabsEl.querySelectorAll('.io-tab').forEach(tab => {
      const active = tab.dataset.ioTab === target;
      tab.classList.toggle('active', active);
      tab.setAttribute('aria-selected', active ? 'true' : 'false');
      tab.tabIndex = active ? 0 : -1;
    });
  }
  document.querySelectorAll('.io-pane').forEach(node => {
    node.classList.toggle('active', node.dataset.ioPane === target);
  });
  if (persistCookie) {
    setCookie(IO_PANE_COOKIE, target);
  }
}

function renderIoFilePane(file) {
  if (!__ioFilesRoot) return;
  const pane = document.createElement('div');
  pane.className = 'io-pane io-file-pane';
  pane.dataset.ioPane = file.id;

  const meta = document.createElement('div');
  meta.className = 'io-file-meta';

  const nameInput = document.createElement('input');
  nameInput.type = 'text';
  nameInput.className = 'io-file-name';
  nameInput.placeholder = 'Имя файла';
  nameInput.value = file.name;
  nameInput.setAttribute('aria-label', 'Имя файла');

  const removeBtn = document.createElement('button');
  removeBtn.type = 'button';
  removeBtn.className = 'io-icon danger';
  removeBtn.title = 'Удалить файл';
  removeBtn.textContent = '×';
  removeBtn.setAttribute('aria-label', 'Удалить файл');

  meta.appendChild(nameInput);
  meta.appendChild(removeBtn);

  const editor = document.createElement('textarea');
  editor.className = 'io-file-text';
  editor.placeholder = 'Содержимое файла';
  editor.spellcheck = false;
  editor.value = file.content;

  pane.appendChild(meta);
  pane.appendChild(editor);
  __ioFilesRoot.appendChild(pane);

  file.elements = { pane, nameInput, editor };

  const commitName = (value) => {
    const wasFil = file.name && file.name.toLowerCase().endsWith('.fil');
    file.name = value;
    refreshIoSelectOptions();
    persistIoFiles();
    // Update robot preview if file became or stopped being .fil
    const isFil = file.name && file.name.toLowerCase().endsWith('.fil');
    if (isFil || wasFil) {
      tryUpdateRobotFieldPreview();
    }
  };

  nameInput.addEventListener('input', () => {
    commitName(nameInput.value);
  });

  nameInput.addEventListener('blur', () => {
    const trimmed = nameInput.value.trim();
    const finalValue = trimmed || 'untitled';
    if (finalValue !== file.name || finalValue !== nameInput.value) {
      nameInput.value = finalValue;
      commitName(finalValue);
    }
  });

  removeBtn.addEventListener('click', () => removeIoFile(file.id));

  editor.addEventListener('input', () => {
    file.content = editor.value;
    persistIoFiles();
    // If this is a .fil file and robot view is active, update the field preview
    if (file.name && file.name.toLowerCase().endsWith('.fil')) {
      tryUpdateRobotFieldPreview();
    }
  });
}

function removeIoFile(fileId) {
  const idx = __ioFiles.findIndex(f => f.id === fileId);
  if (idx === -1) return;
  const [file] = __ioFiles.splice(idx, 1);
  if (file && file.elements && file.elements.pane && file.elements.pane.parentNode) {
    file.elements.pane.parentNode.removeChild(file.elements.pane);
  }
  if (__currentIoPane === fileId) {
    setActiveIoPane('stdout');
  }
  refreshIoSelectOptions();
  persistIoFiles();
  if (file && file.name && file.name.toLowerCase().endsWith('.fil')) {
    tryUpdateRobotFieldPreview();
  }
}

function setupIoTabDragAndDrop() {
  if (!__ioTabsEl) return;
  let draggedFileId = null;

  const clearDropState = () => {
    __ioTabsEl.classList.remove('io-tabs-dragging');
    __ioTabsEl.querySelectorAll('.io-tab').forEach(tab => {
      tab.classList.remove('dragging', 'drop-before', 'drop-after');
      if (tab.dataset.ioFileTab === 'true') tab.setAttribute('aria-grabbed', 'false');
    });
  };

  const getFileTab = (event) => {
    const tab = event.target.closest('.io-tab[data-io-file-tab="true"]');
    return tab && __ioTabsEl.contains(tab) ? tab : null;
  };

  const getDropTarget = (event) => {
    const tab = getFileTab(event);
    if (!tab || !draggedFileId || tab.dataset.ioTab === draggedFileId) return null;
    const rect = tab.getBoundingClientRect();
    const after = event.clientX > rect.left + rect.width / 2;
    return { tab, after };
  };

  const applyDropIndicator = (event) => {
    __ioTabsEl.querySelectorAll('.io-tab.drop-before, .io-tab.drop-after').forEach(tab => {
      tab.classList.remove('drop-before', 'drop-after');
    });
    const target = getDropTarget(event);
    if (!target) return null;
    target.tab.classList.add(target.after ? 'drop-after' : 'drop-before');
    return target;
  };

  const moveIoFile = (sourceId, targetId, after) => {
    const fromIndex = __ioFiles.findIndex(file => file.id === sourceId);
    const targetIndex = __ioFiles.findIndex(file => file.id === targetId);
    if (fromIndex === -1 || targetIndex === -1 || fromIndex === targetIndex) return;
    const [file] = __ioFiles.splice(fromIndex, 1);
    let insertIndex = __ioFiles.findIndex(item => item.id === targetId);
    if (insertIndex === -1) {
      __ioFiles.splice(fromIndex, 0, file);
      return;
    }
    if (after) insertIndex += 1;
    __ioFiles.splice(insertIndex, 0, file);
    refreshIoSelectOptions();
    setActiveIoPane(sourceId, { persistCookie: false });
    persistIoFiles();
  };

  __ioTabsEl.addEventListener('dragstart', (event) => {
    const tab = getFileTab(event);
    if (!tab) return;
    draggedFileId = tab.dataset.ioTab;
    tab.classList.add('dragging');
    tab.setAttribute('aria-grabbed', 'true');
    __ioTabsEl.classList.add('io-tabs-dragging');
    if (event.dataTransfer) {
      event.dataTransfer.effectAllowed = 'move';
      event.dataTransfer.setData('text/plain', draggedFileId);
    }
  });

  __ioTabsEl.addEventListener('dragover', (event) => {
    if (!draggedFileId) return;
    const target = applyDropIndicator(event);
    if (!target) return;
    event.preventDefault();
    if (event.dataTransfer) event.dataTransfer.dropEffect = 'move';
  });

  __ioTabsEl.addEventListener('dragleave', (event) => {
    if (!draggedFileId || __ioTabsEl.contains(event.relatedTarget)) return;
    clearDropState();
  });

  __ioTabsEl.addEventListener('drop', (event) => {
    if (!draggedFileId) return;
    const target = applyDropIndicator(event);
    event.preventDefault();
    if (target) moveIoFile(draggedFileId, target.tab.dataset.ioTab, target.after);
    draggedFileId = null;
    clearDropState();
  });

  __ioTabsEl.addEventListener('dragend', () => {
    draggedFileId = null;
    clearDropState();
  });
}

function initIoWorkspace() {
  __ioSelectEl = document.getElementById('io-select');
  __ioTabsEl = document.getElementById('io-tabs');
  __ioFilesRoot = document.getElementById('io-files');
  const addBtn = document.getElementById('io-add-file');
  if (!__ioFilesRoot || !addBtn) return;

  // IO files are restored from the active project in applyProjectToInputs().
  refreshIoSelectOptions();

  if (__ioSelectEl) {
    __ioSelectEl.addEventListener('change', () => setActiveIoPane(__ioSelectEl.value));
  }
  if (__ioTabsEl) {
    __ioTabsEl.addEventListener('click', (event) => {
      const tab = event.target.closest('.io-tab');
      if (!tab || !__ioTabsEl.contains(tab)) return;
      setActiveIoPane(tab.dataset.ioTab);
    });
    __ioTabsEl.addEventListener('keydown', (event) => {
      if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
      const tabs = Array.from(__ioTabsEl.querySelectorAll('.io-tab'));
      if (tabs.length === 0) return;
      const currentIndex = Math.max(0, tabs.findIndex(tab => tab.dataset.ioTab === __currentIoPane));
      let nextIndex = currentIndex;
      if (event.key === 'ArrowLeft') nextIndex = (currentIndex + tabs.length - 1) % tabs.length;
      if (event.key === 'ArrowRight') nextIndex = (currentIndex + 1) % tabs.length;
      if (event.key === 'Home') nextIndex = 0;
      if (event.key === 'End') nextIndex = tabs.length - 1;
      event.preventDefault();
      const next = tabs[nextIndex];
      setActiveIoPane(next.dataset.ioTab);
      next.focus();
    });
    setupIoTabDragAndDrop();
  }

  addBtn.addEventListener('click', () => {
    const newFile = {
      id: generateIoFileId(),
      name: `file${__ioFiles.length + 1}`,
      content: ''
    };
    __ioFiles.push(newFile);
    renderIoFilePane(newFile);
    refreshIoSelectOptions();
    setActiveIoPane(newFile.id);
    persistIoFiles();
    if (newFile.elements && newFile.elements.nameInput) {
      newFile.elements.nameInput.focus();
      newFile.elements.nameInput.select();
    }
  });

  // Create Errors pane BEFORE setting active pane
  ensureErrorsPane();

  setActiveIoPane(__currentIoPane, { persistCookie: false });
}

// Ensure a dedicated read-only Errors pane exists under IO
function ensureErrorsPane() {
  // Try to get ioFilesRoot if not set
  if (!__ioFilesRoot) {
    __ioFilesRoot = document.getElementById('io-files');
  }
  if (!__ioFilesRoot) return;
  let pane = document.querySelector('.io-pane.errors-pane');
  if (pane) return;
  pane = document.createElement('div');
  pane.className = 'io-pane errors-pane';
  pane.dataset.ioPane = 'errors';
  if (__currentIoPane === 'errors') {
    pane.classList.add('active');
  }
  const viewer = document.createElement('pre');
  viewer.id = 'errors';
  viewer.className = 'io-file-text';
  viewer.style.whiteSpace = 'pre-wrap';
  viewer.style.userSelect = 'text';
  viewer.setAttribute('aria-label', 'Ошибки');
  pane.appendChild(viewer);
  __ioFilesRoot.appendChild(pane);
}

function setErrorsPaneContent(text, { isError = false } = {}) {
  ensureErrorsPane();
  const viewer = document.getElementById('errors');
  if (viewer) {
    viewer.textContent = text || '';
  }
  if (embedded) {
    const result = document.getElementById('example-result');
    if (result) {
      result.textContent = text || '';
      result.classList.toggle('error', isError);
    }
  }
  if (isError && __currentIoPane !== 'errors') {
    setErrorsBadge(true);
  }
}

function getPreviewBody() {
  return document.getElementById('preview-body');
}

function initProjectsUI() {
  __projectToggleBtn = document.getElementById('project-toggle');
  __projectActiveNameEl = document.getElementById('project-active-name');
  __projectsDrawer = document.getElementById('projects-drawer');
  __projectsBackdrop = document.getElementById('projects-backdrop');
  __projectsListEl = document.getElementById('projects-list');
  __projectNewBtn = document.getElementById('project-new');
  const projectCloseBtn = document.getElementById('project-close');

  if (__projectToggleBtn) {
    __projectToggleBtn.addEventListener('click', toggleProjectsDrawer);
  }
  if (__projectsBackdrop) {
    __projectsBackdrop.addEventListener('click', closeProjectsDrawer);
  }
  if (projectCloseBtn) {
    projectCloseBtn.addEventListener('click', closeProjectsDrawer);
  }
  if (__projectNewBtn) {
    __projectNewBtn.addEventListener('click', () => {
      updateActiveProjectFromInputs();
      createProject({ name: `Проект ${__projects.length + 1}` }, { activate: true });
    });
  }
  if (__projectsListEl) {
    __projectsListEl.addEventListener('click', (event) => {
      const row = event.target.closest('.project-row');
      if (!row) return;
      const projectId = row.dataset.projectId;
      if (!projectId) return;
      const actionBtn = event.target.closest('button[data-action]');
      const project = __projects.find(p => p.id === projectId);
      if (actionBtn) {
        const action = actionBtn.dataset.action;
        if (action === 'rename') {
          const suggested = project && project.name ? project.name : '';
          const nextName = typeof window !== 'undefined' ? window.prompt('Имя проекта', suggested) : null;
          if (nextName !== null) renameProject(projectId, nextName);
        } else if (action === 'delete') {
          const label = project && project.name ? `"${project.name}"` : '';
          const question = label ? `Удалить проект ${label}?` : 'Удалить проект?';
          const approved = typeof window === 'undefined' ? true : window.confirm(question);
          if (approved) deleteProject(projectId);
        }
        return;
      }
      const switching = projectId !== __activeProjectId;
      if (switching) {
        setActiveProject(projectId);
        if (!editor) debounceShow();
      }
      closeProjectsDrawer();
    });
  }
  if (typeof window !== 'undefined') {
    window.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && document.body.classList.contains('projects-open')) {
        closeProjectsDrawer();
      }
    });
  }
  renderProjectsList();
}

function getCurrentIoPaneNode() {
  if (__currentIoPane === 'stdout') return document.getElementById('stdout');
  if (__currentIoPane === 'stdin') return document.getElementById('stdin');
  if (__currentIoPane === 'errors') return document.getElementById('errors');
  const file = __ioFiles.find(f => f.id === __currentIoPane);
  return file && file.elements ? file.elements.editor : null;
}

function getCurrentIoPaneLabel() {
  if (__currentIoPane === 'stdout') return 'Вывод';
  if (__currentIoPane === 'stdin') return 'Ввод';
  if (__currentIoPane === 'errors') return 'Ошибки';
  const file = __ioFiles.find(f => f.id === __currentIoPane);
  if (!file) return 'файл';
  return file.name && file.name.trim() ? file.name.trim() : 'файл';
}

function hexdump(bytes) {
  let out = '';
  for (let i = 0; i < bytes.length; i += 16) {
    const chunk = bytes.slice(i, i + 16);
    const hex = Array.from(chunk).map(b => b.toString(16).padStart(2, '0')).join(' ');
    const ascii = Array.from(chunk).map(b => (b >= 32 && b < 127) ? String.fromCharCode(b) : '.').join('');
    out += i.toString(16).padStart(8, '0') + '  ' + hex.padEnd(16*3-1, ' ') + '  ' + ascii + '\n';
  }
  return out;
}

// Helper: collect multiline error text until next error header
function collectErrorText(lines, startIdx, initialText = '') {
  let text = initialText;
  let i = startIdx;
  while (i < lines.length) {
    const nextLine = lines[i];
    // Stop if we hit another error line
    if (/^Error:\s*/.test(nextLine) || /^Строка:\s*\d+/.test(nextLine)) {
      break;
    }
    // Append this line to the error text
    text += (text ? '\n' : '') + nextLine;
    i++;
  }
  return { text, nextIdx: i };
}

// Format compiler error lines like:
// "Error: <text> @ Line: N, Byte: B, Column: C"
// into:
// "Строка: N, Колонка: C\n   <spaces>Ошибка"
function formatCompilerErrors(payload) {
  if (typeof payload !== 'string') return payload;
  const lines = payload.split(/\r?\n/);
  const blocks = [];

  for (let i = 0; i < lines.length; i++) {
    // Check if line starts with "Error:"
    if (!/^Error:\s*/.test(lines[i])) continue;

    const firstLine = lines[i];
    // Check if coordinates are on the same line (single-line error)
    const inlineCoord = /^Error:\s*(.+?)\s*@\s*Line:\s*(\d+),\s*Byte:\s*\d+,\s*Column:\s*(\d+)/.exec(firstLine);

    if (inlineCoord) {
      // Single-line error format
      const errorText = inlineCoord[1].trim();
      const lineNum = Number(inlineCoord[2]) || 0;
      const colNum = Number(inlineCoord[3]) || 0;
      blocks.push(`Строка: ${lineNum}, Колонка: ${colNum}\n  ${errorText}`);
      continue;
    }

    // Multi-line error: collect all lines until we find coordinates
    let errorText = firstLine.replace(/^Error:\s*/, '').trim();
    let lineNum = 0, colNum = 0;
    let j = i + 1;

    while (j < lines.length) {
      const nextLine = lines[j];
      // Check if this line contains coordinates
      const coordMatch = /\s*@\s*Line:\s*(\d+),\s*Byte:\s*\d+,\s*Column:\s*(\d+)/.exec(nextLine);
      if (coordMatch) {
        lineNum = Number(coordMatch[1]) || 0;
        colNum = Number(coordMatch[2]) || 0;
        // Extract text before @ if any
        const textBefore = nextLine.substring(0, coordMatch.index).trim();
        if (textBefore) {
          errorText += '\n' + textBefore;
        }
        j++;
        break;
      }
      // Stop if we hit another error line
      if (/^Error:\s*/.test(nextLine) || /^Строка:\s*\d+/.test(nextLine)) {
        break;
      }
      // Append continuation line
      errorText += '\n' + nextLine.trim();
      j++;
    }

    i = j - 1; // -1 because loop will increment
    blocks.push(`Строка: ${lineNum}, Колонка: ${colNum}\n  ${errorText}`);
  }

  // If we detected any error lines, return the formatted blocks joined by newlines;
  // otherwise, keep the original payload.
  return blocks.length ? blocks.join('\n') : payload;
}

// Parse compiler errors into { line, col, text }
function parseCompilerErrors(payload) {
  if (typeof payload !== 'string') return [];
  const lines = payload.split(/\r?\n/);
  const errs = [];

  for (let i = 0; i < lines.length; i++) {
    // Check if line starts with "Error:"
    if (!/^Error:\s*/.test(lines[i])) {
      // Also check for already formatted errors
      const head = /^Строка:\s*(\d+)\s*,\s*Колонка:\s*(\d+)/.exec(lines[i]);
      if (head && i + 1 < lines.length) {
        const bodyLine = lines[i + 1] || '';
        const text = bodyLine.replace(/^\s+/, '');
        errs.push({ line: Number(head[1]) || 0, col: Number(head[2]) || 0, text });
        i++; // Skip the body line
      }
      continue;
    }

    const firstLine = lines[i];
    // Check if coordinates are on the same line (single-line error)
    const inlineCoord = /^Error:\s*(.+?)\s*@\s*Line:\s*(\d+),\s*Byte:\s*\d+,\s*Column:\s*(\d+)/.exec(firstLine);

    if (inlineCoord) {
      // Single-line error format
      const errorText = inlineCoord[1].trim();
      const lineNum = Number(inlineCoord[2]) || 0;
      const colNum = Number(inlineCoord[3]) || 0;
      errs.push({ line: lineNum, col: colNum, text: errorText });
      continue;
    }

    // Multi-line error: collect all lines until we find coordinates
    let errorText = firstLine.replace(/^Error:\s*/, '').trim();
    let lineNum = 0, colNum = 0;
    let j = i + 1;

    while (j < lines.length) {
      const nextLine = lines[j];
      // Check if this line contains coordinates
      const coordMatch = /\s*@\s*Line:\s*(\d+),\s*Byte:\s*\d+,\s*Column:\s*(\d+)/.exec(nextLine);
      if (coordMatch) {
        lineNum = Number(coordMatch[1]) || 0;
        colNum = Number(coordMatch[2]) || 0;
        // Extract text before @ if any
        const textBefore = nextLine.substring(0, coordMatch.index).trim();
        if (textBefore) {
          errorText += '\n' + textBefore;
        }
        j++;
        break;
      }
      // Stop if we hit another error line
      if (/^Error:\s*/.test(nextLine) || /^Строка:\s*\d+/.test(nextLine)) {
        break;
      }
      // Append continuation line
      errorText += '\n' + nextLine.trim();
      j++;
    }

    i = j - 1;
    errs.push({ line: lineNum, col: colNum, text: errorText });
  }

  return errs;
}let __errorMarks = [];
function clearErrorHighlights() {
  if (editor && __errorMarks && __errorMarks.length) {
    for (const mk of __errorMarks) {
      try { mk.clear(); } catch (_) {}
    }
  }
  __errorMarks = [];
  // Clear line classes
  if (editor && typeof editor.eachLine === 'function') {
    try {
      editor.eachLine((h) => { try { editor.removeLineClass(h, 'background', 'q-error-line'); } catch (_) {} });
    } catch (_) {}
  }
  // Clear gutter markers
  if (editor && typeof editor.clearGutter === 'function') {
    try { editor.clearGutter('q-error-gutter'); } catch (_) {}
  }
  setErrorsBadge(false);
  if (window.__hintRemovePopups) {
    try { window.__hintRemovePopups(); } catch (_) {}
  }
}

function addErrorHighlights(errors) {
  if (!Array.isArray(errors) || !errors.length) return;
  if (!editor || typeof editor.getDoc !== 'function') return;
  const doc = editor.getDoc();
  clearErrorHighlights();
  ensureErrorGutter();

  // Collect error lines first
  const errorLines = new Set();
  for (const err of errors) {
    const lineIdx = Math.max(0, (err.line || 1) - 1);
    errorLines.add(lineIdx);
  }

  // Clear hints only on lines with errors
  try {
    const marks = doc.getAllMarks();
    for (const mark of marks) {
      if (mark.className && mark.className.includes('q-hint-mark')) {
        const pos = mark.find();
        if (pos && pos.from && errorLines.has(pos.from.line)) {
          mark.clear();
        }
      }
    }
  } catch (_) {}

  for (const err of errors) {
    const lineIdx = Math.max(0, (err.line || 1) - 1);
    const chIdx = Math.max(0, (err.col || 1) - 1);
    const from = { line: lineIdx, ch: chIdx };
    const lineText = doc.getLine(lineIdx) || '';
    let endCh = chIdx;
    while (endCh < lineText.length && lineText[endCh] === ' ') endCh++;
    while (endCh < lineText.length && /[^\s\t\n\r]/.test(lineText[endCh])) endCh++;
    if (endCh === chIdx) endCh = Math.min(lineText.length, chIdx + 1);
    const to = { line: lineIdx, ch: endCh };
    const mark = doc.markText(from, to, {
      className: 'q-error-mark',
      // Use data-error only to avoid native title tooltip duplication
      attributes: { 'data-error': err.text || 'Ошибка' }
    });
    __errorMarks.push(mark);
    try { editor.addLineClass(lineIdx, 'background', 'q-error-line'); } catch (_) {}

    // Add gutter marker (red dot + hint bulb container) on the line
    try {
      // Avoid native title to prevent double tooltip; fast tooltip reads data-error
      const container = document.createElement('div');
      container.className = 'q-hint-container';
      container.setAttribute('data-error', err.text || 'Ошибка');
      container.setAttribute('data-line', err.line || 1);
      container.setAttribute('data-col', err.col || 1);

      const dot = document.createElement('div');
      dot.className = 'q-hint-dot';
      container.appendChild(dot);

      if (window.__hintCreateBulb) {
        try {
          const bulb = window.__hintCreateBulb(err);
          if (bulb) container.appendChild(bulb);
        } catch (_) {}
      }

      editor.setGutterMarker(lineIdx, 'q-error-gutter', container);
    } catch (_) {}
  }
}

async function show(mode, { clearErrorsOnSuccess = true } = {}) {
  if (embedded) {
    return;
  }
  const code = getCode();
  const O = $('#opt').value;
  const map = {
    ast: ['/api/compile-ast', false],
    tast: ['/api/compile-transformed-ast', false],
    ir: ['/api/compile-ir', false],
    llvm: ['/api/compile-llvm', false],
    asm: ['/api/compile-asm', false],
    wasm: ['/api/compile-wasm-text', false],
  };
  const [endpoint, bin] = map[mode] || map.ir;
  if (currentAbort) currentAbort.abort();
  currentAbort = new AbortController();
  const { signal } = currentAbort;
  try {
    $('#output').classList.remove('error');
    const data = await api(endpoint, { code, O }, bin, signal);
    if (bin) {
      $('#output').textContent = hexdump(data);
      // Don't clear runtime errors (they were set by runWasm)
      if (!window.__hasRuntimeErrors) {
        clearErrorHighlights();
      }
      // Don't set errors pane here - will be set in runWasm after execution
      if (window.__runHintOnCompilationResult) window.__runHintOnCompilationResult(false);
    } else {
      const formatted = formatCompilerErrors(data);
      $('#output').textContent = formatted;
      const errs = parseCompilerErrors(data);
      if (errs.length) {
        addErrorHighlights(errs);
        // Show compilation errors
        setErrorsPaneContent(formatted, { isError: true });
      } else {
        // Don't clear runtime errors
        if (!window.__hasRuntimeErrors) {
          clearErrorHighlights();
        }
        // Clear errors pane on successful compilation only if explicitly requested;
        // while a program runs the pane belongs to that run.
        if (clearErrorsOnSuccess && !__coroRunning) {
          setErrorsPaneContent('Успешно');
        }
      }
      if (window.__runHintOnCompilationResult) window.__runHintOnCompilationResult(errs.length > 0);
    }
  } catch (e) {
    if (e.name === 'AbortError') return;
    const msg = typeof e?.message === 'string' ? e.message : String(e);
    const formatted = formatCompilerErrors(msg);
    $('#output').textContent = formatted;
    const errs = parseCompilerErrors(msg);
    if (errs.length) addErrorHighlights(errs); else clearErrorHighlights();
    $('#output').classList.add('error');
    // Show compilation errors
    setErrorsPaneContent(formatted, { isError: true });
    if (window.__runHintOnCompilationResult) window.__runHintOnCompilationResult(true);
  }
}

// Ensure basic styles exist for error highlights and operator hints
(function ensureErrorStyles(){
  if (typeof document === 'undefined') return;
  const id = 'q-error-styles';
  if (document.getElementById(id)) return;
  const style = document.createElement('style');
  style.id = id;
  style.textContent = `
    .q-error-line { background: rgba(255, 0, 0, 0.08) !important; }
    .q-error-mark { background: rgba(255, 0, 0, 0.18); border-bottom: 1px solid rgba(255,0,0,0.6); }
    .CodeMirror-gutters { border-right: 1px solid #e0e0e0; }
    /* Make error gutter compact so dot sits close to line number */
    .CodeMirror-gutter.q-error-gutter { width: 10px; }
    .q-hint-container { display: flex; align-items: center; gap: 2px; margin-top: 2px; }
    .q-hint-dot { width: 8px; height: 8px; border-radius: 50%; background: #e53935; box-shadow: 0 0 0 1px rgba(0,0,0,0.1); flex-shrink: 0; }
    /* Operator hints - lighter styling */
    .q-hint-mark { border-bottom: 1px dashed rgba(100, 150, 250, 0.5); cursor: help; }
  `;
  document.head.appendChild(style);
})();

// Ensure a dedicated gutter for error markers exists on the editor
function ensureErrorGutter() {
  if (!editor || typeof editor.setOption !== 'function') return;
  const existing = editor.getOption && editor.getOption('gutters');
  const gutters = Array.isArray(existing) ? existing.slice() : [];
  // Ensure both gutters and order: error gutter first (left), then line numbers
  const hasErr = gutters.includes('q-error-gutter');
  const hasLine = gutters.includes('CodeMirror-linenumbers');
  const next = [];
  if (!hasErr) next.push('q-error-gutter'); else next.push('q-error-gutter');
  if (!hasLine) next.push('CodeMirror-linenumbers'); else next.push('CodeMirror-linenumbers');
  // Append any remaining existing gutters preserving order
  for (const g of gutters) {
    if (g !== 'q-error-gutter' && g !== 'CodeMirror-linenumbers') next.push(g);
  }
  editor.setOption('gutters', next);
}

// Fast, JS-driven tooltip over error marks (appears quicker than native title)
(function enableFastErrorTooltip(){
  if (typeof document === 'undefined') return;
  let tipEl = null;
  const showTip = (target, text) => {
    if (!text) return;
    if (!tipEl) {
      tipEl = document.createElement('div');
      tipEl.className = 'q-tooltip';
      tipEl.style.position = 'fixed';
      tipEl.style.zIndex = '9999';
      tipEl.style.background = '#222';
      tipEl.style.color = '#fff';
      tipEl.style.padding = '6px 8px';
      tipEl.style.borderRadius = '4px';
      tipEl.style.boxShadow = '0 2px 8px rgba(0,0,0,0.25)';
      tipEl.style.fontSize = '12px';
      tipEl.style.pointerEvents = 'none';
      document.body.appendChild(tipEl);
    }
    tipEl.textContent = text;
    tipEl.style.display = 'block';
  };
  const hideTip = () => { if (tipEl) tipEl.style.display = 'none'; };

  // Listen on editor wrapper for hover over error marks or dots
  const attach = () => {
    if (!editor || !editor.getWrapperElement) return;
    const wrap = editor.getWrapperElement();
    if (!wrap) return;
    let hoverTimer = null;
    const delay = 120; // faster than default title
    wrap.addEventListener('mousemove', (ev) => {
      let target = ev.target;

      // Hovering the SVG inside a bulb should resolve to the bulb itself
      if (target.tagName === 'svg' || target.tagName === 'path') {
        const bulb = target.closest('.q-hint-bulb');
        if (bulb) target = bulb;
      }

      const isMark = target.classList && target.classList.contains('q-error-mark');
      const isDot = target.classList && target.classList.contains('q-hint-dot');
      const isContainer = target.classList && target.classList.contains('q-hint-container');
      const isHint = target.classList && target.classList.contains('q-hint-mark');
      const isBulb = target.classList && target.classList.contains('q-hint-bulb');
      if (!(isMark || isDot || isContainer || isHint || isBulb)) { hideTip(); return; }

      let text = '';
      if (isBulb) {
        text = target.getAttribute('data-hint') || 'AI-подсказка';
      } else if (isDot && target.parentElement) {
        text = target.parentElement.getAttribute('data-error') || '';
      } else {
        text = target.getAttribute('data-error') || target.getAttribute('data-hint') || '';
      }
      clearTimeout(hoverTimer);
      hoverTimer = setTimeout(() => {
        showTip(target, text);
        const r = target.getBoundingClientRect();
        const pad = 8;
        const top = r.top - (tipEl ? tipEl.offsetHeight : 20) - pad;
        const left = Math.max(8, Math.min(window.innerWidth - (tipEl ? tipEl.offsetWidth : 100) - 8, r.left + r.width / 2 - ((tipEl ? tipEl.offsetWidth : 100) / 2)));
        if (tipEl) { tipEl.style.top = `${top}px`; tipEl.style.left = `${left}px`; }
      }, delay);
    });
    wrap.addEventListener('mouseleave', () => { hideTip(); });
  };
  // Defer attach slightly in case editor is initialized later
  const readyCheck = () => { if (editor) attach(); else setTimeout(readyCheck, 200); };
  readyCheck();
})();
// Helpers: turtle UI in the compiler output pane
function ensureTurtleUI() {
  showOutputPane();
  const out = document.getElementById('output');
  const preview = getPreviewBody();
  if (!out || !preview) return;

  // In non-dev mode, automatically switch to turtle view
  const isDevMode = document.body.classList.contains('dev-mode');
  if (!isDevMode) {
    setCompilerOutputMode('turtle');
  }

  // Preview no longer switches back to compiler text; keep toolbar hidden for turtle.
  if (!__turtleToggle) {
    const ctr = document.createElement('div');
    ctr.id = 'output-mode';
    ctr.className = 'output-mode';
    ctr.style.margin = '6px 0';
    ctr.style.display = 'flex';
    ctr.style.gap = '12px';
    preview.appendChild(ctr);
    __turtleToggle = ctr;
  }

  const updateTurtleDelay = () => {
    const animCheckbox = document.getElementById('turtle-anim-enabled');
    const speedSlider = document.getElementById('turtle-speed');
    if (!animCheckbox || !speedSlider) return;
    if (__turtleModule && typeof __turtleModule.__setAnimationDelay === 'function') {
      __turtleModule.__setAnimationDelay(animCheckbox.checked ? 300 - parseInt(speedSlider.value, 10) : 0);
    }
  };

  if (__executorToolbarMode !== 'turtle') {
    __turtleToggle.innerHTML = '';
    __executorToolbarMode = 'turtle';

    // Speed controls (animation checkbox + slider)
    const speedContainer = document.createElement('div');
    speedContainer.style.display = 'flex';
    speedContainer.style.alignItems = 'center';
    speedContainer.style.gap = '6px';

    const animCheckbox = document.createElement('input');
    animCheckbox.type = 'checkbox';
    animCheckbox.id = 'turtle-anim-enabled';
    animCheckbox.style.cursor = 'pointer';
    animCheckbox.title = 'Включить анимацию черепахи';
    const savedTurtleAnim = getCookie('q_turtle_anim');
    animCheckbox.checked = embedded || savedTurtleAnim === '1';

    const speedLabel = document.createElement('span');
    speedLabel.textContent = '🐢';
    speedLabel.style.fontSize = '14px';

    const speedSlider = document.createElement('input');
    speedSlider.type = 'range';
    speedSlider.id = 'turtle-speed';
    speedSlider.min = '0';
    speedSlider.max = '290';
    const savedTurtleSpeed = getCookie('q_turtle_speed');
    speedSlider.value = savedTurtleSpeed !== null ? savedTurtleSpeed : '150';
    speedSlider.style.width = '80px';
    speedSlider.style.cursor = 'pointer';
    speedSlider.title = 'Скорость анимации черепахи';
    speedSlider.disabled = !animCheckbox.checked;
    speedSlider.style.opacity = animCheckbox.checked ? '1' : '0.5';

    const speedLabelFast = document.createElement('span');
    speedLabelFast.textContent = '🐇';
    speedLabelFast.style.fontSize = '14px';

    animCheckbox.addEventListener('change', () => {
      speedSlider.disabled = !animCheckbox.checked;
      speedSlider.style.opacity = animCheckbox.checked ? '1' : '0.5';
      speedLabel.style.opacity = animCheckbox.checked ? '1' : '0.5';
      speedLabelFast.style.opacity = animCheckbox.checked ? '1' : '0.5';
      setCookie('q_turtle_anim', animCheckbox.checked ? '1' : '0', 365);
      updateTurtleDelay();
    });
    speedSlider.addEventListener('input', () => {
      updateTurtleDelay();
      setCookie('q_turtle_speed', speedSlider.value, 365);
    });

    speedLabel.style.opacity = animCheckbox.checked ? '1' : '0.5';
    speedLabelFast.style.opacity = animCheckbox.checked ? '1' : '0.5';

    speedContainer.appendChild(animCheckbox);
    speedContainer.appendChild(speedLabel);
    speedContainer.appendChild(speedSlider);
    speedContainer.appendChild(speedLabelFast);
    __turtleToggle.appendChild(speedContainer);
  }

  __turtleToggle.style.display = '';
  updateTurtleDelay();

  if (!__turtleCanvas) {
    const cnv = document.createElement('canvas');
    cnv.id = 'turtle-canvas';
    cnv.style.display = 'none';
    cnv.style.width = '100%';
    cnv.style.flex = '1';
    cnv.style.minHeight = '0';
    cnv.style.background = '#1b1b1b';
    cnv.style.border = '1px solid #2b2b2b44';
    cnv.style.borderRadius = '4px';
    preview.appendChild(cnv);
    __turtleCanvas = cnv;
  }
}

function hideTurtleUI() {
  __compilerOutputMode = 'text';
  setCookie('q_out_mode', 'text');
  if (__turtleToggle) __turtleToggle.style.display = 'none';
  const out = document.getElementById('output');
  if (out) out.style.display = '';
  if (__turtleCanvas) __turtleCanvas.style.display = 'none';
  hideOutputPane();
}

function showOutputPane() {
  const previewPane = document.querySelector('.pane.preview');
  if (previewPane) {
    previewPane.classList.add('executor-active');
  }
}

function hideOutputPane() {
  const previewPane = document.querySelector('.pane.preview');
  if (previewPane) {
    previewPane.classList.remove('executor-active');
  }
}

// Robot UI functions
function ensureRobotUI() {
  showOutputPane();
  const out = document.getElementById('output');
  const preview = getPreviewBody();
  if (!out || !preview) return;

  // In non-dev mode, automatically switch to robot view
  const isDevMode = document.body.classList.contains('dev-mode');
  if (!isDevMode) {
    setCompilerOutputMode('robot');
  }

  // Reuse turtle toggle if exists, just add robot option
  if (!__turtleToggle) {
    const ctr = document.createElement('div');
    ctr.id = 'output-mode';
    ctr.className = 'output-mode';
    ctr.style.margin = '6px 0';
    ctr.style.display = 'flex';
    ctr.style.gap = '12px';
    ctr.style.alignItems = 'center';
    ctr.style.flexWrap = 'wrap';
    preview.appendChild(ctr);
    __turtleToggle = ctr;
  }

  // Update delay based on checkbox and slider
  const updateAnimationDelay = () => {
    const animCheckbox = document.getElementById('robot-anim-enabled');
    const speedSlider = document.getElementById('robot-speed');
    if (!animCheckbox || !speedSlider) return;
    if (__robotModule && typeof __robotModule.__setAnimationDelay === 'function') {
      if (animCheckbox.checked) {
        __robotModule.__setAnimationDelay(300 - parseInt(speedSlider.value, 10));
      } else {
        __robotModule.__setAnimationDelay(0); // instant
      }
    }
  };

  if (__executorToolbarMode !== 'robot') {
    // Preview no longer switches back to compiler text; robot toolbar keeps animation controls only.
    __turtleToggle.innerHTML = '';
    __executorToolbarMode = 'robot';

    // Add animation speed control
    const speedContainer = document.createElement('div');
    speedContainer.style.display = 'flex';
    speedContainer.style.alignItems = 'center';
    speedContainer.style.gap = '6px';
    speedContainer.style.marginLeft = 'auto';

    // Checkbox to disable animation
    const animCheckbox = document.createElement('input');
    animCheckbox.type = 'checkbox';
    animCheckbox.id = 'robot-anim-enabled';
    const savedAnimEnabled = getCookie('q_robot_anim');
    animCheckbox.checked = savedAnimEnabled !== '0'; // enabled by default
    animCheckbox.style.cursor = 'pointer';
    animCheckbox.title = 'Включить анимацию';

    const speedLabel = document.createElement('span');
    speedLabel.textContent = '🐢';
    speedLabel.style.fontSize = '14px';

    const speedSlider = document.createElement('input');
    speedSlider.type = 'range';
    speedSlider.id = 'robot-speed';
    speedSlider.min = '0';
    speedSlider.max = '300';
    // Restore saved value or default to 150
    const savedSpeed = getCookie('q_robot_speed');
    speedSlider.value = savedSpeed !== null ? savedSpeed : '150';
    speedSlider.style.width = '80px';
    speedSlider.style.cursor = 'pointer';
    speedSlider.title = 'Скорость анимации';
    speedSlider.disabled = !animCheckbox.checked;
    speedSlider.style.opacity = animCheckbox.checked ? '1' : '0.5';

    const speedLabelFast = document.createElement('span');
    speedLabelFast.textContent = '🐇';
    speedLabelFast.style.fontSize = '14px';

    animCheckbox.addEventListener('change', () => {
      speedSlider.disabled = !animCheckbox.checked;
      speedSlider.style.opacity = animCheckbox.checked ? '1' : '0.5';
      speedLabel.style.opacity = animCheckbox.checked ? '1' : '0.5';
      speedLabelFast.style.opacity = animCheckbox.checked ? '1' : '0.5';
      setCookie('q_robot_anim', animCheckbox.checked ? '1' : '0', 365);
      updateAnimationDelay();
    });

    speedSlider.addEventListener('input', () => {
      updateAnimationDelay();
      setCookie('q_robot_speed', speedSlider.value, 365);
    });

    // Apply initial opacity
    speedLabel.style.opacity = animCheckbox.checked ? '1' : '0.5';
    speedLabelFast.style.opacity = animCheckbox.checked ? '1' : '0.5';

    speedContainer.appendChild(animCheckbox);
    speedContainer.appendChild(speedLabel);
    speedContainer.appendChild(speedSlider);
    speedContainer.appendChild(speedLabelFast);

    const editBtn = document.createElement('button');
    editBtn.type = 'button';
    editBtn.id = 'robot-edit-toggle';
    editBtn.className = 'robot-edit-toggle';
    editBtn.textContent = 'edit';
    editBtn.title = 'Редактировать поле робота';
    editBtn.setAttribute('aria-pressed', 'false');

    const editTools = document.createElement('select');
    editTools.id = 'robot-edit-tool';
    editTools.className = 'robot-edit-tool';
    editTools.title = 'Что редактировать';
    [
      ['wall', 'стена'],
      ['robot', 'робот'],
      ['color', 'цвет'],
      ['radiation', 'радиация'],
      ['temperature', 'темп.'],
      ['symbol', 'символ'],
      ['symbol1', 'символ1'],
      ['point', 'точка']
    ].forEach(([value, label]) => {
      const option = document.createElement('option');
      option.value = value;
      option.textContent = label;
      editTools.appendChild(option);
    });
    editTools.value = __robotEditTool;
    editTools.style.display = __robotEditEnabled ? '' : 'none';

    const syncEditState = () => {
      editBtn.classList.toggle('active', __robotEditEnabled);
      editBtn.setAttribute('aria-pressed', __robotEditEnabled ? 'true' : 'false');
      editTools.style.display = __robotEditEnabled ? '' : 'none';
      if (__robotCanvas) {
        __robotCanvas.classList.toggle('robot-editing', __robotEditEnabled);
      }
    };

    editBtn.addEventListener('click', () => {
      __robotEditEnabled = !__robotEditEnabled;
      setupRobotFilesAccessor();
      if (__robotEditEnabled && __robotModule && typeof __robotModule.__previewField === 'function') {
        __robotModule.__previewField();
      }
      if (__robotEditEnabled && __robotModule && typeof __robotModule.__writeFieldToFil === 'function') {
        __robotModule.__writeFieldToFil();
      }
      syncEditState();
      renderRobotField();
    });

    editTools.addEventListener('change', () => {
      __robotEditTool = editTools.value || 'wall';
    });

    speedContainer.appendChild(editBtn);
    speedContainer.appendChild(editTools);

    __turtleToggle.appendChild(speedContainer);
  }

  // Apply saved delay to module.
  updateAnimationDelay();
  __turtleToggle.style.display = '';

  if (!__robotCanvas) {
    const cnv = document.createElement('canvas');
    cnv.id = 'robot-canvas';
    cnv.style.display = 'none';
    cnv.style.width = '100%';
    cnv.style.flex = '1';
    cnv.style.minHeight = '0';
    cnv.style.background = '#1b1b1b';
    cnv.style.border = '1px solid #2b2b2b44';
    cnv.style.borderRadius = '4px';
    preview.appendChild(cnv);
    __robotCanvas = cnv;
    __robotCanvas.addEventListener('click', handleRobotCanvasEditClick);
    __robotCanvas.addEventListener('pointerdown', handleRobotCanvasPointerDown);
    __robotCanvas.addEventListener('pointermove', handleRobotCanvasPointerMove);
    __robotCanvas.addEventListener('pointerleave', handleRobotCanvasPointerLeave);
  }
  if (__robotCanvas) {
    __robotCanvas.classList.toggle('robot-editing', __robotEditEnabled);
  }
}

function hideRobotUI() {
  if (__robotCanvas) __robotCanvas.style.display = 'none';
  // Also hide toggle and restore text output
  if (__turtleToggle) __turtleToggle.style.display = 'none';
  const out = document.getElementById('output');
  if (out) out.style.display = '';
  hideOutputPane();
}

function getRobotEditHit(event) {
  if (!__robotCanvas || !__robotCanvas.__robotGrid) return null;
  const grid = __robotCanvas.__robotGrid;
  const rect = __robotCanvas.getBoundingClientRect();
  const px = event.clientX - rect.left;
  const py = event.clientY - rect.top;
  const localX = px - grid.offsetX;
  const localY = py - grid.offsetY;
  if (localX < 0 || localY < 0 || localX > grid.gridW || localY > grid.gridH) return null;

  const x = Math.min(grid.width - 1, Math.max(0, Math.floor(localX / grid.cellSize)));
  const y = Math.min(grid.height - 1, Math.max(0, Math.floor(localY / grid.cellSize)));
  const inCellX = localX - x * grid.cellSize;
  const inCellY = localY - y * grid.cellSize;
  const edge = Math.max(5, Math.min(12, grid.cellSize * 0.22));
  let side = null;
  const distances = [
    ['west', inCellX],
    ['east', grid.cellSize - inCellX],
    ['north', inCellY],
    ['south', grid.cellSize - inCellY]
  ].sort((a, b) => a[1] - b[1]);
  if (distances[0][1] <= edge) side = distances[0][0];
  return { x, y, side };
}

function getRobotResizeHit(event) {
  if (!__robotCanvas || !__robotCanvas.__robotGrid) return null;
  const grid = __robotCanvas.__robotGrid;
  const rect = __robotCanvas.getBoundingClientRect();
  const px = event.clientX - rect.left;
  const py = event.clientY - rect.top;
  const threshold = Math.max(7, Math.min(14, grid.cellSize * 0.25));
  const nearY = py >= grid.offsetY - threshold && py <= grid.offsetY + grid.gridH + threshold;
  const nearX = px >= grid.offsetX - threshold && px <= grid.offsetX + grid.gridW + threshold;
  const east = Math.abs(px - (grid.offsetX + grid.gridW)) <= threshold && nearY;
  const south = Math.abs(py - (grid.offsetY + grid.gridH)) <= threshold && nearX;
  if (!east && !south) return null;
  return { east, south };
}

function setRobotCanvasCursor(event) {
  if (!__robotCanvas) return;
  if (!__robotEditEnabled) {
    __robotCanvas.style.cursor = '';
    return;
  }
  const resizeHit = event ? getRobotResizeHit(event) : null;
  if (resizeHit && resizeHit.east && resizeHit.south) {
    __robotCanvas.style.cursor = 'nwse-resize';
  } else if (resizeHit && resizeHit.east) {
    __robotCanvas.style.cursor = 'ew-resize';
  } else if (resizeHit && resizeHit.south) {
    __robotCanvas.style.cursor = 'ns-resize';
  } else {
    __robotCanvas.style.cursor = 'crosshair';
  }
}

function handleRobotCanvasPointerDown(event) {
  if (!__robotEditEnabled || !__robotModule || typeof __robotModule.__resizeField !== 'function') return;
  const hit = getRobotResizeHit(event);
  if (!hit || !__robotCanvas.__robotGrid) return;
  event.preventDefault();
  __robotResizeDrag = {
    east: hit.east,
    south: hit.south,
    startX: event.clientX,
    startY: event.clientY,
    width: __robotModule.field.width,
    height: __robotModule.field.height,
    cellSize: __robotCanvas.__robotGrid.cellSize,
    pointerId: event.pointerId
  };
  __robotSuppressNextClick = true;
  __robotCanvas.setPointerCapture(event.pointerId);
  window.addEventListener('pointermove', handleRobotResizePointerMove);
  window.addEventListener('pointerup', handleRobotResizePointerUp);
}

function handleRobotCanvasPointerMove(event) {
  if (__robotResizeDrag) return;
  setRobotCanvasCursor(event);
}

function handleRobotCanvasPointerLeave() {
  if (!__robotResizeDrag) setRobotCanvasCursor(null);
}

function handleRobotResizePointerMove(event) {
  if (!__robotResizeDrag || !__robotModule || typeof __robotModule.__resizeField !== 'function') return;
  const dx = event.clientX - __robotResizeDrag.startX;
  const dy = event.clientY - __robotResizeDrag.startY;
  let width = __robotResizeDrag.width;
  let height = __robotResizeDrag.height;
  if (__robotResizeDrag.east) {
    width = Math.max(1, Math.min(200, __robotResizeDrag.width + Math.round(dx / __robotResizeDrag.cellSize)));
  }
  if (__robotResizeDrag.south) {
    height = Math.max(1, Math.min(200, __robotResizeDrag.height + Math.round(dy / __robotResizeDrag.cellSize)));
  }
  if (width === __robotModule.field.width && height === __robotModule.field.height) return;
  __robotSuppressNextClick = true;
  setupRobotFilesAccessor();
  __robotModule.__resizeField(width, height);
  renderRobotField();
}

function handleRobotResizePointerUp(event) {
  if (__robotResizeDrag && __robotCanvas && __robotResizeDrag.pointerId === event.pointerId) {
    try {
      __robotCanvas.releasePointerCapture(event.pointerId);
    } catch {}
  }
  __robotResizeDrag = null;
  window.removeEventListener('pointermove', handleRobotResizePointerMove);
  window.removeEventListener('pointerup', handleRobotResizePointerUp);
  setRobotCanvasCursor(event);
}

function promptRobotCellProperty(x, y, property) {
  if (!__robotModule || typeof __robotModule.__getCellProperties !== 'function') return null;
  const current = __robotModule.__getCellProperties(x, y);
  const labels = {
    color: 'Color',
    radiation: 'Radiation',
    temperature: 'Temperature',
    symbol: 'Symbol',
    symbol1: 'Symbol1',
    point: 'Point'
  };
  const input = window.prompt(labels[property] || property, String(current[property] ?? ''));
  if (input === null) return null;
  return {
    ...current,
    [property]: input.trim()
  };
}

function handleRobotCanvasEditClick(event) {
  if (__robotSuppressNextClick) {
    __robotSuppressNextClick = false;
    return;
  }
  if (!__robotEditEnabled || !__robotModule) return;
  const hit = getRobotEditHit(event);
  if (!hit) return;
  event.preventDefault();
  setupRobotFilesAccessor();

  if (__robotEditTool === 'wall') {
    if (!hit.side || typeof __robotModule.__toggleWall !== 'function') return;
    __robotModule.__toggleWall(hit.x, hit.y, hit.side);
  } else if (__robotEditTool === 'robot') {
    if (typeof __robotModule.__setRobotPosition === 'function') {
      __robotModule.__setRobotPosition(hit.x, hit.y);
    }
  } else if (['color', 'radiation', 'temperature', 'symbol', 'symbol1', 'point'].includes(__robotEditTool)) {
    if (typeof __robotModule.__setCellProperties !== 'function') return;
    const props = promptRobotCellProperty(hit.x, hit.y, __robotEditTool);
    if (!props) return;
    __robotModule.__setCellProperties(hit.x, hit.y, props);
  }

  renderRobotField();
}

// Drawer UI functions
function ensureDrawerUI() {
  showOutputPane();
  const out = document.getElementById('output');
  const preview = getPreviewBody();
  if (!out || !preview) return;

  // In non-dev mode, automatically switch to drawer view
  const isDevMode = document.body.classList.contains('dev-mode');
  if (!isDevMode) {
    setCompilerOutputMode('drawer');
  }

  // Preview no longer switches back to compiler text; keep toolbar hidden for drawer.
  if (!__turtleToggle) {
    const ctr = document.createElement('div');
    ctr.id = 'output-mode';
    ctr.className = 'output-mode';
    ctr.style.margin = '6px 0';
    ctr.style.display = 'flex';
    ctr.style.gap = '12px';
    preview.appendChild(ctr);
    __turtleToggle = ctr;
  }

  __turtleToggle.innerHTML = '';
  __executorToolbarMode = null;
  __turtleToggle.style.display = 'none';

  if (!__drawerCanvas) {
    const cnv = document.createElement('canvas');
    cnv.id = 'drawer-canvas';
    cnv.style.display = 'none';
    cnv.style.width = '100%';
    cnv.style.flex = '1';
    cnv.style.minHeight = '0';
    cnv.style.background = '#1b1b1b';
    cnv.style.border = '1px solid #2b2b2b44';
    cnv.style.borderRadius = '4px';
    preview.appendChild(cnv);
    __drawerCanvas = cnv;
  }
}

function hideDrawerUI() {
  if (__drawerCanvas) __drawerCanvas.style.display = 'none';
  // Also hide toggle and restore text output
  if (__turtleToggle) __turtleToggle.style.display = 'none';
  const out = document.getElementById('output');
  if (out) out.style.display = '';
  hideOutputPane();
}

// ── Painter ruler helpers ─────────────────────────────────────────────────────

const PAINTER_RULER_H = 18;  // height of X ruler (horizontal strip)
const PAINTER_RULER_W = 36;  // width of Y ruler (vertical strip, needs room for 4-digit labels)

function painterNiceStep(x) {
  if (x <= 0) return 1;
  const exp = Math.pow(10, Math.floor(Math.log10(x)));
  const f = x / exp;
  if (f < 1.5) return exp;
  if (f < 3.5) return 2 * exp;
  if (f < 7.5) return 5 * exp;
  return 10 * exp;
}

function drawPainterRulerX(cnv, geo, cursorSX) {
  const w = cnv.offsetWidth, h = PAINTER_RULER_H;
  if (w <= 0) return;
  cnv.width = w; cnv.height = h;
  const rc = cnv.getContext('2d');
  rc.fillStyle = '#252526';
  rc.fillRect(0, 0, w, h);
  if (geo && geo.scale > 0) {
    const step = painterNiceStep(60 / geo.scale);
    const start = Math.ceil(-geo.ox / geo.scale / step) * step;
    rc.strokeStyle = '#555'; rc.fillStyle = '#888';
    rc.font = '9px monospace'; rc.textBaseline = 'top';
    for (let sx = start; sx <= geo.sheetW + step; sx += step) {
      const cx = Math.round(geo.ox + sx * geo.scale) + 0.5;
      if (cx < 0 || cx > w) continue;
      rc.beginPath(); rc.moveTo(cx, h - 5); rc.lineTo(cx, h); rc.stroke();
      rc.textAlign = 'center';
      if (sx >= 0) rc.fillText(String(sx), cx, 1);
    }
    if (cursorSX >= 0) {
      const cx = Math.round(geo.ox + cursorSX * geo.scale) + 0.5;
      if (cx >= 0 && cx <= w) {
        rc.strokeStyle = 'rgba(255,200,0,0.85)'; rc.lineWidth = 1;
        rc.beginPath(); rc.moveTo(cx, 0); rc.lineTo(cx, h); rc.stroke();
      }
    }
  }
  rc.strokeStyle = '#383838'; rc.lineWidth = 1;
  rc.strokeRect(0.5, 0.5, w - 1, h - 1);
}

function drawPainterRulerY(cnv, geo, cursorSY) {
  const w = PAINTER_RULER_W, h = cnv.offsetHeight;
  if (h <= 0) return;
  cnv.width = w; cnv.height = h;
  const rc = cnv.getContext('2d');
  rc.fillStyle = '#252526';
  rc.fillRect(0, 0, w, h);
  if (geo && geo.scale > 0) {
    const step = painterNiceStep(60 / geo.scale);
    const start = Math.ceil(-geo.oy / geo.scale / step) * step;
    rc.strokeStyle = '#555'; rc.fillStyle = '#888';
    rc.font = '9px monospace'; rc.textAlign = 'right'; rc.textBaseline = 'middle';
    for (let sy = start; sy <= geo.sheetH + step; sy += step) {
      const cy = Math.round(geo.oy + sy * geo.scale) + 0.5;
      if (cy < 0 || cy > h) continue;
      rc.beginPath(); rc.moveTo(w - 5, cy); rc.lineTo(w, cy); rc.stroke();
      if (sy >= 0) rc.fillText(String(sy), w - 6, cy);
    }
    if (cursorSY >= 0) {
      const cy = Math.round(geo.oy + cursorSY * geo.scale) + 0.5;
      if (cy >= 0 && cy <= h) {
        rc.strokeStyle = 'rgba(255,200,0,0.85)'; rc.lineWidth = 1;
        rc.beginPath(); rc.moveTo(0, cy); rc.lineTo(w, cy); rc.stroke();
      }
    }
  }
  rc.strokeStyle = '#383838'; rc.lineWidth = 1;
  rc.strokeRect(0.5, 0.5, w - 1, h - 1);
}

function updatePainterRulers(cursorSX, cursorSY) {
  if (!__painterModule || !__painterRulerX || !__painterRulerY) return;
  const geo = typeof __painterModule.__getDrawGeometry === 'function'
    ? __painterModule.__getDrawGeometry() : null;
  drawPainterRulerX(__painterRulerX, geo, cursorSX);
  drawPainterRulerY(__painterRulerY, geo, cursorSY);
}

function ensurePainterUI() {
  showOutputPane();
  const out = document.getElementById('output');
  const preview = getPreviewBody();
  if (!out || !preview) return;

  const isDevMode = document.body.classList.contains('dev-mode');
  if (!isDevMode) setCompilerOutputMode('painter');

  if (!__turtleToggle) {
    const ctr = document.createElement('div');
    ctr.id = 'output-mode';
    ctr.className = 'output-mode';
    ctr.style.margin = '6px 0';
    ctr.style.display = 'flex';
    ctr.style.gap = '12px';
    preview.appendChild(ctr);
    __turtleToggle = ctr;
  }

  const updatePainterDelay = () => {
    const animCheckbox = document.getElementById('painter-anim-enabled');
    const speedSlider = document.getElementById('painter-speed');
    if (!animCheckbox || !speedSlider) return;
    if (__painterModule && typeof __painterModule.__setAnimationDelay === 'function') {
      if (animCheckbox.checked) {
        const v = parseInt(speedSlider.value, 10);
        __painterModule.__setAnimationDelay(Math.round(Math.pow(2, 8 - 4 * v / 50)));
      } else {
        __painterModule.__setAnimationDelay(0);
      }
    }
  };

  if (__executorToolbarMode !== 'painter') {
    __turtleToggle.innerHTML = '';
    __executorToolbarMode = 'painter';

    const speedContainer = document.createElement('div');
    speedContainer.style.display = 'flex';
    speedContainer.style.alignItems = 'center';
    speedContainer.style.gap = '6px';

    const animCheckbox = document.createElement('input');
    animCheckbox.type = 'checkbox';
    animCheckbox.id = 'painter-anim-enabled';
    animCheckbox.style.cursor = 'pointer';
    animCheckbox.title = 'Включить анимацию';
    const savedPainterAnim = getCookie('q_painter_anim');
    animCheckbox.checked = savedPainterAnim !== '0';

    const speedLabel = document.createElement('span');
    speedLabel.textContent = '🐢';
    speedLabel.style.fontSize = '14px';

    const speedSlider = document.createElement('input');
    speedSlider.type = 'range';
    speedSlider.id = 'painter-speed';
    speedSlider.min = '0';
    speedSlider.max = '100';
    const savedPainterSpeed = getCookie('q_painter_speed');
    speedSlider.value = savedPainterSpeed !== null ? savedPainterSpeed : '50';
    speedSlider.style.width = '80px';
    speedSlider.style.cursor = 'pointer';
    speedSlider.title = 'Скорость анимации';
    speedSlider.disabled = !animCheckbox.checked;
    speedSlider.style.opacity = animCheckbox.checked ? '1' : '0.5';

    const speedLabelFast = document.createElement('span');
    speedLabelFast.textContent = '🐇';
    speedLabelFast.style.fontSize = '14px';

    animCheckbox.addEventListener('change', () => {
      speedSlider.disabled = !animCheckbox.checked;
      speedSlider.style.opacity = animCheckbox.checked ? '1' : '0.5';
      speedLabel.style.opacity = animCheckbox.checked ? '1' : '0.5';
      speedLabelFast.style.opacity = animCheckbox.checked ? '1' : '0.5';
      setCookie('q_painter_anim', animCheckbox.checked ? '1' : '0', 365);
      updatePainterDelay();
    });
    speedSlider.addEventListener('input', () => {
      updatePainterDelay();
      setCookie('q_painter_speed', speedSlider.value, 365);
    });

    speedLabel.style.opacity = animCheckbox.checked ? '1' : '0.5';
    speedLabelFast.style.opacity = animCheckbox.checked ? '1' : '0.5';

    speedContainer.appendChild(animCheckbox);
    speedContainer.appendChild(speedLabel);
    speedContainer.appendChild(speedSlider);
    speedContainer.appendChild(speedLabelFast);
    __turtleToggle.appendChild(speedContainer);

    const fitBtn = document.createElement('button');
    fitBtn.type = 'button';
    fitBtn.className = 'robot-edit-toggle';
    fitBtn.textContent = 'вписать';
    fitBtn.title = 'Вписать лист в окно (колесо мыши - масштаб, перетаскивание - сдвиг)';
    fitBtn.addEventListener('click', () => {
      if (__painterModule && typeof __painterModule.__resetPainterView === 'function') {
        __painterModule.__resetPainterView();
        updatePainterRulers(-1, -1);
      }
    });

    const saveBtn = document.createElement('button');
    saveBtn.type = 'button';
    saveBtn.className = 'robot-edit-toggle';
    saveBtn.textContent = 'PNG';
    saveBtn.title = 'Сохранить лист в исходном размере';
    saveBtn.addEventListener('click', () => {
      if (__painterModule && typeof __painterModule.__savePainterPng === 'function') {
        __painterModule.__savePainterPng('sheet.png');
      }
    });

    __turtleToggle.appendChild(fitBtn);
    __turtleToggle.appendChild(saveBtn);
  }

  updatePainterDelay();
  __turtleToggle.style.display = '';

  if (!__painterCanvas) {
    // Wrapper: flex column, takes all remaining pane space
    const ui = document.createElement('div');
    ui.id = 'painter-ui';
    ui.style.display = 'none';
    ui.style.flex = '1';
    ui.style.minHeight = '0';
    ui.style.flexDirection = 'column';

    // Top row: corner + X ruler
    const topRow = document.createElement('div');
    topRow.style.display = 'flex';
    topRow.style.flexShrink = '0';

    const corner = document.createElement('div');
    corner.style.width = PAINTER_RULER_W + 'px';
    corner.style.height = PAINTER_RULER_H + 'px';
    corner.style.flexShrink = '0';
    corner.style.background = '#252526';
    corner.style.borderRight = '1px solid #383838';
    corner.style.borderBottom = '1px solid #383838';
    corner.style.boxSizing = 'border-box';

    const rulerX = document.createElement('canvas');
    rulerX.id = 'painter-ruler-x';
    rulerX.style.flex = '1';
    rulerX.style.height = PAINTER_RULER_H + 'px';
    rulerX.style.display = 'block';
    rulerX.style.minWidth = '0';

    topRow.appendChild(corner);
    topRow.appendChild(rulerX);
    __painterRulerX = rulerX;

    // Mid row: Y ruler + main canvas
    const midRow = document.createElement('div');
    midRow.style.display = 'flex';
    midRow.style.flex = '1';
    midRow.style.minHeight = '0';

    const rulerY = document.createElement('canvas');
    rulerY.id = 'painter-ruler-y';
    rulerY.style.width = PAINTER_RULER_W + 'px';
    rulerY.style.flexShrink = '0';
    rulerY.style.display = 'block';

    const cnv = document.createElement('canvas');
    cnv.id = 'painter-canvas';
    cnv.style.flex = '1';
    cnv.style.minHeight = '0';
    cnv.style.minWidth = '0';
    cnv.style.background = '#1b1b1b';
    cnv.style.borderRadius = '0 4px 4px 0';

    midRow.appendChild(rulerY);
    midRow.appendChild(cnv);
    __painterRulerY = rulerY;
    __painterCanvas = cnv;

    // Status bar
    const status = document.createElement('div');
    status.id = 'painter-status';
    status.style.flexShrink = '0';
    status.style.height = '18px';
    status.style.lineHeight = '18px';
    status.style.background = '#252526';
    status.style.borderTop = '1px solid #383838';
    status.style.padding = '0 6px';
    status.style.fontSize = '11px';
    status.style.fontFamily = 'monospace';
    status.style.color = '#aaa';
    status.style.boxSizing = 'border-box';
    __painterStatus = status;

    ui.appendChild(topRow);
    ui.appendChild(midRow);
    ui.appendChild(status);
    __painterUI = ui;

    preview.appendChild(ui);

    cnv.addEventListener('mousemove', (e) => {
      if (!__painterModule) return;
      const geo = typeof __painterModule.__getDrawGeometry === 'function'
        ? __painterModule.__getDrawGeometry() : null;
      if (!geo) return;
      const sx = Math.floor((e.offsetX - geo.ox) / geo.scale);
      const sy = Math.floor((e.offsetY - geo.oy) / geo.scale);
      updatePainterRulers(sx, sy);
      if (__painterStatus) {
        const hex = typeof __painterModule.__getPixelColor === 'function'
          ? __painterModule.__getPixelColor(sx, sy) : null;
        __painterStatus.textContent = hex
          ? `x: ${sx}  y: ${sy}  ${hex}`
          : (sx >= 0 && sy >= 0 ? `x: ${sx}  y: ${sy}` : '');
      }
    });
    cnv.addEventListener('mouseleave', () => {
      updatePainterRulers(-1, -1);
      if (__painterStatus) __painterStatus.textContent = '';
    });

    cnv.addEventListener('wheel', (e) => {
      if (!__painterModule || typeof __painterModule.__zoomPainterView !== 'function') return;
      e.preventDefault();
      __painterModule.__zoomPainterView(Math.exp(-e.deltaY * 0.0015), e.offsetX, e.offsetY);
      cnv.dispatchEvent(new MouseEvent('mousemove', { clientX: e.clientX, clientY: e.clientY }));
    }, { passive: false });

    let dragX = 0, dragY = 0, dragging = false;
    cnv.addEventListener('mousedown', (e) => {
      if (e.button !== 0) return;
      dragging = true;
      dragX = e.clientX; dragY = e.clientY;
      cnv.style.cursor = 'grabbing';
      e.preventDefault();
    });
    window.addEventListener('mousemove', (e) => {
      if (!dragging || !__painterModule || typeof __painterModule.__panPainterView !== 'function') return;
      __painterModule.__panPainterView(e.clientX - dragX, e.clientY - dragY);
      dragX = e.clientX; dragY = e.clientY;
    });
    window.addEventListener('mouseup', () => {
      if (!dragging) return;
      dragging = false;
      cnv.style.cursor = '';
    });
    cnv.addEventListener('dblclick', () => {
      if (__painterModule && typeof __painterModule.__resetPainterView === 'function') {
        __painterModule.__resetPainterView();
      }
    });
  }
}

function hidePainterUI() {
  if (__painterUI) __painterUI.style.display = 'none';
  if (__turtleToggle) __turtleToggle.style.display = 'none';
  const out = document.getElementById('output');
  if (out) out.style.display = '';
  hideOutputPane();
}

function renderRobotField() {
  if (!__robotCanvas || !__robotModule || !__robotModule.field) return;

  const field = __robotModule.field;
  const canvas = __robotCanvas;
  const ctx = canvas.getContext('2d');

  // Get canvas container size - use cached dimensions if available to prevent shifting
  const rect = canvas.getBoundingClientRect();
  let w, h;
  if (canvas.__cachedWidth && canvas.__cachedHeight &&
      Math.abs(rect.width - canvas.__cachedWidth) < 10 &&
      Math.abs(rect.height - canvas.__cachedHeight) < 10) {
    // Use cached dimensions if container size hasn't changed significantly
    w = canvas.__cachedWidth;
    h = canvas.__cachedHeight;
  } else {
    // First render or significant size change - cache the dimensions
    w = rect.width;
    h = rect.height;
    canvas.__cachedWidth = w;
    canvas.__cachedHeight = h;
  }
  const dpr = window.devicePixelRatio || 1;

  // Set canvas resolution (this clears the canvas and resets the context)
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  ctx.scale(dpr, dpr);

  // Calculate cell size (reserve space for coordinate labels on all sides)
  const labelSpace = 20; // Space for coordinate numbers
  const padding = 5;
  const availW = w - 2 * padding - 2 * labelSpace;
  const availH = h - 2 * padding - 2 * labelSpace;

  // Calculate cell size to fit field with aspect ratio preserved
  const cellW = availW / field.width;
  const cellH = availH / field.height;
  const cellSize = Math.min(cellW, cellH, 40); // Max 40px per cell

  const gridW = cellSize * field.width;
  const gridH = cellSize * field.height;
  // Center grid in available area (between label spaces)
  const offsetX = padding + labelSpace + (availW - gridW) / 2;
  const offsetY = padding + labelSpace + (availH - gridH) / 2;
  canvas.__robotGrid = { offsetX, offsetY, gridW, gridH, cellSize, width: field.width, height: field.height };

  // Clear
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, w, h);

  // Draw painted cells
  ctx.fillStyle = '#a8d4a8';
  for (const key of field.painted) {
    const [x, y] = key.split(',').map(Number);
    ctx.fillRect(offsetX + x * cellSize, offsetY + y * cellSize, cellSize, cellSize);
  }

  if (field.radiation || field.temperature) {
    ctx.font = `${Math.min(11, cellSize * 0.28)}px sans-serif`;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    for (let y = 0; y < field.height; y++) {
      for (let x = 0; x < field.width; x++) {
        const key = `${x},${y}`;
        const rad = field.radiation && field.radiation.get(key);
        const temp = field.temperature && field.temperature.get(key);
        if (!rad && !temp) continue;
        const label = `${rad ? `R${rad}` : ''}${rad && temp ? ' ' : ''}${temp ? `T${temp}` : ''}`;
        ctx.fillStyle = '#6b4f00';
        ctx.fillText(label, offsetX + x * cellSize + 3, offsetY + y * cellSize + 3);
      }
    }
  }

  if (field.symbol || field.symbol1 || field.point) {
    ctx.font = `${Math.min(13, cellSize * 0.34)}px sans-serif`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (let y = 0; y < field.height; y++) {
      for (let x = 0; x < field.width; x++) {
        const key = `${x},${y}`;
        const symbol = field.symbol && field.symbol.get(key);
        const symbol1 = field.symbol1 && field.symbol1.get(key);
        const point = field.point && field.point.get(key);
        if (!symbol && !symbol1 && !point) continue;
        const label = [symbol, symbol1, point ? String(point) : ''].filter(Boolean).join(' ');
        ctx.fillStyle = '#333';
        ctx.fillText(label, offsetX + x * cellSize + cellSize / 2, offsetY + y * cellSize + cellSize / 2);
      }
    }
  }

  // Draw grid lines
  ctx.strokeStyle = '#ccc';
  ctx.lineWidth = 1;
  for (let x = 0; x <= field.width; x++) {
    ctx.beginPath();
    ctx.moveTo(offsetX + x * cellSize, offsetY);
    ctx.lineTo(offsetX + x * cellSize, offsetY + gridH);
    ctx.stroke();
  }
  for (let y = 0; y <= field.height; y++) {
    ctx.beginPath();
    ctx.moveTo(offsetX, offsetY + y * cellSize);
    ctx.lineTo(offsetX + gridW, offsetY + y * cellSize);
    ctx.stroke();
  }

  // Draw coordinate labels on all four sides
  ctx.fillStyle = '#666';
  ctx.font = `${Math.min(12, cellSize * 0.4)}px sans-serif`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';

  // X coordinates (top and bottom)
  for (let x = 0; x < field.width; x++) {
    const cx = offsetX + x * cellSize + cellSize / 2;
    // Top
    ctx.fillText(String(x + 1), cx, offsetY - labelSpace / 2);
    // Bottom
    ctx.fillText(String(x + 1), cx, offsetY + gridH + labelSpace / 2);
  }

  // Y coordinates (left and right)
  for (let y = 0; y < field.height; y++) {
    const cy = offsetY + y * cellSize + cellSize / 2;
    // Left
    ctx.fillText(String(y + 1), offsetX - labelSpace / 2, cy);
    // Right
    ctx.fillText(String(y + 1), offsetX + gridW + labelSpace / 2, cy);
  }

  // Draw outer border (thick)
  ctx.strokeStyle = '#333';
  ctx.lineWidth = 3;
  ctx.strokeRect(offsetX, offsetY, gridW, gridH);

  // Draw walls (thick black lines)
  ctx.strokeStyle = '#333';
  ctx.lineWidth = 3;
  ctx.lineCap = 'round';

  // Horizontal walls (hWalls: "x,y" = wall below cell (x,y))
  for (const key of field.hWalls) {
    const [x, y] = key.split(',').map(Number);
    const px = offsetX + x * cellSize;
    const py = offsetY + (y + 1) * cellSize;
    ctx.beginPath();
    ctx.moveTo(px, py);
    ctx.lineTo(px + cellSize, py);
    ctx.stroke();
  }

  // Vertical walls (vWalls: "x,y" = wall to the right of cell (x,y))
  for (const key of field.vWalls) {
    const [x, y] = key.split(',').map(Number);
    const px = offsetX + (x + 1) * cellSize;
    const py = offsetY + y * cellSize;
    ctx.beginPath();
    ctx.moveTo(px, py);
    ctx.lineTo(px, py + cellSize);
    ctx.stroke();
  }

  // The wall the robot ran into, drawn red; the robot is red too.
  const crash = field.crash;
  if (crash) {
    const cx = offsetX + crash.x * cellSize;
    const cy = offsetY + crash.y * cellSize;
    const segment = {
      left:  [cx, cy, cx, cy + cellSize],
      right: [cx + cellSize, cy, cx + cellSize, cy + cellSize],
      up:    [cx, cy, cx + cellSize, cy],
      down:  [cx, cy + cellSize, cx + cellSize, cy + cellSize],
    }[crash.side];
    ctx.save();
    ctx.strokeStyle = '#d32f2f';
    ctx.lineWidth = Math.max(5, cellSize * 0.12);
    ctx.lineCap = 'round';
    ctx.beginPath();
    ctx.moveTo(segment[0], segment[1]);
    ctx.lineTo(segment[2], segment[3]);
    ctx.stroke();
    ctx.restore();
  }

  // Draw robot
  const rx = offsetX + field.robotX * cellSize + cellSize / 2;
  const ry = offsetY + field.robotY * cellSize + cellSize / 2;
  const robotRadius = cellSize * 0.35;

  // Robot body (blue circle, red after a crash)
  ctx.fillStyle = crash ? '#d32f2f' : '#4a90d9';
  ctx.beginPath();
  ctx.arc(rx, ry, robotRadius, 0, Math.PI * 2);
  ctx.fill();

  // Robot outline
  ctx.strokeStyle = crash ? '#9a1c1c' : '#2563a0';
  ctx.lineWidth = 2;
  ctx.stroke();

  // Robot "eye" (direction indicator - for now just a dot)
  ctx.fillStyle = '#fff';
  ctx.beginPath();
  ctx.arc(rx, ry - robotRadius * 0.3, robotRadius * 0.25, 0, Math.PI * 2);
  ctx.fill();

  if (__robotEditEnabled) {
    ctx.save();
    ctx.strokeStyle = '#0e639c';
    ctx.lineWidth = 2;
    ctx.setLineDash([6, 4]);
    ctx.strokeRect(offsetX - 3, offsetY - 3, gridW + 6, gridH + 6);
    ctx.restore();
  }
}

function setCompilerOutputMode(mode) {
  __compilerOutputMode = (mode === 'turtle' || mode === 'robot' || mode === 'drawer' || mode === 'painter') ? mode : 'text';
  setCookie('q_out_mode', __compilerOutputMode);
  const out = document.getElementById('output');
  const previewPane = document.querySelector('.pane.preview');

  // Hide all special canvases first
  if (__turtleCanvas)  __turtleCanvas.style.display  = 'none';
  if (__robotCanvas)   __robotCanvas.style.display    = 'none';
  if (__drawerCanvas)  __drawerCanvas.style.display   = 'none';
  if (__painterUI)     __painterUI.style.display      = 'none';

  if (__compilerOutputMode === 'turtle') {
    if (previewPane) previewPane.classList.add('executor-active');
    if (out) out.style.display = '';
    if (__turtleCanvas) __turtleCanvas.style.display = 'block';
    try { if (__turtleModule && typeof __turtleModule.__onCanvasShown === 'function') __turtleModule.__onCanvasShown(); } catch {}
  } else if (__compilerOutputMode === 'robot') {
    if (previewPane) previewPane.classList.add('executor-active');
    if (out) out.style.display = '';
    if (__robotCanvas) __robotCanvas.style.display = 'block';
    renderRobotField();
  } else if (__compilerOutputMode === 'drawer') {
    if (previewPane) previewPane.classList.add('executor-active');
    if (out) out.style.display = '';
    if (__drawerCanvas) __drawerCanvas.style.display = 'block';
    try { if (__drawerModule && typeof __drawerModule.__onCanvasShown === 'function') __drawerModule.__onCanvasShown(); } catch {}
  } else if (__compilerOutputMode === 'painter') {
    if (previewPane) previewPane.classList.add('executor-active');
    if (out) out.style.display = '';
    if (__painterUI) __painterUI.style.display = 'flex';
    try { if (__painterModule && typeof __painterModule.__onCanvasShown === 'function') __painterModule.__onCanvasShown(); } catch {}
    updatePainterRulers(-1, -1);
  } else {
    if (previewPane) previewPane.classList.remove('executor-active');
    if (out) out.style.display = '';
  }
}

function getCurrentCompilerOutputNode() {
  return document.getElementById('output');
}

function getCurrentPreviewNode() {
  if (__compilerOutputMode === 'turtle'   && __turtleCanvas)  return __turtleCanvas;
  if (__compilerOutputMode === 'robot'    && __robotCanvas)   return __robotCanvas;
  if (__compilerOutputMode === 'drawer'   && __drawerCanvas)  return __drawerCanvas;
  if (__compilerOutputMode === 'painter'  && __painterUI)     return __painterUI;
  return getPreviewBody();
}

// Celebration animation for successful runs
function showCelebration() {
  const overlay = document.createElement('div');
  overlay.className = 'celebration-overlay';
  overlay.style.cssText = `
    position: fixed;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    pointer-events: none;
    z-index: 10000;
  `;
  document.body.appendChild(overlay);

  const colors = ['#ff0', '#f0f', '#0ff', '#0f0', '#ff4444', '#44ff44', '#4444ff', '#ff44ff'];
  const particleCount = 400;

  for (let i = 0; i < particleCount; i++) {
    const particle = document.createElement('div');
    const size = Math.random() * 8 + 4;
    const startX = Math.random() * 100;
    const startY = Math.random() * 100;
    const angle = (Math.random() - 0.5) * Math.PI;
    const velocity = Math.random() * 300 + 200;
    const color = colors[Math.floor(Math.random() * colors.length)];

    particle.style.cssText = `
      position: absolute;
      width: ${size}px;
      height: ${size}px;
      background: ${color};
      border-radius: 50%;
      left: ${startX}%;
      top: ${startY}%;
      box-shadow: 0 0 ${size * 2}px ${color};
      animation: firework-${i} 3.0s ease-out forwards;
    `;

    const endX = startX + Math.cos(angle) * (velocity / 10);
    const endY = startY + Math.sin(angle) * (velocity / 10);

    const keyframes = `
      @keyframes firework-${i} {
        0% {
          transform: translate(0, 0) scale(1);
          opacity: 1;
        }
        100% {
          transform: translate(${endX - startX}vw, ${endY - startY}vh) scale(0);
          opacity: 0;
        }
      }
    `;

    const style = document.createElement('style');
    style.textContent = keyframes;
    document.head.appendChild(style);

    overlay.appendChild(particle);
  }

  setTimeout(() => {
    overlay.remove();
    // Clean up animation styles
    document.querySelectorAll('style').forEach(s => {
      if (s.textContent.includes('@keyframes firework-')) s.remove();
    });
  }, 1500);
}

async function runWasm(debug = false, { stopAtEntry = true } = {}) {
  if (__coroRunning) return;
  const code = getCode();
  const { type: algType } = parseAlgHeader(code);
  const O = debug ? '0' : $('#opt').value;
  window.__hasRuntimeErrors = false;
  // A run that fails to compile must not leave the previous program's output behind.
  $('#stdout').textContent = '';
  resetCoroStopSignal();
  let runtime = null;
  let compiled = false;
  let outcome = 'run_ok';
  const debuggerInstance = debug ? debuggerUI?.begin({ stopAtEntry }) : null;
  executionAbort = new AbortController();
  setCoroRunning(true);
  if (embedded) {
    document.dispatchEvent(new CustomEvent('qumir-embed-event', { detail: 'run' }));
  }
  try {
    setCompiling(true);
    const bytes = await api('/api/compile-wasm', { code, O, debug }, true, executionAbort.signal);
    compiled = true;
    const { loadRuntime } = await import('./runtime/loader.js');
    runtime = await loadRuntime(bytes, {
      debugger: debuggerInstance,
      render: () => {
        if (__robotModule) renderRobotField();
        __turtleModule?.__onCanvasShown?.();
        __drawerModule?.__onCanvasShown?.();
        __painterModule?.__flushPainter?.();
      },
      delay: () => (__robotModule || __turtleModule || __painterModule)?.__getAnimationDelay?.() || 0,
    });
    executionSession = runtime.session;
    if (__coroStopRequested) executionSession?.stop();
    const {
      module,
      instance,
      ioEnv,
      resultEnv,
      stringEnv,
      arrayEnv,
      complexEnv,
      futureEnv,
    } = runtime;
    __turtleModule = runtime.turtleModule;
    __robotModule = runtime.robotModule;
    __drawerModule = runtime.drawerModule;
    __painterModule = runtime.painterModule;
    __colorsModule = runtime.colorsModule;
    __keyboardModule = runtime.keyboardModule;
    ensureRuntimeFileManager(ioEnv);
    if (__keyboardModule && typeof __keyboardModule.__resetKeyboard === 'function') {
      __keyboardModule.__resetKeyboard();
    }
    const mem = instance.exports && instance.exports.memory;
    if (mem && typeof ioEnv.__bindMemory === 'function') {
      ioEnv.__bindMemory(mem);
    }
    if (mem && typeof stringEnv.__bindMemory === 'function') {
      stringEnv.__bindMemory(mem);
    }
    if (mem && typeof arrayEnv.__bindMemory === 'function') {
      arrayEnv.__bindMemory(mem);
    }
    if (mem && typeof complexEnv.__bindMemory === 'function') {
      complexEnv.__bindMemory(mem);
    }
    // Bind WASM exports to future runtime (needed for __qumir_wrap_coro child coro ops)
    if (typeof futureEnv.__bindWasm === 'function') {
      futureEnv.__bindWasm(instance.exports);
    }
    if (mem && typeof futureEnv.__bindMemory === 'function') {
      futureEnv.__bindMemory(mem);
    }
    // Give async executor modules access to the JS future runtime
    if (__robotModule   && typeof __robotModule.__bindFutureRuntime   === 'function') __robotModule.__bindFutureRuntime(futureEnv);
    if (__turtleModule  && typeof __turtleModule.__bindFutureRuntime  === 'function') __turtleModule.__bindFutureRuntime(futureEnv);
    if (__drawerModule  && typeof __drawerModule.__bindFutureRuntime  === 'function') __drawerModule.__bindFutureRuntime(futureEnv);
    if (__painterModule && typeof __painterModule.__bindFutureRuntime === 'function') __painterModule.__bindFutureRuntime(futureEnv);

    // Bind string runtime to drawer module for text handling
    if (__drawerModule && typeof __drawerModule.__bindStringRuntime === 'function') {
      __drawerModule.__bindStringRuntime(stringEnv);
    }
    if (mem && __colorsModule && typeof __colorsModule.__bindMemory === 'function') {
      __colorsModule.__bindMemory(mem);
    }
    if (mem && __painterModule && typeof __painterModule.__bindMemory === 'function') {
      __painterModule.__bindMemory(mem);
    }
    if (__painterModule && typeof __painterModule.__bindStringRuntime === 'function') {
      __painterModule.__bindStringRuntime(stringEnv);
    }
    // Turtle integration: detect if wasm imports turtle_* and prepare canvas/toggle
    let usesTurtle = false;
    let usesRobot = false;
    let usesDrawer = false;
    let usesPainter = false;
    try {
      const imps = module ? WebAssembly.Module.imports(module) : [];
      usesTurtle  = Array.isArray(imps) && imps.some(imp => imp && imp.module === 'env' && typeof imp.name === 'string' && imp.name.startsWith('turtle_'));
      usesRobot   = Array.isArray(imps) && imps.some(imp => imp && imp.module === 'env' && typeof imp.name === 'string' && imp.name.startsWith('robot_'));
      usesDrawer  = Array.isArray(imps) && imps.some(imp => imp && imp.module === 'env' && typeof imp.name === 'string' && imp.name.startsWith('drawer_'));
      usesPainter = Array.isArray(imps) && imps.some(imp => imp && imp.module === 'env' && typeof imp.name === 'string' && imp.name.startsWith('painter_'));
    } catch {}
    if (usesTurtle && __turtleModule) {
      hideRobotUI();
      hideDrawerUI();
      hidePainterUI();
      ensureTurtleUI();
      if (__turtleCanvas && typeof __turtleModule.__bindTurtleCanvas === 'function') {
        __turtleModule.__bindTurtleCanvas(__turtleCanvas);
      }
      if (typeof __turtleModule.__resetTurtle === 'function') {
        __turtleModule.__resetTurtle(true);
      }
      const saved = getCookie('q_out_mode');
      setCompilerOutputMode(saved === 'turtle' ? 'turtle' : 'turtle');
    } else if (usesRobot && __robotModule) {
      hideTurtleUI();
      hideDrawerUI();
      hidePainterUI();
      ensureRobotUI();
      const saved = getCookie('q_out_mode');
      setCompilerOutputMode(saved === 'robot' ? 'robot' : 'robot');
    } else if (usesDrawer && __drawerModule) {
      hideTurtleUI();
      hideRobotUI();
      hidePainterUI();
      ensureDrawerUI();
      if (__drawerCanvas && typeof __drawerModule.__bindDrawerCanvas === 'function') {
        __drawerModule.__bindDrawerCanvas(__drawerCanvas);
      }
      if (typeof __drawerModule.__resetDrawer === 'function') {
        __drawerModule.__resetDrawer(true);
      }
      setCompilerOutputMode('drawer');
    } else if (usesPainter && __painterModule) {
      hideTurtleUI();
      hideRobotUI();
      hideDrawerUI();
      ensurePainterUI();
      if (__painterCanvas && typeof __painterModule.__bindPainterCanvas === 'function') {
        __painterModule.__bindPainterCanvas(__painterCanvas);
      }
      if (typeof __painterModule.__resetPainter === 'function') {
        __painterModule.__resetPainter();
      }
      setCompilerOutputMode('painter');
    } else {
      hideTurtleUI();
      hideRobotUI();
      hideDrawerUI();
      hidePainterUI();
      setCompilerOutputMode('text');
    }
    if (typeof ioEnv.__resetIO === 'function') {
      ioEnv.__resetIO(true);
    }
    if (typeof stringEnv.__resetStrings === 'function') {
      stringEnv.__resetStrings();
    }
    if (typeof arrayEnv.__resetArrays === 'function') {
      arrayEnv.__resetArrays();
    }
    // Robot integration: setup file accessor and init field only if program uses robot
    if (usesRobot && __robotModule) {
      setupRobotFilesAccessor();
      if (typeof __robotModule.__initRobotField === 'function') {
        __robotModule.__initRobotField();
      }
    }
    if (debug) debuggerUI.bind(runtime);
    setCompiling(false);
    let out = '';
  if (instance && instance.exports) {
      // Call global constructors if present (init_array handlers)
      if (executionSession && runtime.runtimeInfo?.constructorFunction) {
        await executionSession.call(instance.exports[runtime.runtimeInfo.constructorFunction]);
      } else if (typeof instance.exports.__wasm_call_ctors === 'function') {
        instance.exports.__wasm_call_ctors();
      }
      const entries = Object.entries(instance.exports)
        .filter(([k, v]) => typeof v === 'function' && !k.startsWith('__') && k !== '$$module_constructor' && k !== '$$module_destructor');
      const entry = entries.find(([name]) => name === runtime.runtimeInfo?.entryPoint) || entries[0] || null;
      if (entry) {
        const [name, fn] = entry;
        const rawArgs = ($('#args').value || '').trim();
        const argv = rawArgs.length ? rawArgs.split(',').map(s => s.trim()) : [];
        const parsed = argv.map(s => {
          if (s === 'да' || s === 'истина' || s.toLowerCase() === 'true') return 1;
          if (s === 'нет' || s === 'ложь' || s.toLowerCase() === 'false') return 0;
          if (/^[-+]?\d+$/.test(s)) return BigInt(s);
          if (/^[-+]?\d*\.\d+(e[-+]?\d+)?$/i.test(s) || /^[-+]?\d+\.\d*(e[-+]?\d+)?$/i.test(s)) return Number(s);
          if ((s.startsWith('"') && s.endsWith('"')) || (s.startsWith("'") && s.endsWith("'"))) return s.slice(1, -1);
          return s;
        });
        const expected = typeof fn.length === 'number' ? fn.length : undefined;
        if (expected !== undefined && parsed.length !== expected) {
          out += `${name} expects ${expected} arg(s), got ${parsed.length}.\n`;
        } else {
          const t0 = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
          // Bind string runtime to result runtime so that 'алг лит' values
          // (negative handles or C-string pointers) are interpreted via string.js.
          if (typeof resultEnv.setStringRuntime === 'function') {
            resultEnv.setStringRuntime(stringEnv);
          }
          const retType = runtime.runtimeInfo?.entryReturnType
            ?? resultEnv.wasmReturnType(bytes, name);
          const res = runtime.runtimeInfo?.entryIsCoroutine
            ? await executionSession.call(fn, parsed, retType)
            : fn(...parsed);
          const t1 = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
          const elapsedUs = Math.round((t1 - t0) * 1000);
          const normalized = resultEnv.normalizeReturnValue(res, {
            returnType: retType?.startsWith('ptr to ') ? 'i32' : retType,
            algType: runtime.runtimeInfo?.entryReturnsString ? 'лит' : algType
          });
          // Don't print to stdout, save for errors pane
          window.__lastRunInfo = { name, normalized, elapsedUs, hasReturn: retType !== 'void' };
        }
      } else {
        out += 'no exported functions to invoke\n';
      }
      // Call global destructors if present
      if (executionSession && runtime.runtimeInfo?.destructorFunction) {
        await executionSession.call(instance.exports[runtime.runtimeInfo.destructorFunction]);
      } else if (typeof instance.exports.__wasm_call_dtors === 'function') {
        instance.exports.__wasm_call_dtors();
      }
      if (__painterModule && typeof __painterModule.__flushPainter === 'function') {
        __painterModule.__flushPainter();
      }
  // Debug: list of WebAssembly exports (disabled)
  // out += '\nexports:\n';
  // for (const [k, v] of Object.entries(instance.exports)) {
  //   out += ` - ${k}: ${typeof v}\n`;
  // }
    }
    const stdoutEl = $('#stdout');
    if (out) {
      stdoutEl.textContent += "\n";
      stdoutEl.textContent += out;
    }
    // Render final robot field state
    if (__compilerOutputMode === 'robot' && __robotModule) {
      renderRobotField();
    }

    // Show success info in errors pane
    const stopped = __coroStopRequested;
    let successMsg = stopped ? 'Остановлено' : 'Успешно';
    const missingInput = runtime.stdinStream ? runtime.stdinStream.missingReads() : 0;
    if (stopped) {
      outcome = 'run_stopped';
    } else if (missingInput) {
      outcome = 'run_error';
    }
    if (window.__lastRunInfo) {
      const info = window.__lastRunInfo;
      if (info.hasReturn) {
        successMsg += `\n\nФункция "${info.name}" вернула: ${info.normalized}`;
      }
      successMsg += `\nВремя работы программы: ${info.elapsedUs} мкс`;
      delete window.__lastRunInfo;
    }
    if (missingInput) {
      successMsg += '\n\nПрограмма читала данные командой «ввод», но во вкладке «Ввод» их не хватило: '
        + 'вместо недостающих значений прочитаны нули. Введите данные во вкладке «Ввод» и запустите программу снова.';
    }
    setErrorsPaneContent(successMsg, { isError: missingInput > 0 });
    // Show what the run produced: its output, otherwise the result and timing above.
    const hasOutput = stdoutEl.textContent.trim() !== '';
    setActiveIoPane(hasOutput && !missingInput ? 'stdout' : 'errors', { persistCookie: false });

    // Celebration for successful runs
    // ========================================
    if (!stopped && !embedded) {
      __successfulRunsCount++;
      setCookie('q_runs_count', String(__successfulRunsCount), 365);
      if (__successfulRunsCount === 1 || __successfulRunsCount % 10 === 0) {
        showCelebration();
      }
    }
    // ========================================
  } catch (e) {
    const stopped = e instanceof ExecutionStopped || (__coroStopRequested && e.name === 'AbortError');
    outcome = stopped ? 'run_stopped' : compiled ? 'run_error' : 'run_compile_error';
    const errMsg = stopped ? 'Остановлено' : e.message || String(e);

    // Parse error for line number: "@ Line: 8, Byte: 4, Column: 4"
    const lineMatch = errMsg.match(/@\s*Line:\s*(\d+)/i);
    let lineNum = null;
    if (lineMatch) {
      lineNum = parseInt(lineMatch[1], 10);
    }

    // Show error in errors pane (not stdout)
    setErrorsPaneContent(errMsg, { isError: !stopped });
    setActiveIoPane('errors', { persistCookie: false });

    // If we have a line number, highlight it like compilation errors
    if (lineNum !== null && lineNum > 0) {
      const errors = [{
        line: lineNum,
        col: 0,
        text: errMsg
      }];
      addErrorHighlights(errors);
      window.__hasRuntimeErrors = true;
    }    // Render robot field at the point where the error occurred
    if (__compilerOutputMode === 'robot' && __robotModule) {
      renderRobotField();
    }
  } finally {
    setCompiling(false);
    executionSession?.finish();
    executionSession = null;
    executionAbort = null;
    runtime?.futureEnv.__bindWasm?.(null);
    runtime?.futureEnv.__resetFutures?.();
    __keyboardModule?.__disposeKeyboard?.();
    __browserFileManager?.reset?.();
    if (debug) debuggerUI?.finish();
    setCoroRunning(false);
    if (embedded) {
      document.dispatchEvent(new CustomEvent('qumir-embed-event', { detail: outcome }));
    }
  }
}

function setCompiling(compiling) {
  const progress = document.getElementById('compile-progress');
  if (progress) {
    progress.hidden = !compiling;
    progress.parentElement.setAttribute('aria-busy', String(compiling));
  }
}

function setCoroRunning(running) {
  __coroRunning = running;
  const stopBtn = document.getElementById('btn-stop');
  const runBtn = document.getElementById('btn-run');
  if (stopBtn) stopBtn.style.display = running ? '' : 'none';
  if (runBtn) runBtn.disabled = running;
  const debugBtn = document.getElementById('btn-debug');
  if (debugBtn) debugBtn.disabled = running || !debuggerUI;
}

function loadState() {
  bootstrapProjects();
  const active = getActiveProject();
  if (active) {
    applyProjectToInputs(active, { silent: true });
  } else {
    const c = readPersistedValue('q_code');
    const codeToSet = (c !== null && c !== undefined) ? c : sample;
    setCode(codeToSet);
    // Verify code was actually set
    const actualCode = getCode();
    if (!actualCode || actualCode.length === 0) {
      console.error('loadState: code was not set, textarea may be missing or blocked');
      // Try to recover by setting directly
      setTimeout(() => {
        const ta = document.getElementById('code');
        if (ta && !ta.value) {
          ta.value = codeToSet;
          console.log('loadState: recovered by setting textarea directly');
        }
      }, 100);
    }
    const a = readPersistedValue('q_args');
    if (a !== null && a !== undefined) $('#args').value = a;
  }
  const v = readPersistedValue('q_view');
  if (v !== null && v !== undefined) $('#view').value = v;
  const o = readPersistedValue('q_opt');
  if (o !== null && o !== undefined) $('#opt').value = o;
  const pane = getCookie(IO_PANE_COOKIE);
  if (pane) {
    __currentIoPane = pane;
    // Re-apply active pane after loading from cookie to ensure DOM is synced
    setActiveIoPane(pane, { persistCookie: false });
  }
  // Load successful runs counter
  const runsCount = getCookie('q_runs_count');
  if (runsCount !== null) {
    __successfulRunsCount = parseInt(runsCount, 10) || 0;
  }
}

function saveState() {
  if (embedded) {
    return;
  }
  updateActiveProjectFromInputs();
  writePersistedValue('q_code', getCode());
  writePersistedValue('q_args', $('#args').value || '');
  writePersistedValue('q_view', $('#view').value || 'ir');
  writePersistedValue('q_opt', $('#opt').value || '0');
}

// Initialize CodeMirror if available
function initEditor() {
  const ta = document.getElementById('code');
  if (!ta) {
    console.error('initEditor: textarea#code not found');
    return;
  }
  if (typeof window.CodeMirror === 'undefined') {
    console.warn('CodeMirror not loaded, using plain textarea. Check if CDN is blocked.');
    ta.addEventListener('input', () => { saveState(); debounceShow(); });
    return;
  }
  // Define a simple mode for Qumir language (Cyrillic keywords)
  if (window.CodeMirror.simpleMode && !window.CodeMirror.modes['qumir']) {
    window.CodeMirror.defineSimpleMode('qumir', {
      start: [
        { regex: /\s*(\|.*$)/, token: 'comment' },
        { regex: /(ввод|вывод)/u, token: 'keyword io-keyword' },
        { regex: /(открыть \u043dа чтение|открыть \u043dа запись)/u, token: 'keyword file-keyword' },
        { regex: /(алг|нач|кон|если|иначе|все|нц|кц|пока|для|шаг|цел|вещ|лог|стр)/u, token: 'keyword' },
        { regex: /(истина|ложь)/u, token: 'atom' },
        { regex: /[-+]?\d+(?:_\d+)*(?:[eE][-+]?\d+)?/, token: 'number' },
        { regex: /[-+]?\d*\.\d+(?:[eE][-+]?\d+)?/, token: 'number' },
        { regex: /"(?:[^"\\]|\\.)*"/, token: 'string' },
        { regex: /'(?:[^'\\]|\\.)*'/, token: 'string' },
        { regex: /(\+|\-|\*|\/|%|==|!=|<=|>=|<|>|:=|=|,)/, token: 'operator' },
        { regex: /[A-Za-zА-Яа-я_][A-Za-zА-Яа-я_0-9]*/u, token: 'variable' },
      ],
      meta: { lineComment: '|' }
    });
  }
  // Preserve current textarea content
  const initialText = ta.value;
  editor = window.CodeMirror.fromTextArea(ta, {
    lineNumbers: true,
    tabSize: 4,
    indentUnit: 4,
    indentWithTabs: true,
    matchBrackets: true,
    theme: 'material-darker',
    mode: 'qumir',
    extraKeys: {
      Tab: cm => cm.execCommand('indentMore'),
      'Shift-Tab': cm => cm.execCommand('indentLess'),
      'Ctrl-/': cm => cm.execCommand('toggleComment'),
      'Ctrl-Enter': () => {
        if (window.__runHintOnRun) window.__runHintOnRun();
        const runBtn = document.getElementById('btn-run');
        if (runBtn) runBtn.click();
      }
    }
  });
  // Responsive height: fixed on desktop, auto on mobile (CSS controls heights)
  const __applyEditorHeight = () => {
    if (embedded) {
      editor.setSize(null, '300px');
    } else if (window.innerWidth <= 900) {
      editor.setSize(null, 'auto');
    } else {
      editor.setSize(null, '100%');
    }
    editor.refresh();
  };
  __applyEditorHeight();
  window.addEventListener('resize', () => {
    __applyEditorHeight();
    // Clear cached canvas dimensions on resize
    if (__robotCanvas) {
      delete __robotCanvas.__cachedWidth;
      delete __robotCanvas.__cachedHeight;
    }
    if (__turtleCanvas) {
      delete __turtleCanvas.__cachedWidth;
      delete __turtleCanvas.__cachedHeight;
    }
    if (__drawerCanvas) {
      delete __drawerCanvas.__cachedWidth;
      delete __drawerCanvas.__cachedHeight;
    }
  });
  // Set initial text explicitly (getCode would query editor and return empty on first init)
  editor.setValue(initialText);
  // Cursor status line
  const status = document.getElementById('status');
  if (status) {
    editor.on('cursorActivity', () => {
      const p = editor.getCursor();
      status.textContent = `Ln ${p.line + 1}, Col ${p.ch + 1}`;
    });
  }
  // Mirror initial text and change events
  editor.on('change', () => { saveState(); debounceShow(); });

  // Add tooltip for I/O keywords
  const editorWrapper = editor.getWrapperElement();
  let ioTooltip = null;
  let ioTooltipTimer = null;

  editorWrapper.addEventListener('mousemove', (e) => {
    clearTimeout(ioTooltipTimer);

    const target = e.target;
    const isIoKeyword = target.classList && target.classList.contains('cm-io-keyword');
    const isFileKeyword = target.classList && target.classList.contains('cm-file-keyword');

    if (!isIoKeyword && !isFileKeyword) {
      if (ioTooltip) {
        ioTooltip.remove();
        ioTooltip = null;
      }
      return;
    }

    ioTooltipTimer = setTimeout(() => {
      const text = target.textContent.trim();
      let message = '';

      if (text === 'ввод') {
        message = '📥 Оператор ввода\n\nДанные для ввода нужно указать внизу во вкладке "Ввод"';
      } else if (text === 'вывод') {
        message = '📤 Оператор вывода\n\nРезультат появится внизу во вкладке "Вывод" после нажатия Запустить';
      } else if (text.includes('открыть на чтение')) {
        message = '📂 Открыть файл на чтение\n\nФайлы можно добавить внизу через кнопку ＋';
      } else if (text.includes('открыть на запись')) {
        message = '💾 Открыть файл на запись\n\nРезультат записи появится в файле внизу (добавьте через ＋)';
      }

      if (!message) return;

      if (!ioTooltip) {
        ioTooltip = document.createElement('div');
        ioTooltip.className = 'q-tooltip';
        document.body.appendChild(ioTooltip);
      }

      ioTooltip.textContent = message;
      ioTooltip.style.display = 'block';

      const rect = target.getBoundingClientRect();
      const pad = 8;
      const top = rect.bottom + pad;
      const left = Math.max(8, Math.min(
        window.innerWidth - ioTooltip.offsetWidth - 8,
        rect.left + rect.width / 2 - ioTooltip.offsetWidth / 2
      ));

      ioTooltip.style.top = `${top}px`;
      ioTooltip.style.left = `${left}px`;
    }, 400);
  });

  editorWrapper.addEventListener('mouseleave', () => {
    clearTimeout(ioTooltipTimer);
    if (ioTooltip) {
      ioTooltip.remove();
      ioTooltip = null;
    }
  });

  // Ensure layout after attach
  setTimeout(() => editor.refresh(), 0);
}

function refreshWorkspaceLayout() {
  try { if (editor) editor.refresh(); } catch {}
  try { if (__turtleModule && typeof __turtleModule.__onCanvasShown === 'function') __turtleModule.__onCanvasShown(); } catch {}
  try { if (__drawerModule && typeof __drawerModule.__onCanvasShown === 'function') __drawerModule.__onCanvasShown(); } catch {}
  try { if (__painterModule && typeof __painterModule.__onCanvasShown === 'function') __painterModule.__onCanvasShown(); } catch {}
  try {
    if (__compilerOutputMode === 'robot') {
      if (__robotCanvas) {
        delete __robotCanvas.__cachedWidth;
        delete __robotCanvas.__cachedHeight;
      }
      renderRobotField();
    }
  } catch {}
}

const IO_DOCK_STORAGE_KEY = 'q_workspace_io_dock';
const PREVIEW_DOCK_STORAGE_KEY = 'q_workspace_preview_dock_v2';
const PANE_COLLAPSE_STORAGE_KEY = 'q_workspace_collapsed_panes';
const WORKSPACE_SPLITS = {
  mainIo: { storageKey: 'q_workspace_main_io_split_px', vars: ['--workspace-main-fr', '--workspace-io-fr'] },
  workIo: { storageKey: 'q_workspace_work_io_split_px', vars: ['--workspace-work-fr', '--workspace-side-fr'] },
  outputPreview: { storageKey: 'q_workspace_output_preview_split_v5_px', vars: ['--workspace-output-fr', '--workspace-preview-fr'] },
  outputPreviewBottom: { storageKey: 'q_workspace_output_preview_split_bottom_px', vars: ['--workspace-preview-top-fr', '--workspace-preview-bottom-fr'] },
  editorOutput: { storageKey: 'q_workspace_editor_output_split_px', vars: ['--workspace-left-fr', '--workspace-right-fr'] },
  editorOutputPreview: { storageKey: 'q_workspace_editor_output_preview_split_px', vars: ['--workspace-left-fr', '--workspace-output-fr'] },
  editorPreviewBottom: { storageKey: 'q_workspace_editor_preview_bottom_split_px', vars: ['--workspace-preview-top-fr', '--workspace-preview-bottom-fr'] },
};
const WORKSPACE_SPLIT_STORAGE_KEYS = Object.values(WORKSPACE_SPLITS).map(split => split.storageKey);
const WORKSPACE_SIZE_VARS = [
  '--workspace-main-fr',
  '--workspace-io-fr',
  '--workspace-work-fr',
  '--workspace-side-fr',
  '--workspace-left-fr',
  '--workspace-right-fr',
  '--workspace-output-fr',
  '--workspace-preview-fr',
  '--workspace-preview-top-fr',
  '--workspace-preview-bottom-fr',
];

function clearWorkspaceSizeVars(vars = WORKSPACE_SIZE_VARS) {
  const workspace = document.getElementById('workspace');
  if (!workspace) return;
  vars.forEach(name => workspace.style.removeProperty(name));
}

function clearWorkspaceSplitStorage(keys = WORKSPACE_SPLIT_STORAGE_KEYS) {
  keys.forEach(key => {
    try { localStorage.removeItem(key); } catch {}
  });
}

function normalizeWorkspaceAfterDevModeToggle(isDevMode) {
  if (isDevMode) {
    refreshWorkspaceLayout();
    return;
  }
  clearWorkspaceSplitStorage([
    WORKSPACE_SPLITS.editorOutput.storageKey,
    WORKSPACE_SPLITS.editorOutputPreview.storageKey,
    WORKSPACE_SPLITS.outputPreview.storageKey,
  ]);
  clearWorkspaceSizeVars([
    ...WORKSPACE_SPLITS.editorOutput.vars,
    ...WORKSPACE_SPLITS.outputPreview.vars,
  ]);
  refreshWorkspaceLayout();
}

function getIoDockPlacement() {
  if (debuggerUI && !debuggerUI.panel.hidden) {
    return 'bottom';
  }
  const workspace = document.getElementById('workspace');
  if (!workspace) return 'bottom';
  if (workspace.classList.contains('io-dock-left')) return 'left';
  if (workspace.classList.contains('io-dock-right')) return 'right';
  return 'bottom';
}

function updateIoDockButton(placement = getIoDockPlacement()) {
  const btn = document.querySelector('.pane-title-btn[data-pane-action="dock"][data-pane-target="io"]');
  if (!btn) return;
  const next = placement === 'bottom' ? 'right' : placement === 'right' ? 'left' : 'bottom';
  btn.dataset.dockPlacement = next;
  btn.textContent = placement === 'bottom' ? '⇥' : placement === 'right' ? '⇤' : '⇩';
  const label = next === 'right' ? 'Закрепить справа' : next === 'left' ? 'Закрепить слева' : 'Закрепить снизу';
  btn.setAttribute('aria-label', label);
  btn.setAttribute('data-tooltip', label);
}

function setIoDockPlacement(placement, { persist = true } = {}) {
  const workspace = document.getElementById('workspace');
  if (!workspace) return;
  const normalized = placement === 'left' || placement === 'right' ? placement : 'bottom';
  workspace.classList.toggle('io-dock-right', normalized === 'right');
  workspace.classList.toggle('io-dock-left', normalized === 'left');
  if (persist) {
    try { localStorage.setItem(IO_DOCK_STORAGE_KEY, normalized); } catch {}
  }
  updateIoDockButton(normalized);
  refreshWorkspaceLayout();
}

function setupIoDocking() {
  let saved = 'bottom';
  try { saved = localStorage.getItem(IO_DOCK_STORAGE_KEY) || 'bottom'; } catch {}
  setIoDockPlacement(saved, { persist: false });
}

function getPreviewDockPlacement() {
  const main = document.querySelector('main');
  return main && main.classList.contains('preview-dock-bottom') ? 'bottom' : 'right';
}

function updatePreviewDockButton(placement = getPreviewDockPlacement()) {
  const next = placement === 'right' ? 'bottom' : 'right';
  const label = next === 'bottom' ? 'Просмотр снизу' : 'Просмотр справа';
  document.querySelectorAll('.pane-title-btn[data-pane-action="dock"][data-pane-target="preview"]').forEach(btn => {
    btn.dataset.dockPlacement = next;
    btn.textContent = placement === 'right' ? '⇩' : '⇥';
    btn.setAttribute('aria-label', label);
    btn.setAttribute('data-tooltip', label);
  });
}

function setPreviewDockPlacement(placement, { persist = true } = {}) {
  const main = document.querySelector('main');
  if (!main) return;
  const previous = getPreviewDockPlacement();
  const normalized = placement === 'bottom' ? 'bottom' : 'right';
  const workspace = document.getElementById('workspace');
  const editorPane = document.querySelector('.pane.left');
  const outputPane = document.querySelector('.pane.right');
  const previewPane = document.querySelector('.pane.preview');
  const isDevMode = document.body.classList.contains('dev-mode');
  const isPreviewActive = previewPane && previewPane.classList.contains('executor-active');
  let nextTopSplit = null;
  if (workspace && isDevMode && isPreviewActive && previous !== normalized && normalized === 'bottom' && editorPane && outputPane && previewPane) {
    const editorWidth = editorPane.getBoundingClientRect().width;
    const outputWidth = outputPane.getBoundingClientRect().width;
    const previewWidth = previewPane.getBoundingClientRect().width;
    const topWidth = editorWidth + outputWidth + previewWidth + 6;
    const baseWidth = editorWidth + outputWidth;
    const editorRatio = baseWidth > 0 ? editorWidth / baseWidth : 0.5;
    const nextEditorWidth = Math.max(320, Math.round(topWidth * editorRatio));
    const nextOutputWidth = Math.max(260, Math.round(topWidth - nextEditorWidth));
    nextTopSplit = { editor: nextEditorWidth, output: nextOutputWidth };
  }
  main.classList.toggle('preview-dock-bottom', normalized === 'bottom');
  if (workspace && nextTopSplit) {
    workspace.style.setProperty('--workspace-left-fr', `${nextTopSplit.editor}px`);
    workspace.style.setProperty('--workspace-output-fr', '1fr');
    try {
      localStorage.setItem(WORKSPACE_SPLITS.editorOutputPreview.storageKey, JSON.stringify({
        before: nextTopSplit.editor,
        after: nextTopSplit.output,
      }));
    } catch {}
  }
  if (persist) {
    try { localStorage.setItem(PREVIEW_DOCK_STORAGE_KEY, normalized); } catch {}
  }
  updatePreviewDockButton(normalized);
  refreshWorkspaceLayout();
}

function setupPreviewDocking() {
  let saved = 'right';
  try { saved = localStorage.getItem(PREVIEW_DOCK_STORAGE_KEY) || 'right'; } catch {}
  setPreviewDockPlacement(saved, { persist: false });
}

function createSplitter({ axis, splitter, before, after, storageKey, minBefore, minAfter, beforeVar, afterVar, varTarget, unit = '%', resolveConfig = null }) {
  if (!splitter || !before || !after || !storageKey || !beforeVar || !afterVar) return;
  const getConfig = () => {
    const dynamic = typeof resolveConfig === 'function' ? resolveConfig() : {};
    const nextAxis = dynamic.axis || axis;
    return {
      isHorizontal: nextAxis === 'horizontal',
      before: dynamic.before || before,
      after: dynamic.after || after,
      storageKey: dynamic.storageKey || storageKey,
      beforeVar: dynamic.beforeVar || beforeVar,
      afterVar: dynamic.afterVar || afterVar,
      target: dynamic.varTarget || varTarget || before,
      unit: dynamic.unit || unit,
      minBefore: dynamic.minBefore || minBefore,
      minAfter: dynamic.minAfter || minAfter,
    };
  };
  // Pixels for the first track, the remainder for the second: two absolute sizes
  // would stop covering the container as soon as the window is resized.
  const applySplit = (cfg, beforeValue, afterValue) => {
    cfg.target.style.setProperty(cfg.beforeVar, `${beforeValue}${cfg.unit}`);
    cfg.target.style.setProperty(cfg.afterVar, cfg.unit === 'px' ? '1fr' : `${afterValue}${cfg.unit}`);
  };

  const load = () => {
    try {
      const cfg = getConfig();
      const saved = JSON.parse(localStorage.getItem(cfg.storageKey) || 'null');
      if (!saved || typeof saved.before !== 'number' || typeof saved.after !== 'number') return;
      applySplit(cfg, saved.before, saved.after);
    } catch {}
  };
  const save = (beforeValue, afterValue) => {
    try {
      const cfg = getConfig();
      localStorage.setItem(cfg.storageKey, JSON.stringify({ before: beforeValue, after: afterValue }));
    } catch {}
  };
  const reset = () => {
    const cfg = getConfig();
    try { localStorage.removeItem(cfg.storageKey); } catch {}
    cfg.target.style.removeProperty(cfg.beforeVar);
    cfg.target.style.removeProperty(cfg.afterVar);
    refreshWorkspaceLayout();
  };

  load();

  let dragging = null;
  splitter.addEventListener('pointerdown', (event) => {
    const cfg = getConfig();
    if (window.innerWidth <= 900) return;
    event.preventDefault();
    const beforeRect = cfg.before.getBoundingClientRect();
    const afterRect = cfg.after.getBoundingClientRect();
    const beforeSize = cfg.isHorizontal ? beforeRect.height : beforeRect.width;
    const afterSize = cfg.isHorizontal ? afterRect.height : afterRect.width;
    if (beforeSize + afterSize <= cfg.minBefore + cfg.minAfter) return;
    dragging = {
      pointerId: event.pointerId,
      start: cfg.isHorizontal ? event.clientY : event.clientX,
      beforeSize,
      afterSize,
      total: beforeSize + afterSize,
      cfg,
    };
    splitter.classList.add('dragging');
    document.body.classList.add('layout-resizing', cfg.isHorizontal ? 'layout-resizing-horizontal' : 'layout-resizing-vertical');
    splitter.setPointerCapture(event.pointerId);
  });

  splitter.addEventListener('pointermove', (event) => {
    if (!dragging || dragging.pointerId !== event.pointerId) return;
    const cfg = dragging.cfg;
    const delta = (cfg.isHorizontal ? event.clientY : event.clientX) - dragging.start;
    if (dragging.total <= cfg.minBefore + cfg.minAfter) return;
    const nextBefore = Math.max(cfg.minBefore, Math.min(dragging.total - cfg.minAfter, dragging.beforeSize + delta));
    const nextAfter = dragging.total - nextBefore;
    const beforeValue = cfg.unit === 'px' || cfg.unit === 'fr' ? nextBefore : (nextBefore / dragging.total) * 100;
    const afterValue = cfg.unit === 'px' || cfg.unit === 'fr' ? nextAfter : (nextAfter / dragging.total) * 100;
    applySplit(cfg, beforeValue, afterValue);
    refreshWorkspaceLayout();
  });

  const stop = (event) => {
    if (!dragging || dragging.pointerId !== event.pointerId) return;
    const cfg = dragging.cfg;
    const beforeSize = cfg.isHorizontal ? cfg.before.getBoundingClientRect().height : cfg.before.getBoundingClientRect().width;
    const afterSize = cfg.isHorizontal ? cfg.after.getBoundingClientRect().height : cfg.after.getBoundingClientRect().width;
    const total = beforeSize + afterSize;
    if (total > 0) {
      save(cfg.unit === 'px' || cfg.unit === 'fr' ? beforeSize : (beforeSize / total) * 100,
        cfg.unit === 'px' || cfg.unit === 'fr' ? afterSize : (afterSize / total) * 100);
    }
    try { splitter.releasePointerCapture(event.pointerId); } catch {}
    splitter.classList.remove('dragging');
    document.body.classList.remove('layout-resizing', 'layout-resizing-horizontal', 'layout-resizing-vertical');
    dragging = null;
    refreshWorkspaceLayout();
  };
  splitter.addEventListener('pointerup', stop);
  splitter.addEventListener('pointercancel', stop);
  splitter.addEventListener('dblclick', reset);
}

function setupWorkspaceSplitters() {
  const workspace = document.getElementById('workspace');
  const mainRow = document.querySelector('.workspace-main-row');
  const io = document.querySelector('section.io');
  const main = document.querySelector('main');
  const editorPane = document.querySelector('.pane.left');
  const outputPane = document.querySelector('.pane.right');
  const previewPane = document.querySelector('.pane.preview');

  createSplitter({
    axis: 'horizontal',
    splitter: document.getElementById('splitter-main-io'),
    before: mainRow,
    after: io,
    storageKey: WORKSPACE_SPLITS.mainIo.storageKey,
    minBefore: 220,
    minAfter: 120,
    beforeVar: '--workspace-main-fr',
    afterVar: '--workspace-io-fr',
    varTarget: workspace,
    resolveConfig: () => getIoDockPlacement() === 'right' || getIoDockPlacement() === 'left'
      ? {
          axis: 'vertical',
          storageKey: WORKSPACE_SPLITS.workIo.storageKey,
          beforeVar: '--workspace-work-fr',
          afterVar: '--workspace-side-fr',
          minBefore: 420,
          minAfter: 300,
          varTarget: workspace,
          unit: 'px',
        }
      : {
          axis: 'horizontal',
          storageKey: WORKSPACE_SPLITS.mainIo.storageKey,
          beforeVar: '--workspace-main-fr',
          afterVar: '--workspace-io-fr',
          minBefore: 220,
          minAfter: 120,
          varTarget: workspace,
          unit: 'px',
        },
  });

  createSplitter({
    axis: 'vertical',
    splitter: document.getElementById('splitter-output-preview'),
    before: outputPane,
    after: previewPane,
    storageKey: WORKSPACE_SPLITS.outputPreview.storageKey,
    minBefore: 260,
    minAfter: 260,
    beforeVar: '--workspace-output-fr',
    afterVar: '--workspace-preview-fr',
    varTarget: workspace,
    resolveConfig: () => getPreviewDockPlacement() === 'bottom'
      ? {
          axis: 'horizontal',
          storageKey: WORKSPACE_SPLITS.outputPreviewBottom.storageKey,
          beforeVar: '--workspace-preview-top-fr',
          afterVar: '--workspace-preview-bottom-fr',
          minBefore: 160,
          minAfter: 180,
          varTarget: workspace,
          unit: 'px',
        }
      : {
          axis: 'vertical',
          storageKey: WORKSPACE_SPLITS.outputPreview.storageKey,
          beforeVar: '--workspace-output-fr',
          afterVar: '--workspace-preview-fr',
          minBefore: 260,
          minAfter: 260,
          varTarget: workspace,
          unit: 'px',
        },
  });

  createSplitter({
    axis: 'vertical',
    splitter: document.getElementById('splitter-editor-output'),
    before: editorPane,
    after: outputPane,
    storageKey: WORKSPACE_SPLITS.editorOutput.storageKey,
    minBefore: 320,
    minAfter: 260,
    beforeVar: '--workspace-left-fr',
    afterVar: '--workspace-right-fr',
    varTarget: workspace,
    resolveConfig: () => {
      const isDevMode = document.body.classList.contains('dev-mode');
      const isPreviewActive = previewPane && previewPane.classList.contains('executor-active');
      if (getPreviewDockPlacement() === 'bottom' && !isDevMode) {
        return {
          axis: 'horizontal',
          storageKey: WORKSPACE_SPLITS.editorPreviewBottom.storageKey,
          after: previewPane,
          beforeVar: '--workspace-preview-top-fr',
          afterVar: '--workspace-preview-bottom-fr',
          minBefore: 180,
          minAfter: 180,
          varTarget: workspace,
          unit: 'px',
        };
      }
      if (!isDevMode && isPreviewActive) {
        return {
          axis: 'vertical',
          storageKey: WORKSPACE_SPLITS.editorOutput.storageKey,
          after: previewPane,
          beforeVar: '--workspace-left-fr',
          afterVar: '--workspace-right-fr',
          minBefore: 320,
          minAfter: 260,
          varTarget: workspace,
          unit: 'px',
        };
      }
      if (isDevMode && isPreviewActive) {
        return {
          axis: 'vertical',
          storageKey: WORKSPACE_SPLITS.editorOutputPreview.storageKey,
          beforeVar: '--workspace-left-fr',
          afterVar: '--workspace-output-fr',
          minBefore: 320,
          minAfter: 260,
          varTarget: workspace,
          unit: 'px',
        };
      }
      return {
        axis: 'vertical',
        storageKey: WORKSPACE_SPLITS.editorOutput.storageKey,
        beforeVar: '--workspace-left-fr',
        afterVar: '--workspace-right-fr',
        minBefore: 320,
        minAfter: 260,
        varTarget: workspace,
        unit: 'px',
      };
    },
  });

  void main;
  window.addEventListener('resize', refreshWorkspaceLayout);
  setTimeout(refreshWorkspaceLayout, 0);
}

function resetWorkspaceSplit(storageKey, vars, { refresh = true } = {}) {
  const workspace = document.getElementById('workspace');
  try { localStorage.removeItem(storageKey); } catch {}
  if (workspace) {
    vars.forEach(name => workspace.style.removeProperty(name));
  }
  if (refresh) refreshWorkspaceLayout();
}

function resetWorkspaceSplits(...splits) {
  splits.forEach(split => resetWorkspaceSplit(split.storageKey, split.vars, { refresh: false }));
  refreshWorkspaceLayout();
}

function resetWorkspaceLayoutState() {
  clearWorkspaceSplitStorage();
  clearWorkspaceSizeVars();
  try { localStorage.removeItem(IO_DOCK_STORAGE_KEY); } catch {}
  try { localStorage.removeItem(PREVIEW_DOCK_STORAGE_KEY); } catch {}
  try { localStorage.removeItem(PANE_COLLAPSE_STORAGE_KEY); } catch {}
  setIoDockPlacement('bottom', { persist: false });
  setPreviewDockPlacement('right', { persist: false });
  ['code', 'output', 'preview', 'io'].forEach(id => setPaneCollapsed(id, false, { persist: false }));
  refreshWorkspaceLayout();
}

function getPaneById(id) {
  return document.querySelector(`[data-pane-id="${id}"]`);
}

function readCollapsedPanes() {
  try {
    const saved = JSON.parse(localStorage.getItem(PANE_COLLAPSE_STORAGE_KEY) || '[]');
    return new Set(Array.isArray(saved) ? saved : []);
  } catch {
    return new Set();
  }
}

function writeCollapsedPanes(collapsed) {
  try { localStorage.setItem(PANE_COLLAPSE_STORAGE_KEY, JSON.stringify(Array.from(collapsed))); } catch {}
}

function updatePaneCollapseButton(id, collapsed) {
  const btn = document.querySelector(`.pane-title-btn[data-pane-action="collapse"][data-pane-target="${id}"]`);
  if (!btn) return;
  btn.textContent = collapsed ? '▸' : '▾';
  btn.setAttribute('aria-label', collapsed ? 'Развернуть панель' : 'Свернуть панель');
  btn.setAttribute('data-tooltip', collapsed ? 'Развернуть панель' : 'Свернуть панель');
  btn.setAttribute('aria-expanded', collapsed ? 'false' : 'true');
}

function setPaneCollapsed(id, collapsed, { persist = true } = {}) {
  const pane = getPaneById(id);
  if (!pane) return;
  pane.classList.toggle('collapsed', collapsed);
  updatePaneCollapseButton(id, collapsed);
  if (persist) {
    const saved = readCollapsedPanes();
    if (collapsed) saved.add(id); else saved.delete(id);
    writeCollapsedPanes(saved);
  }
  refreshWorkspaceLayout();
}

function resetPaneSize(id) {
  if (id === 'io') {
    resetWorkspaceSplits(WORKSPACE_SPLITS.mainIo);
  } else if (id === 'code') {
    resetWorkspaceSplits(
      WORKSPACE_SPLITS.editorOutput,
      WORKSPACE_SPLITS.editorOutputPreview,
      WORKSPACE_SPLITS.editorPreviewBottom,
    );
  } else if (id === 'output' || id === 'preview') {
    resetWorkspaceSplits(WORKSPACE_SPLITS.outputPreview, WORKSPACE_SPLITS.outputPreviewBottom);
  }
}

function resetCurrentPreviewExecutor() {
  try {
    if (__compilerOutputMode === 'turtle' && __turtleModule && typeof __turtleModule.__resetTurtle === 'function') {
      __turtleModule.__resetTurtle(true);
    } else if (__compilerOutputMode === 'robot' && __robotModule) {
      if (typeof __robotModule.__resetRobot === 'function') __robotModule.__resetRobot();
      else if (typeof __robotModule.__initRobotField === 'function') __robotModule.__initRobotField();
      renderRobotField();
    } else if (__compilerOutputMode === 'drawer' && __drawerModule && typeof __drawerModule.__resetDrawer === 'function') {
      __drawerModule.__resetDrawer(true);
    } else if (__compilerOutputMode === 'painter' && __painterModule && typeof __painterModule.__resetPainter === 'function') {
      __painterModule.__resetPainter();
      updatePainterRulers(-1, -1);
    }
  } catch {}
  refreshWorkspaceLayout();
}

function fitCurrentPreviewView() {
  try {
    if (__compilerOutputMode === 'turtle' && __turtleModule && typeof __turtleModule.__fitTurtleView === 'function') {
      __turtleModule.__fitTurtleView();
    } else if (__compilerOutputMode === 'robot') {
      if (__robotCanvas) {
        delete __robotCanvas.__cachedWidth;
        delete __robotCanvas.__cachedHeight;
      }
      renderRobotField();
    } else if (__compilerOutputMode === 'drawer' && __drawerModule && typeof __drawerModule.__fitDrawerView === 'function') {
      __drawerModule.__fitDrawerView();
    } else if (__compilerOutputMode === 'painter' && __painterModule && typeof __painterModule.__fitPainterView === 'function') {
      __painterModule.__fitPainterView();
      updatePainterRulers(-1, -1);
    }
  } catch {}
  refreshWorkspaceLayout();
}

function setupPaneHeaderControls() {
  const saved = readCollapsedPanes();
  ['code', 'output', 'preview', 'io'].forEach(id => setPaneCollapsed(id, saved.has(id), { persist: false }));

  const resetLayoutBtn = document.getElementById('btn-reset-layout');
  if (resetLayoutBtn) {
    resetLayoutBtn.addEventListener('click', () => resetWorkspaceLayoutState());
  }

  document.querySelectorAll('.pane-title-btn').forEach(btn => {
    btn.addEventListener('mousedown', event => event.stopPropagation());
    btn.addEventListener('click', event => {
      const action = btn.dataset.paneAction;
      const target = btn.dataset.paneTarget;
      if (action !== 'fullscreen') {
        event.preventDefault();
        event.stopPropagation();
      }
      if (action === 'collapse') {
        const pane = getPaneById(target);
        setPaneCollapsed(target, !(pane && pane.classList.contains('collapsed')));
      } else if (action === 'reset') {
        resetPaneSize(target);
      } else if (action === 'preview-reset') {
        event.preventDefault();
        event.stopPropagation();
        resetCurrentPreviewExecutor();
      } else if (action === 'preview-fit') {
        event.preventDefault();
        event.stopPropagation();
        fitCurrentPreviewView();
      } else if (action === 'dock') {
        event.preventDefault();
        event.stopPropagation();
        if (target === 'io') {
          setIoDockPlacement(btn.dataset.dockPlacement);
        } else if (target === 'preview') {
          setPreviewDockPlacement(btn.dataset.dockPlacement);
        }
      }
    });
  });
}

// Diagnostic check for critical elements
(() => {
  const critical = ['code', 'args', 'stdin', 'stdout', 'view', 'opt', 'btn-run'];
  if (!embedded) {
    critical.push('examples');
  }
  const missing = critical.filter(id => !document.getElementById(id));
  if (missing.length > 0) {
    console.error('Critical elements missing:', missing);
  }
})();

initIoWorkspace();  // Must be before loadState() so __ioFilesRoot is ready
if (!embedded) {
  loadState();
  initProjectsUI();
}
// Load examples list
(async function initExamples(){
  if (embedded) {
    return;
  }
  try {
    const data = await apiGet('/api/examples');
    const sel = $('#examples');
    if (sel && data && Array.isArray(data.groups)) {
      data.groups.forEach(group => {
        const optgroup = document.createElement('optgroup');
        optgroup.label = group.title;
        group.examples.forEach(it => {
          const opt = document.createElement('option');
          opt.value = it.path;
          opt.textContent = it.title;
          optgroup.appendChild(opt);
        });
        sel.appendChild(optgroup);
      });
    } else {
      console.warn('examples: unexpected response format', data);
      const sel = $('#examples');
      if (sel) {
        sel.disabled = true;
        sel.title = 'Не удалось загрузить список примеров';
      }
    }
  } catch (e) {
    console.error('examples load failed:', e);
    const sel = $('#examples');
    if (sel) {
      sel.disabled = true;
      sel.title = 'Не удалось загрузить список примеров. Проверьте подключение или отключите блокировщик рекламы.';
      // Add fallback option
      const opt = document.createElement('option');
      opt.value = '';
      opt.textContent = '⚠ Ошибка загрузки примеров';
      sel.appendChild(opt);
    }
    // Show user-visible notification
    setTimeout(() => {
      showToast('⚠ Не удалось загрузить примеры', 3000);
    }, 1000);
  }
})();
// Initialize editor (assets are loaded via HTML)
initEditor();
if (!embedded) {
  setupWorkspaceSplitters();
  setupPaneHeaderControls();
  setupIoDocking();
  setupPreviewDocking();
}

// Relocate the compiler view selector above the Output on mobile
(function relocateViewSelector(){
  const viewEl = document.getElementById('view');
  if (!viewEl) return;
  // Create an anchor to restore original position in header controls
  let anchor = document.getElementById('view-anchor');
  if (!anchor) {
    anchor = document.createElement('span');
    anchor.id = 'view-anchor';
    viewEl.insertAdjacentElement('afterend', anchor);
  }
  const slot = document.getElementById('view-slot');
  const place = () => {
    if (!viewEl) return;
    if (window.innerWidth <= 900 && slot && viewEl.parentElement !== slot) {
      slot.appendChild(viewEl);
    } else if (window.innerWidth > 900 && anchor && viewEl.previousSibling !== anchor) {
      // Put it back before the anchor to keep control layout
      anchor.parentNode.insertBefore(viewEl, anchor);
    }
  };
  place();
  window.addEventListener('resize', place);
})();

// On mobile, place compiler output below IO section to ensure strict vertical flow
(function relocateOutputMobile(){
  if (document.getElementById('workspace')) return;
  const rightPane = document.querySelector('section.pane.right');
  const io = document.querySelector('section.io');
  const anchor = document.getElementById('output-anchor');
  if (!rightPane || !io || !anchor) return;
  const place = () => {
    if (window.innerWidth <= 900) {
      // Move right pane after IO
      if (rightPane.previousElementSibling !== io) {
        io.insertAdjacentElement('afterend', rightPane);
      }
    } else {
      // Restore to anchor position inside main
      if (anchor.parentNode && rightPane !== anchor.nextSibling) {
        anchor.parentNode.insertBefore(rightPane, anchor.nextSibling);
      }
    }
  };
  place();
  window.addEventListener('resize', place);
})();
// If URL has ?share=<id>, load the shared snippet (code, args, stdin, files, ...) as a dedicated project.
// The project name is "Проект (открыт из ссылки <id>)". If a project with that
// name already exists, its contents are overwritten instead of creating a new one.
(async function loadSharedFromQuery(){
  if (embedded) {
    return;
  }
  try {
    const params = new URLSearchParams(window.location.search);
    const sid = params.get('share');
    if (!sid) return;
    const data = await apiGet('/api/share?id=' + encodeURIComponent(sid));

    // Normalize payload into a project-like shape
    let code = '';
    let args = '';
    let stdin = '';
    if (typeof data === 'string') {
      code = data;
    } else if (data && typeof data === 'object') {
      if (typeof data.code === 'string') code = data.code;
      if (typeof data.args === 'string') args = data.args;
      if (typeof data.stdin === 'string') stdin = data.stdin;
    }

  // Derive project name and either reuse existing project with that name
  // or create a new one. We don't include a numeric suffix here so that
  // repeated opens of the same link don't create extra projects.
    const displayName = `Проект (открыт из ссылки ${sid})`;
    let project = __projects.find(p => p.name === displayName);
    if (!project) {
      project = createProject({ name: displayName, code, args, stdin }, { activate: true });
    } else {
      project.code = code;
      project.args = args;
      project.stdin = stdin;
      project.updatedAt = Date.now();
      persistProjects();
      scheduleProjectsRender();
      setActiveProject(project.id, { silent: true });
      applyProjectToInputs(project, { silent: true });
    }

    const stdinEl = $('#stdin');
    if (stdinEl) {
      stdinEl.value = stdin;
    }

    // Restore IO files from shared payload (object name -> content).
    const nextFiles = [];
    if (data && typeof data === 'object' && data.files && typeof data.files === 'object') {
      let idx = 0;
      for (const [name, content] of Object.entries(data.files)) {
        if (typeof content !== 'string') continue;
        nextFiles.push(normalizeIoFile({ name, content }, idx++));
      }
    }

    if (__ioFilesRoot) {
      __ioFilesRoot.innerHTML = '';
      ensureErrorsPane();
    }
    __ioFiles.length = 0;
    nextFiles.forEach(file => {
      __ioFiles.push(file);
      renderIoFilePane(file);
    });
    refreshIoSelectOptions();

    debounceShow();
    const statusEl = document.getElementById('status');
    if (statusEl) statusEl.textContent = `Загружено из ссылки: ${sid}`;
  } catch (e) {
    console.warn('failed to load share:', e);
  }
})();

// If URL has ?example=<path>, load that example directly
(async function loadExampleFromQuery(){
  if (embedded) {
    return;
  }
  try {
    const params = new URLSearchParams(window.location.search);
    const examplePath = params.get('example');
    if (!examplePath) return;

    requestStopRunningProgram();
    const data = await apiGet('/api/example?path=' + encodeURIComponent(examplePath));
    const displayName = data.title || deriveExampleProjectName(examplePath);
    const code = data.code || '';
    const args = data.args || '';

    // Prepare files array for the project
    const projectFiles = [];
    if (Array.isArray(data.files) && data.files.length > 0) {
      for (const f of data.files) {
        projectFiles.push({ name: f.name, content: f.content || '' });
      }
    }

    updateActiveProjectFromInputs();
    createProject({ name: displayName, code: code, args: args, stdin: '', files: projectFiles }, { activate: true });

    // Select example in dropdown if present
    const examplesSel = $('#examples');
    if (examplesSel) {
      examplesSel.value = examplePath;
    }

    debounceShow();
    const statusEl = document.getElementById('status');
    if (statusEl) statusEl.textContent = `Пример: ${displayName}`;
    // A link to an example is a request to see it working; the run refreshes the views itself.
    clearTimeout(showTimer);
    await runWasm();
    show($('#view').value, { clearErrorsOnSuccess: false });
  } catch (e) {
    console.warn('failed to load example from query:', e);
  }
})();

// The footer shows only the compiler revision; the full strings live in a dialog.
(async function showVersion(){
  if (embedded) {
    return;
  }
  // "1.0.0-42,2a77288,2026-08-15" -> "2a77288";  "dev,2026-08-15" -> "dev"
  const shortRev = (value) => {
    const parts = String(value || '').split(',');
    if (!parts[0]) return '—';
    const rev = parts[0] === 'dev' ? 'dev' : (parts[1] || parts[0]);
    // An older compiler may report free-form text; the dialog still shows it whole.
    return rev.length > 14 ? rev.slice(0, 13) + '…' : rev;
  };

  const parse = (raw) => {
    const out = { srv: '', comp: '' };
    String(raw || '').split(';').forEach(chunk => {
      const at = chunk.indexOf(':');
      if (at < 0) return;
      const key = chunk.slice(0, at);
      if (key === 'srv' || key === 'comp') out[key] = chunk.slice(at + 1);
    });
    if (!out.srv && !out.comp) out.comp = String(raw || '');
    return out;
  };

  const openDialog = (info) => {
    let dlg = document.getElementById('version-dialog');
    if (!dlg) {
      dlg = document.createElement('dialog');
      dlg.id = 'version-dialog';
      dlg.className = 'version-dialog';
      dlg.innerHTML = '<h3>Версия</h3><dl></dl>'
        + '<form method="dialog"><button type="submit">Закрыть</button></form>';
      document.body.appendChild(dlg);
      dlg.addEventListener('click', ev => { if (ev.target === dlg) dlg.close(); });
    }
    const list = dlg.querySelector('dl');
    list.textContent = '';
    [['Сервис', info.srv], ['Компилятор', info.comp]].forEach(([label, value]) => {
      if (!value) return;
      const dt = document.createElement('dt');
      dt.textContent = label;
      const dd = document.createElement('dd');
      dd.textContent = value;
      list.append(dt, dd);
    });
    dlg.showModal();
  };

  try {
    const v = await apiGet('/api/version');
    const el = document.getElementById('version');
    if (!el) return;

    let info = null;
    if (typeof v === 'string') {
      info = parse(v);
    } else if (v && v.hash) {
      info = { srv: '', comp: `${v.hash},${v.date || ''}` };
    }
    if (!info) return;

    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'version-link';
    btn.textContent = shortRev(info.comp || info.srv);
    btn.setAttribute('data-tooltip', 'Подробнее о версии');
    btn.addEventListener('click', () => openDialog(info));

    el.textContent = '';
    el.appendChild(btn);
  } catch (e) {
    // ignore if endpoint not available
  }
})();
['#args', '#stdin'].forEach(sel => {
  const el = $(sel);
  if (el) el.addEventListener('input', saveState);
});
const viewSel = $('#view');
if (viewSel) viewSel.addEventListener('change', () => { saveState(); show(viewSel.value); });
const optSel = $('#opt');
if (optSel) optSel.addEventListener('change', () => { saveState(); show($('#view').value); });
const syntaxModeSel = $('#syntax-mode');
if (syntaxModeSel) syntaxModeSel.addEventListener('change', () => {
  writePersistedValue('qumir-syntax-mode', syntaxModeSel.value || 'kumir');
  saveState();
  show($('#view').value);
});

// Best-effort save on page unload to avoid losing recent edits
if (typeof window !== 'undefined') {
  window.addEventListener('beforeunload', () => {
    try { saveState(); } catch (_) {}
  });
}

// Auto-load example when selection changes
const examplesSel = $('#examples');
if (examplesSel) examplesSel.addEventListener('change', async () => {
  const path = examplesSel.value || '';
  if (!path) return;
  requestStopRunningProgram();
  try {
    // Load example (code + optional metadata + files)
    const data = await apiGet('/api/example?path=' + encodeURIComponent(path));
    const displayName = data.title || deriveExampleProjectName(path);

    const code = data.code || '';
    const args = data.args || '';

    // Prepare files array for the project
    const projectFiles = [];
    if (Array.isArray(data.files) && data.files.length > 0) {
      for (const f of data.files) {
        projectFiles.push({ name: f.name, content: f.content || '' });
      }
    }

    updateActiveProjectFromInputs();
    createProject({ name: displayName, code: code, args: args, stdin: '', files: projectFiles }, { activate: true });

    debounceShow();
  } catch (e) {
    alert('Не удалось загрузить пример: ' + (e.message || String(e)));
  } finally {
    examplesSel.value = '';
  }
});

// Snippet insertion
function insertSnippet(kind) {
  const indent = '    ';
  const snippets = {
    while:
`нц пока условие
${indent}| тело
кц`,
    for:
`нц для i от 0 до 10 шаг 1
${indent}| тело
кц`,
    if:
`если условие то
${indent}| then
иначе
${indent}| else
все`,
    switch:
`выбор
${indent}при x = 1:
${indent}${indent}| ветка 1
${indent}при x = 2:
${indent}${indent}| ветка 2
${indent}иначе
${indent}${indent}| иначе
все`,
    func:
`алг цел имя(цел a)
нач
${indent}знач := a
кон`,
    decl:
  `цел x, y
  | табличный тип: цел таб A[0:9]`
  };
  // Selected after insertion so that typing replaces it; left as is it does not compile.
  const placeholders = { while: 'условие', if: 'условие', switch: 'x = 1', func: 'имя' };
  const text = snippets[kind] || '';
  if (!text) return;
  const placeholder = placeholders[kind];
  const selOffset = placeholder ? text.indexOf(placeholder) : -1;
  if (editor) {
    const doc = editor.getDoc();
    const cur = doc.getCursor();
    const startIndex = doc.indexFromPos(cur);
    doc.replaceRange(text, cur);
    if (selOffset >= 0) {
      doc.setSelection(doc.posFromIndex(startIndex + selOffset),
                       doc.posFromIndex(startIndex + selOffset + placeholder.length));
    }
    editor.focus();
  } else {
    const ta = document.getElementById('code');
    if (!ta) return;
    const start = ta.selectionStart || 0;
    const end = ta.selectionEnd || start;
    const before = ta.value.slice(0, start);
    const after = ta.value.slice(end);
    ta.value = before + text + after;
    if (selOffset >= 0) {
      ta.selectionStart = start + selOffset;
      ta.selectionEnd = start + selOffset + placeholder.length;
    } else {
      ta.selectionStart = ta.selectionEnd = start + text.length;
    }
    ta.focus();
  }
  saveState();
  debounceShow();
}

['while','for','if','switch','func','decl'].forEach(k => {
  const btn = document.getElementById(`btn-snippet-${k}`);
  if (btn) {
    btn.addEventListener('click', () => insertSnippet(k));
  }
});

// Build rich tooltips using snippet content (set immediately)
{ const indent = '    ';
  const preview = {
    while:
`Вставить: цикл пока\n\nнц пока условие\n${indent}| тело\nкц`,
    for:
`Вставить: цикл от\n\nнц для i от 0 до 10 шаг 1\n${indent}| тело\nкц`,
    if:
`Вставить: условие\n\nесли условие то\n${indent}| then\nиначе\n${indent}| else\nвсе`,
    switch:
`Вставить: выбор\n\nвыбор\n${indent}при x = 1:\n${indent}${indent}| ветка 1\n${indent}при x = 2:\n${indent}${indent}| ветка 2\n${indent}иначе\n${indent}${indent}| иначе\nвсе`,
    func:
`Вставить: функция\n\nалг цел имя(цел a)\nнач\n${indent}знач := a\nкон`,
    decl:
  `Вставить: тип/объявление\n\nцел x, y\n| табличный тип: цел таб A[0:n]`
  };
  ['while','for','if','switch','func','decl'].forEach(k => {
    const btn = document.getElementById(`btn-snippet-${k}`);
    if (btn) btn.setAttribute('data-tooltip', preview[k]);
  });
}

// JS-driven tooltip (more reliable across browsers)
(() => {
  let tipEl = null;
  const canHoverTooltip = () => window.matchMedia && window.matchMedia('(hover: hover) and (pointer: fine)').matches;
  function showTip(target, { placeAbove = false } = {}) {
    if (!canHoverTooltip()) return;
    const msg = target.getAttribute('data-tooltip');
    if (!msg) return;
    if (!tipEl) {
      tipEl = document.createElement('div');
      tipEl.className = 'q-tooltip';
      document.body.appendChild(tipEl);
    }
    tipEl.textContent = msg;
    tipEl.style.display = 'block';
    const r = target.getBoundingClientRect();
    const pad = 8;
    let top;
    if (placeAbove) {
      // For compact footer icons: tooltip above the element
      top = r.top - tipEl.offsetHeight - pad;
      top = Math.max(4, top);
    } else {
      // Default behavior: tooltip below the element
      top = r.bottom + pad;
    }
    const left = Math.max(8, Math.min(window.innerWidth - tipEl.offsetWidth - 8, r.left + r.width / 2 - (tipEl.offsetWidth / 2)));
    tipEl.style.top = `${top}px`;
    tipEl.style.left = `${left}px`;
  }
  function hideTip() {
    if (tipEl) tipEl.style.display = 'none';
  }
  function attachTooltip(el, options = {}) {
    el.addEventListener('mouseenter', () => showTip(el, options));
    el.addEventListener('mouseleave', hideTip);
    el.addEventListener('focus', () => showTip(el, options));
    el.addEventListener('blur', hideTip);
    el.addEventListener('pointerdown', (event) => {
      if (event.pointerType !== 'mouse') hideTip();
    });
    el.addEventListener('click', hideTip);
  }
  document.addEventListener('touchstart', hideTip, { passive: true, capture: true });
  document.addEventListener('scroll', hideTip, { passive: true, capture: true });
  window.addEventListener('blur', hideTip);
  // Attach default (below) tooltips to snippet buttons
  ['while','for','if','switch','func','decl'].forEach(k => {
    const btn = document.getElementById(`btn-snippet-${k}`);
    if (!btn) return;
    attachTooltip(btn);
  });
  // Attach "above" tooltip only to the bug icon link in the footer
  const bugLink = document.querySelector('footer a[data-tooltip]');
  const bugBtn = document.getElementById('bug-report-btn');
  const bugTarget = bugBtn || bugLink;
  if (bugTarget) {
    attachTooltip(bugTarget, { placeAbove: true });
  }
  // Attach "above" tooltips to docs, tour, and dev-mode buttons in footer
  ['docs-page-btn', 'tour-restart-btn', 'dev-mode-toggle'].forEach(id => {
    const btn = document.getElementById(id);
    if (!btn) return;
    attachTooltip(btn, { placeAbove: true });
  });
  // Attach default (below) tooltips to header controls
  ['opt', 'examples', 'view', 'btn-share', 'btn-help'].forEach(id => {
    const el = document.getElementById(id);
    if (!el) return;
    attachTooltip(el);
  });
  // Attach default (below) tooltips to IO toolbar controls
  ['io-select', 'io-add-file', 'args', 'btn-run', 'btn-debug', 'btn-stop'].forEach(id => {
    const el = document.getElementById(id);
    if (!el) return;
    attachTooltip(el);
  });
  // Attach "above" tooltip to fullscreen close button
  const fsClose = document.getElementById('fs-close');
  if (fsClose) {
    attachTooltip(fsClose, { placeAbove: true });
  }
  document.querySelectorAll('.pane-title-btn').forEach(btn => attachTooltip(btn));
})();

// Bug report button copies diagnostics to clipboard and opens GitHub issues
(() => {
  const btn = document.getElementById('bug-report-btn');
  if (!btn) return;
  const baseUrl = 'https://github.com/resetius/qumir/issues/new';

  function collectDiagnostics() {
    const code = getCode();
    const args = ($('#args') && $('#args').value) || '';
    const stdin = ($('#stdin') && $('#stdin').value) || '';
    const stdout = ($('#stdout') && $('#stdout').textContent) || '';
    const view = ($('#view') && $('#view').value) || '';
    const opt = ($('#opt') && $('#opt').value) || '';
    const revisionEl = document.getElementById('version');
    const revision = revisionEl ? revisionEl.textContent.trim() : '';
    let files = [];
    try {
      const active = getActiveProject();
      if (active && Array.isArray(active.files)) {
        files = active.files.map(f => ({ name: f.name, content: f.content }));
      }
    } catch (_) {}

    const lines = [];
    lines.push('### Программа');
    lines.push('```kumir');
    lines.push(code);
    lines.push('```');
    lines.push('');
    lines.push('### Входные данные (stdin)');
    lines.push('```');
    lines.push(stdin);
    lines.push('```');
    lines.push('');
    lines.push('### Аргументы запуска');
    lines.push(args || '—');
    lines.push('');
    lines.push('### Файлы');
    if (files.length === 0) {
      lines.push('нет файлов');
    } else {
      files.forEach(f => {
        lines.push('#### ' + f.name);
        lines.push('```');
        lines.push(f.content || '');
        lines.push('```');
      });
    }
    lines.push('');
    lines.push('### Вывод компилятора / программы');
    lines.push('```');
    lines.push(stdout);
    lines.push('```');
    lines.push('');
    lines.push('### Среда');
    lines.push(`view: ${view || '—'}, opt: ${opt || '—'}`);
    if (revision) lines.push(`version: ${revision}`);

    return lines.join('\n');
  }

  async function copyDiagnostics() {
    const text = collectDiagnostics();
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(text);
      } else {
        const ta = document.createElement('textarea');
        ta.value = text;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
      }
      showToast('Диагностика скопирована в буфер обмена');
    } catch (_) {
      showToast('Не удалось скопировать диагностику');
    }
  }

  btn.addEventListener('click', async () => {
    await copyDiagnostics();
    const title = 'Баг в Qumir Playground';
    const shortBody = [
      'Опишите, пожалуйста, что вы ожидали и что произошло. ',
      'Полная диагностика (код, stdin, файлы, вывод) уже скопирована в буфер обмена — ',
      'просто вставьте её сюда (Cmd+V / Ctrl+V).',
      '',
      '---'
    ].join('\n');
    const url = `${baseUrl}?title=${encodeURIComponent(title)}&body=${encodeURIComponent(shortBody)}`;
    try { window.open(url, '_blank', 'noopener,noreferrer'); } catch (_) {
      window.location.href = url;
    }
  });
})();

// Debounce auto-show on code edits to avoid spamming service
let showTimer = null;
const debounceShow = () => {
  if (embedded) {
    return;
  }
  if (showTimer) clearTimeout(showTimer);
  showTimer = setTimeout(() => show($('#view').value), 350);
};
// Textarea fallback listener is not needed when CodeMirror is used

// Restore syntax mode before first compile so api() sends the correct header
{
  const sel = document.getElementById('syntax-mode');
  if (sel) {
    const syntaxMode = readPersistedValue('qumir-syntax-mode') || 'kumir';
    sel.value = syntaxMode === 'core' ? 'core' : 'kumir';
  }
}

// Auto show on first load
show($('#view').value);

// Run hint arrow logic
(function setupRunHint() {
  if (embedded) {
    return;
  }
  let hintTimer = null;
  let arrowEl = null;
  let hasCompilationErrors = false;
  let hasRunOnce = false;

  function createArrow() {
    if (arrowEl) return arrowEl;
    arrowEl = document.createElement('div');
    arrowEl.className = 'run-hint-arrow';
    arrowEl.textContent = '↓';
    arrowEl.setAttribute('aria-hidden', 'true');
    document.body.appendChild(arrowEl);
    return arrowEl;
  }

  function showArrow() {
    if (hasRunOnce || hasCompilationErrors) return;
    const runBtn = document.getElementById('btn-run');
    if (!runBtn) return;
    const arrow = createArrow();
    const rect = runBtn.getBoundingClientRect();
    arrow.style.left = `${rect.left + rect.width / 2 - 16}px`;
    arrow.style.top = `${rect.top - 40}px`;
    arrow.classList.add('show');
  }

  function hideArrow() {
    if (arrowEl) {
      arrowEl.classList.remove('show');
    }
  }

  function resetHintTimer() {
    if (hintTimer) clearTimeout(hintTimer);
    hideArrow();
    if (!hasRunOnce && !hasCompilationErrors) {
      hintTimer = setTimeout(showArrow, 10000); // 10 seconds
    }
  }

  function onCompilationResult(hasErrors) {
    hasCompilationErrors = hasErrors;
    if (hasErrors) {
      hideArrow();
      if (hintTimer) clearTimeout(hintTimer);

    } else {
      resetHintTimer();
    }
  }

  function onRun() {
    hasRunOnce = true;
    hideArrow();
    if (hintTimer) clearTimeout(hintTimer);
  }

  // Export to global scope for use in show() function
  window.__runHintOnCompilationResult = onCompilationResult;
  window.__runHintOnRun = onRun;

  // Start timer on first load (assuming no errors initially)
  resetHintTimer();
})();

// Ensure Run also refreshes the right pane
$('#btn-run').addEventListener('click', async () => {
  if (window.__runHintOnRun) window.__runHintOnRun();

  // A user who set breakpoints expects Run to stop at them, not at the first statement.
  activeRun = runWasm(debuggerUI?.breakpoints.size > 0, { stopAtEntry: false });
  await activeRun;
  activeRun = null;
  show($('#view').value, { clearErrorsOnSuccess: false });
});

if (!embedded) {
  if (editor) debuggerUI = new DebuggerUI(editor, document.getElementById('debug-panel'));
  $('#btn-debug').disabled = !debuggerUI;
  $('#btn-debug').addEventListener('click', async () => {
    if (window.__runHintOnRun) {
      window.__runHintOnRun();
    }
    await runWasm(true);
  });
}

$('#btn-stop').addEventListener('click', () => {
  signalCoroStop();
});

// Fullscreen viewer for outputs (stdout/output)
(function setupFullscreenViewer(){
  const overlay = document.getElementById('fs-overlay');
  const body = document.getElementById('fs-body');
  const title = document.getElementById('fs-title');
  const closeBtn = document.getElementById('fs-close');
  if (!overlay || !body || !title || !closeBtn) return;
  let restore = null;
  const open = (label, nodeOrText) => {
    title.textContent = label || 'Output';
    body.innerHTML = '';
    overlay.classList.add('show');
    overlay.setAttribute('aria-hidden', 'false');
    // If node passed, move it; otherwise render text in <pre>
    if (nodeOrText && (nodeOrText.nodeType === 1)) {
      const node = nodeOrText;
      const parent = node.parentNode;
      const next = node.nextSibling;
      body.appendChild(node);
      // Special handling for CodeMirror: refresh after move
      try { if (editor) { editor.refresh(); } } catch {}
      restore = () => { if (parent) parent.insertBefore(node, next); try { if (editor) { editor.refresh(); } } catch {} };
    } else {
      const pre = document.createElement('pre');
      pre.textContent = nodeOrText || '';
      body.appendChild(pre);
      restore = null;
    }
  };
  const close = () => {
    overlay.classList.remove('show');
    overlay.setAttribute('aria-hidden', 'true');
    if (restore) { try { restore(); } finally { restore = null; } }
  };
  closeBtn.addEventListener('click', close);
  overlay.addEventListener('click', (e) => { if (e.target === overlay) close(); });
  window.addEventListener('keydown', (e) => { if (e.key === 'Escape') close(); });
  const compOutEl = document.getElementById('output');
  const titleOutput = document.getElementById('title-output');
  if (compOutEl && titleOutput) {
    titleOutput.addEventListener('mousedown', e => e.preventDefault());
    titleOutput.addEventListener('click', (e) => {
      if (e.target.closest('.pane-title-btn')) return;
      const node = getCurrentCompilerOutputNode();
      open('Компилятор', node);
    });
  }
  const titleIo = document.getElementById('title-io');
  if (titleIo) {
    titleIo.addEventListener('mousedown', e => e.preventDefault());
    titleIo.addEventListener('click', (e) => {
      if (e.target.closest('.pane-title-btn')) return;
      const node = getCurrentIoPaneNode();
      if (node) open(`Ввод/Вывод • ${getCurrentIoPaneLabel()}`, node);
    });
  }
  const titleCode = document.getElementById('title-code');
  if (titleCode) {
    titleCode.addEventListener('mousedown', e => e.preventDefault());
    titleCode.addEventListener('click', (e) => {
      if (e.target.closest('.pane-title-btn')) return;
      // Move the CodeMirror wrapper if exists, otherwise textarea
      const cm = editor && editor.getWrapperElement ? editor.getWrapperElement() : null;
      if (cm) open('Код', cm); else open('Код', document.getElementById('code'));
    });
  }
  const titlePreview = document.getElementById('title-preview');
  if (titlePreview) {
    titlePreview.addEventListener('mousedown', e => e.preventDefault());
    titlePreview.addEventListener('click', (e) => {
      if (e.target.closest('.pane-title-btn')) return;
      const node = getCurrentPreviewNode();
      if (node) open('Просмотр', node);
    });
  }
  document.querySelectorAll('.pane-title-btn[data-pane-action="fullscreen"]').forEach(btn => {
    btn.addEventListener('click', (e) => {
      e.preventDefault();
      e.stopPropagation();
      const target = btn.dataset.paneTarget;
      if (target === 'code') {
        const cm = editor && editor.getWrapperElement ? editor.getWrapperElement() : null;
        open('Код', cm || document.getElementById('code'));
      } else if (target === 'output') {
        open('Компилятор', getCurrentCompilerOutputNode());
      } else if (target === 'io') {
        const node = getCurrentIoPaneNode();
        if (node) open(`Ввод/Вывод • ${getCurrentIoPaneLabel()}`, node);
      } else if (target === 'preview') {
        const node = getCurrentPreviewNode();
        if (node) open('Просмотр', node);
      }
    });
  });
})();

export function embeddedExampleData() {
  if (!embedded) {
    throw new Error('Страница не является учебным примером');
  }
  return exampleData(captureCurrentEditorState());
}

export async function stopEmbeddedExample() {
  if (!embedded) {
    return;
  }
  signalCoroStop();
  await activeRun;
}

export async function loadEmbeddedExample(value) {
  if (!embedded) {
    throw new Error('Страница не является учебным примером');
  }
  const data = exampleData(value);
  await stopEmbeddedExample();
  clearErrorHighlights();
  __robotEditEnabled = false;
  __executorToolbarMode = null;
  __turtleModule?.__resetTurtle?.(true);
  __drawerModule?.__resetDrawer?.(true);
  __painterModule?.__resetPainter?.();
  setCompilerOutputMode('text');
  $('#stdout').textContent = '';
  await applyProjectToInputs({ ...data, args: '' }, { silent: true });
  setErrorsPaneContent('Нажмите «Запустить», чтобы проверить пример');
  setActiveIoPane(data.stdin ? 'stdin' : 'stdout', { persistCookie: false });
}

const importId = new URLSearchParams(location.search).get('doc-import');
if (!embedded) {
  if (importId) {
    try {
      const data = consumeExample(importId);
      updateActiveProjectFromInputs();
      createProject({ name: 'Пример из документации', ...data }, { activate: true });
      saveState();
      debounceShow();
    } catch (error) {
      setErrorsPaneContent(error.message, { isError: true });
      setActiveIoPane('errors', { persistCookie: false });
    }
    const url = new URL(location.href);
    url.searchParams.delete('doc-import');
    history.replaceState(null, '', url);
  }
}

// Toast helper
let __toastEl = null;
let __toastTimer = null;
function showToast(message, ms = 2000) {
  if (!__toastEl) {
    __toastEl = document.createElement('div');
    __toastEl.className = 'q-toast';
    document.body.appendChild(__toastEl);
  }
  __toastEl.textContent = message;
  __toastEl.classList.add('show');
  if (__toastTimer) clearTimeout(__toastTimer);
  __toastTimer = setTimeout(() => {
    __toastEl.classList.remove('show');
  }, Math.max(500, ms|0));
}

// Share: POST current code to /api/share and copy link
const btnShare = document.getElementById('btn-share');
if (btnShare) {
  btnShare.addEventListener('click', async () => {
    const code = getCode();
  const args = $('#args') ? $('#args').value : '';
  const stdin = $('#stdin') ? $('#stdin').value : '';
  // Collect IO files into a simple name->content map
  const files = {};
  for (const file of __ioFiles) {
    const name = canonicalIoFileName(file.name || '');
    const key = name || file.id;
    if (!key) continue;
    files[key] = file.content || '';
  }
    try {
      const r = await fetch('/api/share', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ code, args, stdin, files })
      });
      if (!r.ok) throw new Error(await r.text());
      const res = await r.json();
      const url = res && res.url ? res.url : (res.raw_url || '');
      if (url) {
        try { await navigator.clipboard.writeText(url); } catch {}
        // Update location without reload
        try {
          if (res.id) {
            const pretty = `/s/${encodeURIComponent(res.id)}`;
            window.history.replaceState({}, '', pretty);
          }
        } catch {}
        showToast('Ссылка скопирована в буфер обмена', 2000);
      }
    } catch (e) {
      alert('Не удалось создать ссылку: ' + (e.message || String(e)));
    }
  });
}

// Developer mode toggle
(function setupDevMode() {
  const devModeBtn = document.getElementById('dev-mode-toggle');
  if (!devModeBtn) return;

  // Load saved state from localStorage
  const savedDevMode = localStorage.getItem('qumir-dev-mode') === 'true';
  if (savedDevMode) {
    document.body.classList.add('dev-mode');
  }

  // Toggle dev mode on click
  devModeBtn.addEventListener('click', () => {
    const isDevMode = document.body.classList.toggle('dev-mode');
    localStorage.setItem('qumir-dev-mode', isDevMode);
    normalizeWorkspaceAfterDevModeToggle(isDevMode);

    if (!isDevMode) {
      // Switch to I/O view when leaving dev mode (avoid errors pane)
      const ioSelect = document.getElementById('io-select');
      if (ioSelect && ioSelect.value !== 'stdout' && ioSelect.value !== 'stdin') {
        ioSelect.value = 'stdout';
        ioSelect.dispatchEvent(new Event('change'));
      }
    }
  });
})();

// Auto-start onboarding when desktop initialization is idle; mobile users can start it manually.
(function setupTour() {
  const restartBtn = document.getElementById('tour-restart-btn');
  if (!restartBtn) return;

  const loadTour = () => import('./tour.js');

  restartBtn.addEventListener('click', async () => {
    try {
      const tour = await loadTour();
      tour.resetTour();
      tour.startTour();
    } catch (e) {
      console.warn('Tour module not available:', e);
    }
  });

  if (window.innerWidth <= 900) return;
  // Visitors who came by a link to a program came for that program.
  const params = new URLSearchParams(window.location.search);
  if (params.has('example') || params.has('share') || importId || window.location.pathname.startsWith('/s/')) return;

  const initTour = async () => {
    try {
      const tour = await loadTour();
      tour.initTour();
    } catch (e) {
      console.warn('Tour module not available:', e);
    }
  };

  if ('requestIdleCallback' in window) {
    window.requestIdleCallback(initTour, { timeout: 5000 });
  } else {
    window.setTimeout(initTour, 2000);
  }
})();

// Documentation is outside the critical path and initializes on first use.
(function setupDocs() {
  const helpBtn = document.getElementById('btn-help');
  const docsPageBtn = document.getElementById('docs-page-btn');
  let docsPromise = null;

  if (docsPageBtn) {
    docsPageBtn.addEventListener('click', () => {
      window.open('/docs.html', '_blank');
    });
  }

  if (!helpBtn) return;

  helpBtn.addEventListener('click', async () => {
    try {
      docsPromise ||= import('./docs.js').then(docs => {
        docs.initDocs({ bindOpenButtons: false });
        return docs;
      });
      const docs = await docsPromise;
      docs.openDocs();
    } catch (e) {
      console.warn('Documentation module not available:', e);
    }
  });
})();
