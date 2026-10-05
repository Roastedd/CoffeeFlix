# CoffeeFlix releases

Developer → supporter preview → public stable. Each platform can move at its own pace.
CoffeeFlix stays free. Supporters get selected upcoming features early, without a promised
number of days. Critical fixes can go directly to everyone.

## Initial rollout

- Build and test locally first. Begin supporter delivery with Ko-fi downloads at **ubecatstudio**.
- Keep previews and unreleased source in private development storage. A public branch or
  public Actions artifact does not provide supporter-only access.
- Use manual draft releases for the first few cycles. `release/approved.json` starts with
  `automatic: false` and `candidate: null`: nothing is approved or scheduled for publication.
- The weekly workflow is prepared for Sunday at 21:17 UTC (2:17 PM Los Angeles during
  daylight saving time, 1:17 PM in winter). GitHub can delay scheduled runs.
- After successful manual releases, enable `automatic` explicitly. A week with no approved
  candidate produces no release. Run the workflow manually for an urgent fix.
- Monthly supporters receive each new preview through ubecatstudio’s Ko-fi. One-time donors
  receive the current preview and its fixes through manual delivery after donation verification,
  with no second payment. The 2.4.3 Aroma preview passed its user-reported Wii U smoke test.
  Settings-based preview access is a later phase.

## Repository boundaries

This public repository contains the Wii U app and its website. Android implementation,
Android packages, signing keys, private development files, and supporter access records
belong elsewhere. Website roadmap entries can describe Android without adding its code.

Keep new Wii U feature development in a separate private repository or a local checkout
with no public push remote. Promote only reviewed Wii U changes into this repository.
Do not merge an unrestricted development tree or mirror its full history into public source.
If a commit/history contains private files, create reviewed public commits from selected
changes; preserve required license notices and attribution. The public source must match
the build eventually released. Android needs its own source/export review and release feed.

## Prepare a candidate

Choose a small, coherent set of features. Build from a clean, recorded commit, with the
intended final numeric version such as `2.4.1`. Identify preview revisions in the Ko-fi post
and by manifest hash; do not rebuild a tested package merely to change its preview label.
The existing updater compares numeric versions and is not a preview-channel manager.

For work already in public source, pushing a numeric tag such as `v2.4.1` runs the build
workflow. It creates the three Wii U packages, `release-manifest.json`, and `SHA256SUMS.txt`,
then creates a **draft**, not a public release. Re-running draft creation refuses to
overwrite an existing release. Non-tag builds remain development artifacts.

The manifest binds a version and source commit to each exact file. It is an integrity
record, not a replacement for the app's signature checks or a certificate of hardware testing.

For a private supporter candidate, keep the same five files together. Generate its manifest
with `python3 tools/release-bundle.py create --directory <candidate-folder> --version <version>
--commit <full-source-sha>` after packaging. Keep a matching source snapshot privately.
Create a separate $0 Ko-fi digital shop item for each preview release, restricted to monthly
supporters with pay-what-you-want off. Link it from the matching monthly-supporter post.
Ko-fi posts link to downloads rather than hosting arbitrary ZIP attachments. Avoid a rolling
shop item: claimed items include future file updates, even after the original acquisition.
Do not link a private GitHub draft as the download: supporters do not automatically have access.

When that candidate is ready for public release:

1. Review and publish its matching permitted source. If sanitizing history changes the commit
   ID, record the equivalent public commit in a new manifest and re-review the source mapping;
   package bytes must remain the tested bytes.
2. Tag that source and let draft creation finish. If CI rebuilt it, **those rebuilt packages
   are not the tested supporter packages**. Before any approval, replace the draft's five
   assets with the preserved candidate files and its reviewed manifest/checksums.
3. Verify the downloaded draft with `python3 tools/release-bundle.py verify --directory <folder>`.
   Use the manifest hash of this final draft for approval. Never replace assets after approval;
   clear the approval and repeat testing/review when a candidate changes.

The uploader can be automated later. Never blindly sync a private repo to the public repo.

## Test the exact files

Record the tester, device/firmware, date, changes, and any known issues. Test:

- Installation and launch through both Aroma and Tiramisu while both packages are offered.
- Playback, seeking, audio and subtitles; test services touched by this release.
- GamePad/remote/controller navigation and returning from playback.
- Existing settings and subscriptions after restart.
- Updating from the current stable build, including settings preservation.
- The public source/files and the feedback from preview testing.

Do not infer a pass from compilation. A failed check or a release-blocking bug holds the release.
For an urgent fix with no preview period, write that exception in `notes` and record the
direct hardware testing used instead when completing `preview_feedback`.

## Approve and publish

After testing, replace `candidate: null` in `release/approved.json` with a record shaped like
this. The placeholders and false checks intentionally do not authorize a release:

```json
{
  "schema": 1,
  "automatic": false,
  "candidate": {
    "tag": "v2.4.1",
    "manifest_sha256": "REPLACE_WITH_HASH_OF_FINAL_RELEASE_MANIFEST",
    "tested_by": "Maintainer name",
    "tested_at": "REPLACE_WITH_ISO_TIMESTAMP_WITH_TIMEZONE",
    "devices": "Device, firmware, Aroma/Tiramisu versions",
    "notes": "Testing evidence, preview feedback, and release notes reviewed",
    "blockers": [],
    "checks": {
      "install_aroma": false,
      "install_tiramisu": false,
      "playback": false,
      "navigation": false,
      "settings_persistence": false,
      "update_install": false,
      "public_source_review": false,
      "preview_feedback": false
    }
  }
}
```

Commit the completed record to `master`. In Actions, run **Promote approved release** with
**publish** unchecked for a dry run. It downloads and checks the files, the pinned manifest,
the source tag, and the stable version ordering. After reviewing the dry run, run it with
**publish** checked. This publishes the draft as latest without rebuilding any package.
The same operation runs in the weekly window only when `automatic` is true.

The script uses GitHub CLI with `GH_TOKEN`; it never needs a token embedded in the app.
Locally, `python3 tools/release-bundle.py promote` is also a dry run by default.
After publishing, clear `candidate` to null. An already-published tag is a no-op on retries.
The source commit and artifact hashes remain available with the release.

If a published release is broken, stop promoting it and ship a corrected higher version;
do not silently overwrite published files or assume every device can downgrade.

## Roadmap upkeep

Edit `docs/assets/roadmap.json` for platform milestones and its review date. Entries with
`feature_id` read their title, summary, status and released version from the voting board's
catalogue. Removed or reconsidered ideas leave the roadmap automatically on render.

Run `python3 tools/render-roadmap.py` and commit the generated `docs/index.html` with the
data. The feature-board editor also rebuilds/publishes the homepage when a shared feature
changes. It publishes the catalogue, feature page and homepage, so review changes to all
three before using its Publish button. Other homepage edits will be included.

Use **In progress**, **In testing**, **Planned**, and **Released** honestly. A successful build
does not mean a feature is released. The site's latest stable version comes from GitHub;
roadmap status changes remain deliberate maintainer edits. No promised ship dates or fake
supporter version numbers are generated.

## Verification

```bash
python3 tools/test-release-bundle.py
python3 tools/test-feature-admin.py
python3 tools/render-roadmap.py --check
```

The release tests use fake GitHub responses and never publish anything. A live GitHub Actions
run and hardware testing are still needed before the first real promotion.
