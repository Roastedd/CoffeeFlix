#include "player/h264_packet.hpp"
#include <cassert>
#include <iostream>
#include <vector>
int main(){
 using player::complete_h264_packet;
 const std::vector<uint8_t> avc={0,0,0,3,0x65,0x80,0x55,0,0,0,2,0x06,0x80};
 assert(complete_h264_packet(avc.data(),avc.size(),4));
 for(size_t n=1;n<avc.size();++n)if(n!=7)assert(!complete_h264_packet(avc.data(),n,4));
 const uint8_t zero[]={0,0,0,0};assert(!complete_h264_packet(zero,4,4));
 const uint8_t huge[]={0xff,0xff,0xff,0xff,0x65,0x80};assert(!complete_h264_packet(huge,6,4));
 const uint8_t forbidden[]={2,0xe5,0x80};assert(!complete_h264_packet(forbidden,3,1));
 const uint8_t good[]={2,0x65,0x80};assert(complete_h264_packet(good,3,1));
 const uint8_t annex[]={0,0,0,1,0x67,0x80,0,0,1,0x65,0x80,0,0};assert(complete_h264_packet(annex,sizeof(annex),0));
 const uint8_t empty[]={0,0,1,0,0,1,0x65,0x80};assert(!complete_h264_packet(empty,sizeof(empty),0));
 const uint8_t junk[]={0x65,0x80};assert(!complete_h264_packet(junk,2,0));
 const uint8_t end_avc[]={1,0x0a,1,0x0b};assert(complete_h264_packet(end_avc,sizeof(end_avc),1));
 const uint8_t end_annex[]={0,0,1,0x0a,0,0,1,0x0b};assert(complete_h264_packet(end_annex,sizeof(end_annex),0));
 const uint8_t slice_header[]={1,0x65};assert(!complete_h264_packet(slice_header,sizeof(slice_header),1));
 assert(!complete_h264_packet(nullptr,0,4));
 std::cout<<"PASS complete AVC/Annex B packets, truncation, lengths, forbidden headers and empty units"<<std::endl;
}
