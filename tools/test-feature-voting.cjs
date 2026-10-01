/* Run with Node and Playwright installed. Only reads local files; GitHub responses are mocked. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '../docs');
const features = JSON.parse(fs.readFileSync(path.join(root, 'assets/feature-requests.json')));
const api = '**/api.github.com/repos/Roastedd/CoffeeFlix/issues/15/comments?*';
const fixture = () => features.map(f => ({id: f.commentId, body: `<!-- coffeeflix-status: ${f.status} -->`, user: {login: 'Roastedd'}, reactions: {'+1': 0, heart: 100}}));
const mime = {'.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.woff2': 'font/woff2', '.png': 'image/png', '.jpg': 'image/jpeg'};
const server = http.createServer((request, response) => {
  const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
  const file = path.resolve(root, '.' + (pathname === '/' ? '/index.html' : pathname));
  if (!file.startsWith(root + path.sep)) {response.writeHead(403).end(); return;}
  try {response.writeHead(200, {'Content-Type': mime[path.extname(file)] || 'text/plain'}); response.end(fs.readFileSync(file));}
  catch (_) {response.writeHead(404).end();}
});

(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({headless: true});
  let checks = 0;
  const pass = name => {checks++; console.log(`PASS ${name}`);};
  async function newPage(options = {}) {
    const context = await browser.newContext(options);
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    page.testErrors = errors;
    return page;
  }
  async function finished(page) {await page.waitForFunction(() => !document.getElementById('refresh').disabled);}
  async function close(page) {assert.deepEqual(page.testErrors, []); await page.context().close();}
  async function noOverflow(page) {assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);}
  try {
    const comments = fixture();
    comments[0].reactions['+1'] = 3;
    comments[4].reactions['+1'] = 8;
    comments[0].body = '<!-- coffeeflix-status: in-progress -->';
    comments[1].body = '<!-- coffeeflix-status: shipped -->';
    comments[1].user.login = 'someone-else';
    const page = await newPage({viewport: {width: 1440, height: 1120}});
    await page.route(api, route => route.fulfill({json: comments}));
    await page.goto(base + '/features.html'); await finished(page);
    assert.equal(await page.locator('.feature-card').count(), 18);
    assert.equal(await page.locator('.feature-card').first().getAttribute('id'), 'controller-keyboard');
    assert.equal(await page.locator('#controller-keyboard [data-votes]').innerText(), '8');
    assert.equal(await page.locator('#startup-page [data-votes]').innerText(), '0');
    assert.equal(await page.locator('#hide-sections .badge').innerText(), 'In progress');
    assert.equal(await page.locator('#startup-page .badge').innerText(), 'Proposed');
    assert.match(await page.locator('#controller-keyboard .vote-link').getAttribute('href'), new RegExp(`#issuecomment-${features[4].commentId}$`));
    pass('shared thumbs-up counts, exact vote links, sorting and trusted status markers');

    await page.locator('#category').selectOption('Twitch');
    assert.equal(await page.locator('.feature-card:visible').count(), 4);
    await page.locator('#category').selectOption('all');
    await page.locator('#search').fill('autoplay');
    assert.equal(await page.locator('.feature-card:visible').count(), 1);
    await page.locator('#search').fill('zz-no-such-feature');
    assert.equal(await page.locator('#empty').isVisible(), true);
    await page.locator('#reset').click();
    assert.equal(await page.locator('.feature-card:visible').count(), 18);
    await page.locator('#status').selectOption('in-progress');
    assert.equal(await page.locator('.feature-card:visible').count(), 1);
    await page.locator('#status').selectOption('all');
    await page.locator('#sort').selectOption('title');
    assert.equal(await page.locator('.feature-card').first().getAttribute('id'), 'personal-greeting');
    await noOverflow(page);
    pass('search, category/status filters, empty state, reset and alphabetical sort');
    await close(page);

    const offline = await newPage();
    await offline.route(api, route => route.abort());
    await offline.goto(base + '/features.html'); await finished(offline);
    assert.equal(await offline.locator('#hide-sections [data-votes]').innerText(), '—');
    assert.match(await offline.locator('#vote-status').innerText(), /Couldn’t load.*Counts are unavailable/);
    assert.equal(await offline.locator('.vote-link[href*="#issuecomment-"]').count(), 18);
    pass('offline failure leaves counts unavailable and all voting links usable');
    await close(offline);

    const cached = await newPage();
    await cached.addInitScript(({id}) => localStorage.setItem('coffeeflix-feature-votes-v1', JSON.stringify({fetchedAt: Date.now() - 360000, totals: {[id]: {votes: 7, status: 'planned'}}})), {id: String(features[0].commentId)});
    await cached.route(api, route => route.fulfill({status: 403, json: {message: 'API rate limit exceeded'}}));
    await cached.goto(base + '/features.html'); await finished(cached);
    assert.equal(await cached.locator('#hide-sections [data-votes]').innerText(), '7');
    assert.match(await cached.locator('#vote-status').innerText(), /API limit.*Showing saved totals/);
    pass('rate limiting retains saved totals and labels them as saved');
    await close(cached);

    const pages = await newPage();
    const filler = Array.from({length: 100}, (_, i) => ({id: i + 1, body: 'Discussion, not a registered feature'}));
    await pages.route(api, route => route.fulfill({json: new URL(route.request().url()).searchParams.get('page') === '1' ? filler : fixture()}));
    await pages.goto(base + '/features.html'); await finished(pages);
    assert.equal(await pages.locator('#hide-sections [data-votes]').innerText(), '0');
    assert.match(await pages.locator('#vote-status').innerText(), /Updated at/);
    pass('pagination finds registered voting comments beyond the first 100');
    await close(pages);

    const partial = await newPage();
    await partial.route(api, route => new URL(route.request().url()).searchParams.get('page') === '1' ? route.fulfill({json: [...fixture(), ...filler.slice(0, 82)]}) : route.fulfill({status: 429, json: {message: 'Rate limited'}}));
    await partial.goto(base + '/features.html'); await finished(partial);
    assert.equal(await partial.locator('#hide-sections [data-votes]').innerText(), '—');
    pass('a failed later API page never publishes incomplete counts');
    await close(partial);

    const missing = await newPage();
    await missing.route(api, route => route.fulfill({json: fixture().slice(1)}));
    await missing.goto(base + '/features.html'); await finished(missing);
    assert.equal(await missing.locator('#hide-sections [data-votes]').innerText(), '—');
    assert.equal(await missing.locator('#hide-sections .vote-link').getAttribute('href'), 'https://github.com/Roastedd/CoffeeFlix/issues/15');
    pass('deleted voting comments remain visibly unavailable and link to the thread');
    await close(missing);

    const staticPage = await newPage({javaScriptEnabled: false});
    await staticPage.goto(base + '/features.html');
    assert.equal(await staticPage.locator('.feature-card:visible').count(), 18);
    assert.equal(await staticPage.locator('.vote-link').count(), 18);
    assert.equal(await staticPage.locator('#toolbar').isVisible(), false);
    pass('all cards and vote links work without JavaScript');
    await close(staticPage);

    const mobile = await newPage({viewport: {width: 375, height: 900}});
    await mobile.route(api, route => route.fulfill({json: fixture()}));
    await mobile.route('**/api.github.com/repos/Roastedd/CoffeeFlix/releases/latest', route => route.fulfill({status: 503, json: {}}));
    await mobile.goto(base + '/features.html'); await finished(mobile);
    await noOverflow(mobile);
    await mobile.screenshot({path: '/private/tmp/coffeeflix-voting-mobile.png', fullPage: true});
    await mobile.setViewportSize({width: 320, height: 800}); await noOverflow(mobile);
    await mobile.goto(base + '/'); await noOverflow(mobile);
    assert.equal(await mobile.locator('.cta a[href="features.html"]').isVisible(), true);
    await mobile.setViewportSize({width: 901, height: 900}); await noOverflow(mobile);
    pass('320/375px mobile board and homepage navigation fit; mobile voting CTA is visible');
    await close(mobile);

    const desktop = await newPage({viewport: {width: 1440, height: 1120}});
    await desktop.route(api, route => route.fulfill({json: fixture()}));
    await desktop.goto(base + '/features.html'); await finished(desktop);
    await desktop.screenshot({path: '/private/tmp/coffeeflix-voting-desktop.png'});
    await close(desktop);
    console.log(`${checks} voting-page checks passed.`);
  } finally {await browser.close(); server.close();}
})().catch(error => {console.error(error); server.close(); process.exitCode = 1;});
