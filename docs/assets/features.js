/* GitHub stores the votes (one 👍 per account on each idea's comment). This page only reads public totals;
   it never sends a vote or asks for a token. */
(() => {
  "use strict";
  // The snapshot is a static file a GitHub Action rewrites every 15 minutes (no API limit for visitors).
  // The live API is the fallback, and what "Refresh votes" asks first so a fresh vote shows up.
  const SNAPSHOT = "https://raw.githubusercontent.com/Roastedd/CoffeeFlix/votes/votes.json";
  const CACHE_KEY = "coffeeflix-feature-votes-v4";
  // Only comments by these accounts carry votes, so nobody can plant a look-alike comment with reactions.
  const TRUSTED = new Set(["roastedd", "github-actions[bot]"]);
  const MARKER = /<!--\s*coffeeflix-feature:\s*([a-z0-9-]+)\s*-->/;
  const CACHE_AGE = 5 * 60 * 1000;
  const SNAPSHOT_FRESH = 2 * 60 * 60 * 1000;
  const $ = id => document.getElementById(id);
  const grid = $("feature-grid");
  const ISSUE = /^\d+$/.test(grid.dataset.thread) ? grid.dataset.thread : "0";
  const THREAD = `https://github.com/Roastedd/CoffeeFlix/issues/${ISSUE}`;
  const API = `https://api.github.com/repos/Roastedd/CoffeeFlix/issues/${ISSUE}/comments`;
  const search = $("search");
  const statusFilter = $("status");
  const sort = $("sort");
  const results = $("results");
  const voteStatus = $("vote-status");
  const refresh = $("refresh");
  const pills = [...document.querySelectorAll(".pill")];
  let category = "all";
  let fetchedAt = null;
  let lastAttempt = 0;
  let busy = false;
  let awaitingVote = false;

  const entries = [...grid.querySelectorAll(".card")].map((card, index) => ({
    card, index, id: card.id,
    title: card.querySelector("h3").textContent,
    summary: card.querySelector(".summary").textContent,
    category: card.dataset.category,
    status: card.dataset.status,
    searchText: [...card.querySelectorAll(".category, h3, .summary, .includes li, .reply")].map(node => node.textContent).join(" ").toLowerCase(),
    link: card.querySelector(".vote-btn"),
    count: card.querySelector("[data-votes]"),
    label: card.querySelector("[data-vote-label]"),
    meter: card.querySelector(".meter i"),
    votes: null
  }));

  function updateView() {
    const query = search.value.trim().toLowerCase();
    const ordered = [...entries].sort((a, b) => {
      if (sort.value === "title") return a.title.localeCompare(b.title);
      if (sort.value === "category") return a.category.localeCompare(b.category) || a.title.localeCompare(b.title);
      // Unknown counts stay after loaded counts. Ties preserve the catalogue's order.
      return (b.votes ?? -1) - (a.votes ?? -1) || a.index - b.index;
    });
    let visible = 0;
    for (const entry of ordered) {
      const matches = (!query || entry.searchText.includes(query)) &&
        (category === "all" || entry.category === category) &&
        (statusFilter.value === "all" || entry.status === statusFilter.value);
      entry.card.hidden = !matches;
      if (matches) visible++;
      grid.append(entry.card);
    }
    for (const pill of pills) pill.setAttribute("aria-pressed", String(pill.dataset.category === category));
    results.textContent = `Showing ${visible} of ${entries.length} ideas`;
    $("empty").hidden = visible !== 0;
  }

  function resetFilters() {
    search.value = "";
    category = "all";
    statusFilter.value = "all";
    updateView();
  }

  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function icon(id) {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    const use = document.createElementNS("http://www.w3.org/2000/svg", "use");
    use.setAttribute("href", `#${id}`);
    svg.setAttribute("aria-hidden", "true");
    svg.append(use);
    return svg;
  }

  // The three most-voted ideas. Nothing is ranked until someone has voted.
  function renderTop() {
    if (!entries.some(entry => entry.votes !== null)) return;
    const ranked = entries.filter(entry => entry.votes > 0)
      .sort((a, b) => b.votes - a.votes || a.index - b.index).slice(0, 3);
    const podium = $("podium");
    podium.replaceChildren(...ranked.map((entry, place) => {
      const card = element("article", `podium-card rank-${place + 1}`);
      const rank = element("div", "rank");
      const medal = element("span", "medal");
      if (place === 0) medal.append(icon("i-crown")); else medal.textContent = String(place + 1);
      rank.append(medal, place === 0 ? "Most wanted" : `Number ${place + 1}`);
      const title = element("h3");
      const titleLink = element("a", "", entry.title);
      titleLink.href = `#${entry.id}`;
      title.append(titleLink);
      const tally = element("p", "tally");
      tally.append(element("strong", "", entry.votes.toLocaleString()), ` ${entry.votes === 1 ? "vote" : "votes"}`);
      const vote = element("a", "vote-btn");
      vote.href = entry.link.href;
      vote.target = "_blank";
      vote.rel = "noopener";
      vote.setAttribute("aria-label", `Vote for ${entry.title} on GitHub`);
      vote.append(icon("i-thumb"), "Vote on GitHub ↗");
      const foot = element("div", "podium-foot");
      foot.append(tally, vote);
      const meter = element("div", "meter");
      meter.setAttribute("aria-hidden", "true");
      const bar = element("i");
      bar.style.setProperty("--w", `${Math.round(entry.votes / ranked[0].votes * 100)}%`);
      meter.append(bar);
      card.append(rank, element("span", "category", entry.category), title, element("p", "summary", entry.summary));
      const includes = place === 0 && entry.card.querySelector(".includes");
      if (includes) card.append(includes.cloneNode(true));
      card.append(foot, meter);
      return card;
    }));
    podium.dataset.count = String(ranked.length);
    podium.hidden = ranked.length === 0;
    $("top-empty").hidden = ranked.length !== 0;
    $("top").hidden = false;
  }

  function applyTotals(totals) {
    let best = 0, sum = 0, complete = true;
    for (const entry of entries) {
      const item = totals[entry.id];
      entry.votes = Number.isSafeInteger(item?.votes) && item.votes >= 0 ? item.votes : null;
      if (entry.votes === null) complete = false; else { best = Math.max(best, entry.votes); sum += entry.votes; }
    }
    for (const entry of entries) {
      const item = totals[entry.id];
      entry.count.textContent = entry.votes === null ? "—" : entry.votes.toLocaleString();
      entry.count.setAttribute("aria-label", entry.votes === null ? "Vote count unavailable" : `${entry.votes} ${entry.votes === 1 ? "vote" : "votes"}`);
      entry.label.textContent = entry.votes === 1 ? "vote" : "votes";
      entry.meter.style.setProperty("--w", entry.votes && best ? `${Math.round(entry.votes / best * 100)}%` : "0%");
      // The exact comment is only known from the totals; the static card links to the thread.
      if (Number.isSafeInteger(item?.commentId)) {
        entry.link.href = `${THREAD}#issuecomment-${item.commentId}`;
      } else if (item?.missing === true) {
        entry.link.href = THREAD;
      }
    }
    for (const total of document.querySelectorAll("[data-total-votes]")) {
      total.textContent = complete ? sum.toLocaleString() : "—";
    }
    renderTop();
    updateView();
  }

  function savedTime() {
    return new Date(fetchedAt).toLocaleString([], {month: "short", day: "numeric", hour: "numeric", minute: "2-digit"});
  }

  // Each source resolves to {fetchedAt, totals} or throws "rate-limit" / "unavailable".
  async function fetchSnapshot(signal) {
    const response = await fetch(SNAPSHOT, {signal});
    if (!response.ok) throw new Error("unavailable");
    const data = await response.json();
    const generated = Date.parse(data?.generatedAt);
    if (!Number.isFinite(generated) || generated > Date.now() + 5 * 60 * 1000 || typeof data.features !== "object" || !data.features) throw new Error("unavailable");
    const totals = Object.create(null);
    for (const entry of entries) {
      const item = Object.hasOwn(data.features, entry.id) ? data.features[entry.id] : undefined;
      totals[entry.id] = {votes: item?.votes, commentId: item?.commentId, missing: item?.missing === true};
    }
    return {fetchedAt: generated, totals};
  }

  async function fetchLive(signal) {
    const comments = [];
    // Fetch all pages before replacing counts, so a partial response cannot become false zeroes.
    for (let page = 1; ; page++) {
      if (page > 10) throw new Error("pagination");
      const response = await fetch(`${API}?per_page=100&page=${page}`, {
        headers: {Accept: "application/vnd.github+json"}, signal
      });
      if (!response.ok) throw new Error(response.status === 403 || response.status === 429 ? "rate-limit" : "unavailable");
      const batch = await response.json();
      if (!Array.isArray(batch)) throw new Error("unavailable");
      comments.push(...batch);
      if (batch.length < 100) break;
    }
    // Comments arrive oldest first, so the earliest trusted comment for an idea wins.
    const byIdea = new Map();
    for (const comment of comments) {
      const idea = typeof comment?.body === "string" && TRUSTED.has(comment.user?.login?.toLowerCase()) && comment.body.match(MARKER)?.[1];
      if (idea && !byIdea.has(idea)) byIdea.set(idea, comment);
    }
    const totals = Object.create(null);
    for (const entry of entries) {
      const comment = byIdea.get(entry.id);
      totals[entry.id] = {votes: comment?.reactions?.["+1"], commentId: comment?.id, missing: !comment};
    }
    return {fetchedAt: Date.now(), totals};
  }

  async function attempt(fetcher) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 15000);
    try { return await fetcher(controller.signal); } finally { clearTimeout(timeout); }
  }

  // live=false (page load) prefers the snapshot; live=true (Refresh votes) asks GitHub first.
  // quiet=true is for refreshes nobody asked for: a throttled one is skipped without comment.
  async function loadVotes(live, quiet = false) {
    if (busy) return;
    if (Date.now() - lastAttempt < 10000) {
      if (!quiet) voteStatus.textContent = "Please wait a few seconds before refreshing again.";
      return;
    }
    lastAttempt = Date.now();
    busy = true;
    refresh.disabled = true;
    voteStatus.textContent = "Loading vote totals from GitHub…";
    try {
      let result = null, outdated = null, limited = false;
      for (const source of live ? ["live", "snapshot"] : ["snapshot", "live"]) {
        try {
          const data = await attempt(source === "live" ? fetchLive : fetchSnapshot);
          data.source = source;
          // An old snapshot is a last resort: ask GitHub directly before showing it.
          if (source === "snapshot" && !live && Date.now() - data.fetchedAt > SNAPSHOT_FRESH) outdated = data;
          else { result = data; break; }
        } catch (error) {
          limited = limited || error.message === "rate-limit";
        }
      }
      result ||= outdated;
      if (!result) {
        const reason = limited ? "GitHub’s API limit was reached." : "Couldn’t load vote totals.";
        voteStatus.textContent = `${reason} ${fetchedAt ? `Showing saved totals from ${savedTime()}.` : "Counts are unavailable."} You can still vote on GitHub.`;
        return;
      }
      // Never swap newer totals (say, from Refresh votes) for an older snapshot.
      if (!fetchedAt || result.fetchedAt >= fetchedAt) {
        fetchedAt = result.fetchedAt;
        applyTotals(result.totals);
        try { localStorage.setItem(CACHE_KEY, JSON.stringify({fetchedAt, totals: result.totals})); } catch (_) { /* Storage can be disabled. */ }
      }
      if (entries.some(entry => entry.votes === null)) {
        voteStatus.textContent = "Some vote counts are unavailable. Open the GitHub thread for details.";
      } else if (result.source === "snapshot" && (live || result === outdated)) {
        voteStatus.textContent = `${limited ? "GitHub’s API limit was reached." : "Couldn’t reach GitHub."} Showing totals from ${savedTime()}.`;
      } else if (result.source === "snapshot") {
        voteStatus.textContent = `Totals as of ${savedTime()}, refreshed every 15 minutes. Use Refresh votes right after voting.`;
      } else {
        voteStatus.textContent = `Updated at ${savedTime()}. Refresh after voting.`;
      }
    } finally {
      busy = false;
      refresh.disabled = false;
    }
  }

  // Following a link to its card from the leaderboard: show the card even if a filter hides it.
  function revealHash() {
    let id = "";
    try { id = decodeURIComponent(location.hash.slice(1)); } catch (_) { return; }
    const entry = entries.find(candidate => candidate.id === id);
    if (!entry) return;
    if (entry.card.hidden) resetFilters();
    entry.card.scrollIntoView({block: "center"});
  }

  $("toolbar").hidden = false;
  refresh.hidden = false;
  search.addEventListener("input", updateView);
  [statusFilter, sort].forEach(control => control.addEventListener("change", updateView));
  for (const pill of pills) pill.addEventListener("click", () => { category = pill.dataset.category; updateView(); });
  refresh.addEventListener("click", () => loadVotes(true));
  $("reset").addEventListener("click", () => { resetFilters(); search.focus(); });
  // Voting happens in another tab; when the visitor comes back, fetch the totals again.
  document.addEventListener("click", event => {
    if (event.target.closest?.(".vote-btn")) awaitingVote = true;
    // Choosing the same card twice doesn't change the address, so no hashchange follows.
    const link = event.target.closest?.('a[href^="#"]');
    if (link && link.hash === location.hash) { event.preventDefault(); revealHash(); }
  });
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState !== "visible" || !awaitingVote) return;
    awaitingVote = false;
    setTimeout(() => loadVotes(true, true), 1500);
  });
  window.addEventListener("hashchange", revealHash);
  updateView();
  try {
    const saved = JSON.parse(localStorage.getItem(CACHE_KEY));
    if (saved && Number.isFinite(saved.fetchedAt) && saved.fetchedAt <= Date.now() && saved.totals && typeof saved.totals === "object") {
      fetchedAt = saved.fetchedAt;
      applyTotals(saved.totals);
      voteStatus.textContent = `Saved totals from ${savedTime()}. Refresh after voting.`;
    }
  } catch (_) { /* Missing, invalid or disabled storage: fetch normally. */ }
  if (!fetchedAt || Date.now() - fetchedAt >= CACHE_AGE) loadVotes(false);
  if (location.hash) revealHash();
})();
