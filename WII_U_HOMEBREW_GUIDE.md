# Wii U Homebrew Development Guide
## From Media Apps to System Utilities

This comprehensive guide covers UI development and system architecture for Wii U homebrew. It bridges the gap between rich media apps (like CafeMP) and system utilities (like WUP Installer GX2).

### Table of Contents
1. [Development Environment](#1-development-environment)
2. [The "Utility" Architecture (State Machine)](#2-the-utility-architecture-wup-installer-style)
3. [Graphics & Rendering Strategies](#3-graphics--rendering-strategies)
4. [Input Handling](#4-input-handling-navigation)
5. [Advanced System Access (MCP & FS)](#5-advanced-system-access-mcp--fs)
6. [Threading & Async Operations](#6-threading--async-operations)
7. [Best Practices & Safety](#7-best-practices--safety)

---

### 1. Development Environment

The standard toolchain for Wii U homebrew is **devkitPro** with **wut** (Wii U Toolchain).

#### Required Tools
- **devkitPro** (Standard Toolchain Manager)
- **wut** (Wii U Toolchain - Core Libraries)
- **libwhb** (Wii U Homebrew Helper Library - handles ProcUI, logging, etc.)
- **Portlibs** (SDL2, curl, zlib, etc.)

#### Environment Setup
1.  **Install devkitPro**: Follow the instructions at [devkitPro.org](https://devkitpro.org/wiki/Getting_Started).
    *   **Windows**: Use the graphical installer.
    *   **macOS/Linux**: Use the provided scripts/packages.
2.  **Install Wii U Packages**:
    Use `dkp-pacman` (or just `pacman` on some setups) to install the Wii U group.

```bash
# Update package database
dkp-pacman -Syu

# Install Wii U development tools and libraries
dkp-pacman -S wiiu-dev

# Install common portlibs (optional but recommended)
dkp-pacman -S wiiu-sdl2 wiiu-curl wiiu-freetype wiiu-zlib
```

3.  **Environment Variables**:
    Ensure your shell knows where devkitPro is.
    ```bash
    export DEVKITPRO=/opt/devkitpro
    export DEVKITPPC=$DEVKITPRO/devkitPPC
    ```

---

### 2. The "Utility" Architecture (WUP Installer Style)

Unlike a game, a utility requires a specific architectural pattern: **The Event Loop + State Machine**.
Modern Wii U homebrew uses `libwhb` to manage the process loop (`ProcUI`).

#### The State Machine Pattern
Utilities should not be a spaghetti of code. They operate in distinct states.

```cpp
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>

enum AppState {
    STATE_MENU,
    STATE_CONFIRM_INSTALL,
    STATE_INSTALLING,
    STATE_ERROR,
    STATE_EXIT
};

AppState currentState = STATE_MENU;

void MainLoop() {
    // Initialize ProcUI and Console Logging
    WHBProcInit();
    WHBLogConsoleInit();
    
    while(WHBProcIsRunning()) {
        // 1. Read Input (using VPAD or libwhb helpers)
        updateInput();
        
        // 2. State Logic
        switch(currentState) {
            case STATE_MENU:
                // Logic for menu
                break;
            case STATE_INSTALLING:
                // Logic for install
                break;
        }
        
        // 3. Render
        WHBLogConsoleDraw(); // Draws the console text to TV and GamePad
    }
    
    // Cleanup
    WHBLogConsoleFree();
    WHBProcShutdown();
}
```

---

### 3. Graphics & Rendering Strategies

You have two main choices. `CafeMP` uses Approach A, while utilities often use Approach B (or the simpler `WHBLogConsole` shown above).

#### Approach A: SDL2 (Best for Media/Rich UI)
*   **Used by**: CafeMP, Homebrew App Store
*   **Pros**: Hardware accelerated, supports PNG/JPG, standard API.
*   **Cons**: Larger binary size, slightly more overhead.

(See `CafeMP` source for extensive SDL2 examples).

#### Approach B: OSScreen (Best for Utilities)
*   **Used by**: WUP Installer GX2, NNU-Patcher
*   **Pros**: Zero overhead, writes directly to framebuffer, crash-resistant.
*   **Cons**: Text-heavy, manual buffer management.

**Implementation (The "Blue Screen" Look):**

```cpp
#include <coreinit/screen.h>
#include <coreinit/cache.h>

void InitConsoleUI() {
    OSScreenInit();
    // Allocate buffers for TV and GamePad
    size_t tvSize = OSScreenGetBufferSizeEx(SCREEN_TV);
    size_t drcSize = OSScreenGetBufferSizeEx(SCREEN_DRC);
    
    void* tvBuf = memalign(0x100, tvSize);
    void* drcBuf = memalign(0x100, drcSize);
    
    OSScreenSetBufferEx(SCREEN_TV, tvBuf);
    OSScreenSetBufferEx(SCREEN_DRC, drcBuf);
    
    OSScreenEnableEx(SCREEN_TV, 1);
    OSScreenEnableEx(SCREEN_DRC, 1);
}

void RenderUI() {
    // 1. Clear to Dark Blue (R,G,B,A Hex)
    OSScreenClearBufferEx(SCREEN_TV, 0x00008BFF); 
    OSScreenClearBufferEx(SCREEN_DRC, 0x00008BFF);

    // 2. Draw Text (Line, Column, String)
    OSScreenPutFontEx(SCREEN_TV, 0, 0, "WUP Installer GX2 - Mod");
    OSScreenPutFontEx(SCREEN_TV, 2, 0, "Press A to Install");
    
    // 3. Draw to GamePad
    OSScreenPutFontEx(SCREEN_DRC, 0, 0, "WUP Installer GX2 - Mod");

    // 4. Flush Cache & Flip
    DCFlushRange(tvBuf, tvSize); // Important!
    DCFlushRange(drcBuf, drcSize);
    
    OSScreenFlipBuffersEx(SCREEN_TV);
    OSScreenFlipBuffersEx(SCREEN_DRC);
}
```

---

### 4. Input Handling (Navigation)

For a list-based UI, you need a robust navigation struct.

```cpp
#include <vpad/input.h>

typedef struct {
    char name[64];
    char path[256];
    bool selected;
} ListItem;

ListItem installableContent[50];
int selectedIndex = 0;
int totalItems = 0;

void HandleNavigation() {
    VPADStatus vpad;
    VPADRead(VPAD_CHAN_0, &vpad, 1, NULL); // Read GamePad input

    if (vpad.trigger & VPAD_BUTTON_DOWN) {
        if (selectedIndex < totalItems - 1) selectedIndex++;
    }
    else if (vpad.trigger & VPAD_BUTTON_UP) {
        if (selectedIndex > 0) selectedIndex--;
    }
    
    if (vpad.trigger & VPAD_BUTTON_A) {
        // Toggle selection
        installableContent[selectedIndex].selected = !installableContent[selectedIndex].selected;
    }
}
```

---

### 5. Advanced System Access (MCP & FS)

This is the "magic" behind apps like WUP Installer. They use the **Master Control Program (MCP)** to modify the system.

#### A. Mounting Drives (SD & USB)
You must explicitly mount drives if you are using lower-level APIs. `libwhb` often handles SD card mounting automatically, but for USB or specific paths:

```cpp
#include <coreinit/filesystem.h>

void MountDrives() {
    FSClient client;
    FSAddClient(&client, FS_ERROR_FLAG_NONE);
    
    // Mount SD Card
    FSMountSource(&client, NULL, FS_MOUNT_SOURCE_SD, "/vol/storage_sdcard", NULL);
    
    // Mount USB (Slot 0)
    FSMountSource(&client, NULL, FS_MOUNT_SOURCE_USB, "/vol/storage_usb", NULL);
}
```

#### B. Installing Titles (MCP)
**Warning**: This modifies the NAND/USB contents. Proceed with caution.

```cpp
#include <nn/mcp.h> 

int InstallTitle(const char* installPath) {
    int mcpHandle = MCP_Open();
    if (mcpHandle < 0) return -1;
    
    // MCP commands involve creating a command struct and passing it via IOS_Ioctl.
    // Refer to WUP Installer GX2 source code for the exact command structure 
    // as it involves undocumented system calls.
    
    MCP_Close(mcpHandle);
    return 0;
}
```

---

### 6. Threading & Async Operations

**CRITICAL RULE**: Never block the UI thread.
If you pause the UI thread for more than a few seconds, the OS may think the app has crashed.

#### The "Worker Thread" Pattern

```cpp
#include <coreinit/thread.h>
#include <coreinit/time.h>

OSThread workerThread;
uint8_t stack[0x10000]; // 64KB Stack

// Shared variable (Atomic/Volatile)
volatile int progressPercent = 0;
volatile bool isWorking = false;

// The heavy lifting function
int InstallTask(int argc, const char **argv) {
    isWorking = true;
    for(int i=0; i<=100; i++) {
        // Simulate heavy work
        OSSleepTicks(OSSecondsToTicks(0.1)); 
        progressPercent = i;
    }
    isWorking = false;
    return 0;
}

void StartInstall() {
    OSCreateThread(&workerThread, InstallTask, 0, NULL, 
                   stack + sizeof(stack), sizeof(stack), 
                   16, // Priority (0 is highest, 16 is standard for apps)
                   OS_THREAD_ATTRIB_AFFINITY_ANY);
    OSResumeThread(&workerThread);
}
```

---

### 7. Best Practices & Safety

#### 1. The Home Button (ProcUI / libwhb)
Using `WHBProcInit()` and `WHBProcIsRunning()` automatically handles the Home Button menu and system shutdown requests properly.
If you are doing a critical operation (like installing), you should temporarily disable the Home Button:

```cpp
#include <proc_ui/procui.h>

// When starting a critical task:
ProcUISetHomeButtonEnabled(false); 

// When task finishes:
ProcUISetHomeButtonEnabled(true); 
```

#### 2. Memory Management (MEM1 vs MEM2)
*   **MEM1 (Bucket)**: ~32MB. Very fast. Used for Code, Stack, Framebuffers.
*   **MEM2 (Bucket)**: ~1GB. Slower. Used for Big Assets, Music buffers, File buffers.

When using `memalign` or `malloc`, be aware of where your memory goes. Large buffers for file copying should be in MEM2.

#### 3. Makefile Setup
Ensure you link against `libwhb` for the helper functions.

```makefile
# Standard WUT Makefile rules
include $(DEVKITPRO)/wut/share/wut_rules

TARGET      :=  MyUtility
BUILD       :=  build
SOURCES     :=  src
INCLUDES    :=  include

# Flags
CFLAGS      :=  -g -O2 -Wall -D__WIIU__ -D__WUT__
CXXFLAGS    :=  $(CFLAGS) -std=c++17

# Libraries
# -lwhb: Homebrew helper (ProcUI, Logging)
# -lmcp: Master Control Program (System mods)
# -lcoreinit: OS functions
LIBS        :=  -lwhb -lmcp -lcoreinit -lwut

# Output generation
include $(DEVKITPRO)/wut/share/wut_create_rpx
```
/****************************************************************************
 * Copyright (C) 2015 Dimok
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/
#ifndef VPAD_CONTROLLER_H_
#define VPAD_CONTROLLER_H_

#include <vpad/input.h>
#include "GuiController.h"

class VPadController : public GuiController
{
public:
    //!Constructor
    VPadController(int channel)
        : GuiController(channel)
    {
        memset(&vpad, 0, sizeof(vpad));
    }

    //!Destructor
    virtual ~VPadController()  {}

    bool update(int width, int height)
    {
        lastData = data;

        VPADReadError vpadError = VPAD_READ_NO_SAMPLES;
        VPADChan channel = VPAD_CHAN_0;
        VPADRead(channel, &vpad, 1, &vpadError);

        if(vpadError == VPAD_READ_SUCCESS)
        {
            data.buttons_r = vpad.release;
            data.buttons_h = vpad.hold;
            data.buttons_d = vpad.trigger;
            data.validPointer = !vpad.tpNormal.validity;
            data.touched = vpad.tpNormal.touched;
            //! calculate the screen offsets
            data.x = -(width >> 1) + (int)((vpad.tpFiltered1.x * width) >> 12);
            data.y = (height >> 1) - (int)(height - ((vpad.tpFiltered1.y * height) >> 12));
            return true;
        }
        return false;
    }

private:
    VPADStatus vpad;
};

#endif