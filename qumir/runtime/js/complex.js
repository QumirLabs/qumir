"use strict";

import { __appendStdout } from './io.js';

let MEMORY = null;

export function __bindMemory(mem) {
  MEMORY = mem;
}

function view() {
  if (!MEMORY) {
    throw new Error("complex runtime called before __bindMemory");
  }
  return new DataView(MEMORY.buffer);
}

// `компл` is a by-value struct {re, im}, and codegen coerces external calls to
// the target C ABI, so it crosses the wasm boundary as one pointer (byval)
// rather than as two f64 arguments.
function readComplex(ptr) {
  const dv = view();
  const at = Number(ptr);
  return [dv.getFloat64(at, true), dv.getFloat64(at + 8, true)];
}

export function complex_abs(z) {
  const [re, im] = readComplex(z);
  return Math.hypot(re, im);
}

export function complex_arg(z) {
  const [re, im] = readComplex(z);
  return Math.atan2(im, re);
}

export function complex_print(z) {
  const [re, im] = readComplex(z);
  let s = String(re);
  s += im >= 0 ? '+' : '-';
  s += String(Math.abs(im)) + 'i';
  __appendStdout(s);
}
