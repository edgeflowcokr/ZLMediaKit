#include <iostream>
#include <cstring>
#include "Rtcp/RtcpContext.h"
#include "Util/logger.h"
using namespace mediakit;
static int failures = 0;
static void expect(bool ok, const char *name) {
    std::cout << name << " result=" << (ok ? "PASS" : "FAIL") << '\n';
    failures += !ok;
}
static uint32_t word(const char *p) {
    uint32_t n; std::memcpy(&n,p,4); return ntohl(n);
}
static size_t packetSize(const char *p) {
    return (((uint8_t)p[2] << 8) | uint8_t(p[3])) * 4 + 4;
}
int main() {
    // Construct logging before the toolkit's clock thread so its static
    // shutdown cannot race first-time logger construction in a tiny test.
    auto logger = toolkit::Logger::Instance().shared_from_this();
    std::cout.setf(std::ios::unitbuf);
    RtcpContextForRecv ctx;
    for (int n=100;n<110;++n) ctx.onRtp(n,90000+(n-100)*3000,100000,90000,1200);
    const uint32_t source=0xaabbccdd, sender=source+1;
    auto compound=ctx.createRtcpRRCompound(sender,source,"EdgeFlowAI");
    const char *p=compound->data();
    const auto rrSize=packetSize(p), sdesSize=packetSize(p+rrSize);
    expect((uint8_t(p[0])>>6)==2 && uint8_t(p[1])==201,"compound_starts_with_receiver_report");
    expect(rrSize==32 && uint8_t(p[0]&31)==1,"one_source_report_block_preserved");
    expect(word(p+4)==sender && word(p+8)==source,"receiver_and_reported_source_are_distinct");
    expect(uint8_t(p[rrSize+1])==202 && uint8_t(p[rrSize]&31)==1,"compound_contains_single_sdes_chunk");
    expect(word(p+rrSize+4)==sender,"cname_identifies_receiver_not_source");
    expect(uint8_t(p[rrSize+8])==1 && uint8_t(p[rrSize+9])==10 && std::string(p+rrSize+10,10)=="EdgeFlowAI", "cname_text_and_type_preserved");
    expect(rrSize+sdesSize==compound->size() && compound->size()%4==0,"one_datagram_contains_complete_aligned_compound");
    expect(word(p+16)==109,"received_sequence_statistics_preserved");
    auto next=ctx.createRtcpRRCompound(sender,source,"x");
    expect(packetSize(next->data())+packetSize(next->data()+packetSize(next->data()))==next->size(),"repeated_report_is_complete");
    auto maximum=ctx.createRtcpRRCompound(0,0xffffffff,std::string(255,'a'));
    const auto start=packetSize(maximum->data());
    expect(uint8_t(maximum->data()[start+9])==255 && word(maximum->data()+start+4)==0,"wrapped_sender_ssrc_and_maximum_cname_supported");
    bool empty=false,tooLong=false;
    try {ctx.createRtcpRRCompound(sender,source,"");}catch(const std::invalid_argument&){empty=true;}
    try {ctx.createRtcpRRCompound(sender,source,std::string(256,'a'));}catch(const std::invalid_argument&){tooLong=true;}
    expect(empty&&tooLong,"invalid_cname_rejected_before_report_statistics_change");
    auto legacy=ctx.createRtcpRR(sender,source);
    expect(legacy->size()==32 && uint8_t(legacy->data()[1])==201,"legacy_receiver_report_api_unchanged");
    return failures ? 1 : 0;
}
