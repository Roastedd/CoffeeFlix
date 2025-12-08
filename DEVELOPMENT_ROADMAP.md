# CaféMP Development Roadmap

## 📋 Table of Contents
1. [Architecture Overview](#architecture-overview)
2. [Current Implementation](#current-implementation)
3. [Identified Issues](#identified-issues)
4. [Short-Term Improvements](#short-term-improvements)
5. [Medium-Term Features](#medium-term-features)
6. [Long-Term Goals](#long-term-goals)
7. [Technical Debt](#technical-debt)

---

## 🏗️ Architecture Overview

### Core Components

```
CaféMP Architecture:
┌─────────────────────────────────────────────────────────┐
│                        main.cpp                         │
│  ┌──────────────────────────────────────────────────┐  │
│  │ WHBProc Init → SDL Init → UI Init → Main Loop   │  │
│  └──────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
                           │
         ┌─────────────────┼─────────────────┐
         ▼                 ▼                 ▼
    ┌────────┐      ┌────────────┐   ┌──────────┐
    │  SDL   │      │  Nuklear   │   │  Players │
    │ Layer  │      │  UI/Scenes │   │  Module  │
    └────────┘      └────────────┘   └──────────┘
         │                 │                 │
         │                 │                 │
         ▼                 ▼                 ▼
    Rendering      Scene Management    Media Decoding
    (GPU/CPU)      (State Machine)     (FFmpeg/SDL)
```

### Module Breakdown

#### **1. SDL Layer** (`src/utils/sdl.cpp`)
- Window and renderer management
- Texture handling
- Hardware abstraction for Wii U

#### **2. UI System** (`src/ui/`)
- **Nuklear GUI**: Immediate-mode GUI framework
- **Scene Manager**: State-based scene switching
- **Widgets**: Reusable UI components (HUD, sidebar, tooltips, captions)

#### **3. Media Players** (`src/player/`)
- **Video Player**: FFmpeg-based with H.264 hardware acceleration
- **Audio Player**: Multi-threaded audio decoding with SDL audio
- **Photo Viewer**: Image display with zoom/pan
- **PDF Viewer**: MuPDF integration for documents
- **Subtitle System**: SRT parser for captions

#### **4. Input System** (`src/input/`)
- GamePad input handling
- Touch input support
- Button state management

#### **5. Utilities** (`src/utils/`)
- Media file scanning
- App state management
- Media info tracking
- Format validation

---

## 🔧 Current Implementation

### Data Flow

```
User Input → Input Handler → Scene State Machine → Player/Viewer
                                      ↓
                           UI Updates (Nuklear) → SDL Renderer → Display
                                      ↓
                          Media Decode (FFmpeg) → Audio/Video Output
```

### State Machine

```
States:
- STATE_MENU: Main menu
- STATE_MENU_{VIDEO|AUDIO|IMAGE|PDF}_FILES: File browsers
- STATE_PLAYING_{VIDEO|AUDIO}: Media playback
- STATE_VIEWING_{PHOTO|PDF}: Content viewing
- STATE_SETTINGS: Configuration screen
```

### Threading Model

1. **Main Thread**: UI rendering + input handling
2. **Video Decode Thread**: Video frame decoding (spawned per video)
3. **Audio Decode Thread**: Audio packet decoding (spawned per audio stream)

---

## 🐛 Identified Issues

### Critical Issues

#### **1. Infinite Loop in `media_info.cpp`** ⚠️
**Location**: Lines 12, 40, 68
```cpp
while (1);  // Crash intentionally for debugging
```
**Issue**: Debug code left in production - will freeze app if media_info is null
**Impact**: High - causes hard crash requiring console restart
**Fix**: Replace with proper error handling and recovery

#### **2. Video Seek Disabled** ⚠️
**Location**: `src/ui/scenes/scene_video_player.cpp` lines 38-45
```cpp
#ifdef DEBUG
//        video_player_seek(-5.0f);
#endif
```
**Issue**: Video seeking is commented out even in debug builds
**Impact**: Medium - feature is non-functional
**Fix**: Enable and test seek functionality

#### **3. Missing Error Recovery**
**Location**: Multiple player cleanup functions
**Issue**: If cleanup fails during exit, app may crash
**Impact**: Medium - causes "App Crashes on Exit" issue
**Fix**: Add try-catch blocks and graceful degradation

### Medium Priority Issues

#### **4. Memory Management**
- No cleanup on scene transitions
- Potential memory leaks when switching between media files rapidly
- SwsContext not always freed properly

#### **5. Thread Safety**
- `show_hud` global variable not thread-safe
- Multiple atomic operations without proper memory barriers in some locations

#### **6. Performance**
- No frame skip mechanism for slow decoders
- Audio buffer can grow unbounded in some edge cases
- No profiling or performance metrics

### Low Priority Issues

#### **7. Code Quality**
- Inconsistent error handling patterns
- Magic numbers throughout code
- Limited input validation on file paths

---

## 🚀 Short-Term Improvements (1-2 Months)

### Priority 1: Stability Fixes

**1.1 Fix Critical Bugs**
- [ ] Remove infinite loops in media_info.cpp
- [ ] Add proper null checking with graceful fallbacks
- [ ] Implement proper error recovery in cleanup functions
- [ ] Fix exit crash by ensuring all threads join properly

**1.2 Enable Video Seeking**
- [ ] Uncomment and test video_player_seek()
- [ ] Add seek progress indicator to HUD
- [ ] Implement keyframe-aware seeking for better performance
- [ ] Add audio synchronization after video seek

**1.3 Memory Management Improvements**
- [ ] Audit all malloc/free and new/delete pairs
- [ ] Add RAII wrappers for FFmpeg structures
- [ ] Implement proper cleanup in destructors
- [ ] Add memory usage logging

### Priority 2: User Experience

**2.1 Better Error Messages**
- [ ] Create user-friendly error dialog system
- [ ] Show specific codec/format errors to users
- [ ] Add "Continue" option when non-critical errors occur
- [ ] Log errors to SD card for debugging

**2.2 Progress Indicators**
- [ ] Add loading spinner for file opening
- [ ] Show buffering status during playback
- [ ] Display seek progress bar
- [ ] Add thumbnail generation for video files

**2.3 UI Polish**
- [ ] Improve file browser (icons, sorting, search)
- [ ] Add recently played list
- [ ] Remember last playback position
- [ ] Add keyboard shortcuts reference screen

---

## 🎯 Medium-Term Features (3-6 Months)

### Feature Set 1: Playback Controls

**1.1 Video Playback Enhancement**
```cpp
// Proposed API additions:
void video_player_set_playback_speed(float speed); // 0.5x, 1x, 2x
void video_player_jump_to_timestamp(int64_t timestamp_ms);
void video_player_frame_step(int frames); // Frame-by-frame navigation
bool video_player_screenshot(const char* output_path);
```

**1.2 Audio Enhancement**
```cpp
// Proposed API additions:
void audio_player_set_volume(float volume); // 0.0 to 1.0
void audio_player_set_equalizer(float* bands, int count);
void audio_player_enable_visualization(viz_type type);
```

**1.3 Playlist Support**
```cpp
// New module: src/utils/playlist.cpp
struct playlist {
    std::vector<std::string> files;
    int current_index;
    bool shuffle;
    repeat_mode mode; // NONE, ONE, ALL
};

void playlist_load_m3u(const char* path);
void playlist_next();
void playlist_prev();
```

### Feature Set 2: Network Features

**2.1 DLNA/UPnP Support**
- Stream from network media servers
- Auto-discover DLNA devices on local network
- Support for transcoding if needed

**2.2 Jellyfin Integration**
- Direct API integration
- Authentication support
- Library browsing
- Continue watching

**2.3 YouTube/Invidious**
- Search and playback
- Resolution selection
- Playlist support

### Feature Set 3: Storage Expansion

**3.1 USB Drive Support**
- Mount ext4/exFAT filesystems
- Auto-detect and mount USB drives
- Support for multiple drives
- Safe eject functionality

**3.2 Network Shares**
- SMB/CIFS support
- NFS support
- Credentials storage (encrypted)

---

## 🌟 Long-Term Goals (6-12 Months)

### Advanced Features

**1. Hardware Acceleration Expansion**
```cpp
// Expand hardware decoder support:
- H.265/HEVC full support
- VP9 hardware decode (if available)
- AV1 software decode optimization
```

**2. Multi-Profile Support**
```cpp
// User profiles with individual:
- Playback history
- Bookmarks
- Preferences
- Parental controls
```

**3. Advanced Subtitle Support**
```cpp
// Enhanced subtitle system:
- ASS/SSA format support
- Embedded subtitle extraction
- Multiple subtitle tracks
- Subtitle synchronization adjustment
- Custom subtitle styling
```

**4. Smart Features**
```cpp
// AI-enhanced features:
- Automatic chapter detection
- Scene thumbnail extraction
- Metadata fetching (TheTVDB, TMDB)
- Content categorization
```

### Platform Enhancements

**5. Input Method Expansion**
- Wii Remote support
- Pro Controller support
- USB keyboard support
- Gesture controls

**6. Multi-Display Support**
- TV + GamePad independent content
- Picture-in-picture mode
- Second screen UI controls

---

## 🔨 Technical Debt

### Refactoring Priorities

**1. Modernize C++ Code**
```cpp
// Replace raw pointers with smart pointers:
std::unique_ptr<AVFormatContext, AVFormatContextDeleter> fmt_ctx;
std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_ctx;

// Add RAII wrappers:
class FFmpegContext {
    // Automatic cleanup
};
```

**2. Improve Error Handling**
```cpp
// Create custom error types:
enum class MediaError {
    FileNotFound,
    UnsupportedCodec,
    DecoderFailure,
    OutOfMemory,
    // ...
};

// Result type for better error propagation:
template<typename T>
using Result = std::variant<T, MediaError>;
```

**3. Configuration System**
```cpp
// Replace hardcoded values with config:
class AppConfig {
public:
    int max_audio_buffer_size = 8 * 1024 * 1024;
    int video_buffer_frames = 12;
    bool hardware_decode_enabled = true;
    // ...
    
    void load_from_file(const char* path);
    void save_to_file(const char* path);
};
```

**4. Logging System Enhancement**
```cpp
// Add log levels and filtering:
class Logger {
public:
    enum Level { DEBUG, INFO, WARNING, ERROR };
    void set_level(Level min_level);
    void set_output(const char* file_path);
    void enable_console_output(bool enable);
};
```

**5. Testing Infrastructure**
```cpp
// Add unit tests:
tests/
├── test_audio_player.cpp
├── test_video_player.cpp
├── test_codec_detection.cpp
└── test_playlist.cpp

// Integration tests:
- Test real media files
- Test error conditions
- Test memory leaks
- Performance benchmarks
```

---

## 📊 Implementation Phases

### Phase 1: Stabilization (Month 1-2)
✅ Fix critical bugs
✅ Improve codec support (COMPLETED)
✅ Better error handling (COMPLETED)
⏳ Enable video seeking
⏳ Memory leak fixes

### Phase 2: Enhancement (Month 3-4)
⏳ Playlist support
⏳ USB drive support
⏳ Better UI/UX
⏳ Performance optimization

### Phase 3: Network Features (Month 5-6)
⏳ DLNA support
⏳ Jellyfin integration
⏳ Network share mounting

### Phase 4: Advanced Features (Month 7-12)
⏳ Multi-profile support
⏳ Advanced subtitles
⏳ Smart features
⏳ Multi-input support

---

## 🎯 Success Metrics

### Stability
- [ ] Zero crashes during normal operation
- [ ] Graceful degradation on errors
- [ ] < 1% memory leak over 4 hours playback

### Performance
- [ ] 720p H.264 playback at 30fps consistently
- [ ] < 2 seconds file loading time
- [ ] < 100ms seek response time

### User Experience
- [ ] Support 95% of common media formats
- [ ] Clear error messages for all failure cases
- [ ] < 3 button presses to start playback

---

## 🤝 Contributing Guidelines

### Code Style
- Follow existing naming conventions
- Add detailed comments for complex logic
- Include error handling for all operations
- Write commit messages in present tense

### Testing Requirements
- Test on real hardware before submitting
- Verify no memory leaks
- Check performance impact
- Test error conditions

### Documentation
- Update README for user-facing changes
- Update this roadmap for architectural changes
- Add inline documentation for new APIs
- Include examples for complex features

---

*Last Updated: December 8, 2025*
