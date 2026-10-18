// Turtle graphics runtime for Qumir WebAssembly
// Provides C-style symbols:
//   void turtle_pen_up();
//   void turtle_pen_down();
//   void turtle_forward(double d);
//   void turtle_backward(double d);
//   void turtle_turn_left(double deg);
//   void turtle_turn_right(double deg);
// Helpers:
//   __bindTurtleCanvas(canvas)
//   __resetTurtle(clear)

let ctx = null;              // 2D context
let canvas = null;           // canvas element (logical CSS pixels)
let dpr = 1;                 // devicePixelRatio

const state = {
  x: 0,
  y: 0,
  ang: 0, // degrees, 0: +X (to the right), positive is counter-clockwise
  penDown: true,
  path: [], // segments: [{x1,y1,x2,y2}]
  color: '#111',
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
// Stack for save/restore commands
const savedStack = [];

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

function drawPathAndTurtle() {
  const cssW = canvas.width / dpr;
  const cssH = canvas.height / dpr;

  // Path
  ctx.save();
  ctx.lineWidth = state.width;
  ctx.lineCap = 'round';
  ctx.strokeStyle = state.color;
  ctx.beginPath();
  for (const seg of state.path) {
    const p1 = toScreen(seg.x1, seg.y1);
    const p2 = toScreen(seg.x2, seg.y2);
    ctx.moveTo(p1.x, p1.y);
    ctx.lineTo(p2.x, p2.y);
  }
  ctx.stroke();
  ctx.restore();

  // Turtle icon
  const pos = toScreen(state.x, state.y);
  const angRad = (state.ang * Math.PI) / 180;
  const size = Math.max(6, Math.min(18, 10 + Math.log10(view.scale + 1) * 6));
  const nose = { x: pos.x + Math.cos(-angRad) * size, y: pos.y + Math.sin(-angRad) * size };
  const left = { x: pos.x + Math.cos(-angRad + Math.PI * 0.75) * size * 0.6, y: pos.y + Math.sin(-angRad + Math.PI * 0.75) * size * 0.6 };
  const right = { x: pos.x + Math.cos(-angRad - Math.PI * 0.75) * size * 0.6, y: pos.y + Math.sin(-angRad - Math.PI * 0.75) * size * 0.6 };

  ctx.save();
  ctx.fillStyle = '#0d6efd';
  ctx.strokeStyle = '#09427a';
  ctx.lineWidth = 1.25;
  ctx.beginPath();
  ctx.moveTo(nose.x, nose.y);
  ctx.lineTo(left.x, left.y);
  ctx.lineTo(right.x, right.y);
  ctx.closePath();
  ctx.fill();
  ctx.stroke();
  ctx.restore();
}

function draw() {
  if (!ctx || !canvas) return;
  fitCanvasToCss();
  drawGridAxes();
  drawPathAndTurtle();
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
  if (!canvas || !state.hasPath) return;
  // If canvas is hidden (display:none), its CSS size is near zero; defer fitting until visible
  const rect0 = canvas.getBoundingClientRect();
  if ((rect0.width|0) < 10 || (rect0.height|0) < 10) {
    // Keep autoFitPending so we can refit on show
    return;
  }
  const { minX, minY, maxX, maxY } = state.bounds;
  if (!(isFinite(minX) && isFinite(minY) && isFinite(maxX) && isFinite(maxY))) return;
  const rect = canvas.getBoundingClientRect();
  const cssW = Math.max(1, Math.floor(rect.width));
  const cssH = Math.max(1, Math.floor(rect.height));
  const w = Math.max(1e-6, maxX - minX);
  const h = Math.max(1e-6, maxY - minY);
  const padPx = 24;
  const scaleX = (cssW - padPx * 2) / w;
  const scaleY = (cssH - padPx * 2) / h;
  // Allow much smaller zoom-out for large fractals
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
}

function onWheel(e) {
  if (!canvas) return;
  e.preventDefault();
  const rect = canvas.getBoundingClientRect();
  const sx = (e.clientX - rect.left);
  const sy = (e.clientY - rect.top);

  if (e.ctrlKey || e.metaKey) {
    // Zoom
    // Slightly faster zoom per wheel delta
    const factor = Math.pow(1.006, -e.deltaY); // slightly faster
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

export function __bindTurtleCanvas(cnv) {
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

export function __resetTurtle(clear = true) {
  state.x = 0;
  state.y = 0;
  state.ang = 0;
  state.penDown = true;
  state.hasPath = false;
  state.bounds = { minX: Infinity, minY: Infinity, maxX: -Infinity, maxY: -Infinity };
  autoFitPending = true;
  userInteracted = false;
  if (autoFitTimer) { clearTimeout(autoFitTimer); autoFitTimer = null; }
  if (clear) state.path = [];
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

export function __fitTurtleView() {
  if (!canvas) return;
  userInteracted = false;
  autoFitPending = true;
  fitCanvasToCss();
  fitToContent();
  scheduleAutoFit();
}

function moveBy(dist) {
  const rad = (state.ang * Math.PI) / 180;
  const nx = state.x + Math.cos(rad) * dist;
  const ny = state.y + Math.sin(rad) * dist;
  if (state.penDown) {
    state.path.push({ x1: state.x, y1: state.y, x2: nx, y2: ny });
    // Update bounds with both endpoints
    const b = state.bounds;
    b.minX = Math.min(b.minX, state.x, nx);
    b.maxX = Math.max(b.maxX, state.x, nx);
    b.minY = Math.min(b.minY, state.y, ny);
    b.maxY = Math.max(b.maxY, state.y, ny);
    state.hasPath = true;
  }
  state.x = nx;
  state.y = ny;
  scheduleDraw();
  scheduleAutoFit();
}

// JS future runtime — set by __bindFutureRuntime from app.js.
let __futureRuntime = null;
export function __bindFutureRuntime(fr) { __futureRuntime = fr; }

function _turtleOp(execute) {
  if (!__futureRuntime) { execute(); return 0; }
  const h = __futureRuntime.allocFuture();
  __futureRuntime.enqueuePendingOp({ h, execute });
  return h;
}

export function turtle_pen_up() {
  return _turtleOp(() => { state.penDown = false; scheduleDraw(); });
}

export function turtle_pen_down() {
  return _turtleOp(() => { state.penDown = true; scheduleDraw(); });
}

export function turtle_forward(d) {
  const dist = Number(d) || 0;
  return _turtleOp(() => { moveBy(dist); });
}

export function turtle_backward(d) {
  const dist = Number(d) || 0;
  return _turtleOp(() => { moveBy(-dist); });
}

export function turtle_turn_left(deg) {
  const angle = Number(deg) || 0;
  return _turtleOp(() => { state.ang = (state.ang + angle) % 360; scheduleDraw(); });
}

export function turtle_turn_right(deg) {
  const angle = Number(deg) || 0;
  return _turtleOp(() => { state.ang = (state.ang - angle) % 360; scheduleDraw(); });
}

export function turtle_save_state() {
  return _turtleOp(() => {
    savedStack.push({ x: state.x, y: state.y, ang: state.ang, penDown: state.penDown });
  });
}

export function turtle_restore_state() {
  return _turtleOp(() => {
    if (!savedStack.length) return;
    const s = savedStack.pop();
    state.x = s.x; state.y = s.y; state.ang = s.ang; state.penDown = s.penDown;
    scheduleDraw();
  });
}


// Animation delay for coroutine step pacing (ms between resume steps).
// Default 0 = instant (batch mode, render once at end).
let __turtleAnimDelay = 0;

export function __setAnimationDelay(delay) { __turtleAnimDelay = delay; }
export function __getAnimationDelay() { return __turtleAnimDelay; }
