# Maintaining the feature voting page

The public board is `docs/features.html`, a static GitHub Pages page linked from the homepage. Votes are 👍 reactions on comments in one GitHub issue (the *voting thread*, named in `docs/assets/feature-thread.json`). Each idea has exactly one comment, found by a hidden `<!-- coffeeflix-feature: idea-id -->` marker. Reactions on the issue itself, on the original reports or on any other comment do not count. No tokens, servers or paid voting services are needed.

Voters need a free GitHub account, so every vote belongs to a real account (one 👍 per account per idea) and you can see who voted on GitHub. Ideas themselves only come in as GitHub issues (the page links to the *Feature idea* issue form in `.github/ISSUE_TEMPLATE/`); you then add the ones you accept to the catalogue.

`docs/assets/feature-requests.json` is the single source of truth for every idea's wording, category, status, optional reply and optional GitHub issue links. Ideas are bundled: one card can list several related requests under `includes`, and the votes go to the bundle. A GitHub Action (`.github/workflows/feature-votes.yml`, script `tools/sync-feature-votes.py`) does the rest.

## Run the board from your browser

```bash
python3 tools/feature-admin.py
```

This opens a page on your own computer (nobody else can reach it) where you can change an idea's status, write a reply, edit the wording, GitHub issue numbers and the "what's included" list, add ideas, remove ideas and reorder them. **Save** writes `docs/assets/feature-requests.json` and rebuilds `docs/features.html`; **Publish to website** does that and then commits just those two files and pushes `master`, which also triggers the voting Action. Nothing else in your working folder is touched. Stop it with Ctrl+C. The sections below describe the same changes done by hand in the catalogue.

## What the Action does

It runs every 15 minutes (GitHub may delay scheduled runs), when you run it by hand from the Actions tab, and when the catalogue or thread file changes on `master`. Each run:

1. **Posts a voting comment** for every idea that has none, as `github-actions[bot]`.
2. **Rewrites a comment** whose title, summary, status, issue links or reply no longer match the catalogue. Edits keep the comment, so its votes survive.
3. **Publishes `votes.json`**, a snapshot of the 👍 counts and comment links, to the `votes` branch (one throwaway commit, force-pushed each run, so no history builds up and `master` gets no bot commits). The board reads this file, which avoids GitHub's 60-requests-an-hour limit for anonymous API calls.

It never deletes comments. Comments only count if `Roastedd` or `github-actions[bot]` wrote them, and the earliest comment for an idea wins, so a look-alike comment from someone else cannot hijack a total.

## Add an idea

1. Check the board and the open issues for duplicates. If the new request fits an existing card, add it to that card's `includes` instead of creating another card.
2. Add an entry to `docs/assets/feature-requests.json` with a unique lowercase `id` (letters, digits and hyphens), `category`, `title`, a one-line `summary`, an optional `includes` list (short bullet points), optional `issues` (CoffeeFlix issue numbers to link, like `[13]`), `status` and an optional `reply` (you can also add ideas in the editor below).
3. Run `python3 tools/render-feature-board.py` and commit the catalogue and `docs/features.html`. Category filters and counts are generated for you. Push to `master`. The Action posts the comment within a minute or so, and the card gets its exact vote link as soon as the snapshot updates.

## Change a status

Each card shows a badge from its `status` field. Edit it in the catalogue, run the render script, commit and push; the Action rewrites the comment to match.

| `status` | Badge | Meaning |
| --- | --- | --- |
| `proposed` | Proposed | Suggested by the community |
| `needs-research` | Needs research | Feasibility is being checked |
| `planned` | Planned | Accepted for a future update |
| `in-progress` | In progress | Being built now |
| `shipped` | Shipped | Released |
| `not-planned` | Not planned | Won't be done |

Use Planned, In progress and Shipped only when work is accepted, underway or released; votes are not a schedule or a commitment.

## Reply to an idea

Add a `"reply"` line to the idea in the catalogue (for example `"reply": "Thanks, I'll look at this after the next release."`), render and push. The text shows on the card as a *Maintainer reply* and is added to the voting comment on GitHub. Delete the line to remove it. Plain text only.

## Remove or merge ideas

To remove an idea, delete its entry from the catalogue, run the render script and push. The card disappears from the board. Its old comment stays in the voting thread because the Action never deletes; delete it by hand if you want it gone (votes on it go with it, and it is only a comment, not the issue). To retire an idea but keep it visible, set its `status` to `not-planned` instead.

To merge ideas, add the pieces to one entry's `includes` and delete the other entries. Votes on a deleted comment are gone, so merge before an idea has votes you care about. The linked CoffeeFlix issues (`issues`) are separate GitHub issues; close them rather than deleting them, because a deleted issue cannot be restored.

## Setting up the voting thread (and if it is ever deleted)

Open one empty issue (for example "CoffeeFlix feature voting"), put its number in `docs/assets/feature-thread.json`, run the render script and push. The Action posts a comment for every idea. **Do not delete the issue**: GitHub cannot restore a deleted issue, and every vote on it goes with it. If that happens, open a new one, update the number, render and push; votes start from zero. Do not lock the thread either, since locking can stop people from reacting.

## Counts and availability

Page loads read the snapshot, which can be up to about 15 minutes behind. **Refresh votes** asks GitHub directly, so a new vote shows up straight away; if GitHub is rate limited it keeps the newest totals it has. If the snapshot is more than two hours old (for example the Action stopped) the page asks GitHub itself, and if that fails it shows the old snapshot with its timestamp. It never substitutes a zero for a failure: unknown counts show an em dash. Public results are cached in the visitor's browser for five minutes; no personal or account data is stored.

GitHub pauses scheduled workflows in a public repository after 60 days without repository activity; run the workflow by hand from the Actions tab to resume it. If the thread grows beyond 1,000 comments the script refuses to publish incomplete totals; start a fresh thread as above.

Cards and a "Vote on GitHub" link to the thread stay usable without JavaScript. With JavaScript the page also shows the **Most wanted** top three (hidden until there is at least one vote, and never shown from unavailable counts), search, category pills, a status filter, vote/name/category sorting, and it refreshes the totals when a visitor returns from voting on GitHub. It has keyboard focus styles and reduced-motion support.

## Check changes

- `python3 tools/test-sync-feature-votes.py` checks the Action's script against an in-memory thread.
- `node tools/test-feature-voting.cjs` drives the page in a browser with Playwright (set `NODE_PATH` to its parent `node_modules` directory if it lives outside the project). Both use mock GitHub responses and never cast a vote.
- `python3 tools/sync-feature-votes.py --sync-comments --dry-run --out /tmp/votes.json` previews what the Action would post or change, reading only public data.
