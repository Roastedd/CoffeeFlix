<p align="center">
  <img src="branding/store_screen.png" alt="CoffeeFlix" width="100%">
</p>

# CoffeeFlix – the all-in-one entertainment app for the Wii U

**CoffeeFlix** puts your movies, videos, live streams, music, radio, podcasts, photos and comics in one fast, animated app built for the TV and the GamePad.

- 📺 **YouTube**, no account needed: a *For you* feed that learns what you like on the console itself (nothing leaves the Wii U), subscriptions with a newest-first feed, channel pages (videos, Shorts, live streams, playlists), playlists you can play through or save, search for videos and channels, *Watch later* and history, captions, and skipping sponsor segments with [SponsorBlock](https://sponsor.ajay.app). Plays in up to 720p with hardware H.264 decoding.
- 🍿 **Jellyfin**: your own movie and TV server. Sign in with Quick Connect or a password. Includes continue watching, next up, libraries, seasons and episodes, direct play or transcoding, server subtitles, and resume that syncs back to the server.
- 🟣 **Twitch**: live channels, search and a local follow list, at your chosen quality (60 fps optional).
- 📻 **Radio**: over 40,000 stations from radio-browser.info, by country and genre, with favorites and live "now playing" titles.
- 🎙️ **Podcasts**: search the Apple Podcasts directory and top charts, subscribe to shows, and resume episodes where you left off.
- 📁 **My Media**: videos (with thumbnails, resume and `.srt` subtitles), music (folder queues, embedded and folder cover art), photos (zoom, pan, slideshow), and comics and books (CBZ, EPUB) from the SD card.
- 🖧 **Network shares**: browse and play from a Windows PC, Mac, Samba or NAS share (SMB 2/3).
- 📡 **Media servers (DLNA)**: Plex, Jellyfin, Emby, minidlna and most NAS boxes are found automatically on your network, no setup needed.
- 🔎 **Search everything** at once, with a **Home** screen that pulls it all together.
- 🎵 **Now Playing** with a live spectrum visualizer, and a mini player that keeps music going while you browse.

---

## 📦 Installation

1. Download the latest release from the [CoffeeFlix website](https://roastedd.github.io/CoffeeFlix/) or the [releases page](https://github.com/roastedd/coffeeflix/releases/latest), or grab `coffeeflix-<commit>` from the latest [Build](https://github.com/roastedd/coffeeflix/actions/workflows/build.yml) run.
2. Extract the ZIP file to the **root of your SD card**.
3. Put your own media into `sd:/wiiu/apps/coffeeflix/` (the app creates `Videos`, `Music`, `Photos` and `Books` folders there).
4. Launch **CoffeeFlix** from the Wii U Menu (Aroma).

**or**

<p align="left">
  <a href="https://hb-app.store/wiiu/coffeeflix">
    <img src="branding/hbasbadge-wiiu.png" alt="Get it on the Homebrew App Store!" width="25%">
  </a>
</p>

### Connecting your services

- **Jellyfin**: open *Jellyfin* and enter your server address, e.g. `http://192.168.1.20:8096`. Then approve the Quick Connect code in the Jellyfin app on your phone, or type your password.
- **Network shares**: open *My Media → Network shares → Add share*. Enter the computer's IP address (or `\\host\share`), the share name and your login. Leave the login empty for guest shares.
- **Media servers (DLNA)** show up by themselves under *My Media → Media servers* once DLNA sharing is on in the server.
- **YouTube, Twitch, radio and podcasts** work without an account. Subscriptions, follows and favorites are stored on your SD card.
- **Bring your YouTube subscriptions**: put `subscriptions.csv` from [Google Takeout](https://takeout.google.com) (*YouTube and YouTube Music → subscriptions*) or a NewPipe/LibreTube export named `subscriptions.json` in `sd:/wiiu/apps/coffeeflix/`, then pick *Settings → Import YouTube subscriptions*. *Export* writes them back out in NewPipe's format.

---

## 🎮 Controls

The GamePad, Pro Controller, Classic Controller and Wii Remote (with pointer) all work. The GamePad touch screen works too: tap to select, drag to scroll.

| Button | Everywhere |
|--------|------------|
| `D-Pad` / `Left Stick` | Move around |
| `A` | Select |
| `B` | Back; at the top of a section it opens the sidebar, and again goes Home |
| `X` | The action shown at the bottom right: favorite, follow, remove. On YouTube videos it opens *More*: go to the channel, subscribe, save to *Watch later*, *Not interested* |
| `Y` | On YouTube videos: *Not interested* (you'll see less like it) |
| `+` | Open *Now Playing* while music or radio plays in the background |

| Button | Video player |
|--------|--------------|
| `A` | Play / pause |
| `D-Pad ←/→` | Back / forward 10 s |
| `L`/`ZL`, `R`/`ZR` | Back / forward 30 s |
| `Y` | Subtitles |
| `X` | Audio track |
| `D-Pad ↑/↓` | Show the controls |
| `B` | Close |

| Button | Now Playing |
|--------|-------------|
| `A` | Buttons on screen (previous, play/pause, next, stop) |
| `L` / `R` | Back 15 s / forward 30 s |
| `B` | Back (music keeps playing) |

| Button | Photos |
|--------|--------|
| `D-Pad ←/→`, `L`/`R` | Previous / next photo |
| `A`/`ZR`, `ZL` | Zoom in / out (`D-Pad` pans when zoomed) |
| `Y` | Slideshow |
| `B` | Back |

| Button | Reader (CBZ, EPUB) |
|--------|--------------------|
| `D-Pad ←/→`, `L`/`R`, swipe | Previous / next page |
| `A`/`ZR`, `ZL` | Zoom in / out |
| `D-Pad`, `Left Stick`, drag | Pan when zoomed, scroll in fit width |
| `Y` | Fit page / fit width |
| `B` | Reset zoom, then back |

The reader reopens every book on the page where you stopped.

---

## ⚙️ Settings

Accent color, interface sounds, screensaver delay, streaming quality for YouTube, Jellyfin and Twitch, 60 fps streams, default subtitles, YouTube captions, SponsorBlock, your region (used for popular videos and radio), connection security, Jellyfin sign-out, importing and exporting YouTube subscriptions, and resetting what the YouTube *For you* feed has learned. Everything is saved to `sd:/wiiu/apps/coffeeflix/coffeeflix.json`.

---

## ⚙️ Compatibility tips

The Wii U decodes **H.264 in hardware** (up to 1080p); everything else is decoded in software. For the smoothest local playback, re-encode other formats:

```bash
# 720p – plays everywhere
ffmpeg -i <input> -map 0 -c:v libx264 -profile:v high -level 3.1 -pix_fmt yuv420p \
  -preset medium -crf 21 -vf "scale=-2:720" -c:a aac -b:a 192k -c:s copy <output>.mkv

# 1080p
ffmpeg -i <input> -map 0 -c:v libx264 -profile:v high -level 4.0 -pix_fmt yuv420p \
  -preset medium -crf 20 -vf "scale=-2:1080" -c:a aac -b:a 256k -c:s copy <output>.mkv
```

- **Video**: H.264 (hardware), plus HEVC, VP8/VP9 and MPEG-1/2/4 in software (best at 480p or lower).
- **Audio**: MP3, AAC, FLAC, Vorbis, Opus, ALAC, WavPack, WAV and AC-3/E-AC-3. Surround is downmixed to stereo.
- **Subtitles**: embedded SRT/ASS/WebVTT/mov_text, and `.srt` files next to the video.
- **Jellyfin** direct-plays what the Wii U can decode and asks the server to transcode the rest to H.264/AAC at your quality setting.
- **Something went wrong?** CoffeeFlix keeps a log of each run in `sd:/wiiu/apps/coffeeflix/coffeeflix.log` (and the run before in `coffeeflix-previous.log`, handy after a freeze). Attach it when you report a problem.

---

## 🛠️ Building

Every push is built by GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml)); pushing a `v*` tag publishes a release.

To build locally you only need Docker:

```bash
tools/docker-build.sh            # first run also builds FFmpeg-wiiu and libsmb2 into deps/
tools/docker-build.sh DEBUG=1    # unoptimized build with debug logging
```

With devkitPro installed natively (`wut`, `wiiu-sdl2*`, `wiiu-curl`, `ppc-jansson`, `ppc-tinyxml2`, `ppc-giflib`, `ppc-libzip`, `ppc-libjpeg-turbo`), run `tools/build-deps.sh` once, then `make`.

To push a build to a Wii U running an FTP server: `WIIU_IP=192.168.x.x ./deploy.sh`.

### Desktop preview

The same code runs on macOS and Linux, which makes UI work much faster.

```bash
# macOS (Homebrew)
brew install pkg-config cmake sdl2 sdl2_ttf sdl2_image ffmpeg curl jansson tinyxml2 libzip
tools/build-deps.sh --host libsmb2   # once
make -f desktop.mk -j8 run

# Linux
tools/build-deps.sh --host           # FFmpeg and libsmb2 for the host
make -f desktop.mk && ./build-desktop/coffeeflix
```

Without FFmpeg in `deps/host` the system's is used. Test files go in `data/media/` (`Videos`, `Music`, `Photos`, `Books`).

Keyboard: arrows move, `Enter`/`Z` = A, `Esc`/`Backspace`/`X` = B, `C` = X, `V` = Y, `Tab` = +, `Q`/`E` = L/R, `1`/`3` = ZL/ZR. Set `COFFEEFLIX_DATA=<dir>` to use a separate settings and media folder.

### Website and Homebrew App Store

The download site is in [`docs/`](docs/index.html) and is served by GitHub Pages. The Homebrew App Store package is in [`packaging/hbas`](packaging/hbas/README.md).

### Branding

`tools/make-branding.py` renders the icon, splash screens and store art from the app's own fonts and colors.

---

## 📜 License

CoffeeFlix is **source-available** under the [PolyForm Noncommercial License 1.0.0](LICENSE.md): you're free to use it, change it and share it for any noncommercial purpose. Selling it, or using it in a commercial product or service, isn't allowed without permission.

CoffeeFlix includes open-source libraries, fonts and data under their own licenses. They're listed with their license texts in [`content/licenses`](content/licenses/NOTICES.txt), which ships inside the app (*Settings → Licenses*). FFmpeg and libsmb2 are under the GNU LGPL: you may change them and rebuild CoffeeFlix with your versions using the source and build scripts in this repository.

---

## 🙏 Credits

- 🛠️ **devkitPro, wut and the Wii U SDL2 port**: [devkitPro](https://github.com/devkitPro)
- 🎞️ **FFmpeg** and **FFmpeg-wiiu** (hardware H.264) by GaryOderNichts: [FFmpeg](https://github.com/FFmpeg/FFmpeg) · [FFmpeg-wiiu](https://github.com/GaryOderNichts/FFmpeg-wiiu)
- 🖧 **libsmb2** by Ronnie Sahlberg: [GitHub](https://github.com/sahlberg/libsmb2)
- 🔤 **Inter** by Rasmus Andersson (SIL OFL) and **Material Icons** by Google (Apache 2.0)
- 📻 **radio-browser.info** community station directory
- ⏭️ **SponsorBlock** segment data (CC BY-NC-SA 4.0): [sponsor.ajay.app](https://sponsor.ajay.app)
- 🔔 **Boot sound**: "Game-Main-Menu-Fluids" by [LightMister on Freesound](https://freesound.org/people/LightMister/sounds/769925/) (CC0)
- ☕ CoffeeFlix started as a fork of [cafemp](https://github.com/whateveritwas/cafemp) by whateveritwas, and has since been rewritten from the ground up.

Made with ❤️ and ☕
