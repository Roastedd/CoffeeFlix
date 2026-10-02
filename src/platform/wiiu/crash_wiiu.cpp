// A crash on the Wii U (a bad memory access, a jump to nowhere, an illegal instruction) ends the app
// with the last picture on the screen and no word of why. The system calls a function of ours first,
// on the thread that crashed: it copies down what it can see (where, which thread, the call stack,
// the registers) into memory, writes a line to the card at once, and holds the crashing thread for
// a few seconds while another thread puts the details into the log and sends them. Then the
// system carries on as it would have. A stall of the main loop that isn't a crash gets a line in
// the log too, from the same watching thread.
#include "crash_wiiu.hpp"

#include <coreinit/context.h>
#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "app/crash_report.hpp"
#include "app/dev_log.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace platform::crash {

namespace {

constexpr int TRACE = 20;

// What the crash left, filled in by the exception handler (no allocating, no locks there).
struct Dump {
    uint32_t type, core, pc, lr, sp, dar, dsisr;
    uint32_t gpr[32];
    uint32_t trace[TRACE];
    int traces;
    char thread[40];
};
Dump g_dump;
char g_raw[1400];
std::atomic<int> g_state{0};  // 0 nothing yet, 3 being filled in, 1 ready to be logged, 2 logged

std::atomic<uint32_t> g_frames{0};
std::atomic<const char*> g_phase{"starting"};
std::atomic<bool> g_background{false};

OSExceptionCallbackFn g_previous[16];

const char* type_name(uint32_t type) {
    switch (type) {
    case OS_EXCEPTION_TYPE_MACHINE_CHECK: return "machine check";
    case OS_EXCEPTION_TYPE_DSI: return "DSI (a memory address that can't be read or written)";
    case OS_EXCEPTION_TYPE_ISI: return "ISI (a jump to an address that can't be run)";
    case OS_EXCEPTION_TYPE_PROGRAM: return "program (an instruction that can't be run)";
    default: return "exception";
    }
}

char* put(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}

char* hex(char* p, uint32_t v) {
    for (int shift = 28; shift >= 0; shift -= 4) *p++ = "0123456789ABCDEF"[(v >> shift) & 15];
    return p;
}

// Somewhere a stack can be: the heap's part of the address space, word aligned.
bool stack_address(uint32_t a) { return a >= 0x10000000 && a < 0x50000000 && !(a & 3); }

// From the exception handler.
void fill(OSContext* c, uint32_t type) {
    Dump& d = g_dump;
    d.type = type;
    d.core = OSGetCoreId();
    d.pc = c->srr0;
    d.lr = c->lr;
    d.sp = c->gpr[1];
    d.dar = c->dar;
    d.dsisr = c->dsisr;
    for (int i = 0; i < 32; i++) d.gpr[i] = c->gpr[i];
    d.traces = 0;
    d.trace[d.traces++] = d.lr;
    uint32_t frame = d.sp;
    while (d.traces < TRACE && stack_address(frame)) {
        uint32_t next = *(const uint32_t*)frame;  // the caller's frame; its return address is 4 into it
        if (next <= frame || next - frame > 0x200000 || !stack_address(next)) break;
        d.trace[d.traces++] = *(const uint32_t*)(next + 4);
        frame = next;
    }
    OSThread* thread = OSGetCurrentThread();
    const char* name = thread ? OSGetThreadName(thread) : nullptr;
    size_t n = 0;
    if (name && (uint32_t)name >= 0x02000000 && (uint32_t)name < 0x50000000) {  // a name in memory of ours
        while (n < sizeof(d.thread) - 1 && name[n]) {
            d.thread[n] = name[n];
            n++;
        }
    }
    if (n == 0) d.thread[n++] = '?';
    d.thread[n] = 0;
}

// The same in a line or three for the card, formatted by hand.
size_t raw_text() {
    const Dump& d = g_dump;
    char* p = g_raw;
    p = put(p, "CRASH type ");
    p = hex(p, d.type);
    p = put(p, " core ");
    p = hex(p, d.core);
    p = put(p, " thread ");
    p = put(p, d.thread);
    p = put(p, " pc ");
    p = hex(p, d.pc);
    p = put(p, " lr ");
    p = hex(p, d.lr);
    p = put(p, " sp ");
    p = hex(p, d.sp);
    p = put(p, " dar ");
    p = hex(p, d.dar);
    p = put(p, " dsisr ");
    p = hex(p, d.dsisr);
    p = put(p, "\nstack");
    for (int i = 0; i < d.traces; i++) {
        *p++ = ' ';
        p = hex(p, d.trace[i]);
    }
    p = put(p, "\nregisters");
    for (int i = 0; i < 32; i++) {
        *p++ = ' ';
        p = hex(p, d.gpr[i]);
    }
    *p++ = '\n';
    return (size_t)(p - g_raw);
}

template <int Type>
BOOL on_exception(OSContext* context) {
    if (g_previous[Type] && g_previous[Type](context)) return TRUE;  // someone else dealt with it
    int idle = 0;
    if (!g_state.compare_exchange_strong(idle, 3)) return FALSE;  // one report is enough
    fill(context, Type);
    crash_report::note_raw(g_raw, raw_text());
    g_state = 1;
    // The thread that crashed waits here, so the details get out before the system ends the app.
    const OSTime until = OSGetTime() + OSSecondsToTicks(4);
    while (g_state.load() != 2 && OSGetTime() < until) {
    }
    return FALSE;
}

void hook(OSExceptionType type, OSExceptionCallbackFn callback) {
    g_previous[type] = OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, type, callback);
}

// An address and, for the system's own libraries, the name of the function it is in.
std::string where(uint32_t address) {
    char symbol[80] = "";
    OSGetSymbolName(address, symbol, sizeof(symbol));
    char text[140];
    if (symbol[0])
        std::snprintf(text, sizeof(text), "%08X (%s)", address, symbol);
    else
        std::snprintf(text, sizeof(text), "%08X", address);
    return text;
}

void report() {
    const Dump& d = g_dump;
    const bool write = d.dsisr & 0x02000000;
    if (d.type == OS_EXCEPTION_TYPE_DSI)
        log_message(LOG_ERROR, "Crash", "%s on core %u, thread \"%s\": at %s, address %08X (%s), LR %08X, SP %08X",
                    type_name(d.type), d.core, d.thread, where(d.pc).c_str(), d.dar, write ? "written" : "read", d.lr, d.sp);
    else
        log_message(LOG_ERROR, "Crash", "%s on core %u, thread \"%s\": at %s, LR %08X, SP %08X, address %08X", type_name(d.type),
                    d.core, d.thread, where(d.pc).c_str(), d.lr, d.sp, d.dar);
    for (int i = 0; i < d.traces; i += 3) {
        std::string line;
        for (int j = i; j < i + 3 && j < d.traces; j++) line += (j > i ? "  " : "") + where(d.trace[j]);
        log_message(LOG_ERROR, "Crash", "Call stack %d: %s", i, line.c_str());
    }
    for (int i = 0; i < 32; i += 8)
        log_message(LOG_ERROR, "Crash", "r%d-r%d: %08X %08X %08X %08X %08X %08X %08X %08X", i, i + 7, d.gpr[i], d.gpr[i + 1], d.gpr[i + 2],
                    d.gpr[i + 3], d.gpr[i + 4], d.gpr[i + 5], d.gpr[i + 6], d.gpr[i + 7]);
    log_message(LOG_ERROR, "Crash", "Memory: %s; the main thread was: %s", memory_summary().c_str(), g_phase.load());
}

// Waits for a crash to report, and tells when the main loop stops for seconds. Runs on a thread
// of its own with nothing shared with the rest, so that whatever stopped the others doesn't stop it.
void watch() {
    set_thread_name("crash watch");
    raise_thread_priority();
    using Clock = std::chrono::steady_clock;
    uint32_t seen = 0;
    Clock::time_point moved = Clock::now();
    int told = 0;
    for (int tick = 0;; tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (g_state.load() == 1) {
            report();
            dev_log::flush_now();
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // for the sender
            g_state = 2;
            continue;
        }
        if (tick % 5) continue;
        const uint32_t frames = g_frames.load();
        const Clock::time_point now = Clock::now();
        if (frames != seen || frames == 0) {
            seen = frames;
            moved = now;
            told = 0;
            continue;
        }
        const double stalled = std::chrono::duration<double>(now - moved).count();
        if (told < 12 && stalled >= 3.0 + 5.0 * told) {
            told++;
            const bool away = g_background.load();
            log_message(away ? LOG_OK : LOG_WARNING, "Crash", "The main loop hasn't gone round for %.0f s%s; the main thread is: %s", stalled,
                        away ? " (the app is in the background)" : "", g_phase.load());
            if (!away) dev_log::flush_now();
        }
    }
}

}  // namespace

void install() {
    hook(OS_EXCEPTION_TYPE_MACHINE_CHECK, on_exception<OS_EXCEPTION_TYPE_MACHINE_CHECK>);
    hook(OS_EXCEPTION_TYPE_DSI, on_exception<OS_EXCEPTION_TYPE_DSI>);
    hook(OS_EXCEPTION_TYPE_ISI, on_exception<OS_EXCEPTION_TYPE_ISI>);
    hook(OS_EXCEPTION_TYPE_PROGRAM, on_exception<OS_EXCEPTION_TYPE_PROGRAM>);
    std::thread(watch).detach();
}

void frame() { g_frames.fetch_add(1); }
void phase(const char* what) { g_phase.store(what); }
void background(bool in_background) { g_background.store(in_background); }

}  // namespace platform::crash
