import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { request as httpRequest } from 'node:http';
import { request as httpsRequest } from 'node:https';
import puppeteer from 'puppeteer';

const url = process.env.QUMIR_TEST_URL || 'http://127.0.0.1:8097/';
const server = process.env.QUMIR_TEST_URL ? null : spawn('build/service/server', [
  '--port', '8097', '--static-dir', 'service/static', '--binary-dir', 'build/bin',
  '--examples-dir', 'examples', '--shared-links-dir', 'build/browser-shared',
], { stdio: ['ignore', 'ignore', 'pipe'] });
server?.stderr.on('data', data => {
  if (String(data).includes('Error')) process.stderr.write(data);
});
let browser;
const errors = [];

async function openPage(fallback = false) {
  const page = await browser.newPage();
  await page.setViewport({ width: 1400, height: 1000 });
  await page.browserContext().setCookie({ name: 'q_tour_seen', value: '1', url });
  page.on('pageerror', error => { errors.push(error.message); console.error('PAGE', error.message); });
  if (fallback) {
    await page.evaluateOnNewDocument(() => {
      WebAssembly.Suspending = undefined;
      WebAssembly.promising = undefined;
    });
  }
  await page.goto(url, { waitUntil: 'networkidle0' });
  await page.waitForSelector('.CodeMirror');
  return page;
}

async function code(page, source, core = false) {
  await page.evaluate((text, useCore) => {
    const syntax = document.querySelector('#syntax-mode');
    syntax.value = useCore ? 'core' : 'kumir';
    syntax.dispatchEvent(new Event('change'));
    document.querySelector('.CodeMirror').CodeMirror.setValue(text);
  }, source, core);
}

async function paused(page, line) {
  await page.waitForFunction(expected => {
    const panel = document.querySelector('#debug-panel');
    return panel.dataset.paused === 'true' && (!expected || Number(panel.dataset.line) === expected);
  }, { timeout: 10000 }, line);
}

async function action(page, name, line) {
  await page.click(`[data-debug-action="${name}"]`);
  await paused(page, line);
}

async function locals(page) {
  return await page.$$eval('[data-debug-locals] tr', rows => Object.fromEntries(rows.map(row => {
    const cells = [...row.querySelectorAll('td')].map(cell => cell.textContent);
    return [cells[0], cells[2]];
  })));
}

async function breakpoint(page, line) {
  await page.evaluate(number => {
    const cm = document.querySelector('.CodeMirror').CodeMirror;
    window.CodeMirror.signal(cm, 'gutterClick', cm, number - 1, 'q-debug-gutter');
  }, line);
}

async function finished(page) {
  await page.waitForFunction(() => !document.querySelector('#btn-run').disabled, { timeout: 10000 });
  assert.equal(await page.$eval('#debug-panel', panel => panel.hidden), true);
  assert.equal(await page.$eval('.CodeMirror', el => el.CodeMirror.getOption('readOnly')), false);
}

async function compile(source, headers) {
  return await new Promise((resolve, reject) => {
    const request = (url.startsWith('https:') ? httpsRequest : httpRequest)(new URL('/api/compile-wasm', url), {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain', 'Content-Length': Buffer.byteLength(source), ...headers },
    }, response => {
      const chunks = [];
      response.on('data', chunk => chunks.push(chunk));
      response.on('end', () => resolve({ status: response.statusCode, headers: response.headers, body: Buffer.concat(chunks) }));
    });
    request.on('error', reject);
    request.end(source);
  });
}

try {
  browser = await puppeteer.launch({ headless: true, args: ['--ignore-certificate-errors'] });
  const page = await openPage(true);
  const compilations = [];
  page.on('response', response => {
    if (!response.url().endsWith('/api/compile-wasm') || !response.ok()) return;
    compilations.push(response.buffer().then(bytes => {
      const module = new WebAssembly.Module(bytes);
      return {
        flag: response.request().headers()['x-qumir-debug-points'],
        metadata: WebAssembly.Module.customSections(module, 'qumir.debug').length,
        checkpoint: WebAssembly.Module.imports(module).some(imported => imported.name === '__qumir_debug_point'),
      };
    }));
  });
  assert.equal(await page.evaluate(() => typeof WebAssembly.Suspending), 'undefined');
  const simple = 'алг цел main\nнач\n  цел x\n  x := twice(21)\n  вывод x, нс\n  знач := x\nкон\n\n'
    + 'алг цел twice(цел n)\nнач\n  цел next\n  next := n + 1\n  знач := next * 2\nкон\n';
  for (const header of ['X-Qumir-Debug-Points', 'x-qumir-debug-points', 'x-QuMiR-dEbUg-PoInTs']) {
    const response = await compile(simple, { [header]: '1', 'x-qumir-o': '3' });
    assert.equal(response.status, 200, response.body.toString());
    assert.equal(response.headers['x-qumir-o'], '0');
    assert.equal(WebAssembly.Module.customSections(new WebAssembly.Module(response.body), 'qumir.debug').length, 1);
  }
  const core = await compile('(block (fun main () -> i64 (block (return 42))))', { 'x-qumir-syntax': 'core' });
  assert.equal(core.status, 200, core.body.toString());
  const coroutine = await compile(simple, { 'x-qumir-o': '3' });
  assert.equal(coroutine.status, 200, coroutine.body.toString());
  assert.equal(coroutine.headers['x-qumir-o'], '3');
  const coroutineModule = new WebAssembly.Module(coroutine.body);
  assert.equal(WebAssembly.Module.customSections(coroutineModule, 'qumir.debug').length, 0);
  assert.equal(JSON.parse(new TextDecoder().decode(WebAssembly.Module.customSections(coroutineModule, 'qumir.runtime')[0])).mode, 'coroutine');
  await code(page, simple);
  await page.click('[data-pane-action="dock"][data-pane-target="io"]');
  await page.waitForSelector('.run-hint-arrow.show', { timeout: 15000 });
  await page.click('#btn-debug');
  await paused(page, 3);
  assert.equal(await page.$('.run-hint-arrow.show'), null);
  assert.deepEqual(await compilations.at(-1), { flag: '1', metadata: 1, checkpoint: true });
  const bottomLayout = await page.evaluate(() => {
    const debug = document.querySelector('#debug-panel').getBoundingClientRect();
    const io = document.querySelector('section.io').getBoundingClientRect();
    const editor = document.querySelector('.pane.left').getBoundingClientRect();
    const controls = document.querySelector('.q-debug-controls').getBoundingClientRect();
    const label = document.querySelector('#debug-panel .pane-title-label').getBoundingClientRect();
    const button = document.querySelector('[data-debug-action="over"]');
    return debug.top >= editor.bottom && debug.right <= io.left && debug.top === io.top && debug.bottom === io.bottom
      && controls.left - label.right <= 10 && getComputedStyle(button).backgroundColor !== getComputedStyle(button.parentElement.parentElement).backgroundColor;
  });
  assert.equal(bottomLayout, true);
  assert.equal(await page.$$eval('[data-debug-action]', buttons => buttons.every(button =>
    button.querySelector('svg') && button.dataset.tooltip && button.getAttribute('aria-label'))), true);
  await page.hover('[data-debug-action="over"]');
  await page.waitForFunction(() => [...document.querySelectorAll('.q-tooltip')].some(el =>
    el.style.display === 'block' && el.textContent === 'Шаг с обходом'));
  await action(page, 'over', 4);
  await action(page, 'in', 11);
  assert.equal((await locals(page)).n, '21');
  assert.equal(await page.$$eval('[data-debug-stack] button', buttons => buttons.length), 2);
  await page.click('[data-debug-stack] button:nth-child(2)');
  assert.equal((await locals(page)).x, '0');
  await page.click('[data-debug-stack] button:first-child');
  await action(page, 'out', 4);
  await action(page, 'over', 5);
  assert.equal((await locals(page)).x, '44');
  const coords = await page.evaluate(() => {
    const cm = document.querySelector('.CodeMirror').CodeMirror;
    return cm.charCoords({ line: 4, ch: 8 }, 'window');
  });
  await page.mouse.move(coords.left + 2, (coords.top + coords.bottom) / 2);
  await page.waitForFunction(() => !document.querySelector('.q-debug-tooltip').hidden);
  assert.match(await page.$eval('.q-debug-tooltip', el => el.textContent), /x = 44/);
  await page.screenshot({ path: 'build/browser_debugger.png' });
  await page.click('[data-debug-action="continue"]');
  await finished(page);
  assert.equal((await page.$eval('#stdout', el => el.textContent)).trim(), '44');
  assert.match(await page.$eval('#errors', el => el.textContent), /вернула: 44/);
  assert.equal(await page.$eval('.q-debug-tooltip', el => el.hidden), true);
  assert.equal(await page.evaluate(() => document.querySelector('section.io').getBoundingClientRect().left
    >= document.querySelector('.workspace-main-row').getBoundingClientRect().right), true);
  await page.click('[data-pane-action="dock"][data-pane-target="io"]');
  await page.click('[data-pane-action="dock"][data-pane-target="io"]');

  const demo = readFileSync('test/debugger_demo.kum', 'utf8');
  await code(page, demo);
  await breakpoint(page, 43);
  await breakpoint(page, 56);
  await breakpoint(page, 72);
  await page.click('#btn-debug');
  await paused(page);
  await action(page, 'continue', 43);
  assert.equal((await locals(page)).n, '6');
  await action(page, 'continue', 43);
  assert.equal((await locals(page)).n, '5');
  assert.equal(await page.$$eval('[data-debug-stack] button', buttons => buttons.filter(button => button.textContent.startsWith('fibonacci')).length), 2);
  await page.click('[data-debug-stack] button:nth-child(2)');
  assert.equal((await locals(page)).n, '6');
  await page.click('[data-debug-stack] button:first-child');
  await breakpoint(page, 43);
  await action(page, 'continue', 56);
  assert.equal((await locals(page)).value, '21');
  await action(page, 'over', 57);
  assert.equal((await locals(page)).value, '22');
  await action(page, 'continue', 72);
  const values = await locals(page);
  assert.equal(values.next, '23');
  assert.equal(values.scaled, '76.5');
  assert.equal(values.positive, 'true');
  assert.equal(values.marker, '"A"');
  assert.equal(values.letter, '"A"');
  assert.equal(values.message, '"LLDB sample locals"');
  await page.click('#btn-stop');
  await finished(page);

  const shadow = '(block\n  (fun main () -> i64\n    (block\n      (var x i64)\n      (= x 10)\n      (block\n        (var x i64)\n        (= x 20)\n        (output x "\\n"))\n      (return x))))\n';
  await code(page, shadow, true);
  await breakpoint(page, 9);
  await page.click('#btn-debug');
  await paused(page, 4);
  await action(page, 'continue', 9);
  assert.equal((await locals(page)).x, '20');
  await action(page, 'over', 10);
  assert.equal((await locals(page)).x, '10');
  await page.click('#btn-stop');
  await finished(page);

  const sameLine = '(block\n (fun main () -> i64 (block (return (+ (call first) (call second)))))\n'
    + ' (fun first () -> i64 (block (return 1)))\n (fun second () -> i64 (block (return 2))))\n';
  await code(page, sameLine, true);
  await page.click('#btn-debug');
  await paused(page, 2);
  await action(page, 'in', 3);
  await action(page, 'out', 2);
  await action(page, 'in', 4);
  await action(page, 'out', 2);
  await page.click('[data-debug-action="continue"]');
  await finished(page);

  const loop = 'алг main\nнач\n  цел x\n  x := 0\n  нц пока x < 3\n    x := x + 1\n  кц\n  вывод x, нс\nкон\n';
  await code(page, loop);
  await breakpoint(page, 5);
  await page.click('#btn-debug');
  await paused(page, 3);
  await action(page, 'continue', 5);
  assert.equal((await locals(page)).x, '0');
  await action(page, 'continue', 5);
  assert.equal((await locals(page)).x, '1');
  await page.click('#btn-stop');
  await finished(page);

  await code(page, loop.replace('x < 3', '1 = 1'));
  await page.click('#btn-debug');
  await paused(page);
  await page.click('[data-debug-action="continue"]');
  await page.click('[data-debug-action="pause"]');
  await paused(page);
  await page.click('#btn-stop');
  await finished(page);

  const asyncProgram = 'алг цел main\nнач\n  пауза(1)\n  вывод "async", нс\n  знач := 42\nкон\n';
  const sleepSource = asyncProgram.replace('пауза(1)', 'ждать(1)');
  await code(page, sleepSource);
  await page.click('#btn-run');
  await finished(page);
  assert.match(await page.$eval('#stdout', el => el.textContent), /async/);
  assert.deepEqual(await compilations.at(-1), { flag: undefined, metadata: 0, checkpoint: false });
  assert.equal(await page.$('.run-hint-arrow.show'), null);

  const waitingKeyboard = 'использовать Клавиатура\nалг main\nнач\n  цел key\n  key := код клав\n  вывод key, нс\nкон\n';
  await code(page, waitingKeyboard);
  await page.click('#btn-run');
  await page.waitForNetworkIdle({ idleTime: 200 });
  await page.click('#btn-stop');
  await finished(page);
  await page.click('#btn-run');
  await page.waitForNetworkIdle({ idleTime: 200 });
  await page.keyboard.press('KeyA');
  await finished(page);
  assert.equal((await page.$eval('#stdout', el => el.textContent)).trim(), '65');

  const graphics = [
    ['Робот', 'закрасить'],
    ['Черепаха', 'вперед(10)\n  вправо(90)\n  назад(5)'],
    ['Чертежник', 'сместиться в точку(1, 2)\n  сместиться на вектор(3, 4)\n  написать(10, "text")'],
    ['Рисователь', 'новый лист(20, 30, белый)\n  вывод ширина листа, " ", высота листа, нс'],
  ];
  for (const [module, commands] of graphics) {
    await code(page, `использовать ${module}\nалг main\nнач\n  ${commands}\nкон\n`);
    await page.click('#btn-run');
    await finished(page);
    assert.equal(await page.evaluate(async () => (await import('./runtime/future.js')).hasPendingOp()), false);
    assert.equal(await page.$eval('#errors', el => el.textContent.includes('Успешно')), true);
    if (module === 'Рисователь') assert.match(await page.$eval('#stdout', el => el.textContent), /20 30/);
  }

  // Async imports and IR-promoted calls must share the same scheduler.
  await code(page, sleepSource);
  await page.click('#btn-debug');
  await paused(page);
  await page.click('[data-debug-action="continue"]');
  await finished(page);
  assert.match(await page.$eval('#stdout', el => el.textContent), /async/);

  const globals = '(block\n (var x = (call init))\n'
    + ' (fun main () (block (output x "\\n")))\n'
    + ' (fun init () -> i64 (block (return 42))))\n';
  await code(page, globals, true);
  await page.click('#btn-debug');
  await paused(page);
  await page.click('[data-debug-action="continue"]');
  await finished(page);
  assert.match(await page.$eval('#stdout', el => el.textContent), /42/);

  for (const [type, literal, expected] of [
    ['вещ', '2.5', '2.500000000000000'],
    ['лит', '"hello"', 'hello'],
    ['лог', 'да', '1'],
  ]) {
    await code(page, `алг ${type} first\nнач\n  знач := main\nкон\n`
      + `алг ${type} main\nнач\n  знач := ${literal}\nкон\n`);
    await page.click('#btn-debug');
    await paused(page);
    await page.click('[data-debug-action="continue"]');
    await finished(page);
    const result = await page.$eval('#errors', el => el.textContent);
    assert.match(result, /Функция "first" вернула:/);
    assert.ok(result.includes(expected), result);
  }

  // Ordinary Run must distinguish a synchronous entry from unrelated coroutines.
  await code(page, 'алг цел first\nнач\n  знач := 42\nкон\n'
    + 'алг other\nнач\n  ждать(1)\nкон\n');
  await page.click('#btn-run');
  await finished(page);
  assert.match(await page.$eval('#errors', el => el.textContent), /Функция "first" вернула: 42/);

  // Ordinary Run reads the entry promise using its source result type.
  for (const [type, literal, expected] of [
    ['цел', '42', '42'],
    ['вещ', '2.5', '2.500000000000000'],
    ['лит', '"hello"', 'hello'],
    ['лог', 'да', '1'],
  ]) {
    await code(page, `алг ${type} first\nнач\n  ждать(1)\n  знач := ${literal}\nкон\n`);
    await page.click('#btn-run');
    await finished(page);
    const result = await page.$eval('#errors', el => el.textContent);
    assert.match(result, /Функция "first" вернула:/);
    assert.ok(result.includes(expected), result);
  }

  const asyncGlobals = globals.replace('(block (return 42))', '(block (call ждать 1) (return 42))');
  for (const debug of [false, true]) {
    await code(page, asyncGlobals, true);
    await page.click(debug ? '#btn-debug' : '#btn-run');
    if (debug) {
      await paused(page);
      await page.click('[data-debug-action="continue"]');
    }
    await finished(page);
    assert.match(await page.$eval('#stdout', el => el.textContent), /42/);
  }

  const printerResult = await page.evaluate(async () => {
    const { ValuePrinter } = await import('./runtime/value_printer.js');
    const memory = new WebAssembly.Memory({ initial: 1 });
    const type = { kind: 'Int', bits: 64, signed: true };
    new DataView(memory.buffer).setBigInt64(8, 9007199254740993n, true);
    const printer = new ValuePrinter(memory, {});
    const before = printer.value(type, 8);
    memory.grow(1);
    return [before, printer.value(type, 8)];
  });
  assert.deepEqual(printerResult, ['9007199254740993', '9007199254740993']);

  const fallback = await openPage(true);
  assert.equal(await fallback.$eval('#btn-debug', el => el.disabled), false);
  await code(fallback, simple);
  await fallback.click('#btn-debug');
  await paused(fallback);
  await fallback.click('#btn-stop');
  await finished(fallback);
  await code(fallback, sleepSource);
  await fallback.click('#btn-run');
  await finished(fallback);
  assert.match(await fallback.$eval('#stdout', el => el.textContent), /async/);
  assert.deepEqual(errors, []);
  console.log(`${await browser.version()}: stepping, recursive stack, references, locals, hover, shadowing, Pause/Stop, keyboard, graphics, coroutines without browser suspension extensions passed`);
} catch (error) {
  for (const page of await browser?.pages() || []) {
    console.error('STATE', await page.evaluate(() => ({panel:document.querySelector('#debug-panel')?.innerText,stdout:document.querySelector('#stdout')?.innerText,errors:document.querySelector('[data-io-pane=errors]')?.innerText || document.querySelector('#errors')?.innerText})));
  }
  throw error;
} finally {
  await browser?.close();
  server?.kill();
}
