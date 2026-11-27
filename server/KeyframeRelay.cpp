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
#include "Rtsp/RtspMediaSource.h"
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
constexpr uint64_t kFlushDelayMS = 15;       // 키프레임 입력 뒤 RTP 를 내보내기까지(같은 접근 단위의 조각을 기다리는 틈)
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
        // 원본 먹서의 프레임 링 독자로 붙는다(상시). 이 링은 modify_stamp 보정 「후」 프레임이 흘러서, ~key(보정 Off)가 원본 RTSP 와
        // 같은 RTP 타임스탬프(보정 pts x 90)를 낸다. 예전 위임(video->addDelegate)은 보정 「전」 원시 프레임이라 어긋났다.
        if (!attachOriginReader(origin)) {
            return false;
        }
        // 원본 RTSP 링에 시청자로 붙는다 — 녹화기·짝 서버 중계처럼 「무시청이면 닫는」 원본이 키프레임 갈래만 볼 때 닫히지 않게.
        if (auto rtsp = dynamic_pointer_cast<RtspMediaSource>(origin)) {
            _origin_reader = rtsp->getRing()->attach(_poller, false);
        }
        _idle.resetTime();
        _timer = make_shared<Timer>(kCheckSeconds, [weak_self]() {
            auto self = weak_self.lock();
            return self && self->check();
        }, _poller);
        InfoL << "키프레임 갈래 시작: " << _tuple.shortUrl();
        return true;
    }

    // 키프레임 뒤 짧게 기다렸다가(같은 접근 단위의 다른 조각이 붙을 틈) RTSP 출력의 대기 RTP 를 링으로 내보낸다.
    // ~key 는 프레임이 초당 1장이라, RTP 인코더·합쳐 쓰기가 「다음 프레임이 와야 내보내는」 한 프레임 지연이 곧 1 GOP 지연이 된다.
    void scheduleFlush() {
        if (_flush_pending) {
            return;
        }
        _flush_pending = true;
        weak_ptr<KeyframeRelay> weak_self = shared_from_this();
        _poller->doDelayTask(kFlushDelayMS, [weak_self]() {
            if (auto self = weak_self.lock()) {
                self->_flush_pending = false;
                if (self->_muxer) {
                    self->_muxer->flushRtsp();
                }
            }
            return 0;
        });
    }

    // 원본 먹서의 프레임 링 독자를 붙인다. 원본 poller 에서 호출. 링의 GOP 캐시는 setReadCB 안에서 동기로 재생되므로,
    // 그동안은 모아 두었다가 마지막 설정+키프레임 묶음만 ~key 먹서에 넣는다(안 그러면 원본의 다음 키프레임 1 GOP 을 기다린다).
    // 그 뒤 독자 콜백은 키·설정 프레임만 넣는다. 캐시 재생과 이후 프레임은 겹치지 않으므로 dts 중복 제거는 하지 않는다.
    bool attachOriginReader(const MediaSource::Ptr &origin) {
        try {
            auto muxer = origin->getMuxer();
            _frame_reader = muxer ? muxer->getFrameReader() : nullptr;
        } catch (std::exception &ex) {
            WarnL << "키프레임 갈래: 원본 프레임 링을 열지 못했습니다: " << _tuple.shortUrl() << " " << ex.what();
            _frame_reader = nullptr;
        }
        if (!_frame_reader) {
            WarnL << "키프레임 갈래: 원본 프레임 링 독자를 만들 수 없습니다: " << _tuple.shortUrl();
            return false;
        }
        weak_ptr<KeyframeRelay> weak_self = shared_from_this();
        _replaying = true;
        _frame_reader->setDetachCB([weak_self]() {
            // 원본 링이 사라질 때(원본 닫힘·재등록) — 갈래도 닫아 시청자가 새 원본으로 다시 만들게 한다.
            if (auto self = weak_self.lock()) {
                self->_poller->async([weak_self]() {
                    if (auto s = weak_self.lock()) {
                        InfoL << "키프레임 갈래: 원본 프레임 링이 분리되어 닫습니다: " << s->_tuple.shortUrl();
                        s->stop();
                    }
                }, false);
            }
        });
        try {
            _frame_reader->setReadCB([weak_self](const Frame::Ptr &frame) {
                auto self = weak_self.lock();
                if (!self || !self->_muxer) {
                    return;
                }
                if (frame->getTrackType() != TrackVideo || (!frame->keyFrame() && !frame->configFrame())) {
                    return; // 오디오·P·B 프레임은 버린다.
                }
                if (self->_replaying) {
                    self->_kept.emplace_back(frame);
                    return;
                }
                ++self->_keyframes;
                self->_muxer->inputFrame(frame);
                if (frame->keyFrame()) {
                    self->scheduleFlush();
                }
            });
        } catch (std::exception &ex) {
            WarnL << "키프레임 갈래: 원본 GOP 캐시 재생 중 예외(다음 키프레임을 기다립니다): " << _tuple.shortUrl() << " " << ex.what();
        }
        _replaying = false;
        auto kept = std::move(_kept);
        _kept.clear();
        injectKept(kept);
        return true;
    }

    // 모아 둔 설정·키프레임 중 마지막 키프레임(같은 dts 의 조각 포함)+그 앞에 붙은 설정 프레임을 넣는다.
    void injectKept(const vector<Frame::Ptr> &kept) {
        ssize_t last = static_cast<ssize_t>(kept.size()) - 1;
        while (last >= 0 && !kept[last]->keyFrame()) {
            --last;
        }
        if (last < 0) {
            return;
        }
        auto key_dts = kept[last]->dts();
        ssize_t first = last;
        while (first > 0) {
            auto &prev = kept[first - 1];
            if (prev->configFrame() || (prev->keyFrame() && prev->dts() == key_dts)) {
                --first;
            } else {
                break;
            }
        }
        // 설정 프레임이 같은 dts 의 키프레임 조각 사이에 있어도 순서를 그대로 넣는다.
        size_t n = 0;
        for (ssize_t i = first; i <= last; ++i) {
            if (_muxer->inputFrame(kept[i])) {
                ++n;
            }
        }
        _keyframes += n;
        scheduleFlush();
        InfoL << "키프레임 갈래: 원본 GOP 캐시의 마지막 키프레임을 즉시 넣었습니다: " << _tuple.shortUrl() << " 프레임=" << n;
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
    MultiMediaSourceMuxer::RingType::RingReader::Ptr _frame_reader; // 원본 먹서 프레임 링 독자(보정 후 프레임)
    vector<Frame::Ptr> _kept;      // GOP 캐시 재생 중 모아 둔 설정·키프레임
    bool _replaying = false;       // setReadCB 의 동기 캐시 재생 중인가
    RtspMediaSource::RingType::RingReader::Ptr _origin_reader; // 원본 시청자 1 유지(내용은 쓰지 않는다)
    Timer::Ptr _timer;
    Ticker _idle;
    uint64_t _keyframes = 0;
    bool _flush_pending = false;
};

mutex g_mtx;
unordered_map<string, KeyframeRelay::Ptr> g_relays; // vhost/app/stream → 갈래(만드는 중 포함)

void KeyframeRelay::stop() {
    if (_frame_reader) {
        _frame_reader->setReadCB(nullptr);
        _frame_reader->setDetachCB(nullptr);
    }
    _frame_reader = nullptr;
    _origin_reader = nullptr;
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
