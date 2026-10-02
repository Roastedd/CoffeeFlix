#!/usr/bin/env bash
# Developer updates: builds CoffeeFlix, signs it with your developer key and offers it to your
# Wii U on this network, which installs it from Settings > Updates once developer updates are on
# (Up Up Down Down Left Right Left Right B A on that screen). Ctrl-C stops offering it.
# While it runs, the Wii U sends its log here too: logs/wiiu-latest.log is the newest run's.
#   tools/dev-update.sh              # build, then offer it
#   tools/dev-update.sh --no-build   # offer the coffeeflix.wuhb built last
#   tools/dev-update.sh --no-build --also-project /path/to/another/checkout
# The app only installs builds signed with ~/.coffeeflix/dev-key.pem (keep a copy of it safe:
# a new key needs a release with its public half in src/app/updater.cpp).
set -euo pipefail
cd "$(dirname "$0")/.."
KEY="${COFFEEFLIX_DEV_KEY:-$HOME/.coffeeflix/dev-key.pem}"
[ -f "$KEY" ] || { echo "No developer key at $KEY" >&2; exit 1; }

build=1
extra=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --no-build) build=0; shift ;;
        --also-project)
            [ "$#" -ge 2 ] || { echo "--also-project needs a checkout path" >&2; exit 1; }
            extra+=(--also-project "$2"); shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ "$build" = 1 ]; then
    version="$(git describe --tags --always --dirty | sed 's/^v//')"
    started=""
    if command -v colima >/dev/null && ! colima status 2>&1 | grep -q "is running"; then
        colima start
        started=1
    fi
    JOBS="${JOBS:-6}" tools/docker-build.sh APP_VERSION="$version" || { [ -n "$started" ] && colima stop; exit 1; }
    [ -n "$started" ] && colima stop
fi

[ -f coffeeflix.wuhb ] || { echo "No coffeeflix.wuhb: build it first" >&2; exit 1; }
# The version the build reports, which the Wii U shows.
version="$(sed -n 's/^#define APP_VERSION "\(.*\)"$/\1/p' build/app_version.h 2>/dev/null)"
version="${version:-$(git describe --tags --always --dirty | sed 's/^v//')}"

# What changed since the last release, newest first (the Wii U shows the first 8 lines).
notes="$(date '+Built %b %-d, %-I:%M %p')"
[ -n "$(git status --porcelain --untracked-files=no)" ] && notes+=$'\n- Changes not committed yet'
last="$(git describe --tags --abbrev=0 2>/dev/null || true)"
notes+=$'\n'"$(git log --no-merges --format='- %s' ${last:+"$last"..HEAD} | head -n 12)"

build_id="$(sed -n 's/^#define APP_BUILD_ID "\(.*\)"$/\1/p' src/core/build_variant.hpp)"
build_name="$(sed -n 's/^#define APP_BUILD_NAME "\(.*\)"$/\1/p' src/core/build_variant.hpp)"
exec python3 tools/dev_server.py --wuhb coffeeflix.wuhb --version "$version" --notes "$notes" --key "$KEY" \
    --build-id "$build_id" --build-name "$build_name" ${extra[@]+"${extra[@]}"}
