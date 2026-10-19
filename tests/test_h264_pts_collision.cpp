#include <iostream>
#include <vector>
#include "ext-codec/H264Rtp.h"
#include "Common/PacketCache.h"

using namespace mediakit;
using namespace toolkit;

struct Capture : PacketCache<RtpPacket>, RingDelegate<RtpPacket::Ptr> {
    std::vector<uint32_t> markers;
    int video_nals = 0;
    H264RtpDecoder decoder;
    Capture() {
        decoder.addDelegate([this](const Frame::Ptr &f) {
            auto type = H264_TYPE(f->data()[f->prefixSize()]);
            if (type == 1 || type == 5) ++video_nals;
            return true;
        });
    }
    void onWrite(RtpPacket::Ptr p, bool key) override {
        p->ntp_stamp = p->getStampMS(false);
        inputPacket(p->getStampMS(), true, p, key);
    }
    void onFlush(std::shared_ptr<List<RtpPacket::Ptr>> packets, bool) override {
        packets->for_each([this](const RtpPacket::Ptr &p) {
            if (p->getHeader()->mark) markers.push_back(p->getStamp());
            decoder.inputRtp(p);
        });
    }
};

Frame::Ptr nal(int type, uint64_t pts, bool first = true) {
    auto f = FrameImp::create<H264Frame>();
    f->_prefix_size = 4;
    f->_buffer.assign("\0\0\0\1", 4);
    f->_buffer.push_back(static_cast<char>(type | 0x60));
    f->_buffer.push_back(static_cast<char>(first ? 0x80 : 0x40));
    f->_pts = f->_dts = pts;
    return f;
}

bool run(const char *name, const std::vector<uint64_t> &pts, bool multi_slice,
         bool preserve_reorder = false, bool low_latency = false, bool config_collision = false) {
    mINI::Instance()[Rtp::kLowLatency] = low_latency ? 1 : 0;
    // GET_CONFIG caches values until the production reload notification.
    NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    H264RtpEncoder encoder;
    encoder.setRtpInfo(1, 1400, 90000, 96);
    auto capture = std::make_shared<Capture>();
    auto ring = std::make_shared<RtpRing::RingType>();
    ring->setDelegate(capture);
    encoder.setRtpRing(ring);
    encoder.inputFrame(nal(7, pts.front()));
    encoder.inputFrame(nal(8, pts.front()));
    for (size_t i = 0; i < pts.size(); ++i) {
        if (config_collision && i) {
            encoder.inputFrame(nal(7, pts[i]));
            encoder.inputFrame(nal(8, pts[i]));
        }
        encoder.inputFrame(nal(i && !config_collision ? 1 : 5, pts[i]));
        if (multi_slice) encoder.inputFrame(nal(i ? 1 : 5, pts[i], false));
    }
    if (low_latency) capture->flush();
    const bool immediate_ok = !low_latency || capture->markers.size() == pts.size();
    encoder.flush();
    capture->flush();
    bool stamps_ok = capture->markers.size() == pts.size();
    for (size_t i = 0; i < capture->markers.size(); ++i) {
        if (preserve_reorder || low_latency) {
            stamps_ok = stamps_ok && capture->markers[i] == pts[i] * 90;
        } else if (i) {
            stamps_ok = stamps_ok && capture->markers[i] > capture->markers[i-1];
        }
    }
    const bool ok = immediate_ok && stamps_ok && capture->video_nals == pts.size() * (multi_slice ? 2 : 1);
    std::cout << name << " pictures=" << pts.size()
        << " video_nals=" << capture->video_nals
        << " access_unit_markers=" << capture->markers.size()
        << " stamps_ok=" << stamps_ok << " result=" << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}

static bool malformedPayloads() {
    bool ok=true;
    auto check=[&](bool pass,const char *name){std::cout<<name<<" result="<<(pass?"PASS":"FAIL")<<'\n';ok=ok&&pass;};
    {
        H264RtpDecoder decoder; RtpInfo info(7,1400,90000,96,0,0);int pictures=0;
        decoder.addDelegate([&](const Frame::Ptr &f){if(!f->configFrame())++pictures;return true;});
        const char start[]={0x7c,char(0x85),char(0x80)};
        decoder.inputRtp(info.makeRtp(TrackVideo,start,sizeof(start),false,1000));
        // 선언된 RTP payload는1바이트지만, backing allocation의 다음 바이트는
        // 0으로 고정한다. 기존 코드의 음수 append길이를 안전하게 재현한다.
        const char short_fu[]={0x7c,0};
        auto bad=info.makeRtp(TrackVideo,short_fu,sizeof(short_fu),false,1000);
        bad->setSize(bad->size()-1); --bad->data()[3];
        bool threw=false;try{decoder.inputRtp(bad);}catch(const std::exception &){threw=true;}
        check(!threw&&pictures==0,"truncated_fua_does_not_throw_or_emit_partial_picture");
        const char p[]={0x61,char(0x80)},key[]={0x65,char(0x80)};
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1040));
        check(pictures==0,"truncated_fua_waits_for_a_complete_new_gop");
        decoder.inputRtp(info.makeRtp(TrackVideo,key,sizeof(key),true,1080));
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1120));
        check(pictures==2,"truncated_fua_recovers_on_idr_without_reconnect");
    }
    {
        H264RtpDecoder decoder;RtpInfo info(8,1400,90000,96,0,0);int pictures=0;
        decoder.addDelegate([&](const Frame::Ptr &f){if(!f->configFrame())++pictures;return true;});
        const char short_stap[]={0x78,0,3,0x65,char(0x80),0};
        auto bad=info.makeRtp(TrackVideo,short_stap,sizeof(short_stap),true,1000);
        bad->setSize(bad->size()-1); --bad->data()[3];
        bool threw=false;try{decoder.inputRtp(bad);}catch(const std::exception &){threw=true;}
        check(!threw&&pictures==0,"truncated_stapa_length_does_not_read_past_payload");
        const char key[]={0x65,char(0x80)};
        decoder.inputRtp(info.makeRtp(TrackVideo,key,sizeof(key),true,1040));
        check(pictures==1,"truncated_stapa_recovers_on_next_idr");
    }
    {
        H264RtpDecoder decoder;RtpInfo info(9,1400,90000,96,0,0);int pictures=0;
        decoder.addDelegate([&](const Frame::Ptr &f){if(!f->configFrame())++pictures;return true;});
        const char start[]={0x7c,char(0x85),char(0x80)},empty[]={0x7c,5},end[]={0x7c,0x45,0};
        decoder.inputRtp(info.makeRtp(TrackVideo,start,sizeof(start),false,1000));
        decoder.inputRtp(info.makeRtp(TrackVideo,empty,sizeof(empty),false,1000));
        decoder.inputRtp(info.makeRtp(TrackVideo,end,sizeof(end),true,1000));
        check(pictures==1,"valid_empty_fua_fragment_remains_supported");
        const char stap[]={0x78,0,2,0x65,char(0x80),0,2,0x61,char(0x80)};
        decoder.inputRtp(info.makeRtp(TrackVideo,stap,sizeof(stap),true,1040));
        check(pictures==3,"valid_stapa_preserves_all_nals");
    }
    return ok;
}

static bool gapRecovery() {
    bool ok=true;
    auto check=[&](bool pass,const char *name){std::cout<<name<<" result="<<(pass?"PASS":"FAIL")<<'\n';ok=ok&&pass;};
    const char key[]={0x65,char(0x80)},p[]={0x61,char(0x80)};
    {
        H264RtpDecoder decoder;RtpInfo info(21,1400,90000,96,0,0);int pictures=0;
        decoder.addDelegate([&](const Frame::Ptr &f){if(!f->configFrame())++pictures;return true;});
        decoder.inputRtp(info.makeRtp(TrackVideo,key,sizeof(key),true,1000));
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1040));
        info.makeRtp(TrackVideo,p,sizeof(p),true,1080); // 이 진단 소유 시료만 유실시킨다.
        decoder.inputRtp(info.makeRtp(TrackVideo,key,sizeof(key),true,1120));
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1160));
        check(pictures==4,"sequence_gap_idr_recovers_before_following_p_picture");
    }
    {
        H264RtpDecoder decoder;RtpInfo info(22,1400,90000,96,0,0);int pictures=0;
        decoder.addDelegate([&](const Frame::Ptr &f){if(!f->configFrame())++pictures;return true;});
        decoder.inputRtp(info.makeRtp(TrackVideo,key,sizeof(key),true,1000));
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1040));
        info.makeRtp(TrackVideo,p,sizeof(p),true,1080);
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1120));
        check(pictures==2,"sequence_gap_rejects_dependent_picture_before_output");
        decoder.inputRtp(info.makeRtp(TrackVideo,key,sizeof(key),true,1160));
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1200));
        check(pictures==4,"sequence_gap_recovers_exactly_on_next_complete_idr");
    }
    {
        H264RtpDecoder decoder;RtpInfo info(23,1400,90000,96,0,0);int pictures=0;
        decoder.addDelegate([&](const Frame::Ptr &f){if(!f->configFrame())++pictures;return true;});
        decoder.inputRtp(info.makeRtp(TrackVideo,key,sizeof(key),true,1000));
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1040));
        info.makeRtp(TrackVideo,p,sizeof(p),true,1080);
        const char start[]={0x7c,char(0x85),char(0x80)},end[]={0x7c,0x45,0};
        decoder.inputRtp(info.makeRtp(TrackVideo,start,sizeof(start),false,1120));
        decoder.inputRtp(info.makeRtp(TrackVideo,end,sizeof(end),true,1120));
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1160));
        check(pictures==4,"sequence_gap_keeps_complete_fragmented_idr_recovery");
    }
    {
        H264RtpDecoder decoder;RtpInfo info(24,1400,90000,96,0,0);int pictures=0;
        decoder.addDelegate([&](const Frame::Ptr &f){if(!f->configFrame())++pictures;return true;});
        decoder.inputRtp(info.makeRtp(TrackVideo,key,sizeof(key),true,1000)); // 순번0
        info.makeRtp(TrackVideo,p,sizeof(p),true,1040);
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1080));
        check(pictures==1,"sequence_zero_is_valid_history_for_gap_detection");
        decoder.inputRtp(info.makeRtp(TrackVideo,key,sizeof(key),true,1120));
        decoder.inputRtp(info.makeRtp(TrackVideo,p,sizeof(p),true,1160));
        check(pictures==3,"sequence_zero_gap_recovers_without_extra_gop_wait");
    }
    {
        H264RtpDecoder decoder;RtpInfo info(25,1400,90000,96,0,0);int pictures=0;
        decoder.addDelegate([&](const Frame::Ptr &f){if(!f->configFrame())++pictures;return true;});
        auto first=info.makeRtp(TrackVideo,key,sizeof(key),true,1000);first->getHeader()->seq=htons(65535);decoder.inputRtp(first);
        auto second=info.makeRtp(TrackVideo,p,sizeof(p),true,1040);second->getHeader()->seq=htons(0);decoder.inputRtp(second);
        auto third=info.makeRtp(TrackVideo,p,sizeof(p),true,1080);third->getHeader()->seq=htons(1);decoder.inputRtp(third);
        check(pictures==3,"sequence_wrap_keeps_contiguous_pictures");
    }
    return ok;
}

int main() {
    mINI::Instance()[General::kMergeWriteMS] = 0;
    mINI::Instance()[Rtsp::kLowLatency] = 0;
    bool ok = run("distinct_pts", {1000,1040,1080,1120,1160,1200}, false);
    ok = run("same_pts_distinct_pictures", {1000,1000,1000,1040,1040,1080}, false) && ok;
    ok = run("same_pts_multislice_pictures", {1000,1000,1000,1040,1040,1080}, true) && ok;
    ok = run("collision_catch_up", {1000,1000,1001,1001,1002,1040}, false) && ok;
    ok = run("same_pts_config_changes", {1000,1000,1000,1040,1040,1080}, false, false, false, true) && ok;
    ok = run("reordered_pts_unchanged", {1000,1080,1040,1160,1120,1200}, false, true) && ok;
    ok = run("low_latency_unchanged", {1000,1040,1080,1120,1160,1200}, false, false, true) && ok;
    ok = malformedPayloads() && ok;
    ok = gapRecovery() && ok;
    return ok ? 0 : 1;
}
