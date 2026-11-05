import { allocFuture, resolveFuture, enqueuePendingOp } from './future.js';

const KEY_CODES = new Map([
  ['Backspace', 8],
  ['Tab', 9],
  ['Enter', 13],
  ['NumpadEnter', 13],
  ['Space', 32],
  ['PageUp', 33],
  ['PageDown', 34],
  ['End', 35],
  ['Home', 36],
  ['ArrowLeft', 37],
  ['ArrowUp', 38],
  ['ArrowRight', 39],
  ['ArrowDown', 40],
  ['Insert', 45],
  ['Delete', 46],
  ['BracketLeft', 219],
  ['BracketRight', 221],
  ['Semicolon', 186],
  ['Quote', 222],
  ['Comma', 188],
  ['Period', 190],
]);

for (let i = 1; i <= 12; i++) {
  KEY_CODES.set(`F${i}`, 111 + i);
}
for (let i = 0; i <= 9; i++) {
  KEY_CODES.set(`Digit${i}`, 48 + i);
  KEY_CODES.set(`Numpad${i}`, 48 + i);
}
for (let i = 0; i < 26; i++) {
  const letter = String.fromCharCode(65 + i);
  KEY_CODES.set(`Key${letter}`, 65 + i);
}

const keyQueue = [];
const codeWaiters = [];
let hasSignal = false;
let installed = false;

function mapEventCode(event) {
  if (!event || typeof event.code !== 'string') return 0;
  return KEY_CODES.get(event.code) || 0;
}

function onKeyDown(event) {
  const code = mapEventCode(event);
  if (code === 0) return;
  console.log('Key down: %s (code %d)', event.code, code);
  hasSignal = true;
  if (codeWaiters.length > 0) {
    codeWaiters.shift().resolve(BigInt(code));
    return;
  }
  keyQueue.push(code);
}

function installKeyboardListener() {
  if (installed) return;
  if (typeof window === 'undefined' || typeof window.addEventListener !== 'function') return;
  window.addEventListener('keydown', onKeyDown, true);
  installed = true;
}

export function __resetKeyboard() {
  keyQueue.length = 0;
  for (const waiter of codeWaiters.splice(0)) waiter.resolve(0n);
  hasSignal = false;
  installKeyboardListener();
}

export function keyboard_signal() {
  installKeyboardListener();
  const result = hasSignal;
  hasSignal = false;
  return result ? 1 : 0;
}

export function keyboard_code() {
  installKeyboardListener();
  const h = allocFuture();
  if (keyQueue.length > 0) {
    resolveFuture(h, BigInt(keyQueue.shift()));
  } else {
    const waitPromise = new Promise(resolve => {
      codeWaiters.push({ resolve });
    });
    enqueuePendingOp({ h, execute: () => waitPromise });
  }
  console.log('keyboard_code() called, returning future handle %d', h);
  return h;
}

export function keyboard_reset() {
  __resetKeyboard();
}

installKeyboardListener();

export function __disposeKeyboard() {
  __resetKeyboard();
  if (installed && typeof window !== 'undefined') {
    window.removeEventListener('keydown', onKeyDown, true);
    installed = false;
  }
}
