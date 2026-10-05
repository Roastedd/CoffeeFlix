/* Decoration only: things rising into view, cards that catch the light, numbers that count up,
   confetti from a click, and the small Ko-fi pill. The pages work the same without it. */
(() => {
  "use strict";
  const calm = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const fine = matchMedia("(hover: hover) and (pointer: fine)").matches;
  const KOFI = "https://ko-fi.com/ubecatstudio";

  // ---- rising into view ----
  const targets = [...document.querySelectorAll(
    ".grid .card, .gallery figure, .steps li, .support-card, .community-callout, .suggest, details, .section-heading, .podium-card, .roadmap-card, .marquee")];
  targets.forEach((node, i) => { node.classList.add("reveal"); node.style.setProperty("--d", String(i % 4)); });
  if ("IntersectionObserver" in window && !calm) {
    const seen = new IntersectionObserver(entries => {
      for (const entry of entries) if (entry.isIntersecting) { entry.target.classList.add("in"); seen.unobserve(entry.target); }
    }, { rootMargin: "0px 0px -8% 0px", threshold: 0.08 });
    targets.forEach(node => seen.observe(node));
    // Cards drawn later (the leaderboard) rise in too.
    new MutationObserver(list => {
      for (const mutation of list) for (const node of mutation.addedNodes) {
        if (node.nodeType === 1 && node.matches?.(".podium-card")) { node.classList.add("reveal"); seen.observe(node); }
      }
    }).observe(document.body, { childList: true, subtree: true });
  } else {
    targets.forEach(node => node.classList.add("in"));
  }

  // ---- numbers that bounce when they change (the text itself is never touched, so it is always the real total) ----
  for (const node of document.querySelectorAll("[data-total-votes]")) {
    let last = node.textContent;
    new MutationObserver(() => {
      if (node.textContent === last) return;
      last = node.textContent;
      if (calm || !/\d/.test(last)) return;
      node.classList.remove("bump"); void node.offsetWidth; node.classList.add("bump");
    }).observe(node, { childList: true, characterData: true, subtree: true });
  }

  // ---- cards that tilt toward the pointer and catch the light ----
  if (fine && !calm) {
    let current = null;
    const leave = () => { if (current) { current.style.transform = ""; current.classList.remove("is-tilting"); current = null; } };
    document.addEventListener("pointermove", event => {
      const card = event.target.closest?.(".feature-grid .card, .podium-card, .grid .card");
      if (card !== current) leave();
      if (!card) return;
      current = card;
      card.classList.add("tilt", "is-tilting");
      const box = card.getBoundingClientRect();
      const x = (event.clientX - box.left) / box.width, y = (event.clientY - box.top) / box.height;
      card.style.setProperty("--mx", `${x * 100}%`);
      card.style.setProperty("--my", `${y * 100}%`);
      card.style.transform = `perspective(900px) rotateX(${((0.5 - y) * 5).toFixed(2)}deg) rotateY(${((x - 0.5) * 6).toFixed(2)}deg) translateY(-3px)`;
    }, { passive: true });
    document.addEventListener("pointerleave", leave);
    window.addEventListener("blur", leave);
  }

  // ---- confetti ----
  function burst(from, pieces, count = 14) {
    if (calm) return;
    const box = from.getBoundingClientRect();
    const cx = box.left + box.width / 2, cy = box.top + box.height / 2;
    for (let i = 0; i < count; i++) {
      const bit = document.createElement("span");
      bit.className = "confetti";
      bit.textContent = pieces[i % pieces.length];
      bit.style.fontSize = `${14 + Math.random() * 14}px`;
      document.body.append(bit);
      const angle = Math.random() * Math.PI * 2, far = 60 + Math.random() * 110;
      const dx = Math.cos(angle) * far, dy = Math.sin(angle) * far - 40;
      bit.animate([
        { transform: `translate(${cx}px, ${cy}px) scale(0.4) rotate(0)`, opacity: 1 },
        { transform: `translate(${cx + dx}px, ${cy + dy}px) scale(1.1) rotate(${(Math.random() - 0.5) * 240}deg)`, opacity: 1, offset: 0.55 },
        { transform: `translate(${cx + dx * 1.2}px, ${cy + dy + 90}px) scale(0.8) rotate(${(Math.random() - 0.5) * 360}deg)`, opacity: 0 }
      ], { duration: 900 + Math.random() * 500, easing: "cubic-bezier(0.2, 0.7, 0.3, 1)" }).finished.then(() => bit.remove(), () => bit.remove());
    }
  }
  document.addEventListener("click", event => {
    const vote = event.target.closest?.(".vote-btn");
    if (vote) { vote.classList.remove("pop"); void vote.offsetWidth; vote.classList.add("pop"); burst(vote, ["👍", "✨", "🔥", "⭐", "👍"]); }
    const kofi = event.target.closest?.(".kofi-btn");
    if (kofi) burst(kofi, ["☕", "❤️", "✨", "🫘", "❤️"], 16);
  });

  // ---- the Ko-fi pill: appears once you've scrolled, steps aside at the support card ----
  let closed = false;
  try { closed = sessionStorage.getItem("coffeeflix-kofi-pill") === "1"; } catch (_) { /* storage off */ }
  if (!closed) {
    const pill = document.createElement("div");
    pill.className = "kofi-float";
    pill.innerHTML = `<a class="kofi-btn small" href="${KOFI}" target="_blank" rel="noopener"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 8h12v6a5 5 0 0 1-5 5H9a5 5 0 0 1-5-5V8zm12 1h2a3 3 0 0 1 0 6h-2v-2h2a1 1 0 0 0 0-2h-2V9zM8 2h2v3H8V2zm4 0h2v3h-2V2z"/></svg><span>Buy me a coffee</span></a><button class="close" type="button" aria-label="Hide this">×</button>`;
    document.body.append(pill);
    pill.querySelector(".close").addEventListener("click", () => {
      pill.classList.remove("on");
      closed = true;
      try { sessionStorage.setItem("coffeeflix-kofi-pill", "1"); } catch (_) { /* storage off */ }
      setTimeout(() => pill.remove(), 600);
    });
    const support = document.querySelector(".support");
    let near = false;
    if (support && "IntersectionObserver" in window) new IntersectionObserver(([e]) => { near = e.isIntersecting; update(); }).observe(support);
    function update() { pill.classList.toggle("on", !closed && !near && scrollY > 700); }
    addEventListener("scroll", update, { passive: true });
    update();
  }
})();
