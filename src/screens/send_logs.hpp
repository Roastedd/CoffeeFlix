// Settings > Send logs (send_logs_screen.cpp, app/bug_report).
#pragma once

#include <memory>

#include "app/app.hpp"

namespace screens {

std::unique_ptr<app::Screen> make_send_logs();

}  // namespace screens
