#include "player/h264_packet.hpp"
namespace player {
namespace {
bool unit(const uint8_t* data, size_t size) {
    if (!size || (data[0] & 128)) return false;
    const int type = data[0] & 31;
    // End-of-sequence/end-of-stream NALs have no RBSP payload.
    return type > 0 && type < 24 && (size >= 2 || type == 10 || type == 11);
}
size_t start_code(const uint8_t* data,size_t size,size_t at) {
    if(at+3<=size&&data[at]==0&&data[at+1]==0&&data[at+2]==1)return 3;
    if(at+4<=size&&data[at]==0&&data[at+1]==0&&data[at+2]==0&&data[at+3]==1)return 4;
    return 0;
}
}
bool complete_h264_packet(const uint8_t* data,size_t size,int length_size) {
    if(!data||!size||length_size<0||length_size>4)return false;
    if(length_size){
        size_t at=0;int units=0;
        while(at<size){
            if(size-at<(size_t)length_size)return false;
            uint32_t bytes=0;for(int n=0;n<length_size;++n)bytes=(bytes<<8)|data[at++];
            if(bytes>size-at||!unit(data+at,bytes))return false;
            at+=bytes;++units;
        }
        return units>0;
    }
    size_t at=0;while(at<size&&!start_code(data,size,at)){if(data[at++]!=0)return false;}
    int units=0;
    while(at<size){
        const size_t prefix=start_code(data,size,at);if(!prefix)return false;
        const size_t begin=at+prefix;at=begin;
        while(at<size&&!start_code(data,size,at))++at;
        size_t end=at;while(end>begin&&data[end-1]==0)--end;
        if(!unit(data+begin,end-begin))return false;
        ++units;
    }
    return units>0;
}
}
