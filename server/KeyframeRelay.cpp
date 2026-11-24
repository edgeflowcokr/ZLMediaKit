/*
 * EdgeFlowAI: 키프레임만 라이브 갈래 — KeyframeRelay.h 참조.
 */
#include "KeyframeRelay.h"

#include <mutex>
#include <unordered_map>

#include "Common/MediaSource.h"
#include "Common/MultiMediaSourceMuxer.h"
#include "Common/config.h"
#include "Extension/Frame.h"
#include "Extension/Track.h"
#include "Poller/EventPoller.h"
#include "Poller/Timer.h"
#include "Util/NoticeCenter.h"
#include "Util/TimeTicker.h"
#include "Util/util.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace {

const string kKeySuffix = "~key";
constexpr float kCheckSeconds = 2.0f;        // 상태 점검 주기
constexpr uint64_t kIdleCloseMS = 15 * 1000; // 시청자 0 이 이만큼 이어지면 닫는다(첫 시청자 대기 포함)
constexpr size_t kMaxRelays = 512;           // 동시 갈래 상한(채널 수 상한보다 넉넉히)

class KeyframeRelay : public MediaSourceEvent, public enable_shared_from_this<KeyframeRelay> {
public:
    using Ptr = shared_ptr<KeyframeRelay>;

    KeyframeRelay(MediaTuple tuple, const MediaSource::Ptr &origin, EventPoller::Ptr poller)
        : _tuple(std::move(tuple)), _origin(origin), _poller(std::move(poller)) {}

    // 원본 poller 에서 호출한다. 성공하면 true.
    bool start(const MediaSource::Ptr &origin) {
        Track::Ptr video;
        for (auto &track : origin->getTracks(true)) {
            if (track->getTrackType() == TrackVideo) {
                video = track;
                break;
            }
        }
        if (!video) {
            WarnL << "키프레임 갈래: 원본에 준비된 영상 트랙이 없습니다: " << _tuple.shortUrl();
            return false;
        }
        ProtocolOption option;
        option.enable_audio = false;
        option.add_mute_audio = false;
        option.auto_close = false;
        option.modify_stamp = ProtocolOption::kModifyStampOff; // I프레임 사이 1~2초 간격을 그대로 둔다(상대 보정이 간격을 접지 않게)
        option.enable_rtsp = true;  // WHEP 은 RTSP 원천을 읽는다
        option.enable_fmp4 = true;  // fMP4 폴백
        option.enable_hls = true;   // HLS 폴백(시청할 때만 생성)
        option.hls_demand = true;
        option.enable_hls_fmp4 = false;
        option.enable_rtmp = false;
        option.enable_ts = false;
        option.enable_mp4 = false;  // 녹화 안 함
        option.rtsp_demand = false;
        option.fmp4_demand = false;
        _muxer = make_shared<MultiMediaSourceMuxer>(_tuple, 0.0f, option);
        _muxer->setMediaListener(shared_from_this());
        _muxer->addTrack(video->clone());
        _muxer->addTrackCompleted();
        weak_ptr<KeyframeRelay> weak_self = shared_from_this();
        _video = video;
        _delegate = video->addDelegate([weak_self](const Frame::Ptr &frame) {
            auto self = weak_self.lock();
            if (!self || !self->_muxer) {
                return false;
            }
            if (!frame->keyFrame() && !frame->configFrame()) {
                return true; // P·B 프레임은 버린다.
            }
            ++self->_keyframes;
            return self->_muxer->inputFrame(frame);
        });
        _idle.resetTime();
        _timer = make_shared<Timer>(kCheckSeconds, [weak_self]() {
            auto self = weak_self.lock();
            return self && self->check();
        }, _poller);
        InfoL << "키프레임 갈래 시작: " << _tuple.shortUrl();
        return true;
    }

    // poller 에서 호출. 계속 점검하면 true.
    bool check() {
        if (!_muxer) {
            return false;
        }
        if (_origin.expired()) {
            InfoL << "키프레임 갈래: 원본이 사라져 닫습니다: " << _tuple.shortUrl();
            stop();
            return false;
        }
        if (_muxer->totalReaderCount() > 0) {
            _idle.resetTime();
        } else if (_idle.elapsedTime() >= kIdleCloseMS) {
            InfoL << "키프레임 갈래: 시청자가 없어 닫습니다: " << _tuple.shortUrl() << " 보낸 키프레임=" << _keyframes;
            stop();
            return false;
        }
        return true;
    }

    // poller 에서 호출.
    void stop();

    // MediaSourceEvent — 먹서가 넘겨주는 이벤트.
    MediaOriginType getOriginType(MediaSource &) const override { return MediaOriginType::pull; }
    string getOriginUrl(MediaSource &) const override { return "keyframe://" + _tuple.shortUrl(); }
    bool close(MediaSource &) override {
        weak_ptr<KeyframeRelay> weak_self = shared_from_this();
        _poller->async([weak_self]() {
            if (auto self = weak_self.lock()) {
                self->stop();
            }
        }, false);
        return true;
    }
    void onReaderChanged(MediaSource &, int size) override {
        if (size > 0) {
            _idle.resetTime(); // 닫기는 check() 가 정한다(기본 무시청 처리·훅을 쓰지 않는다).
        }
    }
    EventPoller::Ptr getOwnerPoller(MediaSource &) override { return _poller; }

private:
    MediaTuple _tuple;
    weak_ptr<MediaSource> _origin;
    EventPoller::Ptr _poller;
    MultiMediaSourceMuxer::Ptr _muxer;
    Track::Ptr _video;
    FrameWriterInterface *_delegate = nullptr;
    Timer::Ptr _timer;
    Ticker _idle;
    uint64_t _keyframes = 0;
};

mutex g_mtx;
unordered_map<string, KeyframeRelay::Ptr> g_relays; // vhost/app/stream → 갈래(만드는 중 포함)

void KeyframeRelay::stop() {
    if (_video && _delegate) {
        _video->delDelegate(_delegate);
    }
    _delegate = nullptr;
    _video = nullptr;
    _muxer = nullptr; // 등록 해제
    _timer = nullptr;
    lock_guard<mutex> lck(g_mtx);
    auto it = g_relays.find(_tuple.shortUrl());
    if (it != g_relays.end() && it->second.get() == this) {
        g_relays.erase(it);
    }
}

void onNotFound(const MediaInfo &info, const function<void()> &close_player) {
    if (info.stream.size() <= kKeySuffix.size() || !end_with(info.stream, kKeySuffix)) {
        return;
    }
    MediaTuple tuple(info.vhost, info.app, info.stream);
    auto base = info.stream.substr(0, info.stream.size() - kKeySuffix.size());
    auto origin = MediaSource::find(RTSP_SCHEMA, info.vhost, info.app, base);
    if (!origin) {
        origin = MediaSource::find(info.vhost, info.app, base);
    }
    if (!origin) {
        DebugL << "키프레임 갈래: 원본 스트림이 없습니다: " << info.vhost << "/" << info.app << "/" << base;
        close_player();
        return;
    }
    EventPoller::Ptr poller;
    try {
        poller = origin->getOwnerPoller();
    } catch (std::exception &ex) {
        WarnL << "키프레임 갈래: 원본 poller 를 알 수 없습니다: " << ex.what();
        close_player();
        return;
    }
    KeyframeRelay::Ptr relay;
    {
        lock_guard<mutex> lck(g_mtx);
        if (g_relays.count(tuple.shortUrl())) {
            return; // 이미 만드는 중 — 등록되면 기다리던 플레이어가 받는다.
        }
        if (g_relays.size() >= kMaxRelays) {
            WarnL << "키프레임 갈래 상한(" << kMaxRelays << ")에 도달했습니다: " << tuple.shortUrl();
            close_player();
            return;
        }
        relay = make_shared<KeyframeRelay>(tuple, origin, poller);
        g_relays.emplace(tuple.shortUrl(), relay);
    }
    weak_ptr<MediaSource> weak_origin = origin;
    poller->async([relay, weak_origin, close_player]() {
        auto origin = weak_origin.lock();
        if (!origin || !relay->start(origin)) {
            relay->stop();
            close_player();
        }
    }, false);
}

} // namespace

void installKeyframeRelay() {
    static onceToken token([]() {
        static int tag = 0;
        NoticeCenter::Instance().addListener(&tag, Broadcast::kBroadcastNotFoundStream, [](BroadcastNotFoundStreamArgs) {
            onNotFound(args, closePlayer);
        });
        InfoL << "키프레임 갈래(~key) 사용";
    });
}
