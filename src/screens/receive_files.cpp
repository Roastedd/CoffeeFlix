#include "core/media_transfer.hpp"
#include "core/qr.hpp"
#include "core/i18n.hpp"
#include "core/util.hpp"
#include "platform/platform.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
namespace screens {
namespace {
using namespace ui;
class ReceiveFilesScreen : public app::Screen {
public:
    ReceiveFilesScreen() : folder_(util::join_path(platform::media_root(), "Received")), server_(folder_), code_(qr::encode(server_.url())) {}
    bool prevents_sleep() const override { return !server_.url().empty(); }
    app::Section section() const override { return app::SEC_MEDIA; }
    void frame() override {
        const auto& t=theme();const float x=content_x();const Id g=id("receive_files");auto status=server_.status();
        section_title(x,52,tr("Receive files"),tr("Send videos, music and photos from your phone or computer."));
        Rect left(x,154,640,385),right(x+665,154,W-x-725,385);
        gfx::fill_rrect(left,22,t.surface);gfx::fill_rrect(right,22,t.surface);
        text::icon(ic::WIFI,38,left.x+40,left.y+40,t.accent);
        text::draw(font::title,left.x+75,left.y+22,tr(status.active?N_("Receiving"):N_("Ready to receive")),t.text);
        if(!status.active)text::draw_wrapped(font::body,Rect(left.x+28,left.y+85,left.w-56,80),tr("Open the address or scan the code on a device connected to the same Wi-Fi."),t.text2,3);
        if(status.active){
            // Which file of how many, where it goes, and whether it continues one an earlier attempt kept.
            std::string where;
            const auto add=[&](const std::string& part){if(!where.empty())where+=" · ";where+=part;};
            if(status.queue_count>1)add(util::fmt(tr("File %d of %d"),status.queue_index,status.queue_count));
            if(!status.folder.empty())add(status.folder);
            if(status.resumed_from>0)add(util::fmt(tr("Resuming from %s"),util::format_bytes(status.resumed_from).c_str()));
            if(!where.empty())text::draw_wrapped(font::body,Rect(left.x+28,left.y+85,left.w-56,80),where,t.text2,3);
            text::draw_fit(font::body_bold,left.x+28,left.y+181,left.w-56,status.filename,t.text);
            progress_bar(Rect(left.x+28,left.y+226,left.w-56,8),status.total?(float)((double)status.received/status.total):0);
            const auto progress=util::format_bytes(status.received)+" / "+util::format_bytes(status.total);
            text::draw(font::small,left.x+28,left.y+250,progress,t.text2);
            if(status.bytes_per_second>0){
                auto pace=util::format_bytes((uint64_t)status.bytes_per_second)+"/s";
                if(status.seconds_left>=0)pace+=" · "+util::fmt(tr("%s left"),util::format_duration(status.seconds_left).c_str());
                text::draw_fit(font::small,left.r()-28,left.y+250,left.w-56-text::measure(font::small,progress)-20,pace,t.text2,text::RIGHT);
            }
            if(button(id(g,"cancel"),Rect(left.x+28,left.y+295,240,48),tr("Cancel transfer"),ic::CLOSE,BTN_NORMAL,g))server_.cancel();
        }else{
            std::string message=status.error.empty()?(status.completed?util::fmt(tr("%d files received"),status.completed):tr("Choose files on your phone or computer to begin.")):status.error;
            text::draw_wrapped(font::body,Rect(left.x+28,left.y+181,left.w-56,80),message,status.error.empty()?t.text2:t.warn,3);
            if(status.interrupted)text::draw_wrapped(font::small,Rect(left.x+28,left.y+225,left.w-56,60),tr("What arrived is kept. Send the file again to continue where it stopped."),t.text3,2);
            else if(!status.saved_name.empty())text::draw_fit(font::small,left.x+28,left.y+265,left.w-56,status.saved_name,t.accent);
        }
        if(status.free_bytes>=0)text::draw_fit(font::small,left.x+28,left.b()-36,left.w-56,util::fmt(tr("%s free on SD card"),util::format_bytes(status.free_bytes).c_str()),t.text3);
        if(!server_.url().empty()){
            const float q=std::min(210.0f,right.w-40);qr_code(code_,right.cx()-q/2,right.y+33,q);
            text::draw_wrapped(font::small,Rect(right.x+20,right.y+q+57,right.w-40,75),tr("Keep this screen open while sending files."),t.text2,3);
            if(button(id(g,"address"),Rect(x,553,W-x-60,46),server_.url().c_str(),ic::LINK,BTN_NORMAL,g))
                show_menu(tr("Receive files"),server_.url(),{{tr("Back"),ic::ARROW_BACK,[]{}}});
        }
        if(!status.active && button(id(g,"received"),Rect(x,610,360,46),tr("Received files"),ic::FOLDER,BTN_PRIMARY,g,F_DEFAULT)){
            if(!status.active){auto folder=folder_;app::pop();app::push(make_media_folder(folder,tr("Received files")));return;}
        }
        text::draw_fit(font::small,x+380,622,W-x-440,tr("Up to 2 GB per file. H.264 video works best."),t.text3);
        hint_bar({{"A",tr("Select")},{"B",tr("Stop receiving")}});
    }
private:
    std::string folder_;media_transfer::Server server_;qr::Code code_;
};
}
std::unique_ptr<app::Screen> make_receive_files(){return std::make_unique<ReceiveFilesScreen>();}
}
