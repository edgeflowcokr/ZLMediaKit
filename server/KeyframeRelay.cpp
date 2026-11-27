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
#include "Rtsp/RtspMuxer.h"
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

constexpr size_t kMaxAuPackets = 4096;       // 한 접근 단위로 모으는 RTP 패킷 상한(넘으면 그 단위는 버린다)

// RTP 페이로드가 IDR/IRAP 슬라이스를 담았는지 본다(H.264: NAL 5, H.265: NAL 16~21). 설정(SPS/PPS/VPS)·SEI 는 같은 접근 단위에 딸려 가므로 보지 않는다.
bool isKeySliceType(CodecId codec, uint8_t nal_type) {
    return codec == CodecH264 ? nal_type == 5 : (nal_type >= 16 && nal_type <= 21);
}

bool rtpHasKeySlice(CodecId codec, const uint8_t *p, size_t n) {
    if (n < 2) {
        return false;
    }
    if (codec == CodecH264) {
        const auto type = p[0] & 0x1f;
        if (type == 24) { // STAP-A
            size_t i = 1;
            while (i + 3 <= n) {
                const size_t size = (size_t(p[i]) << 8) | p[i + 1];
                i += 2;
                if (!size || i + size > n) {
                    break;
                }
                if (isKeySliceType(codec, p[i] & 0x1f)) {
                    return true;
                }
                i += size;
            }
            return false;
        }
        if (type == 28) { // FU-A
            return isKeySliceType(codec, p[1] & 0x1f);
        }
        return isKeySliceType(codec, type);
    }
    const auto type = (p[0] >> 1) & 0x3f;
    if (type == 48) { // AP
        size_t i = 2;
        while (i + 4 <= n) {
            const size_t size = (size_t(p[i]) << 8) | p[i + 1];
            i += 2;
            if (!size || i + size > n) {
                break;
            }
            if (isKeySliceType(codec, (p[i] >> 1) & 0x3f)) {
                return true;
            }
            i += size;
        }
        return false;
    }
    if (type == 49) { // FU
        return n >= 3 && isKeySliceType(codec, p[2] & 0x3f);
    }
    return isKeySliceType(codec, type);
}

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
        // RTP 수준 갈래(H1, 2026-11-27): H.264 이고 원본이 RTSP 원천이면 ~key 의 RTSP 출력(WHEP·RTSP)은 원본 RTSP 링의 키프레임 접근 단위
        // RTP 패킷을 그대로 복사해 만든다 — RTP 타임스탬프가 구조상 원본과 같다(원천 시간표가 멈춰 원본 RTP 인코더가 출력마다 +1ms 보정해도 같다).
        // fMP4·HLS 폴백은 RTP 가 없으니 예전처럼 원본 프레임 링에서 만든다.
        // H.265 는 접근 단위 끝(마커)이 「다음 화면의 pts 가 달라질 때」만 서서(H265Rtp.cpp) 시간표가 멈춘 원천에서 단위를 가를 수 없어 예전 방식을 유지한다.
        _codec = video->getCodecId();
        auto origin_rtsp = dynamic_pointer_cast<RtspMediaSource>(origin);
        _rtp_mode = origin_rtsp && _codec == CodecH264;
        ProtocolOption option;
        option.enable_audio = false;
        option.add_mute_audio = false;
        option.auto_close = false;
        option.modify_stamp = ProtocolOption::kModifyStampOff; // I프레임 사이 1~2초 간격을 그대로 둔다(상대 보정이 간격을 접지 않게)
        option.enable_rtsp = !_rtp_mode; // WHEP 은 RTSP 원천을 읽는다(RTP 모드면 아래 _key_rtsp 가 그 원천이다)
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
        // RTP 모드면 이 독자가 키프레임 접근 단위의 RTP 를 읽어 ~key RTSP 원천으로 복사한다.
        if (_rtp_mode) {
            if (!attachOriginRtp(origin_rtsp, video)) {
                return false;
            }
        } else if (origin_rtsp && origin_rtsp->getRing()) {
            _origin_reader = origin_rtsp->getRing()->attach(_poller, false);
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

    // 원본 RTSP 링 독자를 붙이고 ~key 의 RTSP 원천(_key_rtsp)을 만든다. 원본 poller 에서 호출.
    // 링은 GOP 캐시를 setReadCB 안에서 동기로 재생하므로 마지막 키프레임이 즉시 ~key 에 들어간다(원본의 다음 키프레임을 기다리지 않는다).
    bool attachOriginRtp(const RtspMediaSource::Ptr &origin, const Track::Ptr &video) {
        auto ring = origin->getRing();
        if (!ring) {
            WarnL << "키프레임 갈래: 원본 RTSP 링이 아직 없습니다: " << _tuple.shortUrl();
            return false;
        }
        // SDP 는 영상 트랙만으로 새로 만든다(원본이 오디오 트랙을 가져도 ~key 는 영상만). 페이로드 타입도 이 SDP 와 맞춘다.
        string sdp;
        {
            RtspMuxer sdp_muxer;
            if (!sdp_muxer.addTrack(video->clone())) {
                WarnL << "키프레임 갈래: RTSP SDP 를 만들 수 없습니다: " << _tuple.shortUrl();
                return false;
            }
            sdp_muxer.addTrackCompleted();
            sdp = sdp_muxer.getSdp();
        }
        const auto pt = RtpPayload::getPayloadType(*video);
        _pt = static_cast<uint8_t>(pt < 0 ? 96 : pt);
        _key_rtsp = make_shared<RtspMediaSource>(_tuple);
        _key_rtsp->setListener(shared_from_this());
        _key_rtsp->setSdp(sdp);
        weak_ptr<KeyframeRelay> weak_self = shared_from_this();
        _origin_reader = ring->attach(_poller, true);
        _origin_reader->setDetachCB([weak_self]() {
            // 원본 RTSP 링이 사라질 때 — 갈래도 닫아 시청자가 새 원본으로 다시 만들게 한다.
            if (auto self = weak_self.lock()) {
                self->_poller->async([weak_self]() {
                    if (auto s = weak_self.lock()) {
                        InfoL << "키프레임 갈래: 원본 RTSP 링이 분리되어 닫습니다: " << s->_tuple.shortUrl();
                        s->stop();
                    }
                }, false);
            }
        });
        try {
            _origin_reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pkt) {
                auto self = weak_self.lock();
                if (!self || !self->_key_rtsp || !pkt) {
                    return;
                }
                pkt->for_each([&](const RtpPacket::Ptr &rtp) { self->onOriginRtp(rtp); });
            });
        } catch (std::exception &ex) {
            WarnL << "키프레임 갈래: 원본 RTSP GOP 캐시 재생 중 예외(다음 키프레임을 기다립니다): " << _tuple.shortUrl() << " " << ex.what();
        }
        return true;
    }

    // 원본 RTP 패킷을 같은 RTP 타임스탬프끼리 접근 단위로 모은다. 마커 비트나 타임스탬프 변화로 단위가 끝나면 키프레임 단위만 내보낸다.
    void onOriginRtp(const RtpPacket::Ptr &rtp) {
        if (rtp->type != TrackVideo) {
            return;
        }
        const auto stamp = rtp->getStamp();
        if (_au_discard) {
            // 상한을 넘은 접근 단위는 경계(마커·다음 타임스탬프)까지 통째로 버린다 — 뒷조각만 남아 키프레임으로 나가지 않게.
            if (stamp == _au_stamp) {
                if (rtp->getHeader()->mark) {
                    _au_discard = false;
                }
                return;
            }
            _au_discard = false;
        }
        if (!_au.empty() && stamp != _au_stamp) {
            finishAccessUnit();
        }
        _au_stamp = stamp;
        if (_au.size() >= kMaxAuPackets) {
            WarnL << "키프레임 갈래: 접근 단위가 너무 커서 버립니다: " << _tuple.shortUrl() << " 패킷=" << _au.size();
            _au.clear();
            _au_key = false;
            _au_discard = !rtp->getHeader()->mark;
            return;
        }
        _au.emplace_back(rtp);
        if (!_au_key && rtp->getPayloadSize() > 0) {
            _au_key = rtpHasKeySlice(_codec, rtp->getPayload(), rtp->getPayloadSize());
        }
        if (rtp->getHeader()->mark) {
            finishAccessUnit();
        }
    }

    // 모은 접근 단위가 키프레임이면 패킷을 복사해(seq 만 다시 매김) ~key RTSP 원천에 쓰고 링으로 즉시 내보낸다.
    void finishAccessUnit() {
        if (_au_key && _key_rtsp && !_au.empty()) {
            bool first = true;
            for (auto &src : _au) {
                auto dst = RtpPacket::create();
                dst->setCapacity(src->size());
                dst->setSize(src->size());
                memcpy(dst->data(), src->data(), src->size());
                dst->data()[1] = 2 * TrackVideo; // rtp over tcp 머리의 채널(영상 = 0)
                dst->type = TrackVideo;
                dst->sample_rate = src->sample_rate;
                dst->ntp_stamp = src->ntp_stamp;
                dst->track_index = 0;
                auto header = dst->getHeader();
                header->pt = _pt;
                header->seq = htons(_seq++);
                if (!_ssrc) {
                    // 원본과 겹치지 않는 고정 SSRC(0 은 쓰지 않는다).
                    _ssrc = src->getSSRC() ^ 0x4b4b4b4b;
                    if (!_ssrc) {
                        _ssrc = 1;
                    }
                }
                header->ssrc = htonl(_ssrc);
                _key_rtsp->onWrite(std::move(dst), first);
                first = false;
            }
            _key_rtsp->flushRtp();
            ++_keyframes;
        }
        _au.clear();
        _au_key = false;
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
        if (_muxer->totalReaderCount() > 0 || (_key_rtsp && _key_rtsp->readerCount() > 0)) {
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
    // _key_rtsp 의 리스너도 이 갈래다 — WHEP 은 재생 전에 트랙(getTracks → getMuxer)을, getMediaList 는 totalReaderCount 를 부른다.
    int totalReaderCount(MediaSource &) override {
        return (_muxer ? _muxer->totalReaderCount() : 0) + (_key_rtsp ? _key_rtsp->readerCount() : 0);
    }
    shared_ptr<MultiMediaSourceMuxer> getMuxer(MediaSource &) const override {
        if (!_muxer) {
            // 닫힌 뒤에 늦게 온 조회 — 빈 포인터를 돌려주면 호출자(MediaSource::getTracks)가 바로 역참조한다.
            throw std::runtime_error("keyframe relay closed");
        }
        return _muxer;
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
    RtspMediaSource::RingType::RingReader::Ptr _origin_reader; // 원본 시청자 1 유지. RTP 모드에서는 키프레임 RTP 를 읽는 독자
    // RTP 수준 갈래(H1)
    bool _rtp_mode = false;
    CodecId _codec = CodecInvalid;
    RtspMediaSource::Ptr _key_rtsp;      // ~key 의 RTSP 원천(원본 키프레임 RTP 복사본)
    uint8_t _pt = 96;
    uint16_t _seq = 0;
    uint32_t _ssrc = 0;
    vector<RtpPacket::Ptr> _au;          // 모으는 중인 접근 단위(같은 RTP 타임스탬프)
    uint32_t _au_stamp = 0;
    bool _au_key = false;
    bool _au_discard = false;            // 상한을 넘겨 버리는 중인 접근 단위(경계까지)
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
    if (_origin_reader) {
        _origin_reader->setReadCB(nullptr);
        _origin_reader->setDetachCB(nullptr);
    }
    _origin_reader = nullptr;
    _au.clear();
    _au_discard = false;
    _key_rtsp = nullptr; // 등록 해제
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
