#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace mediakit {
// 연결 세대별 상수 크기 누계. 실패 때만 읽으며 별도 타이머/패킷 로그는 만들지 않는다.
struct RtspInputProgress {
    uint64_t tcp_reads=0,tcp_bytes=0,framed_rtp=0,accepted_rtp=0,sorted_rtp=0;
    uint64_t tcp_at=0,framed_at=0,rtp_at=0,sorted_at=0;
    void reset(uint64_t now) { *this={};tcp_at=framed_at=rtp_at=sorted_at=now; }
    void onTcp(size_t bytes,uint64_t now) { ++tcp_reads;tcp_bytes+=bytes;tcp_at=now; }
    void onFramedRtp(uint64_t now) { ++framed_rtp;framed_at=now; }
    void onAcceptedRtp(uint64_t now) { ++accepted_rtp;rtp_at=now; }
    void onSortedRtp(uint64_t now) { ++sorted_rtp;sorted_at=now; }
    static uint64_t age(uint64_t now,uint64_t then) {return now>=then?now-then:0;}
};
static constexpr const char *kRtspDiagnosticStream="edgeflow.input_stream";
static constexpr const char *kRtspDiagnosticRoute="edgeflow.input_route";
inline std::string safeRtspDiagnosticStream(const std::string &s) {
    return !s.empty()&&s.size()<=160&&s.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_-")==std::string::npos?s:"unclassified";
}
inline std::string safeRtspDiagnosticRoute(const std::string &s) {
    return s=="upstream"||s=="shared-mirror"?s:"unclassified";
}
} // namespace mediakit
