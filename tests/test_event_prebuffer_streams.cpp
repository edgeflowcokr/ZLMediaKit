// EdgeFlowAI: pre-event 프리버퍼 링 범위 시험([record]eventPreBufferStreams · rtp_proxy.gop_cache).
//   - 접두사 목록에 든 스트림(예 main/)만 gop_cache 개수만큼 GOP 를 쥔다. 나머지(보조 스트림 등)는 1 GOP.
//   - 링을 줄여도 이벤트 클립 앞부분(startRecord back_ms=5000)에 pre-event 5초가 들어간다(키프레임 간격 1초·0.5초).
//   - 접두사가 비면 상위 동작 그대로(모든 스트림이 긴 링).
//   실행: ./test_event_prebuffer_streams → 각 줄 result=PASS|FAIL, 실패가 있으면 종료 코드 1.
#if !defined(_WIN32) // 이벤트 클립을 절대 경로(/tmp)에 쓰는 startRecord 경로·mkdtemp 를 쓰므로 리눅스 전용이다.
#include "Common/config.h"
#include "Common/MultiMediaSourceMuxer.h"
#include "Poller/EventPoller.h"
#include "Util/NoticeCenter.h"
#include "Util/base64.h"
#include "Util/File.h"
#include "ext-codec/H264.h"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>

using namespace mediakit;
using namespace toolkit;

static int failures = 0;
static void check(bool ok, const std::string &name) {
    std::cout << name << " result=" << (ok ? "PASS" : "FAIL") << '\n';
    failures += !ok;
}

static Frame::Ptr nal(uint64_t stamp, bool key) {
    auto f = FrameImp::create<H264Frame>();
    f->_prefix_size = 4;
    f->_buffer.assign("\0\0\0\1", 4);
    f->_buffer.push_back(key ? '\x65' : '\x61');
    for (int i = 0; i < 64; ++i) f->_buffer.push_back('\x80');
    f->_dts = f->_pts = stamp;
    return f;
}

static Track::Ptr video() {
    auto t = std::make_shared<H264Track>(decodeBase64("Z00AM4qKUDwBE/Kg"), decodeBase64("aO48gA=="), 0, 0);
    t->setIndex(0);
    return t;
}

static void setConfig(size_t gop_cache, const std::string &streams) {
    mINI::Instance()[RtpProxy::kGopCache] = gop_cache;
    mINI::Instance()[Record::kEventPreBufferStreams] = streams;
    NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
}

struct Result {
    size_t cached_gops = 0;
    size_t cached_frames = 0;
    uint64_t history_ms = 0;
};

// 25fps 로 seconds 초 동안 키프레임 간격 약 gop_ms(프레임 단위로 내림) 의 영상을 넣고, 링에 남은 GOP 수와 back_ms 클립의 실제 이력 길이를 잰다.
static Result run(const std::string &stream, int gop_ms, int seconds, int back_ms, const std::string &clip_dir) {
    Result out;
    auto poller = EventPollerPool::Instance().getPoller();
    poller->sync([&] {
        ProtocolOption option;
        option.modify_stamp = ProtocolOption::kModifyStampOff;
        option.paced_sender_ms = 0;
        option.enable_hls = option.enable_hls_fmp4 = option.enable_mp4 = false;
        option.enable_rtsp = option.enable_rtmp = option.enable_ts = option.enable_fmp4 = false;
        option.mp4_save_path = clip_dir;
        auto mux = std::make_shared<MultiMediaSourceMuxer>(MediaTuple{"__defaultVhost__", "live", stream, ""}, 0.0f, option);
        mux->addTrack(video());
        mux->addTrackCompleted();
        const int frame_ms = 40;
        const int frames_per_gop = gop_ms / frame_ms > 0 ? gop_ms / frame_ms : 1; // 1000 → 25장(1초), 500 → 12장(0.48초)
        for (int i = 0, t = 0; t <= seconds * 1000; ++i, t += frame_ms) {
            mux->inputFrame(nal(t, i % frames_per_gop == 0));
        }
        auto reader = mux->getFrameReader();
        reader->setReadCB([&](const Frame::Ptr &frame) {
            ++out.cached_frames;
            if (frame->keyFrame()) ++out.cached_gops;
        });
        reader->setReadCB(nullptr);
        if (back_ms > 0) {
            auto name = "clip-" + std::to_string(getCurrentMillisecond()) + "-" + std::to_string(gop_ms) + ".mp4";
            mux->startRecord(clip_dir + "/" + name, back_ms, 0, &out.history_ms);
        }
    });
    return out;
}

int main() {
    EventPollerPool::setPoolSize(1); // 먹서 소유 poller 와 시험이 sync 하는 poller 를 같게 한다(getFrameReader 가 소유 스레드를 확인).
    // 1) 접두사 해석
    check(MultiMediaSourceMuxer::eventPreBufferWanted("main/ch-1", "main/"), "prefix_main_matches");
    check(!MultiMediaSourceMuxer::eventPreBufferWanted("sub/ch-1", "main/"), "prefix_sub_excluded");
    check(MultiMediaSourceMuxer::eventPreBufferWanted("sub/ch-1", ""), "prefix_empty_means_all");
    check(MultiMediaSourceMuxer::eventPreBufferWanted("sub/ch-1", " , ,"), "prefix_only_separators_means_all");
    check(MultiMediaSourceMuxer::eventPreBufferWanted("sub/ch-1", " main/ , sub/ "), "prefix_list_trims_spaces");
    check(!MultiMediaSourceMuxer::eventPreBufferWanted("main", "main/"), "prefix_requires_full_prefix");

    char tmpl[] = "/tmp/edgeflow-prebuffer-XXXXXX";
    std::string clip_dir = mkdtemp(tmpl) ? tmpl : "/tmp";

    // 2) 백엔드 렌더 값: pre 5초 → gop_cache = 2 x 5 + 2 = 12, 긴 링은 main/ 만
    setConfig(12, "main/");
    auto main1s = run("main/ch-a", 1000, 30, 5000, clip_dir);
    check(main1s.cached_gops == 12, "main_gop1s_ring_holds_12_gops(got=" + std::to_string(main1s.cached_gops) + ")");
    check(main1s.history_ms >= 5000, "main_gop1s_clip_has_5s_pre_event(history_ms=" + std::to_string(main1s.history_ms) + ")");
    auto main05 = run("main/ch-b", 500, 30, 5000, clip_dir);
    check(main05.history_ms >= 5000, "main_gop0.48s_clip_has_5s_pre_event(history_ms=" + std::to_string(main05.history_ms) + ")");
    auto sub1s = run("sub/ch-a", 1000, 30, 0, clip_dir);
    check(sub1s.cached_gops == 1, "sub_ring_holds_1_gop(got=" + std::to_string(sub1s.cached_gops) + ")");
    check(sub1s.cached_frames <= 30, "sub_ring_holds_at_most_one_gop_of_frames(got=" + std::to_string(sub1s.cached_frames) + ")");

    // 3) 예전 렌더 값(gop_cache=60, 접두사 없음) — 상위 동작 그대로: 보조도 긴 링
    setConfig(60, "");
    auto legacySub = run("sub/ch-c", 1000, 70, 0, clip_dir);
    check(legacySub.cached_gops == 60, "empty_prefix_sub_keeps_gop_cache(got=" + std::to_string(legacySub.cached_gops) + ")");

    // 이벤트 클립 파일 정리(비동기 작업자가 마감할 시간을 준다)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    File::delete_file(clip_dir, true);
    std::cout << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}
#else
#include <iostream>
int main() {
    std::cout << "test_event_prebuffer_streams skipped on Windows" << std::endl;
    return 0;
}
#endif
