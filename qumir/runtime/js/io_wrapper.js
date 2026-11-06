'use strict';

// Helper utilities to adapt DOM elements (#stdin/#stdout) into the runtime IO
// stream API. Keeps the core IO runtime free of browser-specific references.

function resolveElement(target, fallbackSelector) {
  if (!target && typeof document !== 'undefined') {
    return document.querySelector(fallbackSelector);
  }
  if (typeof target === 'string' && typeof document !== 'undefined') {
    return document.querySelector(target);
  }
  return target || null;
}

function createTokenizingInputStream(resolveFn) {
  const WHITESPACE = new Set([9, 10, 11, 12, 13, 32, 160]); // \t \n \v \f \r space nbsp
  let raw = '';
  let cursor = 0;
  // Reads past the end of the input: they get 0 / '' instead of data.
  let missing = 0;

  const reload = () => {
    const el = resolveFn();
    raw = el ? String(el.value || '') : '';
    cursor = 0;
    missing = 0;
  };

  const isWhitespace = (code) => WHITESPACE.has(code);

  const skipWhitespace = () => {
    while (cursor < raw.length && isWhitespace(raw.charCodeAt(cursor))) {
      cursor++;
    }
  };

  const readToken = () => {
    skipWhitespace();
    if (cursor >= raw.length) {
      missing++;
      return '0';
    }
    const start = cursor;
    while (cursor < raw.length && !isWhitespace(raw.charCodeAt(cursor))) {
      cursor++;
    }
    const token = raw.slice(start, cursor);
    return token.length ? token : '0';
  };

  const readLine = () => {
    if (cursor >= raw.length) {
      missing++;
      return '';
    }
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
  };

  reload();

  return {
    readToken,
    readLine,
    hasMore() {
      return cursor < raw.length;
    },
    missingReads() {
      return missing;
    },
    reset() {
      reload();
    }
  };
}

function createTextContentOutputStream(resolveFn) {
  return {
    write(str) {
      const el = resolveFn();
      if (!el) return;
      el.textContent += String(str);
    },
    clear() {
      const el = resolveFn();
      if (!el) return;
      el.textContent = '';
      el.classList.remove('error');
    }
  };
}

export function bindBrowserIO(ioRuntime, options = {}) {
  if (!ioRuntime || typeof ioRuntime.setInputStream !== 'function' || typeof ioRuntime.setOutputStream !== 'function') {
    throw new Error('ioRuntime must expose setInputStream/setOutputStream.');
  }
  const {
    stdin: stdinTarget = '#stdin',
    stdout: stdoutTarget = '#stdout'
  } = options;
  const stdinEl = resolveElement(stdinTarget, '#stdin');
  const stdoutEl = resolveElement(stdoutTarget, '#stdout');
  if (!stdinEl) throw new Error('STDIN element not found.');
  if (!stdoutEl) throw new Error('STDOUT element not found.');

  const inputStream = createTokenizingInputStream(() => stdinEl);
  const outputStream = createTextContentOutputStream(() => stdoutEl);
  ioRuntime.setInputStream(inputStream);
  ioRuntime.setOutputStream(outputStream);
  return { inputStream, outputStream, stdinEl, stdoutEl };
}
