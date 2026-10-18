// Drawer (Чертежник) graphics runtime for Qumir WebAssembly
// Colors come from colors.js (цвет = ARGB BigInt, same as painter).
// Provides C-style symbols:
//   void drawer_pen_up();
//   void drawer_pen_down();
//   void drawer_set_color(int64_t color);
//   Future<void> drawer_move_to(double x, double y);
//   Future<void> drawer_move_by(double dx, double dy);
//   Future<void> drawer_write_text(double width, const char* text);
// Helpers:
//   __bindDrawerCanvas(canvas)
//   __resetDrawer(clear)

import { argbToStyle } from './colors.js';

let ctx = null;              // 2D context
let canvas = null;           // canvas element (logical CSS pixels)
let dpr = 1;                 // devicePixelRatio

const state = {
  x: 0,
  y: 0,
  penDown: true,
  path: [], // segments: [{x1,y1,x2,y2,color}]
  texts: [], // text objects: [{x,y,width,text,color}]
  color: '#000000', // black by default
  width: 2,
  hasPath: false,
  bounds: { minX: Infinity, minY: Infinity, maxX: -Infinity, maxY: -Infinity },
};

// View transform (world units -> CSS pixels)
const view = {
  scale: 24,      // px per unit
  ox: 0,          // offset X in CSS px
  oy: 0,          // offset Y in CSS px
};

let rafScheduled = false;
let autoFitPending = false;
let autoFitTimer = null;
let userInteracted = false;

function scheduleDraw() {
  if (rafScheduled || !ctx) return;
  rafScheduled = true;
  requestAnimationFrame(() => {
    rafScheduled = false;
    draw();
  });
}

function toScreen(x, y) {
  return {
    x: x * view.scale + view.ox,
    y: -y * view.scale + view.oy,
  };
}

function toWorld(sx, sy) {
  return {
    x: (sx - view.ox) / view.scale,
    y: -(sy - view.oy) / view.scale,
  };
}

function fitCanvasToCss() {
  if (!canvas) return;
  const rect = canvas.getBoundingClientRect();
  dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.floor(rect.width));
  const h = Math.max(1, Math.floor(rect.height));
  if (canvas.width !== Math.floor(w * dpr) || canvas.height !== Math.floor(h * dpr)) {
    canvas.width = Math.floor(w * dpr);
    canvas.height = Math.floor(h * dpr);
  }
  if (ctx) {
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
}

function pickGridStep(scale) {
  // Aim ~40-80 px between major lines, use 1-2-5 progression
  const target = 56;
  const raw = target / Math.max(1e-6, scale);
  const pow10 = Math.pow(10, Math.floor(Math.log10(raw)));
  const units = raw / pow10;
  const step = units < 2 ? 1 : (units < 5 ? 2 : 5);
  return step * pow10;
}

function drawGridAxes() {
  const { width, height } = canvas;
  const cssW = width / dpr;
  const cssH = height / dpr;

  // Background
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, cssW, cssH);

  // Grid
  const step = pickGridStep(view.scale);
  const stepPx = step * view.scale;

  const leftW = 0;
  const topH = 0;
  const rightW = cssW;
  const bottomH = cssH;

  ctx.save();
  ctx.beginPath();
  ctx.lineWidth = 1;
  ctx.strokeStyle = '#e9e9ef';

  // Vertical grid lines
  const x0World = toWorld(leftW, 0).x;
  const x1World = toWorld(rightW, 0).x;
  const startX = Math.floor(x0World / step) * step;
  for (let x = startX; x <= x1World + step; x += step) {
    const sx = toScreen(x, 0).x;
    ctx.moveTo(sx, topH);
    ctx.lineTo(sx, bottomH);
  }

  // Horizontal grid lines
  const y0World = toWorld(0, bottomH).y;
  const y1World = toWorld(0, topH).y;
  const startY = Math.floor(y0World / step) * step;
  for (let y = startY; y <= y1World + step; y += step) {
    const sy = toScreen(0, y).y;
    ctx.moveTo(leftW, sy);
    ctx.lineTo(rightW, sy);
  }
  ctx.stroke();
  ctx.restore();

  // Axes
  ctx.save();
  ctx.lineWidth = 2;
  ctx.strokeStyle = '#b3b3c6';
  // X axis (y=0)
  const y0 = toScreen(0, 0).y;
  ctx.beginPath();
  ctx.moveTo(leftW, y0);
  ctx.lineTo(rightW, y0);
  ctx.stroke();
  // Y axis (x=0)
  const x0 = toScreen(0, 0).x;
  ctx.beginPath();
  ctx.moveTo(x0, topH);
  ctx.lineTo(x0, bottomH);
  ctx.stroke();
  ctx.restore();
}

function drawPathAndCursor() {
  const cssW = canvas.width / dpr;
  const cssH = canvas.height / dpr;

  // Draw path segments
  ctx.save();
  ctx.lineWidth = state.width;
  ctx.lineCap = 'round';

  for (const seg of state.path) {
    ctx.strokeStyle = seg.color;
    ctx.beginPath();
    const p1 = toScreen(seg.x1, seg.y1);
    const p2 = toScreen(seg.x2, seg.y2);
    ctx.moveTo(p1.x, p1.y);
    ctx.lineTo(p2.x, p2.y);
    ctx.stroke();
  }
  ctx.restore();

  // Draw text
  ctx.save();
  for (const txt of state.texts) {
    const pos = toScreen(txt.x, txt.y);
    ctx.fillStyle = txt.color;
    // Calculate font size from width
    const fontSize = Math.max(8, txt.width / txt.text.length * 1.5);
    ctx.font = `${fontSize}px monospace`;
    ctx.fillText(txt.text, pos.x, pos.y);
  }
  ctx.restore();

  // Draw cursor (pen position indicator)
  const pos = toScreen(state.x, state.y);
  const size = 8;

  ctx.save();
  if (state.penDown) {
    // Filled circle when pen is down
    ctx.fillStyle = state.color;
    ctx.beginPath();
    ctx.arc(pos.x, pos.y, size / 2, 0, Math.PI * 2);
    ctx.fill();
    ctx.strokeStyle = '#000';
    ctx.lineWidth = 1;
    ctx.stroke();
  } else {
    // Empty circle when pen is up
    ctx.strokeStyle = state.color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.arc(pos.x, pos.y, size / 2, 0, Math.PI * 2);
    ctx.stroke();
  }
  ctx.restore();
}

function draw() {
  if (!ctx || !canvas) return;
  fitCanvasToCss();
  drawGridAxes();
  drawPathAndCursor();
}

function resetView() {
  if (!canvas) return;
  const rect = canvas.getBoundingClientRect();
  view.ox = rect.width / 2;
  view.oy = rect.height / 2;
  view.scale = 24;
}

function scheduleAutoFit() {
  if (!autoFitPending || userInteracted) return;
  if (autoFitTimer) clearTimeout(autoFitTimer);
  // Debounce so we fit once shortly after drawing settles
  autoFitTimer = setTimeout(() => {
    autoFitTimer = null;
    if (!userInteracted) fitToContent();
  }, 150);
}

function fitToContent() {
  if (!canvas || !state.hasPath) return false;
  // If canvas is hidden (display:none), its CSS size is near zero; defer fitting until visible
  const rect0 = canvas.getBoundingClientRect();
  if ((rect0.width|0) < 10 || (rect0.height|0) < 10) {
    // Keep autoFitPending so we can refit on show
    return false;
  }
  const { minX, minY, maxX, maxY } = state.bounds;
  if (!(isFinite(minX) && isFinite(minY) && isFinite(maxX) && isFinite(maxY))) return false;
  const rect = canvas.getBoundingClientRect();
  const cssW = Math.max(1, Math.floor(rect.width));
  const cssH = Math.max(1, Math.floor(rect.height));
  const w = Math.max(1e-6, maxX - minX);
  const h = Math.max(1e-6, maxY - minY);
  const padPx = 24;
  const scaleX = (cssW - padPx * 2) / w;
  const scaleY = (cssH - padPx * 2) / h;
  // Allow much smaller zoom-out for large drawings
  const s = Math.max(0.15, Math.min(800, Math.min(scaleX, scaleY)));
  view.scale = s;
  const cx = (minX + maxX) / 2;
  const cy = (minY + maxY) / 2;
  // Center content
  const centerScreenX = cssW / 2;
  const centerScreenY = cssH / 2;
  const screen = toScreen(cx, cy);
  view.ox += centerScreenX - screen.x;
  view.oy += centerScreenY - screen.y;
  scheduleDraw();
  return true;
}

function contentNeedsFocus(padPx = 32) {
  if (!canvas || !state.hasPath) return false;
  const rect = canvas.getBoundingClientRect();
  const cssW = Math.max(1, Math.floor(rect.width));
  const cssH = Math.max(1, Math.floor(rect.height));
  if (cssW < 10 || cssH < 10) return true;
  const { minX, minY, maxX, maxY } = state.bounds;
  if (!(isFinite(minX) && isFinite(minY) && isFinite(maxX) && isFinite(maxY))) return false;
  const p1 = toScreen(minX, minY);
  const p2 = toScreen(maxX, maxY);
  const left = Math.min(p1.x, p2.x);
  const right = Math.max(p1.x, p2.x);
  const top = Math.min(p1.y, p2.y);
  const bottom = Math.max(p1.y, p2.y);
  return left < padPx || right > cssW - padPx || top < padPx || bottom > cssH - padPx;
}

function focusContentAfterChange() {
  if (userInteracted) return false;
  autoFitPending = true;
  if (!contentNeedsFocus()) return false;
  if (autoFitTimer) {
    clearTimeout(autoFitTimer);
    autoFitTimer = null;
  }
  return fitToContent();
}

function onWheel(e) {
  if (!canvas) return;
  e.preventDefault();
  const rect = canvas.getBoundingClientRect();
  const sx = (e.clientX - rect.left);
  const sy = (e.clientY - rect.top);

  if (e.ctrlKey || e.metaKey) {
    // Zoom
    const factor = Math.pow(1.006, -e.deltaY);
    const oldScale = view.scale;
    const newScale = Math.max(0.15, Math.min(800, oldScale * factor));
    if (newScale !== oldScale) {
      const w = toWorld(sx, sy);
      view.scale = newScale;
      const s = toScreen(w.x, w.y);
      view.ox += sx - s.x;
      view.oy += sy - s.y;
      scheduleDraw();
    }
    userInteracted = true;
  } else {
    // Pan via wheel scroll
    view.ox -= e.deltaX;
    view.oy -= e.deltaY;
    scheduleDraw();
    userInteracted = true;
  }
}

let drag = null;
function onPointerDown(e) {
  if (!canvas) return;
  canvas.setPointerCapture(e.pointerId);
  drag = { id: e.pointerId, x: e.clientX, y: e.clientY };
  userInteracted = true;
}
function onPointerMove(e) {
  if (!drag || drag.id !== e.pointerId) return;
  const dx = e.clientX - drag.x;
  const dy = e.clientY - drag.y;
  drag.x = e.clientX;
  drag.y = e.clientY;
  view.ox += dx;
  view.oy += dy;
  scheduleDraw();
}
function onPointerUp(e) {
  if (drag && drag.id === e.pointerId) {
    drag = null;
  }
}

function attachEvents() {
  if (!canvas) return;
  canvas.addEventListener('wheel', onWheel, { passive: false });
  canvas.addEventListener('pointerdown', onPointerDown);
  window.addEventListener('pointermove', onPointerMove);
  window.addEventListener('pointerup', onPointerUp);
}

function detachEvents() {
  if (!canvas) return;
  canvas.removeEventListener('wheel', onWheel);
  canvas.removeEventListener('pointerdown', onPointerDown);
  window.removeEventListener('pointermove', onPointerMove);
  window.removeEventListener('pointerup', onPointerUp);
}

export function __bindDrawerCanvas(cnv) {
  if (canvas === cnv) return;
  detachEvents();
  canvas = cnv || null;
  ctx = canvas ? canvas.getContext('2d') : null;
  if (canvas && ctx) {
    fitCanvasToCss();
    resetView();
    attachEvents();
    scheduleDraw();
    // After binding, if we've already drawn something before, a fit may help
    scheduleAutoFit();
  }
}

export function __resetDrawer(clear = true) {
  state.x = 0;
  state.y = 0;
  state.penDown = true;
  state.color = '#000000';
  state.hasPath = false;
  state.bounds = { minX: Infinity, minY: Infinity, maxX: -Infinity, maxY: -Infinity };
  autoFitPending = true;
  userInteracted = false;
  if (autoFitTimer) { clearTimeout(autoFitTimer); autoFitTimer = null; }
  if (clear) {
    state.path = [];
    state.texts = [];
  }
  scheduleDraw();
}

// Notify runtime that canvas has just become visible; ensure correct sizing and fit
export function __onCanvasShown() {
  if (!canvas) return;
  fitCanvasToCss();
  if (autoFitPending) {
    // Try to fit now that we have real dimensions
    fitToContent();
    // If still pending due to immediate zero size, schedule another try
    scheduleAutoFit();
  } else {
    scheduleDraw();
  }
}

export function __fitDrawerView() {
  if (!canvas) return;
  userInteracted = false;
  autoFitPending = true;
  fitCanvasToCss();
  fitToContent();
  scheduleAutoFit();
}

function updateBounds(x, y) {
  const b = state.bounds;
  b.minX = Math.min(b.minX, x);
  b.maxX = Math.max(b.maxX, x);
  b.minY = Math.min(b.minY, y);
  b.maxY = Math.max(b.maxY, y);
}

// JS future runtime — set by __bindFutureRuntime from app.js.
let __futureRuntime = null;
export function __bindFutureRuntime(fr) { __futureRuntime = fr; }

function _drawerOp(execute) {
  if (!__futureRuntime) { execute(); return 0; }
  const h = __futureRuntime.allocFuture();
  __futureRuntime.enqueuePendingOp({ h, execute });
  return h;
}

export function drawer_pen_up() {
  state.penDown = false;
  scheduleDraw();
}

export function drawer_pen_down() {
  state.penDown = true;
  scheduleDraw();
}

export function drawer_set_color(color) {
  state.color = argbToStyle(color);
  scheduleDraw();
}

export function drawer_move_to(x, y) {
  const nx = Number(x) || 0;
  const ny = Number(y) || 0;
  return _drawerOp(() => {
    if (state.penDown) {
      state.path.push({
        x1: state.x,
        y1: state.y,
        x2: nx,
        y2: ny,
        color: state.color
      });
      updateBounds(state.x, state.y);
      updateBounds(nx, ny);
      state.hasPath = true;
    }

    state.x = nx;
    state.y = ny;
    if (!focusContentAfterChange()) scheduleDraw();
  });
}

export function drawer_move_by(dx, dy) {
  const ddx = Number(dx) || 0;
  const ddy = Number(dy) || 0;
  const nx = state.x + ddx;
  const ny = state.y + ddy;
  return _drawerOp(() => {
    if (state.penDown) {
      state.path.push({
        x1: state.x,
        y1: state.y,
        x2: nx,
        y2: ny,
        color: state.color
      });
      updateBounds(state.x, state.y);
      updateBounds(nx, ny);
      state.hasPath = true;
    }

    state.x = nx;
    state.y = ny;
    if (!focusContentAfterChange()) scheduleDraw();
  });
}


// Need access to string runtime for loading strings from handles/memory
let stringRuntime = null;

export function __bindStringRuntime(runtime) {
  stringRuntime = runtime;
}

export function drawer_write_text(width, textPtr) {
  // textPtr can be:
  // - A negative number (JS string handle from string pool)
  // - A non-negative number (C-string pointer in WASM memory)
  // - A JS string (when called from IR interpreter)
  let text = '';

  if (typeof textPtr === 'string') {
    // Direct JS string from IR interpreter
    text = textPtr;
  } else if (typeof textPtr === 'number') {
    // Handle or pointer - use string runtime to load it
    if (stringRuntime && typeof stringRuntime.__loadString === 'function') {
      text = stringRuntime.__loadString(textPtr);
    } else {
      // Fallback if string runtime not available
      text = String(textPtr);
    }
  } else {
    text = String(textPtr);
  }

  const w = Number(width) || 50;

  return _drawerOp(() => {
    state.texts.push({
      x: state.x,
      y: state.y,
      width: w,
      text: text,
      color: state.color
    });

    updateBounds(state.x, state.y);
    updateBounds(state.x + w / view.scale, state.y);
    state.hasPath = true;

    if (!focusContentAfterChange()) scheduleDraw();
  });
}
