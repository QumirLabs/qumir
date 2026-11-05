// Robot executor runtime for Qumir
// Implements the Robot field logic with walls, painted cells, radiation/temperature

import { currentLocator, formatLocator } from './locator.js';

let __filesAccessor = null;
let __addFileCallback = null;
let __updateFileCallback = null;

let __animationDelay = 150; // ms between steps

// JS future runtime (set by __bindFutureRuntime from app.js).
let __futureRuntime = null;

export function __bindFutureRuntime(fr) {
  __futureRuntime = fr;
}

// Default field: 7x7, robot at (0,0), no walls, no painted cells
const DEFAULT_FIELD = `; Kumir Robot Field Format
; ========================
;
; Line 1: Field size (width height)
; Line 2: Robot position (x y), 1-indexed from top-left
;
; Remaining lines: Special cell properties
; Format: x y Wall Color Radiation Temperature Symbol Symbol1 Point
;
; Wall is a bit mask: 1 = North, 2 = South, 4 = West, 8 = East
;
; Color: 0 = not painted, non-zero = painted
; Radiation: float value for radiation sensor
; Temperature: float value for temperature sensor
;
; Example special cell: 3 2 8 1 5.5 20.0 $ $ 0
;   Cell at (3,2) with wall to East, painted, radiation=5.5, temp=20.0
;
; Field Size
7 7
; Robot Position
1 1
`.trim();

export class RobotField {
  constructor() {
    this.width = 7;
    this.height = 7;
    this.robotX = 0;
    this.robotY = 0;
    this.painted = new Set();
    this.hWalls = new Set(); // horizontal walls: "x,y" means wall below cell (x,y)
    this.vWalls = new Set(); // vertical walls: "x,y" means wall to the right of cell (x,y)
    this.colors = new Map(); // "x,y" -> numeric color from .fil
    this.radiation = new Map(); // "x,y" -> value
    this.temperature = new Map(); // "x,y" -> value
    this.symbol = new Map(); // "x,y" -> Symbol column from .fil
    this.symbol1 = new Map(); // "x,y" -> Symbol1 column from .fil
    this.point = new Map(); // "x,y" -> Point column from .fil
    this.crash = null; // {x, y, side} of the wall the robot ran into
  }

  reset() {
    this.width = 7;
    this.height = 7;
    this.robotX = 0;
    this.robotY = 0;
    this.painted.clear();
    this.hWalls.clear();
    this.vWalls.clear();
    this.colors.clear();
    this.radiation.clear();
    this.temperature.clear();
    this.symbol.clear();
    this.symbol1.clear();
    this.point.clear();
    this.crash = null;
  }

  resize(width, height) {
    this.width = Math.max(1, Math.min(200, Number(width) | 0));
    this.height = Math.max(1, Math.min(200, Number(height) | 0));
    this.robotX = Math.max(0, Math.min(this.width - 1, this.robotX));
    this.robotY = Math.max(0, Math.min(this.height - 1, this.robotY));

    const inCellBounds = (key) => {
      const [x, y] = key.split(',').map(Number);
      return x >= 0 && y >= 0 && x < this.width && y < this.height;
    };
    const inHorizontalWallBounds = (key) => {
      const [x, y] = key.split(',').map(Number);
      return x >= 0 && y >= 0 && x < this.width && y < this.height - 1;
    };
    const inVerticalWallBounds = (key) => {
      const [x, y] = key.split(',').map(Number);
      return x >= 0 && y >= 0 && x < this.width - 1 && y < this.height;
    };
    for (const set of [this.painted, this.hWalls, this.vWalls]) {
      const predicate = set === this.hWalls ? inHorizontalWallBounds : set === this.vWalls ? inVerticalWallBounds : inCellBounds;
      for (const key of Array.from(set)) {
        if (!predicate(key)) set.delete(key);
      }
    }
    for (const map of [this.colors, this.radiation, this.temperature, this.symbol, this.symbol1, this.point]) {
      for (const key of Array.from(map.keys())) {
        if (!inCellBounds(key)) map.delete(key);
      }
    }
  }

  addWalls(x, y, wallN, wallE, wallS, wallW) {
    // Wall to North = horizontal wall above cell (x,y).
    // In our model: hWalls stores "x,y" meaning wall below (x,y).
    if (wallN && y > 0) {
      this.hWalls.add(`${x},${y - 1}`);
    }
    // Wall to South = wall below (x,y).
    if (wallS && y < this.height - 1) {
      this.hWalls.add(`${x},${y}`);
    }
    // Wall to West = wall to the left of (x,y).
    // In our model: vWalls stores "x,y" meaning wall to the right of (x,y).
    if (wallW && x > 0) {
      this.vWalls.add(`${x - 1},${y}`);
    }
    // Wall to East = wall to the right of (x,y).
    if (wallE && x < this.width - 1) {
      this.vWalls.add(`${x},${y}`);
    }
  }

  parseField(text) {
    this.reset();
    const lines = text.split(/\r?\n/);
    let lineIndex = 0;

    // Helper to get next non-comment, non-empty line
    const nextDataLine = () => {
      while (lineIndex < lines.length) {
        const line = lines[lineIndex++].trim();
        if (line && !line.startsWith(';')) {
          return line;
        }
      }
      return null;
    };

    // Line 1: Field size (x, y)
    const sizeLine = nextDataLine();
    if (sizeLine) {
      const parts = sizeLine.split(/\s+/);
      this.width = parseInt(parts[0], 10) || 7;
      this.height = parseInt(parts[1], 10) || 7;
    }

    // Line 2: Robot position (x, y), 1-indexed in .fil.
    const robotLine = nextDataLine();
    if (robotLine) {
      const parts = robotLine.split(/\s+/);
      this.robotX = Math.max(0, (parseInt(parts[0], 10) || 1) - 1);
      this.robotY = Math.max(0, (parseInt(parts[1], 10) || 1) - 1);
    }

    // Remaining lines: x y Wall Color Radiation Temperature Symbol Symbol1 Point.
    // Coordinates are 1-indexed in .fil.
    // Wall is a bit mask: 1 = North, 2 = South, 4 = West, 8 = East.
    let dataLine;
    while ((dataLine = nextDataLine()) !== null) {
      const parts = dataLine.split(/\s+/);
      if (parts.length < 2) continue;

      const x = parseInt(parts[0], 10) - 1;
      const y = parseInt(parts[1], 10) - 1;
      if (isNaN(x) || isNaN(y)) continue;

      const wall = parseInt(parts[2] || '0', 10) || 0;
      this.addWalls(x, y, (wall & 1) !== 0, (wall & 8) !== 0, (wall & 2) !== 0, (wall & 4) !== 0);

      const color = parseInt(parts[3] || '0', 10) || 0;
      if (color !== 0) {
        this.colors.set(`${x},${y}`, color);
        this.painted.add(`${x},${y}`);
      }

      if (parts[4] !== undefined) {
        const rad = parseFloat(parts[4]);
        if (!isNaN(rad) && rad !== 0) {
          this.radiation.set(`${x},${y}`, rad);
        }
      }

      if (parts[5] !== undefined) {
        const temp = parseFloat(parts[5]);
        if (!isNaN(temp) && temp !== 0) {
          this.temperature.set(`${x},${y}`, temp);
        }
      }

      if (parts[6] !== undefined && parts[6] !== '$') {
        this.symbol.set(`${x},${y}`, parts[6]);
      }
      if (parts[7] !== undefined && parts[7] !== '$') {
        this.symbol1.set(`${x},${y}`, parts[7]);
      }
      if (parts[8] !== undefined) {
        const point = parseInt(parts[8], 10);
        if (!isNaN(point) && point !== 0) {
          this.point.set(`${x},${y}`, point);
        }
      }
    }
  }

  // Check if there's a wall preventing movement
  hasWallLeft() {
    if (this.robotX <= 0) return true;
    return this.vWalls.has(`${this.robotX - 1},${this.robotY}`);
  }

  hasWallRight() {
    if (this.robotX >= this.width - 1) return true;
    return this.vWalls.has(`${this.robotX},${this.robotY}`);
  }

  hasWallUp() {
    if (this.robotY <= 0) return true;
    return this.hWalls.has(`${this.robotX},${this.robotY - 1}`);
  }

  hasWallDown() {
    if (this.robotY >= this.height - 1) return true;
    return this.hWalls.has(`${this.robotX},${this.robotY}`);
  }

  isPainted() {
    return this.painted.has(`${this.robotX},${this.robotY}`);
  }

  paint() {
    this.painted.add(`${this.robotX},${this.robotY}`);
    this.colors.set(`${this.robotX},${this.robotY}`, 1);
  }

  getRadiation() {
    return this.radiation.get(`${this.robotX},${this.robotY}`) || 0;
  }

  getTemperature() {
    return this.temperature.get(`${this.robotX},${this.robotY}`) || 0;
  }

  toText() {
    let lines = [];
    lines.push('; Field Size: x, y');
    lines.push(`${this.width} ${this.height}`);
    lines.push('; Robot position: x, y');
    lines.push(`${this.robotX + 1} ${this.robotY + 1}`);
    lines.push('; A set of special Fields: x, y, Wall, Color, Radiation, Temperature, Symbol, Symbol1, Point');

    // Collect all cells with special properties
    const specialCells = new Set();
    for (const key of this.painted) specialCells.add(key);
    for (const key of this.colors.keys()) specialCells.add(key);
    for (const key of this.hWalls) {
      // hWalls "x,y" = wall below (x,y), which is South wall for (x,y) and North wall for (x,y+1)
      const [x, y] = key.split(',').map(Number);
      specialCells.add(`${x},${y}`);
      if (y + 1 < this.height) specialCells.add(`${x},${y + 1}`);
    }
    for (const key of this.vWalls) {
      // vWalls "x,y" = wall right of (x,y), which is East wall for (x,y) and West wall for (x+1,y)
      const [x, y] = key.split(',').map(Number);
      specialCells.add(`${x},${y}`);
      if (x + 1 < this.width) specialCells.add(`${x + 1},${y}`);
    }
    for (const key of this.radiation.keys()) specialCells.add(key);
    for (const key of this.temperature.keys()) specialCells.add(key);
    for (const key of this.symbol.keys()) specialCells.add(key);
    for (const key of this.symbol1.keys()) specialCells.add(key);
    for (const key of this.point.keys()) specialCells.add(key);

    const sortedCells = Array.from(specialCells).sort((a, b) => {
      const [ax, ay] = a.split(',').map(Number);
      const [bx, by] = b.split(',').map(Number);
      return ay === by ? ax - bx : ay - by;
    });

    for (const key of sortedCells) {
      const [x, y] = key.split(',').map(Number);
      const wallN = (y > 0 && this.hWalls.has(`${x},${y - 1}`)) ? 1 : 0;
      const wallS = (y < this.height - 1 && this.hWalls.has(`${x},${y}`)) ? 1 : 0;
      const wallW = (x > 0 && this.vWalls.has(`${x - 1},${y}`)) ? 1 : 0;
      const wallE = (x < this.width - 1 && this.vWalls.has(`${x},${y}`)) ? 1 : 0;
      const wall = wallN + wallS * 2 + wallW * 4 + wallE * 8;
      const color = this.colors.get(key) || (this.painted.has(key) ? 1 : 0);
      const rad = this.radiation.get(key) || 0;
      const temp = this.temperature.get(key) || 0;
      const symbol = this.symbol.get(key) || '$';
      const symbol1 = this.symbol1.get(key) || '$';
      const point = this.point.get(key) || 0;
      lines.push(`${x + 1} ${y + 1} ${wall} ${color} ${rad.toFixed(6)} ${temp.toFixed(6)} ${symbol} ${symbol1} ${point}`);
    }

    return lines.join('\n');
  }
}

export const field = new RobotField();

// Remembers the wall for the field view and throws with the source location
// of the command.
function robotCrash(side, msg, locator) {
  field.crash = { x: field.robotX, y: field.robotY, side };
  throw new Error(`Робот: ${msg}${formatLocator(locator)}`);
}

// File manager integration - same pattern as io.js
export function __setRobotFilesAccessor(accessor, addFile, updateFile) {
  __filesAccessor = accessor;
  __addFileCallback = addFile;
  __updateFileCallback = updateFile;
}

function getFiles() {
  return typeof __filesAccessor === 'function' ? __filesAccessor() : [];
}

function findFilFile() {
  const files = getFiles();
  for (const f of files) {
    const name = (f.name || '').toLowerCase();
    if (name.endsWith('.fil')) return f;
  }
  return null;
}

function writeFieldToFile() {
  let filFile = findFilFile();
  const content = field.toText();
  if (!filFile) {
    if (typeof __addFileCallback !== 'function') return false;
    __addFileCallback({ name: 'robot.fil', content });
    return true;
  }
  filFile.content = content;
  if (typeof __updateFileCallback === 'function') {
    __updateFileCallback(filFile.id, content);
  }
  return true;
}

// Lazy loading flag - field is loaded on first robot command
let __fieldLoaded = false;

// Load field from .fil file (called lazily on first robot command)
function ensureFieldLoaded() {
  if (__fieldLoaded) return;
  __fieldLoaded = true;

  const filFile = findFilFile();

  if (filFile && filFile.content) {
    field.parseField(filFile.content);
  } else {
    field.parseField(DEFAULT_FIELD);
  }

}

export function __initRobotField() {
  field.reset();
  __fieldLoaded = false;
}

// Preview field from .fil file without running program
// Returns true if field was loaded, false otherwise
export function __previewField() {
  field.reset();
  __fieldLoaded = true;

  const filFile = findFilFile();

  if (filFile && filFile.content) {
    field.parseField(filFile.content);
    return true;
  } else {
    field.parseField(DEFAULT_FIELD);
    return false;
  }
}

export function __resetRobot() {
  __initRobotField();
}

export function __getRobotState() {
  return {
    x: field.robotX,
    y: field.robotY,
    width: field.width,
    height: field.height,
    painted: Array.from(field.painted),
    hWalls: Array.from(field.hWalls),
    vWalls: Array.from(field.vWalls)
  };
}

export function __writeFieldToFil() {
  return writeFieldToFile();
}

export function __setRobotPosition(x, y) {
  ensureFieldLoaded();
  field.robotX = Math.max(0, Math.min(field.width - 1, Number(x) | 0));
  field.robotY = Math.max(0, Math.min(field.height - 1, Number(y) | 0));
  return writeFieldToFile();
}

export function __resizeField(width, height) {
  ensureFieldLoaded();
  field.resize(width, height);
  return writeFieldToFile();
}

export function __toggleWall(x, y, side) {
  ensureFieldLoaded();
  x = Number(x) | 0;
  y = Number(y) | 0;
  let set = null;
  let key = null;
  if (side === 'north' && y > 0) {
    set = field.hWalls;
    key = `${x},${y - 1}`;
  } else if (side === 'south' && y < field.height - 1) {
    set = field.hWalls;
    key = `${x},${y}`;
  } else if (side === 'west' && x > 0) {
    set = field.vWalls;
    key = `${x - 1},${y}`;
  } else if (side === 'east' && x < field.width - 1) {
    set = field.vWalls;
    key = `${x},${y}`;
  }
  if (!set || !key) return false;
  if (set.has(key)) set.delete(key);
  else set.add(key);
  return writeFieldToFile();
}

export function __togglePainted(x, y) {
  ensureFieldLoaded();
  const key = `${Number(x) | 0},${Number(y) | 0}`;
  if (field.painted.has(key) || field.colors.has(key)) {
    field.painted.delete(key);
    field.colors.delete(key);
  } else {
    field.painted.add(key);
    field.colors.set(key, 1);
  }
  return writeFieldToFile();
}

export function __getCellProperties(x, y) {
  ensureFieldLoaded();
  const key = `${Number(x) | 0},${Number(y) | 0}`;
  return {
    color: field.colors.get(key) || (field.painted.has(key) ? 1 : 0),
    radiation: field.radiation.get(key) || 0,
    temperature: field.temperature.get(key) || 0,
    symbol: field.symbol.get(key) || '$',
    symbol1: field.symbol1.get(key) || '$',
    point: field.point.get(key) || 0
  };
}

export function __setCellProperties(x, y, props = {}) {
  ensureFieldLoaded();
  const key = `${Number(x) | 0},${Number(y) | 0}`;
  const color = parseInt(props.color ?? 0, 10) || 0;
  if (color) {
    field.colors.set(key, color);
    field.painted.add(key);
  } else {
    field.colors.delete(key);
    field.painted.delete(key);
  }
  const radiation = parseFloat(props.radiation ?? 0);
  if (!isNaN(radiation) && radiation !== 0) field.radiation.set(key, radiation);
  else field.radiation.delete(key);
  const temperature = parseFloat(props.temperature ?? 0);
  if (!isNaN(temperature) && temperature !== 0) field.temperature.set(key, temperature);
  else field.temperature.delete(key);
  const symbol = String(props.symbol ?? '$').trim() || '$';
  if (symbol !== '$') field.symbol.set(key, symbol);
  else field.symbol.delete(key);
  const symbol1 = String(props.symbol1 ?? '$').trim() || '$';
  if (symbol1 !== '$') field.symbol1.set(key, symbol1);
  else field.symbol1.delete(key);
  const point = parseInt(props.point ?? 0, 10) || 0;
  if (point) field.point.set(key, point);
  else field.point.delete(key);
  return writeFieldToFile();
}

// Runtime API functions (exported for WASM)

function _robotOp(execute) {
  if (!__futureRuntime) { execute(); return 0; }
  const h = __futureRuntime.allocFuture();
  __futureRuntime.enqueuePendingOp({ h, execute });
  return h;
}

export function robot_left() {
  const locator = currentLocator();
  return _robotOp(() => {
    ensureFieldLoaded();
    if (field.hasWallLeft()) robotCrash('left', 'слева стена', locator);
    field.robotX--;
  });
}

export function robot_right() {
  const locator = currentLocator();
  return _robotOp(() => {
    ensureFieldLoaded();
    if (field.hasWallRight()) robotCrash('right', 'справа стена', locator);
    field.robotX++;
  });
}

export function robot_up() {
  const locator = currentLocator();
  return _robotOp(() => {
    ensureFieldLoaded();
    if (field.hasWallUp()) robotCrash('up', 'сверху стена', locator);
    field.robotY--;
  });
}

export function robot_down() {
  const locator = currentLocator();
  return _robotOp(() => {
    ensureFieldLoaded();
    if (field.hasWallDown()) robotCrash('down', 'снизу стена', locator);
    field.robotY++;
  });
}

export function robot_paint() {
  return _robotOp(() => {
    ensureFieldLoaded();
    field.paint();
  });
}

export function robot_left_free() {
  ensureFieldLoaded();
  return !field.hasWallLeft();
}

export function robot_right_free() {
  ensureFieldLoaded();
  return !field.hasWallRight();
}

export function robot_top_free() {
  ensureFieldLoaded();
  return !field.hasWallUp();
}

export function robot_bottom_free() {
  ensureFieldLoaded();
  return !field.hasWallDown();
}

export function robot_left_wall() {
  ensureFieldLoaded();
  return field.hasWallLeft();
}

export function robot_right_wall() {
  ensureFieldLoaded();
  return field.hasWallRight();
}

export function robot_top_wall() {
  ensureFieldLoaded();
  return field.hasWallUp();
}

export function robot_bottom_wall() {
  ensureFieldLoaded();
  return field.hasWallDown();
}

export function robot_cell_painted() {
  ensureFieldLoaded();
  return field.isPainted();
}

export function robot_cell_clean() {
  ensureFieldLoaded();
  return !field.isPainted();
}

export function robot_radiation() {
  ensureFieldLoaded();
  return field.getRadiation();
}

export function robot_temperature() {
  ensureFieldLoaded();
  return field.getTemperature();
}

export function __setAnimationDelay(delay) {
  __animationDelay = delay;
}

export function __getAnimationDelay() {
  return __animationDelay;
}
