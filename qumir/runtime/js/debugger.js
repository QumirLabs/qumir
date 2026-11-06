export class Debugger {
  constructor({ breakpoints = new Set(), onChange = () => {} } = {}) {
    this.breakpoints = breakpoints;
    this.onChange = onChange;
    this.data = null;
    this.frames = [];
    this.nextFrame = 0;
    this.mode = 'in';
    this.target = null;
    this.paused = false;
    this.pauseRequested = false;
    this.resumeCallback = null;
  }

  bindWasm(exports) {
    if (typeof exports.__qumir_debug_init !== 'function') return;
    this.wasm = exports;
    const maxLocals = this.data.functions.reduce((maximum, fun) => Math.max(maximum, fun.locals.length), 0);
    this.frameStride = 3 + maxLocals;
    const pointer = exports.__qumir_debug_init(this.data.points.length, maxLocals) >>> 0;
    const view = new DataView(exports.memory.buffer);
    const kinds = { enter: 1, leave: 2, variable: 3, statement: 4, instruction: 5 };
    for (let i = 0; i < this.data.points.length; ++i) {
      const point = this.data.points[i];
      const fields = [kinds[point.kind], point.function, point.line, point.local, 0];
      for (let field = 0; field < fields.length; ++field) {
        view.setBigInt64(pointer + (i * 5 + field) * 8, BigInt(fields[field]), true);
      }
    }
    for (const line of this.breakpoints) this.setBreakpoint(line, true);
  }

  setBreakpoint(line, enabled) {
    this.wasm?.__qumir_debug_breakpoint(line, enabled ? 1 : 0);
  }

  readWasmFrames() {
    const depth = this.wasm.__qumir_debug_depth();
    const pointer = this.wasm.__qumir_debug_frame_data() >>> 0;
    const view = new DataView(this.wasm.memory.buffer);
    const frames = [];
    const read = index => Number(view.getBigInt64(pointer + index * 8, true));
    for (let i = 0; i < depth; ++i) {
      const offset = i * this.frameStride;
      const fun = read(offset + 1);
      const locals = new Map();
      for (let local = 0; local < this.data.functions[fun].locals.length; ++local) {
        const address = read(offset + 3 + local) >>> 0;
        if (address) locals.set(local, address);
      }
      frames.push({ id: read(offset), function: fun, point: this.data.points[read(offset + 2)], locals });
    }
    this.frames = frames;
  }

  pauseHere() {
    this.pauseRequested = false;
    this.paused = true;
    return new Promise(resolve => {
      this.resumeCallback = resolve;
      this.onChange(this);
    });
  }

  point(id, address) {
    if (this.wasm) {
      this.readWasmFrames();
      return this.pauseHere();
    }
    const point = this.data?.points[id];
    if (!point) return;
    if (point.kind === 'enter') {
      this.frames.push({ id: ++this.nextFrame, function: point.function, point, locals: new Map() });
      return;
    }
    const frame = this.frames.at(-1);
    if (!frame || frame.function !== point.function) return;
    if (point.kind === 'variable') {
      frame.locals.set(point.local, Number(address) >>> 0);
      return;
    }
    if (point.kind === 'leave') {
      this.frames.pop();
      return;
    }
    frame.point = point;
    const statement = point.kind === 'statement' && point.line > 0;
    const returned = this.target !== null && !this.frames.some(f => f.id === this.target);
    const step = (statement && (this.mode === 'in' || (this.mode === 'over' && frame.id === this.target)))
      || ((this.mode === 'out' || this.mode === 'over') && returned);
    if (!this.pauseRequested && !step && !(statement && this.breakpoints.has(point.line))) return;
    return this.pauseHere();
  }

  resume(mode = 'continue', frame = this.frames.at(-1)) {
    this.mode = mode;
    this.target = frame?.id ?? null;
    const modes = { continue: 0, in: 1, over: 2, out: 3 };
    this.wasm?.__qumir_debug_control(modes[mode] ?? 0, BigInt(this.target ?? 0));
    if (!this.paused) return;
    this.paused = false;
    const resume = this.resumeCallback;
    this.resumeCallback = null;
    this.onChange(this);
    resume?.();
  }

  pause() {
    this.pauseRequested = true;
    this.wasm?.__qumir_debug_control(4, 0n);
  }

  requestStop() {
    this.wasm?.__qumir_debug_control(5, 0n);
  }

  visibleLocals(frame) {
    if (!frame) return [];
    const fun = this.data.functions[frame.function];
    const scopes = new Set();
    let scope = frame.point.scope;
    while (scope >= 0 && !scopes.has(scope)) {
      scopes.add(scope);
      scope = fun.parents[scope] ?? -1;
    }
    const names = new Set();
    const result = [];
    // Inner scopes win when source names are shadowed.
    for (const scopeId of scopes) {
      for (const [id, address] of frame.locals) {
        const local = fun.locals[id];
        if (!local || local.scope !== scopeId || !local.name || local.name.startsWith('$') || names.has(local.name)) continue;
        names.add(local.name);
        result.push({ ...local, id, address });
      }
    }
    return result;
  }

  binding(frame, line, column) {
    const binding = this.data.bindings.find(b => b.function === frame.function && b.line === line
      && column >= b.column && column < b.column + [...b.name].length);
    return binding && this.visibleLocals(frame).find(local => local.id === binding.local);
  }

  finish(dispose = true) {
    if (dispose) {
      this.wasm?.__qumir_debug_dispose();
      this.wasm = null;
    }
    this.paused = false;
    this.frames = [];
    this.resumeCallback?.();
    this.resumeCallback = null;
    this.onChange(this);
  }
}
