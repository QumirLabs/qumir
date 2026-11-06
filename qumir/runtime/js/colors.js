"use strict";

// Colors (Цвета) runtime — ARGB color values as BigInt (0xAARRGGBB)

import { __appendStdout } from './io.js';

// Exported utility for painter.js and drawer.js
export function argbToStyle(color) {
  const c = BigInt.asUintN(32, BigInt(color));
  const a = Number((c >> 24n) & 0xFFn);
  const r = Number((c >> 16n) & 0xFFn);
  const g = Number((c >>  8n) & 0xFFn);
  const b = Number( c         & 0xFFn);
  return `rgba(${r},${g},${b},${(a / 255).toFixed(6)})`;
}

export function color_print(color) {
  const c = BigInt.asUintN(32, BigInt(color));
  const r = Number((c >> 16n) & 0xFFn);
  const g = Number((c >>  8n) & 0xFFn);
  const b = Number( c         & 0xFFn);
  const hex = '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0').toUpperCase()).join('');
  __appendStdout(hex);
}
