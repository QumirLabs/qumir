// Painter (Рисователь) runtime for Qumir WebAssembly
// Implements all painter_* host functions as WASM imports.
// Color constants/constructors/decomposition live in colors.js.
// Helpers exported for app.js:
//   __bindMemory(mem)
//   __bindPainterCanvas(canvas)
//   __bindStringRuntime(stringEnv)
//   __resetPainter()
//   __onCanvasShown()
//   __zoomPainterView(factor, cx, cy), __panPainterView(dx, dy), __resetPainterView()
//   __savePainterPng(filename)

import { argbToStyle } from './colors.js';

let MEMORY = null;
let stringRuntime = null;

// Offscreen buffer — all drawing goes here
let offscreen = null;
let offCtx = null;

// Visible canvas
let canvas = null;
let ctx = null;
let dpr = 1;

// Last draw() geometry — exposed via __getDrawGeometry() for ruler display in app.js
let currentOx = 0;
let currentOy = 0;
let currentScale = 1;

// User zoom/pan of the preview on top of the fit-to-canvas scale
let viewZoom = 1;
let viewPanX = 0;
let viewPanY = 0;

let rafScheduled = false;
let rafId = 0;
let pendingSheet = null;
let hasDrawnSinceSheet = false;

const state = {
  sheetW: 800, sheetH: 600,
  penWidth: 1,
  penColor: 0xFF000000n,
  brushColor: 0xFFFFFFFFn,
  hasBrush: true,
  density: 100,
  fontFamily: 'Arial',
  fontSize: 12,
  fontBold: false,
  fontItalic: false,
  curX: 0, curY: 0,
};

// ── Helpers ───────────────────────────────────────────────────────────────────

function wasmAddr(ptr) {
  return Number(BigInt.asUintN(32, BigInt(ptr)));
}

function readCString(ptr) {
  if (!MEMORY) return '';
  if (stringRuntime && typeof stringRuntime.__loadString === 'function') {
    return stringRuntime.__loadString(ptr);
  }
  const addr = typeof ptr === 'bigint' ? wasmAddr(ptr) : Number(ptr);
  const bytes = new Uint8Array(MEMORY.buffer, addr);
  let end = 0;
  while (end < bytes.length && bytes[end] !== 0) end++;
  return new TextDecoder().decode(bytes.subarray(0, end));
}

function writeI64(ptr, value) {
  if (!MEMORY) return;
  new DataView(MEMORY.buffer).setBigInt64(wasmAddr(ptr), BigInt(value), true);
}

function readI64Array(ptr, n) {
  if (!MEMORY) return [];
  const base = wasmAddr(ptr);
  const dv = new DataView(MEMORY.buffer);
  const out = [];
  for (let i = 0; i < n; i++) out.push(Number(dv.getBigInt64(base + i * 8, true)));
  return out;
}

function makeFont() {
  const style  = state.fontItalic ? 'italic ' : '';
  const weight = state.fontBold   ? 'bold '   : '';
  return `${style}${weight}${state.fontSize}px ${state.fontFamily}`;
}

function applyStroke() {
  if (!offCtx) return;
  offCtx.strokeStyle = argbToStyle(state.penColor);
  offCtx.lineWidth   = Number(state.penWidth);
}

function applyFill() {
  if (!offCtx) return;
  offCtx.fillStyle = state.hasBrush ? argbToStyle(state.brushColor) : 'rgba(0,0,0,0)';
}

function scheduleDraw() {
  if (rafScheduled || !ctx || !canvas || !offscreen) return;
  rafScheduled = true;
  rafId = requestAnimationFrame(() => {
    rafScheduled = false;
    rafId = 0;
    draw();
  });
}

function drawNow() {
  if (rafScheduled && rafId) {
    cancelAnimationFrame(rafId);
  }
  rafScheduled = false;
  rafId = 0;
  draw();
}

function draw() {
  if (!ctx || !canvas || !offscreen) return;
  fitCanvas();
  const cssW = canvas.width / dpr;
  const cssH = canvas.height / dpr;
  ctx.clearRect(0, 0, cssW, cssH);
  const scale = fitScale(cssW, cssH) * viewZoom;
  const dw = state.sheetW * scale;
  const dh = state.sheetH * scale;
  const ox = (cssW - dw) / 2 + viewPanX;
  const oy = (cssH - dh) / 2 + viewPanY;
  ctx.imageSmoothingEnabled = scale < 1;
  ctx.drawImage(offscreen, ox, oy, dw, dh);
  currentOx = ox; currentOy = oy; currentScale = scale;
}

function fitScale(cssW, cssH) {
  return Math.min(cssW / state.sheetW, cssH / state.sheetH);
}

function resetView() {
  viewZoom = 1; viewPanX = 0; viewPanY = 0;
}

function fitCanvas() {
  if (!canvas) return;
  const rect = canvas.getBoundingClientRect();
  dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.floor(rect.width));
  const h = Math.max(1, Math.floor(rect.height));
  if (canvas.width !== Math.floor(w * dpr) || canvas.height !== Math.floor(h * dpr)) {
    canvas.width  = Math.floor(w * dpr);
    canvas.height = Math.floor(h * dpr);
    if (ctx) ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
}

function makeOffscreen(w, h) {
  if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(w, h);
  if (typeof document !== 'undefined') {
    const cnv = document.createElement('canvas');
    cnv.width = w;
    cnv.height = h;
    return cnv;
  }
  return null;
}

function applySheet(w, h, color) {
  const sw = Number(w), sh = Number(h);
  if (sw <= 0 || sh <= 0 || sw > 32767 || sh > 32767) return false;
  if (sw !== state.sheetW || sh !== state.sheetH) resetView();
  state.sheetW = sw; state.sheetH = sh;
  offscreen = makeOffscreen(sw, sh);
  offCtx = offscreen ? offscreen.getContext('2d') : null;
  if (offCtx) {
    offCtx.fillStyle = argbToStyle(color);
    offCtx.fillRect(0, 0, sw, sh);
  }
  pendingSheet = null;
  hasDrawnSinceSheet = false;
  return true;
}

function ensureActiveSheet() {
  if (!pendingSheet) return;
  const next = pendingSheet;
  applySheet(next.w, next.h, next.color);
}

function markDrawn() {
  hasDrawnSinceSheet = true;
}

// ── Scanline flood fill ───────────────────────────────────────────────────────

function floodFill(imageData, startX, startY, fr, fg, fb, fa) {
  const { data, width: W, height: H } = imageData;
  const si = (startY * W + startX) * 4;
  const tr = data[si], tg = data[si+1], tb = data[si+2], ta = data[si+3];
  if (tr === fr && tg === fg && tb === fb && ta === fa) return;

  const matches = (x, y) => {
    const i = (y * W + x) * 4;
    return data[i]===tr && data[i+1]===tg && data[i+2]===tb && data[i+3]===ta;
  };
  const fillSpan = (xl, xr, y) => {
    let i = (y * W + xl) * 4;
    for (let x = xl; x <= xr; x++, i += 4) {
      data[i]=fr; data[i+1]=fg; data[i+2]=fb; data[i+3]=fa;
    }
  };

  // Find initial span
  let xl0 = startX, xr0 = startX;
  while (xl0 > 0   && matches(xl0 - 1, startY)) xl0--;
  while (xr0 < W-1 && matches(xr0 + 1, startY)) xr0++;
  fillSpan(xl0, xr0, startY);

  // Stack: [xl, xr, y, dir]  dir = ±1 (direction to continue scanning)
  const stack = [];
  if (startY > 0)   stack.push([xl0, xr0, startY - 1, -1]);
  if (startY < H-1) stack.push([xl0, xr0, startY + 1,  1]);

  while (stack.length > 0) {
    const [lx, rx, y, dir] = stack.pop();
    if (y < 0 || y >= H) continue;

    // Walk across [lx, rx], find and fill all matching spans
    let x = lx;
    while (x <= rx) {
      while (x <= rx && !matches(x, y)) x++;
      if (x > rx) break;

      // Extend span to the left (may go beyond lx)
      let spanL = x;
      while (spanL > 0   && matches(spanL - 1, y)) spanL--;
      // Extend span to the right
      let spanR = x;
      while (spanR < W-1 && matches(spanR + 1, y)) spanR++;

      fillSpan(spanL, spanR, y);

      // Continue in same direction
      const nextY = y + dir;
      if (nextY >= 0 && nextY < H) stack.push([spanL, spanR, nextY, dir]);

      // Extensions beyond [lx,rx] need a reverse scan
      const prevY = y - dir;
      if (prevY >= 0 && prevY < H) {
        if (spanL < lx) stack.push([spanL, lx - 1, prevY, -dir]);
        if (spanR > rx) stack.push([rx + 1, spanR, prevY, -dir]);
      }

      x = spanR + 1;
    }
  }
}

// ── Public helpers ────────────────────────────────────────────────────────────

export function __getDrawGeometry() {
  return { ox: currentOx, oy: currentOy, scale: currentScale,
           sheetW: state.sheetW, sheetH: state.sheetH };
}

export function __getPixelColor(sx, sy) {
  if (!offCtx || sx < 0 || sy < 0 || sx >= state.sheetW || sy >= state.sheetH) return null;
  const px = offCtx.getImageData(sx, sy, 1, 1).data;
  return '#' + [px[0], px[1], px[2]].map(v => v.toString(16).padStart(2, '0').toUpperCase()).join('');
}

export function __bindMemory(mem)          { MEMORY = mem; }
export function __bindStringRuntime(env)   { stringRuntime = env; }

export function __bindPainterCanvas(cnv) {
  canvas = cnv || null;
  ctx    = canvas ? canvas.getContext('2d') : null;
  if (canvas && ctx) {
    fitCanvas();
    scheduleDraw();
  }
}

export function __resetPainter() {
  state.sheetW = 800; state.sheetH = 600;
  state.penWidth = 1; state.penColor = 0xFF000000n;
  state.brushColor = 0xFFFFFFFFn; state.hasBrush = true;
  state.density = 100;
  state.fontFamily = 'Arial'; state.fontSize = 12;
  state.fontBold = false; state.fontItalic = false;
  state.curX = 0; state.curY = 0;
  pendingSheet = null;
  resetView();
  applySheet(state.sheetW, state.sheetH, 0xFFFFFFFFn);
  scheduleDraw();
}

export function __onCanvasShown() {
  if (!canvas) return;
  fitCanvas();
  scheduleDraw();
}

export function __fitPainterView() {
  if (!canvas) return;
  fitCanvas();
  scheduleDraw();
}

// Zoom by `factor` keeping the sheet point under canvas CSS point (cx, cy) fixed.
export function __zoomPainterView(factor, cx, cy) {
  if (!canvas || !offscreen) return;
  const cssW = canvas.width / dpr;
  const cssH = canvas.height / dpr;
  const fit = fitScale(cssW, cssH);
  const zoom = Math.min(Math.max(viewZoom * factor, 1), Math.max(1, 64 / fit));
  if (zoom === viewZoom) return;
  const sx = (cx - currentOx) / currentScale;
  const sy = (cy - currentOy) / currentScale;
  const scale = fit * zoom;
  viewZoom = zoom;
  if (zoom === 1) {
    viewPanX = 0; viewPanY = 0;
  } else {
    viewPanX = cx - sx * scale - (cssW - state.sheetW * scale) / 2;
    viewPanY = cy - sy * scale - (cssH - state.sheetH * scale) / 2;
  }
  drawNow();
}

export function __panPainterView(dx, dy) {
  if (viewZoom === 1) return;
  viewPanX += dx; viewPanY += dy;
  drawNow();
}

export function __resetPainterView() {
  resetView();
  drawNow();
}

export function __savePainterPng(filename) {
  saveOffscreen(filename || 'sheet.png');
}

let __painterAnimDelay = 0;
export function __setAnimationDelay(ms) { __painterAnimDelay = ms; }
export function __getAnimationDelay() { return __painterAnimDelay; }

// ── Sheet info ────────────────────────────────────────────────────────────────

export function painter_sheet_width()  { ensureActiveSheet(); return BigInt(state.sheetW); }
export function painter_sheet_height() { ensureActiveSheet(); return BigInt(state.sheetH); }
export function painter_center_x()     { ensureActiveSheet(); return BigInt(Math.floor(state.sheetW / 2)); }
export function painter_center_y()     { ensureActiveSheet(); return BigInt(Math.floor(state.sheetH / 2)); }

export function painter_text_width(charPtr) {
  ensureActiveSheet();
  const text = readCString(charPtr);
  if (!offCtx) return BigInt(text.length * Math.floor(state.fontSize / 2));
  offCtx.save();
  offCtx.font = makeFont();
  const w = offCtx.measureText(text).width;
  offCtx.restore();
  return BigInt(Math.round(w));
}

export function painter_get_pixel(x, y) {
  ensureActiveSheet();
  if (!offCtx) return 0n;
  const px = Number(x), py = Number(y);
  if (px < 0 || py < 0 || px >= state.sheetW || py >= state.sheetH) return 0n;
  const d = offCtx.getImageData(px, py, 1, 1).data;
  return packARGB(d[3], d[0], d[1], d[2]);
}

// ── Drawing parameters ────────────────────────────────────────────────────────

export function painter_pen(width, color) {
  state.penWidth = Number(width);
  state.penColor = BigInt(color);
}

export function painter_brush(color) {
  state.brushColor = BigInt(color);
  state.hasBrush = true;
}

export function painter_no_brush() {
  state.hasBrush = false;
}

export function painter_density(d) {
  state.density = Number(d);
}

export function painter_font(familyPtr, size, bold, italic) {
  state.fontFamily = readCString(familyPtr) || 'Arial';
  state.fontSize   = Number(size);
  state.fontBold   = Boolean(bold);
  state.fontItalic = Boolean(italic);
}

// ── Drawing commands ──────────────────────────────────────────────────────────

export function painter_move_to(x, y) {
  ensureActiveSheet();
  state.curX = Number(x);
  state.curY = Number(y);
}

export function painter_line(x1, y1, x2, y2) {
  ensureActiveSheet();
  if (!offCtx) return;
  offCtx.save();
  applyStroke();
  offCtx.globalAlpha = state.density / 100;
  offCtx.beginPath();
  offCtx.moveTo(Number(x1), Number(y1));
  offCtx.lineTo(Number(x2), Number(y2));
  offCtx.stroke();
  offCtx.restore();
  markDrawn();
}

export function painter_line_to(x, y) {
  ensureActiveSheet();
  if (!offCtx) return;
  const nx = Number(x), ny = Number(y);
  offCtx.save();
  applyStroke();
  offCtx.globalAlpha = state.density / 100;
  offCtx.beginPath();
  offCtx.moveTo(state.curX, state.curY);
  offCtx.lineTo(nx, ny);
  offCtx.stroke();
  offCtx.restore();
  state.curX = nx; state.curY = ny;
  markDrawn();
}

export function painter_polygon(n, xsPtr, ysPtr) {
  ensureActiveSheet();
  if (!offCtx) return;
  const count = Number(n);
  if (count < 2) return;
  const xs = readI64Array(xsPtr, count);
  const ys = readI64Array(ysPtr, count);
  offCtx.save();
  applyStroke(); applyFill();
  offCtx.globalAlpha = state.density / 100;
  offCtx.beginPath();
  offCtx.moveTo(xs[0], ys[0]);
  for (let i = 1; i < count; i++) offCtx.lineTo(xs[i], ys[i]);
  offCtx.closePath();
  if (state.hasBrush) offCtx.fill();
  offCtx.stroke();
  offCtx.restore();
  markDrawn();
}

export function painter_pixel(x, y, color) {
  ensureActiveSheet();
  if (!offCtx) return;
  offCtx.fillStyle = argbToStyle(color);
  offCtx.fillRect(Number(x), Number(y), 1, 1);
  markDrawn();
}

export function painter_rect(x1, y1, x2, y2) {
  ensureActiveSheet();
  if (!offCtx) return;
  const x = Math.min(Number(x1), Number(x2));
  const y = Math.min(Number(y1), Number(y2));
  const w = Math.abs(Number(x2) - Number(x1));
  const h = Math.abs(Number(y2) - Number(y1));
  offCtx.save();
  applyStroke(); applyFill();
  offCtx.globalAlpha = state.density / 100;
  if (state.hasBrush) offCtx.fillRect(x, y, w, h);
  offCtx.strokeRect(x, y, w, h);
  offCtx.restore();
  markDrawn();
}

export function painter_ellipse(x1, y1, x2, y2) {
  ensureActiveSheet();
  if (!offCtx) return;
  const cx = (Number(x1) + Number(x2)) / 2;
  const cy = (Number(y1) + Number(y2)) / 2;
  const rx = Math.abs(Number(x2) - Number(x1)) / 2;
  const ry = Math.abs(Number(y2) - Number(y1)) / 2;
  offCtx.save();
  applyStroke(); applyFill();
  offCtx.globalAlpha = state.density / 100;
  offCtx.beginPath();
  offCtx.ellipse(cx, cy, rx, ry, 0, 0, Math.PI*2);
  if (state.hasBrush) offCtx.fill();
  offCtx.stroke();
  offCtx.restore();
  markDrawn();
}

export function painter_circle(x, y, r) {
  ensureActiveSheet();
  if (!offCtx) return;
  const radius = Number(r);
  offCtx.save();
  applyStroke(); applyFill();
  offCtx.globalAlpha = state.density / 100;
  offCtx.beginPath();
  offCtx.arc(Number(x), Number(y), radius, 0, Math.PI*2);
  if (state.hasBrush) offCtx.fill();
  offCtx.stroke();
  offCtx.restore();
  markDrawn();
}

export function painter_text(x, y, charPtr) {
  ensureActiveSheet();
  if (!offCtx) return;
  const text = readCString(charPtr);
  offCtx.save();
  offCtx.globalAlpha = state.density / 100;
  offCtx.font = makeFont();
  offCtx.fillStyle = argbToStyle(state.penColor);
  offCtx.fillText(text, Number(x), Number(y));
  offCtx.restore();
  markDrawn();
}

export function painter_fill(x, y) {
  ensureActiveSheet();
  if (!offCtx) return;
  const px = Number(x), py = Number(y);
  if (px < 0 || py < 0 || px >= state.sheetW || py >= state.sheetH) return;

  const c = BigInt.asUintN(32, state.hasBrush ? state.brushColor : 0xFF000000n);
  const fr = Number((c >> 16n) & 0xFFn);
  const fg = Number((c >>  8n) & 0xFFn);
  const fb = Number( c         & 0xFFn);
  const fa = Number((c >> 24n) & 0xFFn);

  const imageData = offCtx.getImageData(0, 0, state.sheetW, state.sheetH);
  floodFill(imageData, px, py, fr, fg, fb, fa);
  offCtx.putImageData(imageData, 0, 0);
  markDrawn();
}

// ── Sheet management ──────────────────────────────────────────────────────────

// JS future runtime — set by __bindFutureRuntime from app.js.
let __futureRuntime = null;
export function __bindFutureRuntime(fr) { __futureRuntime = fr; }

export function painter_new_sheet(w, h, color) {
  const sw = Number(w), sh = Number(h);
  const execute = () => {
    if (sw <= 0 || sh <= 0 || sw > 32767 || sh > 32767) return;
    if (offscreen && hasDrawnSinceSheet) {
      drawNow();
      pendingSheet = { w: sw, h: sh, color };
      hasDrawnSinceSheet = false;
    } else {
      applySheet(sw, sh, color);
    }
  };
  if (!__futureRuntime) { execute(); return 0; }
  const hf = __futureRuntime.allocFuture();
  __futureRuntime.enqueuePendingOp({ h: hf, execute });
  return hf;
}

export function __flushPainter() {
  if (pendingSheet) return;
  scheduleDraw();
}

export function painter_load_sheet(charPtr) {
  const filename = readCString(charPtr);
  console.warn(`painter_load_sheet("${filename}"): not supported in browser runtime`);
}

export function painter_save_sheet(charPtr) {
  if (!offscreen) return;
  saveOffscreen(readCString(charPtr) || 'sheet.png');
}

function saveOffscreen(filename) {
  if (!offscreen) return;
  const download = (blob) => {
    if (!blob) return;
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = filename;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  };
  if (typeof offscreen.convertToBlob === 'function') {
    offscreen.convertToBlob({ type: 'image/png' }).then(download);
  } else {
    offscreen.toBlob(download, 'image/png');
  }
}
