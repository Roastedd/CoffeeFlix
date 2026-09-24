<p align="center">
  <img src="branding/store_screen.png" alt="CoffeeFlix" width="100%">
</p>

# CoffeeFlix

A media app for the Wii U. It plays YouTube, Twitch, your Jellyfin server, internet radio, podcasts, and whatever you have on the SD card or on a computer on your network, all from one place on the TV or the GamePad.

**Download:** [roastedd.github.io/CoffeeFlix](https://roastedd.github.io/CoffeeFlix/) or the [latest release](https://github.com/roastedd/coffeeflix/releases/latest).

## What it does

- **YouTube** without signing in. Subscriptions live on your SD card, and so does the *For you* feed, which learns from what you watch without sending anything anywhere. You get channel pages (videos, Shorts, live, playlists), search, *Watch later*, history, captions and [SponsorBlock](https://sponsor.ajay.app) skipping. Pick 360p to 1080p from the player; 720p is the default.
- **Jellyfin**: sign in with Quick Connect or a password. Continue watching, next up, seasons and episodes, server subtitles, and your progress syncs back to the server.
- **Twitch**: live channels, search and a follow list, at the quality you choose.
- **Radio**: the 40,000+ stations of radio-browser.info, by country or genre, with the song that's playing.
- **Podcasts**: search Apple's directory, subscribe, and pick up episodes where you left off.
- **Your own files**: videos (with `.srt` subtitles), music, photos, and comics and books (CBZ, EPUB) from the SD card, from a Windows, Mac or NAS share (SMB), or from a DLNA server such as Plex, which it finds on its own.
- A Home screen and search that cover all of the above, and music that keeps playing while you browse.

## Installing

1. Download the zip from the website or the releases page.
2. Extract it to the root of your SD card. The app ends up in `sd:/wiiu/apps/coffeeflix/`.
3. Start CoffeeFlix from the Wii U Menu. You need [Aroma](https://aroma.foryour.cafe).

For local media, put files in the `Videos`, `Music`, `Photos` and `Books` folders the app creates next to itself.

### Connecting things

- **Jellyfin**: enter your server's address, like `http://192.168.1.20:8096`, then approve the Quick Connect code from the Jellyfin app on your phone, or type your password.
- **Network shares**: *My Media > Network shares > Add share*. Enter the computer's IP address (or `\\host\share`), the share name and your login; leave the login empty for guest shares.
- **DLNA servers** appear under *My Media > Media servers* once sharing is turned on in the server.
- **Your YouTube subscriptions**: get `subscriptions.csv` from [Google Takeout](https://takeout.google.com) (*YouTube and YouTube Music > subscriptions*), or a NewPipe or LibreTube export saved as `subscriptions.json`. Put it in `sd:/wiiu/apps/coffeeflix/` and choose *Settings > Import YouTube subscriptions*.

## Controls

The GamePad (including the touch screen), Pro Controller, Classic Controller and Wii Remote all work.

| Button | Anywhere |
|--------|----------|
| D-Pad / left stick | Move |
| A | Select |
| B | Back. At the top of a section it opens the sidebar; press it again to go Home |
| X | The action shown at the bottom right. On a YouTube video it opens a menu: go to the channel, subscribe, *Watch later*, *Not interested* |
| Y | On a YouTube video: *Not interested* |
| + | *Now Playing*, while music or radio plays in the background |

| Button | Video player |
|--------|--------------|
| A | Play / pause |
| D-Pad left / right | Back / forward 10 seconds |
| L, ZL / R, ZR | Back / forward 30 seconds |
| Y | Subtitles |
| X | Audio track |
| D-Pad up / down | Show the controls: quality, subtitles and, on YouTube, *Subscribe* |
| B | Close |

In photos, L and R flip through pictures, A zooms and Y starts a slideshow. In the reader, L and R turn pages and Y switches between fitting the page and the width. Books reopen where you stopped.

## Settings

Accent color, sounds, screensaver, default quality for YouTube, Jellyfin and Twitch, 60 fps video, how the Wii U decodes video, subtitles and captions, SponsorBlock, your region, and importing or exporting YouTube subscriptions. They're saved in `sd:/wiiu/apps/coffeeflix/coffeeflix.json`.

## Formats and tips

The Wii U decodes H.264 in hardware, up to 1080p. Everything else (HEVC, VP8/VP9, MPEG-1/2/4) is decoded on the CPU and is only smooth at 480p or so. If a local file stutters, convert it:

```bash
ffmpeg -i input.mkv -map 0 -c:v libx264 -profile:v high -level 4.0 -pix_fmt yuv420p \
  -preset medium -crf 21 -vf "scale=-2:720" -c:a aac -b:a 192k -c:s copy output.mkv
```

Audio can be MP3, AAC, FLAC, Vorbis, Opus, ALAC, WavPack, WAV or AC-3 (surround is mixed down to stereo). Subtitles can be embedded SRT, ASS, WebVTT or mov_text, or an `.srt` file next to the video. Jellyfin plays what the Wii U can handle directly and has the server convert the rest.

If a video stutters, lower the quality from the player. 60 fps video is off by default because it's twice the work for the console; YouTube only has 30 fps versions of 60 fps videos up to 480p.

If the Wii U's hardware decoder gives no picture, the player switches to a safer mode by itself (hardware decoding without B-frames, then software decoding) and remembers it. You can change it back under *Settings > Video decoding*.

**If something goes wrong**, CoffeeFlix writes a log of every run to `sd:/wiiu/apps/coffeeflix/coffeeflix.log`, and keeps the one before as `coffeeflix-previous.log` (the one you want after a freeze). Please attach it when you report a problem.

## Building

GitHub Actions builds every push ([build.yml](.github/workflows/build.yml)), and pushing a `v*` tag publishes a release.

To build it yourself, Docker is enough:

```bash
tools/docker-build.sh            # the first run also builds FFmpeg-wiiu and libsmb2 into deps/
tools/docker-build.sh DEBUG=1    # debug build
```

With devkitPro installed (`wut`, `wiiu-sdl2*`, `wiiu-curl`, `ppc-jansson`, `ppc-tinyxml2`, `ppc-giflib`, `ppc-libzip`, `ppc-libjpeg-turbo`), run `tools/build-deps.sh` once and then `make`. `WIIU_IP=192.168.x.x tools/deploy.sh` copies a build to a Wii U running an FTP server.

FFmpeg-wiiu is patched during the build; the patches are in [tools/patches](tools/patches).

### Running it on a computer

The same code runs on macOS and Linux, which is much quicker for working on the interface.

```bash
# macOS
brew install pkg-config cmake sdl2 sdl2_ttf sdl2_image ffmpeg curl jansson tinyxml2 libzip
tools/build-deps.sh --host libsmb2   # once
make -f desktop.mk -j8 run

# Linux: install the same libraries, then
tools/build-deps.sh --host
make -f desktop.mk && ./build-desktop/coffeeflix
```

Keys: arrows move, Enter or Z is A, Esc, Backspace or X is B, C is X, V is Y, Tab is +, Q and E are L and R, 1 and 3 are ZL and ZR. Test media goes in `data/media/`, or set `COFFEEFLIX_DATA` to another folder.

The download site is in [docs](docs/index.html) (GitHub Pages), the Homebrew App Store package in [packaging/hbas](packaging/hbas/README.md), and `tools/make-branding.py` draws the icon, splash screens and store art.

## License

CoffeeFlix is source-available under the [PolyForm Noncommercial License 1.0.0](LICENSE.md). You can use it, change it and share it for anything noncommercial. Selling it or building it into a commercial product needs permission.

The libraries, fonts and data it includes keep their own licenses. They're listed in [content/licenses](content/licenses/NOTICES.txt), which also ships inside the app (*Settings > Licenses*). FFmpeg and libsmb2 are LGPL: you can change them and rebuild CoffeeFlix with your versions using this repository.

## Credits

- [devkitPro](https://github.com/devkitPro): the toolchain, wut and the Wii U SDL2 port
- [FFmpeg](https://github.com/FFmpeg/FFmpeg), and [FFmpeg-wiiu](https://github.com/GaryOderNichts/FFmpeg-wiiu) by GaryOderNichts for hardware H.264
- [libsmb2](https://github.com/sahlberg/libsmb2) by Ronnie Sahlberg
- Inter by Rasmus Andersson (SIL OFL) and Material Icons by Google (Apache 2.0)
- The [radio-browser.info](https://www.radio-browser.info) station directory
- [SponsorBlock](https://sponsor.ajay.app) segment data (CC BY-NC-SA 4.0)
- Boot sound: "Game-Main-Menu-Fluids" by [LightMister](https://freesound.org/people/LightMister/sounds/769925/) (CC0)
- CoffeeFlix began as a fork of [cafemp](https://github.com/whateveritwas/cafemp) by whateveritwas and has since been rewritten.
