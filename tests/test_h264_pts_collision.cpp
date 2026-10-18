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
    return ok ? 0 : 1;
}
