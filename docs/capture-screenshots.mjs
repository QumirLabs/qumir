#!/usr/bin/env node
// Retakes the screenshots used in the documentation.
//
//   npm install --no-save puppeteer  (once, in the repository root)
//   npx puppeteer browsers install chrome-headless-shell
//   build/service/server --port 8080 --static-dir service/static \
//       --qumir-runtime qumir/runtime/js \
//       --binary-dir build/bin --examples-dir examples --shared-links-dir /tmp/shared
//   node docs/capture-screenshots.mjs [http://localhost:8080]
//
// The pictures are written to docs/ru/screenshots.

import path from 'path';
import { fileURLToPath } from 'url';
import puppeteer from 'puppeteer';

const base = (process.argv[2] || 'http://localhost:8080').replace(/\/$/, '');
const out = path.join(path.dirname(fileURLToPath(import.meta.url)), 'ru', 'screenshots');
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));

const DESKTOP = { width: 1280, height: 800 };
const PHONE = { width: 390, height: 844, isMobile: true, hasTouch: true, deviceScaleFactor: 2 };

const browser = await puppeteer.launch({ headless: 'shell' });

// Every picture starts from a clean browser profile with the tour already seen
// and without the confetti of the first successful run.
async function open(viewport, url = '/') {
  const context = await browser.createBrowserContext();
  const page = await context.newPage();
  await page.setViewport(viewport);
  await page.setCookie(
    { name: 'q_tour_seen', value: '1', url: base },
    { name: 'q_runs_count', value: '1', url: base });
  await page.goto(base + url, { waitUntil: 'networkidle0' });
  await wait(500);
  return page;
}

const setCode = (page, code) =>
  page.evaluate(c => document.querySelector('.CodeMirror').CodeMirror.setValue(c), code);

async function run(page, ms = 1500) {
  await wait(700);
  await page.click('#btn-run');
  await wait(ms);
}

const shot = (page, name) => page.screenshot({ path: path.join(out, name) });

const SQUARES = `алг
нач
    вывод "Привет, мир!", нс
    цел i
    нц для i от 1 до 5
        вывод i, " в квадрате = ", i * i, нс
    кц
кон
`;

{
  const page = await open(DESKTOP);
  await setCode(page, SQUARES);
  await run(page);
  await shot(page, 'ui-overview.png');
}

{
  const page = await open(DESKTOP);
  await setCode(page, 'алг\nнач\n    цел сумма\n    сумма := 10\n    вывод сума, нс\nкон\n');
  await wait(1500);
  await shot(page, 'ui-error.png');
}

{
  const page = await open(DESKTOP);
  await setCode(page, 'алг\nнач\n    цел a, b\n    ввод a, b\n    вывод a, " + ", b, " = ", a + b, нс\nкон\n');
  await page.evaluate(() => { document.getElementById('stdin').value = '2 3'; });
  await run(page);
  await page.evaluate(() => document.querySelector('.io-tab[data-io-tab="stdin"]').click());
  await wait(300);
  await shot(page, 'ui-input.png');
}

{
  const page = await open(DESKTOP);
  await page.select('#examples', 'turtle/koch_snowflake.kum');
  await wait(800);
  await page.select('#examples', 'robot/simple.kum');
  await wait(800);
  await page.click('#project-toggle');
  await wait(500);
  await shot(page, 'ui-projects.png');
}

{
  const page = await open(DESKTOP);
  await page.click('#btn-help');
  await wait(1500);
  await shot(page, 'ui-help.png');
}

{
  const page = await open(PHONE);
  await setCode(page, SQUARES);
  await page.tap('#btn-run');
  await wait(1500);
  await shot(page, 'ui-phone.png');
}

{
  const page = await open(DESKTOP);
  await page.select('#examples', 'robot/simple.kum');
  await run(page, 4000);
  await shot(page, 'robot.png');
}

{
  const page = await open(DESKTOP);
  await setCode(page, 'использовать Робот\nалг\nнач\n    вниз\n    нц 10 раз\n        закрасить\n        вправо\n    кц\nкон\n');
  await run(page, 9000);
  await shot(page, 'robot-wall.png');
}

for (const [example, name, ms] of [
  ['turtle/fractal_tree.kum', 'turtle.png', 4000],
  ['drawer/function.kum', 'drawer.png', 6000],
  ['painter/mandelbrot.kum', 'painter.png', 8000],
]) {
  const page = await open(DESKTOP);
  await page.select('#examples', example);
  await run(page, ms);
  await shot(page, name);
}

await browser.close();
