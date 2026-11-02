// The sleep timer's menu, shared by the players: how long, or "at the end of this one", and turning it off.
#include <cmath>

#include "core/i18n.hpp"
#include "core/sleep_timer.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

std::string length_text(int minutes) {
    if (minutes < 60) return util::fmt(tr("%d min"), minutes);
    return util::fmt(tr("%s h"), minutes % 60 ? util::fmt("%d.%d", minutes / 60, (minutes % 60) / 6).c_str() : util::fmt("%d", minutes / 60).c_str());
}

}  // namespace

std::string sleep_timer_label() {
    if (!sleep_timer::active()) return tr("Sleep timer");
    if (sleep_timer::at_end_of_item()) return tr("Sleep at the end");
    return util::fmt(tr("Sleep in %d min"), std::max(1, (int)std::ceil(sleep_timer::remaining_seconds() / 60.0)));
}

void show_sleep_timer_menu() {
    using ui::toast;
    std::vector<MenuItem> items;
    const int chosen = sleep_timer::chosen_minutes();
    auto radio = [chosen](int m) { return chosen == m ? ic::RADIO_CHECKED : ic::RADIO_UNCHECKED; };
    if (sleep_timer::active()) items.push_back({tr("Turn off the timer"), ic::CLOSE, [] { sleep_timer::cancel(); toast(tr("Sleep timer off"), ic::SCHEDULE); }, ""});
    for (int m : {15, 30, 45, 60, 90, 120})
        items.push_back({length_text(m), radio(m), [m] {
                             sleep_timer::start(m);
                             store::set_int("sleep_last", m);
                             toast(util::fmt(tr("Sleep timer: %s"), length_text(m).c_str()), ic::SCHEDULE);
                         }, ""});
    items.push_back({tr("At the end of this one"), radio(sleep_timer::END_OF_ITEM), [] {
                         sleep_timer::start(sleep_timer::END_OF_ITEM);
                         toast(tr("Sleep timer: at the end of this one"), ic::SCHEDULE);
                     }, tr("Plays to the end, then stops")});
    show_menu(tr("Sleep timer"), tr("The sound fades out and it stops"), std::move(items));
}

}  // namespace screens
