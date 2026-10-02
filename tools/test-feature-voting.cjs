/* Run with Node and Playwright installed. Only reads local files; GitHub responses are mocked. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '../docs');
const features = JSON.parse(fs.readFileSync(path.join(root, 'assets/feature-requests.json')));
const issue = JSON.parse(fs.readFileSync(path.join(root, 'assets/feature-thread.json'))).issue;
const threadUrl = `https://github.com/Roastedd/CoffeeFlix/issues/${issue}`;
const api = `**/api.github.com/repos/Roastedd/CoffeeFlix/issues/${issue}/comments?*`;
const snapshotUrl = '**/raw.githubusercontent.com/Roastedd/CoffeeFlix/votes/votes.json';
const N = features.length;
const at = id => features.findIndex(f => f.id === id);
const cid = id => 9000 + at(id);  // stand-in comment ids; the real ones come from GitHub
const fixture = () => features.map((f, i) => ({id: 9000 + i, body: `<!-- coffeeflix-feature: ${f.id} -->\n<!-- coffeeflix-status: ${f.status} -->`, user: {login: 'github-actions[bot]'}, reactions: {'+1': 0, heart: 100}}));
const withVotes = (id, votes) => fixture().map(c => c.id === cid(id) ? {...c, reactions: {'+1': votes}} : c);
const snapshot = (votes = {}, ageMs = 60000) => ({generatedAt: new Date(Date.now() - ageMs).toISOString(), features: Object.fromEntries(features.map(f => [f.id, {commentId: cid(f.id), votes: votes[f.id] ?? 0}]))});
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
    await page.route(snapshotUrl, route => route.fulfill({status: 404, body: 'Not Found'}));  // tests opt in to a snapshot
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
    comments[at('personalize-home')].reactions['+1'] = 3;
    comments[at('controller-keyboard')].reactions['+1'] = 8;
    comments[at('personalize-home')].body += '\n<!-- coffeeflix-status: shipped -->';  // statuses come from the catalogue, never from comments
    comments[at('display-options')].user.login = 'Roastedd';  // the owner's comments count too
    comments[at('display-options')].reactions['+1'] = 2;
    const marker = id => `<!-- coffeeflix-feature: ${id} -->`;
    comments.unshift({id: 1, body: marker('controller-keyboard'), user: {login: 'mallory'}, reactions: {'+1': 500}});  // look-alike from a stranger
    comments.push({id: 2, body: marker('controller-keyboard'), user: {login: 'github-actions[bot]'}, reactions: {'+1': 40}});  // a later duplicate
    const page = await newPage({viewport: {width: 1440, height: 1120}});
    await page.route(api, route => route.fulfill({json: comments}));
    await page.goto(base + '/features.html'); await finished(page);
    assert.equal(await page.locator('.card').count(), N);
    assert.equal(await page.locator('.card').first().getAttribute('id'), 'controller-keyboard');
    assert.equal(await page.locator('#controller-keyboard [data-votes]').innerText(), '8');
    assert.equal(await page.locator('#personalize-home [data-votes]').innerText(), '3');
    assert.equal(await page.locator('#display-options [data-votes]').innerText(), '2');
    assert.equal(await page.locator('#personalize-home .badge').innerText(), 'Planned');
    assert.equal(await page.locator('#display-options .badge').innerText(), 'Proposed');
    assert.match(await page.locator('#controller-keyboard .vote-btn').getAttribute('href'), new RegExp(`#issuecomment-${cid('controller-keyboard')}$`));
    pass('live thumbs-up counts, exact vote links and sorting; strangers, duplicates and status markers cannot change the board');

    await page.locator('.pill', {hasText: 'Twitch'}).click();
    assert.equal(await page.locator('.card:visible').count(), features.filter(f => f.category === 'Twitch').length);
    await page.locator('.pill[data-category="all"]').click();
    await page.locator('#search').fill('autoplay');
    assert.equal(await page.locator('.card:visible').count(), 1);
    await page.locator('#search').fill('zz-no-such-feature');
    assert.equal(await page.locator('#empty').isVisible(), true);
    await page.locator('#reset').click();
    assert.equal(await page.locator('.card:visible').count(), N);
    await page.locator('#status').selectOption('planned');
    assert.equal(await page.locator('.card:visible').count(), 1);
    await page.locator('#status').selectOption('all');
    await page.locator('#sort').selectOption('title');
    assert.equal(await page.locator('.card').first().getAttribute('id'), [...features].sort((a, b) => a.title.localeCompare(b.title))[0].id);
    await noOverflow(page);
    pass('search, category pills, status filter, empty state, reset and alphabetical sort');
    await close(page);

    const offline = await newPage();
    await offline.route(api, route => route.abort());
    await offline.goto(base + '/features.html'); await finished(offline);
    assert.equal(await offline.locator('#personalize-home [data-votes]').innerText(), '—');
    assert.match(await offline.locator('#vote-status').innerText(), /Couldn’t load.*Counts are unavailable/);
    assert.equal(await offline.locator(`.vote-btn[href="${threadUrl}"]`).count(), N);
    pass('offline failure leaves counts unavailable and every card linked to the voting thread');
    await close(offline);

    const cached = await newPage();
    await cached.addInitScript(({id}) => localStorage.setItem('coffeeflix-feature-votes-v4', JSON.stringify({fetchedAt: Date.now() - 360000, totals: {[id]: {votes: 7}}})), {id: features[0].id});
    await cached.route(api, route => route.fulfill({status: 403, json: {message: 'API rate limit exceeded'}}));
    await cached.goto(base + '/features.html'); await finished(cached);
    assert.equal(await cached.locator('#personalize-home [data-votes]').innerText(), '7');
    assert.match(await cached.locator('#vote-status').innerText(), /API limit.*Showing saved totals/);
    pass('rate limiting retains saved totals and labels them as saved');
    await close(cached);

    const pages = await newPage();
    const filler = Array.from({length: 100}, (_, i) => ({id: i + 1, body: 'Discussion, not a registered feature'}));
    await pages.route(api, route => route.fulfill({json: new URL(route.request().url()).searchParams.get('page') === '1' ? filler : fixture()}));
    await pages.goto(base + '/features.html'); await finished(pages);
    assert.equal(await pages.locator('#personalize-home [data-votes]').innerText(), '0');
    assert.match(await pages.locator('#vote-status').innerText(), /Updated at/);
    pass('pagination finds registered voting comments beyond the first 100');
    await close(pages);

    const partial = await newPage();
    await partial.route(api, route => new URL(route.request().url()).searchParams.get('page') === '1' ? route.fulfill({json: [...fixture(), ...filler.slice(0, 100 - N)]}) : route.fulfill({status: 429, json: {message: 'Rate limited'}}));
    await partial.goto(base + '/features.html'); await finished(partial);
    assert.equal(await partial.locator('#personalize-home [data-votes]').innerText(), '—');
    pass('a failed later API page never publishes incomplete counts');
    await close(partial);

    const missing = await newPage();
    await missing.route(api, route => route.fulfill({json: fixture().slice(1)}));
    await missing.goto(base + '/features.html'); await finished(missing);
    assert.equal(await missing.locator('#personalize-home [data-votes]').innerText(), '—');
    assert.equal(await missing.locator('#personalize-home .vote-btn').getAttribute('href'), threadUrl);
    pass('deleted voting comments remain visibly unavailable and link to the thread');
    await close(missing);

    const snapshotFirst = await newPage();
    let snapshotApiCalls = 0;
    await snapshotFirst.route(snapshotUrl, route => route.fulfill({json: snapshot({'controller-keyboard': 8, 'personalize-home': 3})}));
    await snapshotFirst.route(api, route => {snapshotApiCalls++; route.fulfill({json: fixture()});});
    await snapshotFirst.goto(base + '/features.html'); await finished(snapshotFirst);
    assert.equal(await snapshotFirst.locator('#controller-keyboard [data-votes]').innerText(), '8');
    assert.equal(await snapshotFirst.locator('#personalize-home [data-votes]').innerText(), '3');
    assert.equal(await snapshotFirst.locator('.card').first().getAttribute('id'), 'controller-keyboard');
    assert.match(await snapshotFirst.locator('#vote-status').innerText(), /Totals as of/);
    assert.match(await snapshotFirst.locator('#controller-keyboard .vote-btn').getAttribute('href'), new RegExp(`#issuecomment-${cid('controller-keyboard')}$`));
    assert.equal(snapshotApiCalls, 0);
    pass('a fresh snapshot supplies counts and exact comment links without calling the GitHub API');
    await close(snapshotFirst);

    const refreshed = await newPage();
    await refreshed.clock.install({time: new Date()});
    await refreshed.route(snapshotUrl, route => route.fulfill({json: snapshot({'personalize-home': 1})}));
    await refreshed.route(api, route => route.fulfill({json: withVotes('personalize-home', 5)}));
    await refreshed.goto(base + '/features.html'); await finished(refreshed);
    assert.equal(await refreshed.locator('#personalize-home [data-votes]').innerText(), '1');
    await refreshed.clock.fastForward(11000);
    await refreshed.locator('#refresh').click(); await finished(refreshed);
    assert.equal(await refreshed.locator('#personalize-home [data-votes]').innerText(), '5');
    assert.match(await refreshed.locator('#vote-status').innerText(), /Updated at/);
    pass('Refresh votes asks GitHub directly so a new vote appears straight away');

    await refreshed.unroute(api);
    await refreshed.route(api, route => route.fulfill({status: 403, json: {message: 'API rate limit exceeded'}}));
    await refreshed.clock.fastForward(11000);
    await refreshed.locator('#refresh').click(); await finished(refreshed);
    assert.equal(await refreshed.locator('#personalize-home [data-votes]').innerText(), '5');
    assert.match(await refreshed.locator('#vote-status').innerText(), /API limit.*Showing (saved )?totals/);
    pass('a rate-limited Refresh keeps the newer totals instead of reverting to the snapshot');
    await close(refreshed);

    const stale = await newPage();
    await stale.route(snapshotUrl, route => route.fulfill({json: snapshot({'personalize-home': 4}, 3 * 3600 * 1000)}));
    await stale.route(api, route => route.fulfill({json: withVotes('personalize-home', 6)}));
    await stale.goto(base + '/features.html'); await finished(stale);
    assert.equal(await stale.locator('#personalize-home [data-votes]').innerText(), '6');
    pass('an outdated snapshot is replaced by live GitHub totals');
    await close(stale);

    const staleLimited = await newPage();
    await staleLimited.route(snapshotUrl, route => route.fulfill({json: snapshot({'personalize-home': 4}, 3 * 3600 * 1000)}));
    await staleLimited.route(api, route => route.fulfill({status: 403, json: {message: 'API rate limit exceeded'}}));
    await staleLimited.goto(base + '/features.html'); await finished(staleLimited);
    assert.equal(await staleLimited.locator('#personalize-home [data-votes]').innerText(), '4');
    assert.match(await staleLimited.locator('#vote-status').innerText(), /API limit.*Showing totals from/);
    pass('when GitHub is rate limited, an outdated snapshot is shown and labelled');
    await close(staleLimited);

    const brokenSnapshot = await newPage();
    await brokenSnapshot.route(snapshotUrl, route => route.fulfill({json: {generatedAt: 'not a date', features: {}}}));
    await brokenSnapshot.route(api, route => route.fulfill({json: withVotes('personalize-home', 2)}));
    await brokenSnapshot.goto(base + '/features.html'); await finished(brokenSnapshot);
    assert.equal(await brokenSnapshot.locator('#personalize-home [data-votes]').innerText(), '2');
    pass('a malformed snapshot is ignored in favour of the live API');
    await close(brokenSnapshot);

    const newerCache = await newPage();
    let newerCacheApiCalls = 0;
    await newerCache.addInitScript(({id}) => localStorage.setItem('coffeeflix-feature-votes-v4', JSON.stringify({fetchedAt: Date.now() - 6 * 60000, totals: {[id]: {votes: 9}}})), {id: 'personalize-home'});
    await newerCache.route(snapshotUrl, route => route.fulfill({json: snapshot({'personalize-home': 2}, 10 * 60000)}));
    await newerCache.route(api, route => {newerCacheApiCalls++; route.fulfill({json: fixture()});});
    await newerCache.goto(base + '/features.html'); await finished(newerCache);
    assert.equal(await newerCache.locator('#personalize-home [data-votes]').innerText(), '9');
    assert.equal(newerCacheApiCalls, 0);
    pass('an older snapshot never replaces newer totals the visitor already has');
    await close(newerCache);

    const staticPage = await newPage({javaScriptEnabled: false});
    await staticPage.goto(base + '/features.html');
    assert.equal(await staticPage.locator('.card:visible').count(), N);
    assert.equal(await staticPage.locator(`.vote-btn[href="${threadUrl}"]`).count(), N);
    assert.equal(await staticPage.locator('#toolbar').isVisible(), false);
    pass('all cards and vote links work without JavaScript');
    await close(staticPage);

    const board = await newPage({viewport: {width: 1440, height: 1120}});
    const votes = {'controller-keyboard': 12, 'twitch-watching': 7, 'personalize-home': 7, 'jellyfin-autoplay': 1};
    await board.route(snapshotUrl, route => route.fulfill({json: snapshot(votes)}));
    await board.goto(base + '/features.html'); await finished(board);
    assert.equal(await board.locator('#top').isVisible(), true);
    assert.deepEqual(await board.locator('.podium-card h3').allInnerTexts(), features.filter(f => ['controller-keyboard', 'personalize-home', 'twitch-watching'].includes(f.id)).sort((a, b) => votes[b.id] - votes[a.id] || features.indexOf(a) - features.indexOf(b)).map(f => f.title));
    assert.equal(await board.locator('.podium-card').count(), 3);
    assert.equal(await board.locator('.podium-card.rank-1 .tally strong').innerText(), '12');
    assert.equal(await board.locator('.podium-card.rank-1 .meter i').evaluate(node => node.style.getPropertyValue('--w')), '100%');
    assert.equal(await board.locator('#controller-keyboard .meter i').evaluate(node => node.style.getPropertyValue('--w')), '100%');
    assert.equal(await board.locator('#jellyfin-autoplay .meter i').evaluate(node => node.style.getPropertyValue('--w')), '8%');
    assert.equal(await board.locator('[data-total-votes]').first().innerText(), '27');
    assert.match(await board.locator('.podium-card.rank-1 .vote-btn').getAttribute('href'), new RegExp(`#issuecomment-${cid('controller-keyboard')}$`));
    pass('most wanted shows the top three by votes (ties in catalogue order) with vote links, bars and the hero total');

    await board.locator('.podium-card.rank-1 h3 a').click();
    assert.equal(await board.evaluate(() => location.hash), '#controller-keyboard');
    await board.locator('.pill', {hasText: 'Twitch'}).click();
    assert.equal(await board.locator('#controller-keyboard').isVisible(), false);
    await board.locator('.podium-card.rank-1 h3 a').click();
    assert.equal(await board.locator('#controller-keyboard').isVisible(), true);
    assert.equal(await board.locator('.pill[aria-pressed="true"]').innerText(), `All ${N}`);
    pass('a leaderboard link reveals its card even when a filter had hidden it');

    const voteLink = board.locator('#personalize-home .vote-btn');
    assert.equal(await voteLink.getAttribute('target'), '_blank');
    assert.match(await voteLink.getAttribute('rel'), /noopener/);
    await board.locator('#personalize-home .includes li').first().waitFor();
    assert.equal(await board.locator('#personalize-home .includes li').count(), features.find(f => f.id === 'personalize-home').includes.length);
    assert.equal(await board.locator('#more-servers .source a').count(), 2);
    await board.locator('#search').fill('kavita');
    assert.deepEqual(await board.locator('.card:visible').evaluateAll(nodes => nodes.map(node => node.id)), ['more-servers']);
    await board.screenshot({path: '/private/tmp/coffeeflix-voting-desktop.png', fullPage: true});
    await close(board);
    pass('cards list what each bundle includes, vote links open GitHub in a new tab, and search reaches the included items');

    const refocus = await newPage();
    await refocus.clock.install({time: new Date()});
    await refocus.route(snapshotUrl, route => route.fulfill({json: snapshot({'personalize-home': 1})}));
    await refocus.route(api, route => route.fulfill({json: withVotes('personalize-home', 2)}));
    await refocus.goto(base + '/features.html'); await finished(refocus);
    assert.equal(await refocus.locator('#personalize-home [data-votes]').innerText(), '1');
    await refocus.clock.fastForward(11000);
    await refocus.locator('#personalize-home .vote-btn').evaluate(link => link.addEventListener('click', event => event.preventDefault()));
    await refocus.locator('#personalize-home .vote-btn').click();
    await refocus.evaluate(() => { Object.defineProperty(document, 'visibilityState', {value: 'visible', configurable: true}); document.dispatchEvent(new Event('visibilitychange')); });
    await refocus.clock.fastForward(2000); await finished(refocus);
    await refocus.waitForFunction(() => document.querySelector('#personalize-home [data-votes]').textContent === '2');
    pass('coming back from voting on GitHub refreshes the totals by itself');
    await close(refocus);

    const quiet = await newPage();
    await quiet.route(snapshotUrl, route => route.fulfill({json: snapshot({})}));
    await quiet.goto(base + '/features.html'); await finished(quiet);
    assert.equal(await quiet.locator('#top').isVisible(), true);
    assert.equal(await quiet.locator('.podium-card').count(), 0);
    assert.equal(await quiet.locator('#top-empty').isVisible(), true);
    assert.equal(await quiet.locator('[data-total-votes]').first().innerText(), '0');
    pass('with no votes yet the leaderboard invites the first one');
    await close(quiet);

    const unknown = await newPage();
    await unknown.route(api, route => route.abort());
    await unknown.goto(base + '/features.html'); await finished(unknown);
    assert.equal(await unknown.locator('#top').isVisible(), false);
    assert.equal(await unknown.locator('[data-total-votes]').first().innerText(), '—');
    pass('unavailable totals never produce a leaderboard or a made-up total');
    await close(unknown);

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
    await desktop.screenshot({path: '/private/tmp/coffeeflix-voting-desktop-novotes.png', fullPage: true});
    await close(desktop);
    console.log(`${checks} voting-page checks passed.`);
  } finally {await browser.close(); server.close();}
})().catch(error => {console.error(error); server.close(); process.exitCode = 1;});
