# CoffeeFlix – Premium Media Player for the Nintendo Wii U

## 🎬 About

**CoffeeFlix** is a feature-rich, open-source media player for the Wii U, supporting local media playback and YouTube streaming. It handles most common video and audio formats with **H.264 hardware acceleration up to 1080p**. Crafted with a coffee-themed aesthetic for an elegant viewing experience.

This is an **actively developed project**—most features are stable, but some experimental functionality may have occasional issues.

Made with ❤️ and ☕ 

> ℹ️ **Note:** Best performance with H.264 videos. 1080p requires hardware acceleration, 720p works perfectly for all content.

---

## 📦 Installation

1. [Download the latest release](https://github.com/roastedd/coffeeflix/releases/latest) (or grab `coffeeflix-<commit>` from the latest [Build](https://github.com/roastedd/coffeeflix/actions/workflows/build.yml) run).
2. Extract the ZIP file to the **root of your SD card**.
3. Place your media files into:  
   `sd:/wiiu/apps/coffeeflix/`

**or**

<p align="left">
  <a href="https://hb-app.store/wiiu/coffeeflix">
    <img src="branding/hbasbadge-wiiu.png" alt="Get it on the Homebrew App Store!" width="25%">
  </a>
</p>

---

## 🎮 Using CoffeeFlix

1. Launch **CoffeeFlix** from the Wii U main menu or Homebrew Launcher.
2. Select your desired media type from the sidebar.
3. Use the file browser to locate and select and play your media.

---

## 🛠️ Building

Every push is built by GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml)); pushing a `v*` tag publishes a release.

To build locally you only need Docker:

```bash
tools/docker-build.sh            # first run also builds FFmpeg-wiiu and MuPDF into deps/
tools/docker-build.sh DEBUG=1    # unoptimized build with debug logging
```

With devkitPro installed natively (`wut`, `wiiu-sdl2*`, `wiiu-curl`, `ppc-jansson`, `ppc-giflib`, `ppc-libzip`, `ppc-libjpeg-turbo`), run `tools/build-deps.sh` once, then `make`.
Without MuPDF in `deps/` the app still builds; the reader then opens comic books (CBZ) only.
To push a build to a Wii U running an FTP server: `WIIU_IP=192.168.x.x ./deploy.sh`.

### 🎥 Controls – Video Player

| Button | Action                |
|--------|-----------------------|
| `A`    | Play / Pause          |
| `B`    | Return to file browser|
| `X`    | Change audio track    |
| `D-Pad L/R` | Seek backward / forward (5s) |

### 🎵 Controls – Audio Player

| Button      | Action                 |
|-------------|------------------------|
| `A`         | Play / Pause           |
| `B`         | Return to file browser |
| `D-Pad L/R` | Skip / Rewind          |

### 🖼️ Controls – Photo Viewer

| Button           | Action                     |
|------------------|----------------------------|
| `B`              | Return to file browser     |
| `X`              | Change audio track         |
| `Left Stick L/R` | Show next / previous photo |
| `ZR / RL`        | Zoom in / Zoom out         |
| `Touch`          | Pan                        |

### 📖 Controls – Reader (CBZ, PDF, EPUB)

| Button                 | Action                                  |
|------------------------|-----------------------------------------|
| `D-Pad L/R`, `L / R`, swipe | Previous / next page               |
| `A` / `ZR`, `ZL`       | Zoom in, zoom out                       |
| `D-Pad`, `Left Stick`, drag | Pan when zoomed, scroll in fit width |
| `Y`                    | Fit page / fit width                    |
| `B`                    | Reset zoom, then back                   |

The reader reopens every book on the page you left it.

### 📺 Controls – YouTube

| Button     | Action                       |
|------------|------------------------------|
| `A`        | Select video / Play          |
| `B`        | Back to previous screen      |
| `D-Pad`    | Navigate menus               |
| `MINUS`    | Toggle sidebar               |
| `Touch`    | Touch input for UI           |

### 🎮 Universal Controls

| Button     | Action                       |
|------------|------------------------------|
| `MINUS`    | Toggle sidebar on/off        |
| `D-Pad`    | Navigate UI elements         |
| `Left Stick` | Navigate UI (analog)       |
| `A`        | Select / Confirm             |
| `B`        | Back / Cancel                |

---

## ⚙️ Compatibility Tips

For best results, re-encode your videos using this FFmpeg command:

**For 720p (recommended for maximum compatibility):**
```bash
ffmpeg -i <input> \
-map 0 \
-c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
-preset ultrafast -tune fastdecode -crf 23 -vf "scale=-2:720" \
-c:a aac -b:a 256k \
-c:s copy \
<output>
```

**For 1080p (requires H.264 hardware acceleration):**
```bash
ffmpeg -i <input> \
-map 0 \
-c:v libx264 -profile:v high -level 4.0 -pix_fmt yuv420p \
-preset medium -tune film -crf 20 -vf "scale=-2:1080" \
-c:a aac -b:a 320k \
-c:s copy \
<output>
```

---

## ✅ Features

* 🎥 **Video Playback**: H.264 up to 1080p with hardware acceleration, VP8/VP9, HEVC, MPEG1/2/4
* 🎵 **Audio Playback**: MP3, AAC, FLAC (16/24-bit), Vorbis, Opus, and more
* 🖼️ **Image Viewer**: JPEG, PNG, BMP, GIF with zoom and pan
* 📄 **Reader**: Comic books (CBZ), PDF and EPUB with zoom, fit width and resume
* 📺 **YouTube Integration**: Search, trending, popular videos, and direct URL playback
* ⏩ **Media Seeking**: 5-second skip/rewind for videos and audio
* 🎮 **Full Controller Support**: GamePad, Wii Remote, Wii Remote + Nunchuk, Pro Controller
* 🎬 **Subtitles**: SRT subtitle support with customizable display
* 🎚️ **Multi-Audio**: Switch between audio tracks on-the-fly
* 💾 **Settings Persistence**: Saves preferences to SD card

---

## 🔜 Planned Features

* 🌐 DLNA / Jellyfin streaming
* 💾 USB drive support (ext4, exFAT)
* 📊 Audio visualizations
* 📺 Playlist support (M3U, M3U8)
* 🔍 YouTube search history and favorites
* 🎨 Theme customization
* 📱 Resume playback from last position

---

## 🐞 Known Issues

* ❗ **Audio/Video Desync**
  Playback may fall out of sync, especially with high-resolution or complex video files. Re-encoding with the recommended FFmpeg settings may help.

* ❗ **App Stability on Exit**
  Significantly improved cleanup procedures have reduced exit crashes. Proper resource cleanup and thread management now prevent most crashes when returning to the Wii U system menu.

* ❗ **Unstable / Experimental Behavior**
  CoffeeFlix is in early development. Expect occasional hangs, crashes, or features not working as intended.

* ✅ **Controller Support**
  All Wii U controllers now supported: GamePad, Wii Remote, Wii Remote + Nunchuk, and Pro Controller with full D-Pad/stick navigation.

* ℹ️ **Video Codec Performance**
  - **H.264**: Best performance with hardware acceleration (up to 1080p)
  - **VP8/VP9**: Software decoding (recommended ≤480p for smooth playback)
  - **HEVC/H.265**: Experimental hardware + software (performance varies)
  - **MPEG1/2/4**: Software decoding (good performance at standard resolutions)

* ℹ️ **Audio Format Support**
  - **FLAC**: 16-bit and 24-bit work perfectly, multichannel auto-downmixes to stereo
  - **High sample rates**: >192kHz may cause performance issues
  - **MP3/AAC/Vorbis/Opus**: Full support with excellent performance

* ℹ️ **YouTube Playback**
  - Streams via YouTube InnerTube API (quality: 360p/480p/720p/1080p)
  - Best with 720p setting for H.264 hardware acceleration
  - Network connection required, performance depends on internet speed

---

## 🙏 Credits

* 🎵 **Ambiance Music**: [LightMister on Freesound](https://freesound.org/people/LightMister/sounds/769925/)
* 🛠️ **devkitPro**: [GitHub](https://github.com/devkitPro)
* 💬 **stdout implementation by dkosmari**: [Github](https://github.com/dkosmari/devkitpro-autoconf/blob/main/examples/wiiu/sdl2-swkbd/src/stdout.cpp)
* 💬 **srtparser.h**: [Github](https://github.com/saurabhshri/simple-yet-powerful-srt-subtitle-parser-cpp)
* 🎞️ **FFmpeg**: [GitHub](https://github.com/FFmpeg/FFmpeg/)
* 🧰 **FFmpeg Wii U Configure Script by GaryOderNichts**: [Github](https://github.com/GaryOderNichts/FFmpeg-wiiu/blob/master/configure-wiiu)
* 🖼️ **Nuklear GUI Library**: [GitHub](https://github.com/Immediate-Mode-UI/Nuklear)
* 🔧 **Wii U Toolchain (WUT)**: [GitHub](https://github.com/devkitPro/wut)
* 📄 **MuPDF**: [GitHub](https://github.com/ArtifexSoftware/mupdf)
* 📄 **mupdf port by hito16**: [GitHub](https://github.com/hito16/mupdf-devkitppc)
* 🔧 **Helper file from hito16** [Github](https://github.com/hito16/SDLReader/blob/main/ports/wiiu/wiiu_time_utils.c)
