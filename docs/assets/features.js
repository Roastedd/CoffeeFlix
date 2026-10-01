/* GitHub stores votes. This page reads public totals; it never sends a vote or asks for a token. */
(() => {
  "use strict";
  const API = "https://api.github.com/repos/Roastedd/CoffeeFlix/issues/15/comments";
  const THREAD = "https://github.com/Roastedd/CoffeeFlix/issues/15";
  const CACHE_KEY = "coffeeflix-feature-votes-v1";
  const CACHE_AGE = 5 * 60 * 1000;
  const statuses = {
    proposed: "Proposed", "needs-research": "Needs research", planned: "Planned",
    "in-progress": "In progress", shipped: "Shipped", "not-planned": "Not planned"
  };
  const grid = document.getElementById("feature-grid");
  const cards = [...grid.querySelectorAll(".feature-card")];
  const search = document.getElementById("search");
  const category = document.getElementById("category");
  const status = document.getElementById("status");
  const sort = document.getElementById("sort");
  const results = document.getElementById("results");
  const voteStatus = document.getElementById("vote-status");
  const refresh = document.getElementById("refresh");
  let fetchedAt = null;
  let lastAttempt = 0;
  let busy = false;

  const entries = cards.map((card, index) => ({
    card, index, id: card.dataset.commentId,
    title: card.querySelector("h3").textContent,
    searchText: card.textContent.toLowerCase(),
    category: card.dataset.category, votes: null
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
        (category.value === "all" || entry.category === category.value) &&
        (status.value === "all" || entry.card.dataset.status === status.value);
      entry.card.hidden = !matches;
      if (matches) visible++;
      grid.append(entry.card);
    }
    results.textContent = `Showing ${visible} of ${entries.length} ideas`;
    document.getElementById("empty").hidden = visible !== 0;
  }

  function applyTotals(totals) {
    for (const entry of entries) {
      const item = totals[entry.id];
      entry.votes = Number.isSafeInteger(item?.votes) && item.votes >= 0 ? item.votes : null;
      const count = entry.card.querySelector("[data-votes]");
      count.textContent = entry.votes === null ? "—" : entry.votes.toLocaleString();
      count.setAttribute("aria-label", entry.votes === null ? "Vote count unavailable" : `${entry.votes} ${entry.votes === 1 ? "vote" : "votes"}`);
      entry.card.querySelector("[data-vote-label]").textContent = entry.votes === 1 ? "vote" : "votes";
      if (Object.hasOwn(statuses, item?.status)) {
        entry.card.dataset.status = item.status;
        const badge = entry.card.querySelector(".badge");
        badge.textContent = statuses[item.status];
        badge.dataset.state = item.status;
      }
      const link = entry.card.querySelector(".vote-link");
      if (item?.missing === true) {
        link.href = THREAD;
        link.textContent = "View thread ↗";
        link.setAttribute("aria-label", `View voting thread for ${entry.title}`);
      } else {
        link.href = `${THREAD}#issuecomment-${entry.id}`;
        link.textContent = "Vote on GitHub ↗";
        link.setAttribute("aria-label", `Vote on GitHub for ${entry.title}`);
      }
    }
    updateView();
  }

  function savedTime() {
    return new Date(fetchedAt).toLocaleString([], {month: "short", day: "numeric", hour: "numeric", minute: "2-digit"});
  }

  async function loadVotes() {
    if (busy) return;
    if (Date.now() - lastAttempt < 10000) {
      voteStatus.textContent = "Please wait a few seconds before refreshing again.";
      return;
    }
    lastAttempt = Date.now();
    busy = true;
    refresh.disabled = true;
    voteStatus.textContent = "Loading vote totals from GitHub…";
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 15000);
    try {
      const comments = [];
      // Fetch all pages before replacing counts, so a partial response cannot become false zeroes.
      for (let page = 1; ; page++) {
        if (page > 10) throw new Error("pagination");
        const response = await fetch(`${API}?per_page=100&page=${page}`, {
          headers: {Accept: "application/vnd.github+json"}, signal: controller.signal
        });
        if (!response.ok) throw new Error(response.status === 403 || response.status === 429 ? "rate-limit" : "unavailable");
        const batch = await response.json();
        if (!Array.isArray(batch)) throw new Error("unavailable");
        comments.push(...batch);
        if (batch.length < 100) break;
      }
      const byId = new Map(comments.filter(comment => comment && Number.isSafeInteger(comment.id)).map(comment => [String(comment.id), comment]));
      const totals = Object.create(null);
      let missing = 0;
      for (const entry of entries) {
        const comment = byId.get(entry.id);
        const votes = comment?.reactions?.["+1"];
        const marker = typeof comment?.body === "string" && comment.body.match(/<!--\s*coffeeflix-status:\s*([a-z-]+)\s*-->/);
        const owned = comment?.user?.login?.toLowerCase() === "roastedd";
        totals[entry.id] = {
          votes: Number.isSafeInteger(votes) && votes >= 0 ? votes : null,
          status: owned && marker && Object.hasOwn(statuses, marker[1]) ? marker[1] : entry.card.dataset.status,
          missing: !comment
        };
        if (!comment || totals[entry.id].votes === null) missing++;
      }
      fetchedAt = Date.now();
      applyTotals(totals);
      voteStatus.textContent = missing ? "Some vote counts are unavailable. Open the GitHub thread for details." : `Updated at ${savedTime()}. Refresh after voting.`;
      try { localStorage.setItem(CACHE_KEY, JSON.stringify({fetchedAt, totals})); } catch (_) { /* Storage can be disabled. */ }
    } catch (error) {
      const reason = error.message === "rate-limit" ? "GitHub’s API limit was reached." : "Couldn’t load vote totals.";
      voteStatus.textContent = `${reason} ${fetchedAt ? `Showing saved totals from ${savedTime()}.` : "Counts are unavailable."} You can still vote on GitHub.`;
    } finally {
      clearTimeout(timeout);
      busy = false;
      refresh.disabled = false;
    }
  }

  document.getElementById("toolbar").hidden = false;
  refresh.hidden = false;
  search.addEventListener("input", updateView);
  [category, status, sort].forEach(control => control.addEventListener("change", updateView));
  refresh.addEventListener("click", loadVotes);
  document.getElementById("reset").addEventListener("click", () => {
    search.value = "";
    category.value = status.value = "all";
    updateView();
    search.focus();
  });
  updateView();
  try {
    const saved = JSON.parse(localStorage.getItem(CACHE_KEY));
    if (saved && Number.isFinite(saved.fetchedAt) && saved.fetchedAt <= Date.now() && saved.totals && typeof saved.totals === "object") {
      fetchedAt = saved.fetchedAt;
      applyTotals(saved.totals);
      voteStatus.textContent = `Saved totals from ${savedTime()}. Refresh after voting.`;
    }
  } catch (_) { /* Missing, invalid or disabled storage: fetch normally. */ }
  if (!fetchedAt || Date.now() - fetchedAt >= CACHE_AGE) loadVotes();
})();
