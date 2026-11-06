export class ValuePrinter {
  constructor(memory, strings, pointerSize = 4) {
    this.memory = memory;
    this.strings = strings;
    this.pointerSize = pointerSize;
    this.formatters = new Map([
      ['Int', (view, address, type) => this.integer(view, address, type)],
      ['Float', (view, address) => String(view.getFloat64(address, true))],
      ['Bool', (view, address) => view.getUint8(address) ? 'true' : 'false'],
      ['Char', (view, address) => JSON.stringify(String.fromCodePoint(view.getUint32(address, true)))],
      ['String', (view, address) => JSON.stringify(this.strings.__loadString(view.getInt32(address, true)))],
      ['Ref', (view, address, type) => this.value(type.target, this.pointer(view, address))],
    ]);
  }

  pointer(view, address) {
    return this.pointerSize === 4 ? view.getUint32(address, true) : Number(view.getBigUint64(address, true));
  }

  integer(view, address, type) {
    const bits = type.bits || 64;
    if (bits === 128) {
      const value = view.getBigUint64(address, true) | (view.getBigUint64(address + 8, true) << 64n);
      return String(type.signed ? BigInt.asIntN(128, value) : value);
    }
    const method = `get${bits === 64 ? 'Big' : ''}${type.signed ? 'Int' : 'Uint'}${bits}`;
    return String(view[method](address, true));
  }

  value(type, address) {
    try {
      // memory.grow replaces memory.buffer, including while stepping.
      const view = new DataView(this.memory.buffer);
      const formatter = this.formatters.get(type.kind);
      return formatter ? formatter(view, address, type) : `<${type.kind} @ 0x${address.toString(16)}>`;
    } catch {
      return '<недоступно>';
    }
  }
}
