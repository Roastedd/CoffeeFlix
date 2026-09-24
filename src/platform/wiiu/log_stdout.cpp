// Routes stdout and stderr (our log lines, FFmpeg's messages) to the Wii U system log: Aroma's
// logging module when it's loaded, otherwise Cafe OS's own log and UDP broadcast.
#include <sys/iosupport.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_module.h>
#include <whb/log_udp.h>

#include <mutex>
#include <string>

namespace {

bool g_module = false, g_cafe = false, g_udp = false;
std::mutex g_mutex;
std::string g_pending;  // text written so far without a newline
devoptab_t g_console;

ssize_t console_write(struct _reent*, void*, const char* ptr, size_t len) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < len; i++) {
        if (ptr[i] != '\n') g_pending += ptr[i];
        // Whole lines only (WHBLogPrint adds the newline); very long ones are split.
        if (ptr[i] == '\n' || g_pending.size() >= 1000) {
            WHBLogPrint(g_pending.c_str());
            g_pending.clear();
        }
    }
    return (ssize_t)len;
}

__attribute__((constructor)) void start_console_log() {
    g_module = WHBLogModuleInit();
    if (!g_module) {
        g_cafe = WHBLogCafeInit();
        g_udp = WHBLogUdpInit();
    }
    g_console.name = "whblog";
    g_console.structSize = sizeof(int);
    g_console.write_r = console_write;
    devoptab_list[STD_OUT] = &g_console;
    devoptab_list[STD_ERR] = &g_console;
}

__attribute__((destructor)) void stop_console_log() {
    if (g_module) WHBLogModuleDeinit();
    if (g_cafe) WHBLogCafeDeinit();
    if (g_udp) WHBLogUdpDeinit();
}

}  // namespace
