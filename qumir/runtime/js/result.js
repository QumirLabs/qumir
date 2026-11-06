const DBL_MAX_DECIMAL = '179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.000000000000000';

// String runtime to interpret 'лит' return values using the same
// pointer/handle rules as the main string.js runtime. Host code should
// inject the already-loaded string.js module via setStringRuntime.
let stringRuntime = null;

// Simple UTF-8 decoder for small byte sequences (e.g., export names in wasm)
function decodeUtf8(bytes) {
  if (!bytes) return '';
  if (typeof TextDecoder !== 'undefined') {
    try {
      return new TextDecoder('utf-8').decode(bytes);
    } catch {}
  }
  if (typeof Buffer !== 'undefined') {
    return Buffer.from(bytes).toString('utf8');
  }
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    out += String.fromCharCode(bytes[i]);
  }
  return out;
}

// Allow host environments (Node test harness, browser app) to provide the
// string.js runtime instance explicitly so we do not rely on require/ESM
// interop inside this module.
export function setStringRuntime(rt) {
  if (rt && typeof rt.__loadString === 'function') {
    stringRuntime = rt;
  }
}

function interpretLit(value) {
  const raw = Number(value);
  if (!Number.isInteger(raw)) return '';

  // If value is a non-negative pointer/handle and string.js is available,
  // delegate to its __loadString so we get the same semantics for both
  // JS-string handles (negative) and C-string pointers (non-negative).
	const runtime = stringRuntime;
  if (runtime && typeof runtime.__loadString === 'function') {
    const s = runtime.__loadString(raw);
    if (s) return s;
  }

  // Fallback: treat as a Unicode code point if within range.
  if (raw >= 0 && raw <= 0x10FFFF) {
    try {
      return String.fromCodePoint(raw);
    } catch {
      return '';
    }
  }
  return '';
}

export function normalizeReturnValue(value, options = {}) {
  const { returnType, algType } = options;
  if (value === undefined || value === null) return '';
  let normalized = String(value);
  if (returnType === 'f32' || returnType === 'f64') {
    normalized = (value === Number.MAX_VALUE) ? DBL_MAX_DECIMAL : Number(value).toFixed(15);
  } else if (typeof value === 'number' && isFinite(value)) {
    const str = String(value);
    if (/e[+-]\d+/i.test(str) || Math.abs(value) >= 1e21) {
      normalized = (value === Number.MAX_VALUE) ? DBL_MAX_DECIMAL : Number(value).toFixed(15);
    }
  }
  if (algType === 'лит' && returnType === 'i32') {
    const lit = interpretLit(value);
    if (lit) normalized = lit;
  }
  return normalized;
}

export function wasmReturnType(bytes, exportName) {
  if (!bytes || !exportName) return null;
  const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (data.length < 8) return null;
  let pos = 8;
  const types = [];
  const funcTypeIndices = [];
  let importFuncCount = 0;
  const exportFuncMap = new Map();
  const readU32 = () => {
    let result = 0;
    let shift = 0;
    while (true) {
      const b = data[pos++];
      result |= (b & 0x7F) << shift;
      if ((b & 0x80) === 0) break;
      shift += 7;
    }
    return result >>> 0;
  };
  while (pos < data.length) {
    const id = data[pos++];
    const size = readU32();
    const sectionStart = pos;
    if (id === 1) {
      const count = readU32();
      for (let i = 0; i < count; i++) {
        const form = data[pos++];
        if (form !== 0x60) return null;
        const paramCount = readU32();
        for (let p = 0; p < paramCount; p++) pos++;
        const retCount = readU32();
        const returns = [];
        for (let r = 0; r < retCount; r++) returns.push(data[pos++]);
        types.push({ returns });
      }
    } else if (id === 2) {
      const count = readU32();
      for (let i = 0; i < count; i++) {
        const modLen = readU32(); pos += modLen;
        const fieldLen = readU32(); pos += fieldLen;
        const kind = data[pos++];
        if (kind === 0x00) {
          importFuncCount++;
          readU32();
        } else if (kind === 0x01) {
          pos++;
          const flags = readU32();
          readU32();
          if (flags & 0x01) readU32();
        } else if (kind === 0x02) {
          const flags = readU32();
          readU32();
          if (flags & 0x01) readU32();
        } else if (kind === 0x03) {
          pos += 2;
        }
      }
    } else if (id === 3) {
      const count = readU32();
      for (let i = 0; i < count; i++) funcTypeIndices.push(readU32());
    } else if (id === 7) {
      const count = readU32();
      for (let i = 0; i < count; i++) {
        const nameLen = readU32();
        const nameBytes = data.slice(pos, pos + nameLen);
        pos += nameLen;
        const name = decodeUtf8(nameBytes);
        const kind = data[pos++];
        const index = readU32();
        if (kind === 0x00) exportFuncMap.set(name, index);
      }
    } else {
      pos = sectionStart + size;
      continue;
    }
    pos = sectionStart + size;
  }
  const funcIdx = exportFuncMap.get(exportName);
  if (funcIdx == null) return null;
  let typeIdx;
  if (funcIdx < importFuncCount) {
    return null;
  } else {
    const definedIdx = funcIdx - importFuncCount;
    typeIdx = funcTypeIndices[definedIdx];
  }
  if (typeIdx == null || typeIdx >= types.length) return null;
  const returns = types[typeIdx].returns;
  if (!returns || returns.length === 0) return 'void';
  const t = returns[0];
  switch (t) {
    case 0x7F: return 'i32';
    case 0x7E: return 'i64';
    case 0x7D: return 'f32';
    case 0x7C: return 'f64';
    default: return null;
  }
}
