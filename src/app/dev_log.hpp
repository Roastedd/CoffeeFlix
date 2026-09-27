// Developer updates: the log goes live to the computer offering them (tools/dev-update.sh),
// which keeps each run's in logs/ there. Nothing is sent with developer updates off.
#pragma once

namespace dev_log {

void tick();      // main thread, every frame
void shutdown();  // sends what's left (briefly) and stops

}  // namespace dev_log
