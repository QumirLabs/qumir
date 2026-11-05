// Source location of the latest call to a command marked NeedsLocator: the
// compiler calls __record_locator(line, byte, column) right before such a call
// (qumir/modules/module.h).

let current = { line: 0, byte: 0, column: 0 };

export function __record_locator(line, byte, column) {
  current = { line: Number(line), byte: Number(byte), column: Number(column) };
}

export function currentLocator() {
  return current;
}

// The suffix the compiler puts on located errors (TLocation::ToString); the
// app highlights the line from it.
export function formatLocator(loc) {
  return loc && loc.line ? ` @ Line: ${loc.line}, Byte: ${loc.byte}, Column: ${loc.column}` : '';
}
