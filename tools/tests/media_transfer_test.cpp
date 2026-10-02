#include "core/media_transfer.hpp"
#include "core/json.hpp"
#include <cassert>
#include <iostream>
#include <sys/resource.h>
#include <csignal>
int main(int argc,char**argv){
 assert(argc==2);
 assert(media_transfer::valid_name("My video.mp4"));
 assert(media_transfer::valid_name("字幕.srt"));
 for(const char* s:{"Anime","Season 1","字幕"}) assert(media_transfer::valid_folder(s));
 for(const char* s:{"","..",".hidden","a/b","a\\b","bad:name","trailing.","trailing ","tab\t"}) assert(!media_transfer::valid_folder(s));
 assert(media_transfer::valid_upload_id("0123456789abcdef")&&!media_transfer::valid_upload_id("short")&&!media_transfer::valid_upload_id("0123456789ABCDEF")&&!media_transfer::valid_upload_id(std::string(65,'a')));
 for(const char* s:{"../bad.mp4","/bad.mp4",".hidden.mp4","file.wuhb","x.json","bad:movie.mp4","bad\\movie.mp4","bad\r.mp4"}) assert(!media_transfer::valid_name(s));
 media_transfer::Server server(argv[1]);assert(!server.url().empty());std::cout<<server.url()<<std::endl;
 struct rlimit original{};getrlimit(RLIMIT_FSIZE,&original);signal(SIGXFSZ,SIG_IGN);
 std::string command;
 while(std::getline(std::cin,command)){
  if(command=="quit")break;
  if(command=="cancel")server.cancel();
  if(command=="limit"){auto limited=original;limited.rlim_cur=131072;setrlimit(RLIMIT_FSIZE,&limited);}
  if(command=="unlimit")setrlimit(RLIMIT_FSIZE,&original);
  auto s=server.status();json::Doc d(json_pack("{s:b,s:I,s:I,s:i,s:I,s:i,s:i,s:b,s:f,s:f,s:s,s:s,s:s}","active",s.active,"received",(json_int_t)s.received,"total",(json_int_t)s.total,"completed",s.completed,
   "resumed_from",(json_int_t)s.resumed_from,"queue_index",s.queue_index,"queue_count",s.queue_count,"interrupted",s.interrupted,"speed",s.bytes_per_second,"seconds_left",s.seconds_left,
   "folder",s.folder.c_str(),"saved_name",s.saved_name.c_str(),"error",s.error.c_str()));std::cout<<json::dump(d.get())<<std::endl;
 }
}
