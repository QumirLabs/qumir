export class ExecutionStopped extends Error {
  constructor() { super('Остановлено'); }
}

export class ExecutionSession {
  constructor({ debugger: debuggerInstance = null, render = () => {}, delay = () => 0 } = {}) {
    this.debugger = debuggerInstance;
    this.render = render;
    this.delay = delay;
    this.controller = new AbortController();
    this.lastRender = performance.now();
    this.debugCallbacks = 0;
    this.debugYields = 0;
    this.instance = null;
  }

  check() {
    if (this.controller.signal.aborted) throw this.controller.signal.reason;
  }

  wait(promise) {
    const signal = this.controller.signal;
    this.check();
    return new Promise((resolve, reject) => {
      const abort = () => {
        signal.removeEventListener('abort', abort);
        reject(signal.reason);
      };
      signal.addEventListener('abort', abort, { once: true });
      Promise.resolve(promise).then(resolve, reject).finally(() => {
        signal.removeEventListener('abort', abort);
      });
    });
  }

  sleep(milliseconds) {
    const signal = this.controller.signal;
    this.check();
    return new Promise((resolve, reject) => {
      const abort = () => { clearTimeout(timer); reject(signal.reason); };
      const timer = setTimeout(() => {
        signal.removeEventListener('abort', abort);
        this.instance?.exports.__qumir_debug_reset_interval?.();
        resolve();
      }, milliseconds);
      signal.addEventListener('abort', abort, { once: true });
    });
  }

  operation(execute) {
    this.check();
    const result = execute();
    const finish = value => {
      this.check();
      const delay = this.delay();
      const now = performance.now();
      if (delay > 0 || now - this.lastRender >= 1000) {
        this.render();
        this.lastRender = now;
        return this.sleep(delay).then(() => value);
      }
      return value;
    };
    if (result instanceof Promise) {
      this.render();
      return this.wait(result).then(finish);
    }
    return finish(result);
  }

  checkpoint(id, address) {
    this.check();
    const paused = this.debugger?.point(id, address);
    if (paused instanceof Promise) {
      return this.wait(paused).then(value => {
        this.instance?.exports.__qumir_debug_reset_interval?.();
        return value;
      });
    }
    return undefined;
  }

  async yieldCheckpoint(id, address) {
    // A browser task boundary lets queued input update the WASM control flags.
    await this.sleep(0);
    this.check();
    if (this.debugger?.pauseRequested) return this.checkpoint(id, address);
  }

  async call(fn, args = [], returnType = 'void') {
    this.check();
    const runtime = this.futureRuntime;
    this.instance.exports.__qumir_debug_reset_interval?.();
    const raw = fn(...args);
    if (!raw) return;
    const future = runtime.__qumir_wrap_coro(raw, 0);
    try {
      while (!runtime.__qumir_future_done(future)) {
        this.check();
        if (runtime.hasPendingOp()) {
          const { h, execute, checkpoint = false } = runtime.shiftPendingOp();
          const result = checkpoint
            ? execute()
            : this.operation(execute);
          const value = result instanceof Promise ? await this.wait(result) : result;
          this.check();
          // Synchronous operations and settled Promises do not let input events run.
          runtime.resolveFuture(h, value);
        } else {
          runtime.__qumir_future_resume(future);
        }
      }
      const pointer = this.instance.exports.__qumir_coro_promise_ptr(raw) >>> 0;
      if (!pointer || returnType === 'void') return undefined;
      const view = new DataView(this.instance.exports.memory.buffer);
      if (returnType === 'f32') return view.getFloat32(pointer, true);
      if (returnType === 'f64') return view.getFloat64(pointer, true);
      if (returnType === 'i64') return view.getBigInt64(pointer, true);
      if (returnType === 'u64') return view.getBigUint64(pointer, true);
      if (returnType === 'i16') return view.getInt16(pointer, true);
      if (returnType === 'u16') return view.getUint16(pointer, true);
      if (returnType === 'i1' || returnType === 'u8') return view.getUint8(pointer);
      if (returnType === 'i8') return view.getInt8(pointer);
      if (returnType === 'u32') return view.getUint32(pointer, true);
      return view.getInt32(pointer, true);
    } finally {
      runtime.__qumir_future_destroy(future);
      if (this.controller.signal.aborted) runtime.__resetFutures();
    }
  }

  stop() {
    if (!this.controller.signal.aborted) this.controller.abort(new ExecutionStopped());
    this.debugger?.requestStop();
    this.debugger?.finish(false);
    this.instance = null;
  }

  finish() {
    if (!this.controller.signal.aborted) this.controller.abort(new ExecutionStopped());
    this.debugger?.finish();
    this.render();
    this.instance = null;
  }
}

export function bindDebugImport(env, session, futureRuntime) {
  session.futureRuntime = futureRuntime;
  const checkpoint = (id, address, reason = 1) => {
    session.check();
    if (reason === 2) throw new ExecutionStopped();
    if (reason === 3) ++session.debugYields;
    else ++session.debugCallbacks;
    const h = futureRuntime.allocFuture();
    futureRuntime.enqueuePendingOp({
      h,
      checkpoint: true,
      execute: () => reason === 3 ? session.yieldCheckpoint(id, address) : session.checkpoint(id, address),
    });
    return h;
  };
  env.__qumir_debug_point = checkpoint;
  env.__qumir_debug_slow = checkpoint;
  env.__qumir_debug_now = () => performance.now();
}
