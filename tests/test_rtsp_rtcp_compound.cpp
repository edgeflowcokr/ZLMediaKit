#include <iostream>
#include <cstring>
#include "Rtcp/RtcpContext.h"
#include "Rtsp/RtspInputProgress.h"
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
    RtspInputProgress progress;progress.reset(1000);
    expect(progress.tcp_bytes==0&&progress.accepted_rtp==0&&progress.sorted_rtp==0,"input_boundary_starts_with_empty_generation");
    progress.onTcp(123,1100);progress.onTcp(77,1200);
    expect(progress.tcp_reads==2&&progress.tcp_bytes==200&&progress.tcp_at==1200,"input_boundary_counts_tcp_not_media_assumptions");
    progress.onFramedRtp(1250);
    expect(progress.framed_rtp==1&&progress.accepted_rtp==0&&progress.framed_at==1250,"input_boundary_distinguishes_framed_rtp_from_accepted");
    progress.onAcceptedRtp(1300);
    expect(progress.accepted_rtp==1&&progress.sorted_rtp==0&&progress.rtp_at==1300,"input_boundary_distinguishes_accepted_from_sorted");
    progress.onSortedRtp(1400);
    expect(progress.sorted_rtp==1&&progress.sorted_at==1400,"input_boundary_records_sorted_progress");
    expect(RtspInputProgress::age(1500,progress.tcp_at)==300&&RtspInputProgress::age(1500,progress.sorted_at)==100,"input_boundary_ages_use_same_monotonic_clock");
    expect(RtspInputProgress::age(10,20)==0,"input_boundary_age_never_wraps");
    progress.reset(2000);
    expect(progress.tcp_reads==0&&progress.tcp_bytes==0&&progress.framed_rtp==0&&progress.accepted_rtp==0&&progress.sorted_rtp==0&&progress.tcp_at==2000&&progress.framed_at==2000&&progress.rtp_at==2000&&progress.sorted_at==2000,"input_boundary_reconnect_resets_all_counts_and_ages");
    expect(safeRtspDiagnosticStream("main/ch-test_1")=="main/ch-test_1","input_boundary_allows_only_safe_stream_identity");
    expect(safeRtspDiagnosticStream("rtsp://user:secret@host/live")=="unclassified"&&safeRtspDiagnosticStream("main/ch-test\nsecret")=="unclassified","input_boundary_rejects_urls_and_log_injection");
    expect(safeRtspDiagnosticStream(std::string(161,'a'))=="unclassified","input_boundary_caps_identity_length");
    expect(safeRtspDiagnosticRoute("upstream")=="upstream"&&safeRtspDiagnosticRoute("shared-mirror")=="shared-mirror","input_boundary_separates_upstream_from_local_mirror");
    expect(safeRtspDiagnosticRoute("http://user:secret@host")=="unclassified","input_boundary_route_is_an_allowlist");
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
