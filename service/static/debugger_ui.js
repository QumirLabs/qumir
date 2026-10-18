import { Debugger } from './runtime/debugger.js';
import { ValuePrinter } from './runtime/value_printer.js';

export class DebuggerUI {
  constructor(editor, panel) {
    this.editor = editor;
    this.panel = panel;
    this.breakpoints = new Set();
    this.debugger = null;
    this.selected = null;
    this.printer = null;
    this.line = null;
    this.mark = null;
    this.tooltip = document.createElement('div');
    this.tooltip.className = 'q-debug-tooltip';
    this.tooltip.hidden = true;
    document.body.append(this.tooltip);
    this.editor.setOption('gutters', [...(editor.getOption('gutters') || []), 'q-debug-gutter']);
    editor.on('gutterClick', (cm, line, gutter) => {
      if (gutter !== 'q-debug-gutter') return;
      if (this.breakpoints.has(line + 1)) this.breakpoints.delete(line + 1);
      else this.breakpoints.add(line + 1);
      this.drawBreakpoints();
      this.debugger?.setBreakpoint(line + 1, this.breakpoints.has(line + 1));
      if (this.breakpoints.has(line + 1)) {
        panel.dispatchEvent(new CustomEvent('qumir-breakpoint-set', { bubbles: true, detail: { line: line + 1 } }));
      }
    });
    editor.on('changes', () => {
      if (this.debugger) return;
      // A changed source has different executable locations.
      this.breakpoints.clear();
      this.drawBreakpoints();
    });
    panel.querySelectorAll('[data-debug-action]').forEach(button => {
      button.addEventListener('click', () => {
        const action = button.dataset.debugAction;
        if (action === 'pause') this.debugger?.pause();
        else this.debugger?.resume(action, this.selected);
      });
    });
    editor.getWrapperElement().addEventListener('mousemove', event => this.hover(event));
    editor.getWrapperElement().addEventListener('mouseleave', () => this.clearHover());
  }

  drawBreakpoints() {
    this.editor.clearGutter('q-debug-gutter');
    for (const line of this.breakpoints) {
      const marker = document.createElement('span');
      marker.className = 'q-debug-breakpoint';
      marker.textContent = '●';
      marker.title = 'Точка останова';
      this.editor.setGutterMarker(line - 1, 'q-debug-gutter', marker);
    }
  }

  begin() {
    this.debugger = new Debugger({ breakpoints: this.breakpoints, onChange: () => this.update() });
    this.previousReadOnly = this.editor.getOption('readOnly');
    this.editor.setOption('readOnly', true);
    this.panel.hidden = false;
    this.editor.refresh();
    this.update();
    return this.debugger;
  }

  bind(runtime) {
    if (!runtime.debugData) throw new Error('В программе нет debug points');
    this.debugger.bindWasm(runtime.instance.exports);
    this.printer = new ValuePrinter(runtime.instance.exports.memory, runtime.stringEnv, runtime.debugData.pointerSize);
  }

  clearHover() {
    this.mark?.clear();
    this.mark = null;
    this.tooltip.hidden = true;
  }

  update() {
    this.clearHover();
    if (this.line !== null) this.editor.removeLineClass(this.line, 'background', 'q-debug-current');
    this.line = null;
    const paused = !!this.debugger?.paused;
    this.panel.dataset.paused = String(paused);
    this.panel.querySelector('[data-debug-status]').textContent = paused ? 'Остановлено' : 'Выполняется';
    this.panel.querySelectorAll('[data-debug-action]').forEach(button => {
      button.disabled = button.dataset.debugAction === 'pause' ? paused : !paused;
    });
    const stack = this.panel.querySelector('[data-debug-stack]');
    const locals = this.panel.querySelector('[data-debug-locals]');
    stack.replaceChildren();
    locals.replaceChildren();
    if (!paused) return;
    this.selected = this.debugger.frames.at(-1);
    if (this.selected.point.kind === 'statement' && this.breakpoints.has(this.selected.point.line)) {
      this.panel.dispatchEvent(new CustomEvent('qumir-breakpoint-hit', { bubbles: true, detail: { line: this.selected.point.line } }));
    }
    for (const frame of [...this.debugger.frames].reverse()) {
      const button = document.createElement('button');
      button.type = 'button';
      const fun = this.debugger.data.functions[frame.function];
      button.textContent = `${fun.name} — ${frame.point.line}:${frame.point.column}`;
      button.addEventListener('click', () => { this.selected = frame; this.showFrame(); });
      stack.append(button);
    }
    this.showFrame();
  }

  showFrame() {
    this.clearHover();
    if (this.line !== null) this.editor.removeLineClass(this.line, 'background', 'q-debug-current');
    this.line = this.selected.point.line - 1;
    this.editor.addLineClass(this.line, 'background', 'q-debug-current');
    this.editor.scrollIntoView({ line: this.line, ch: 0 }, 80);
    this.panel.dataset.line = String(this.line + 1);
    const rows = this.panel.querySelector('[data-debug-locals]');
    rows.replaceChildren();
    for (const local of this.debugger.visibleLocals(this.selected)) {
      const row = document.createElement('tr');
      for (const text of [local.name, local.type.kind, this.printer?.value(local.type, local.address) ?? '']) {
        const cell = document.createElement('td');
        cell.textContent = text;
        row.append(cell);
      }
      rows.append(row);
    }
  }

  hover(event) {
    this.clearHover();
    if (!this.debugger?.paused || !this.selected || !this.printer) return;
    const pos = this.editor.coordsChar({ left: event.clientX, top: event.clientY }, 'window');
    const text = this.editor.getLine(pos.line) || '';
    const column = [...text.slice(0, pos.ch)].length + 1;
    const local = this.debugger.binding(this.selected, pos.line + 1, column);
    if (!local) return;
    const binding = this.debugger.data.bindings.find(b => b.function === this.selected.function
      && b.local === local.id && b.line === pos.line + 1 && column >= b.column
      && column < b.column + [...b.name].length);
    const ch = [...text].slice(0, binding.column - 1).join('').length;
    if (text.slice(ch, ch + local.name.length) !== local.name) return;
    this.mark = this.editor.markText({ line: pos.line, ch }, { line: pos.line, ch: ch + local.name.length }, { className: 'q-debug-variable' });
    this.tooltip.textContent = `${local.name} = ${this.printer.value(local.type, local.address)}`;
    this.tooltip.style.left = `${Math.min(event.clientX + 12, window.innerWidth - 220)}px`;
    this.tooltip.style.top = `${event.clientY + 18}px`;
    this.tooltip.hidden = false;
  }

  finish() {
    this.debugger?.finish();
    this.debugger = null;
    this.printer = null;
    this.selected = null;
    this.clearHover();
    this.panel.hidden = true;
    this.editor.refresh();
    this.editor.setOption('readOnly', this.previousReadOnly ?? false);
  }
}
