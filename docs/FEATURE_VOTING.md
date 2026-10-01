# Maintaining the feature voting page

The public board is `docs/features.html`. It is a static GitHub Pages page, linked from the homepage. The shared votes live in [issue #15](https://github.com/Roastedd/CoffeeFlix/issues/15): each feature has one maintainer-authored comment, and its 👍 reaction count is that feature's total. Reactions on the issue itself or the original reports do not count. No tokens, server or paid voting service are needed.

## Add an idea

1. Check the catalogue and original issues for duplicates. Bug reports belong in the issue tracker, separate from feature voting.
2. Add one comment to issue #15 with the feature title, a short explanation, its category, source and instructions to add a 👍 reaction. Include these hidden markers:

   ```html
   <!-- coffeeflix-feature: unique-feature-id -->
   <!-- coffeeflix-status: proposed -->
   ```

3. Add the entry to `docs/assets/feature-requests.json`. Copy the numeric comment ID from the comment's permalink (`#issuecomment-ID`) into `commentId`. Each feature must have a unique ID and comment ID. Keep the existing comment when wording changes so votes survive.
4. Run `python3 tools/render-feature-board.py` from the repository and commit the updated catalogue and `docs/features.html`. The page needs no build step at deployment. If adding a category, also add it to the category select in the HTML.

## Update a status

Edit the original voting comment's `coffeeflix-status` marker and its visible status text. Supported values are `proposed`, `needs-research`, `planned`, `in-progress`, `shipped` and `not-planned`. Update the catalogue's `status` too and regenerate the page so offline visitors see the same state. Use Planned, In progress and Shipped only when accepted, underway or released; votes are not a schedule or a commitment.

The page uses status markers only from the existing registered comments authored by `Roastedd`. Other community comments cannot create cards or alter their statuses. Source and feature text are escaped when generating HTML.

## Counts and availability

The browser reads the public GitHub issue-comments API, including pagination, and displays only `reactions["+1"]` for registered comment IDs. Counts are fetched on page load and with Refresh votes. Public results are cached in the visitor's browser for five minutes to reduce API usage; no personal or account data is stored. The refresh button is for checking votes after returning from GitHub.

If GitHub is offline or rate limited, the page retains saved counts with a notice, or shows an em dash when no count is available. It never substitutes a zero for an API failure. If a voting comment is deleted, the card links to the full thread and shows an unavailable count. Avoid deleting voting comments. If discussion grows beyond 1,000 comments, the fetch deliberately reports unavailable totals rather than silently publishing incomplete counts; consider a fresh thread and update the API/thread constants and generator together.

Static cards and voting links remain usable without JavaScript. The page also has search, category/status filters, vote/name/category sorting, keyboard focus styles and reduced-motion support.

## Check changes

With Node and Playwright available, run `node tools/test-feature-voting.cjs`. If Playwright is provided outside the project, set `NODE_PATH` to its parent `node_modules` directory. The checks use a local server and mock GitHub responses; they do not cast votes. They cover counts, comment permalinks, filters, trusted status updates, pagination, missing comments, offline/rate-limited responses, JavaScript disabled and mobile layouts.
