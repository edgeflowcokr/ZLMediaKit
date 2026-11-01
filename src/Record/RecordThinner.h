/*
 * 가변 프레임 녹화(EdgeFlowAI 포크) — 정지 구간은 연속 녹화에 키프레임만 넘긴다.
 * 설계: docs/VARIABLE_FRAME_RECORDING.md(00.NK_Edge). 다시 압축하지 않고 원본 프레임을 고르기만 한다.
 *
 * - keyframe 모드: 비디오는 키프레임·설정 프레임만 넘기고, 마지막 키프레임 이후 프레임(1 GOP)은 들고 있는다. 오디오는 그대로.
 * - full 로 바뀌는 순간: 들고 있던 프레임을 먼저 넘긴다(움직임 시작 장면이 빠지지 않게). 상한을 넘겨 비웠으면 다음 키프레임까지 비디오를 버린다
 *   (앞 참조가 없는 P프레임을 쓰면 깨진다).
 * - 모든 호출은 소스 owner poller 에서 한다(onTrackFrame·API async) — 잠금이 필요 없다.
 */
#ifndef ZLMEDIAKIT_RECORDTHINNER_H
#define ZLMEDIAKIT_RECORDTHINNER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "Extension/Frame.h"
#include "Util/logger.h"
#include "Util/TimeTicker.h"

namespace mediakit {

class RecordThinner {
public:
    static constexpr size_t kMaxGopFrames = 600;
    static constexpr size_t kMaxGopBytes = 8u << 20;

    void setKeyframeOnly(bool on) { _want_keyframe_only.store(on); }
    bool keyframeOnly() const { return _want_keyframe_only.load(); }

    // feed 는 녹화기에 넘길 프레임을 write 로 넘긴다(0개 이상).
    template <typename Write>
    void feed(const Frame::Ptr &frame, const std::string &stream, Write &&write) {
        if (frame->getTrackType() != TrackVideo) {
            write(frame);
            return;
        }
        const bool want_thin = _want_keyframe_only.load();
        if (want_thin) {
            _active = true;
            if (frame->keyFrame() || frame->configFrame()) {
                clearGop();
                _wait_key = false;
                count(frame, true);
                write(frame);
                return;
            }
            if (_wait_key) { // 상한으로 비운 뒤 — 키프레임 전까지는 들고 있을 의미가 없다
                count(frame, false);
                return;
            }
            if (_gop.size() >= kMaxGopFrames || _gop_bytes + frame->size() > kMaxGopBytes) {
                ++_gop_overflow;
                WarnL << "[record-thin] stream=" << stream << " GOP 버퍼 상한(프레임 " << _gop.size() << "·" << _gop_bytes
                      << "B) — 비우고 다음 키프레임부터(움직임으로 바뀌면 그 사이 일부가 빠진다)";
                clearGop();
                _wait_key = true;
                count(frame, false);
                return;
            }
            _gop_bytes += frame->size();
            _gop.emplace_back(Frame::getCacheAbleFrame(frame));
            count(frame, false);
            return;
        }
        if (_active) { // keyframe → full 전환: 들고 있던 GOP 를 먼저 넘긴다
            _active = false;
            if (!_gop.empty()) {
                ++_gop_flush;
                for (auto &f : _gop) {
                    write(f);
                    _flushed_bytes += f->size(); // 버렸다고 센 바이트를 되돌려 셈
                }
            }
            clearGop();
        }
        if (_wait_key) {
            if (!(frame->keyFrame() || frame->configFrame())) {
                count(frame, false);
                return;
            }
            _wait_key = false;
        }
        count(frame, true);
        write(frame);
    }

    // logIfDue 는 10분마다 누적 통계를 로그로 남긴다(관측: 정지 구간 절감량).
    void logIfDue(const std::string &stream) {
        if (_log_ticker.elapsedTime() < 600 * 1000) return;
        _log_ticker.resetTime();
        if (_kept == 0 && _dropped == 0) return;
        const uint64_t dropped_bytes = _dropped_bytes > _flushed_bytes ? _dropped_bytes - _flushed_bytes : 0;
        InfoL << "[record-thin] stream=" << stream << " mode=" << (_want_keyframe_only.load() ? "keyframe" : "full") << " kept=" << _kept
              << " kept_bytes=" << _kept_bytes << " dropped=" << _dropped << " dropped_bytes=" << dropped_bytes << " gop_flush=" << _gop_flush
              << " gop_overflow=" << _gop_overflow;
    }

private:
    void clearGop() {
        _gop.clear();
        _gop_bytes = 0;
    }
    void count(const Frame::Ptr &frame, bool kept) {
        if (kept) {
            ++_kept;
            _kept_bytes += frame->size();
        } else {
            ++_dropped;
            _dropped_bytes += frame->size();
        }
    }

    std::atomic<bool> _want_keyframe_only { false };
    bool _active = false;
    bool _wait_key = false;
    std::vector<Frame::Ptr> _gop;
    size_t _gop_bytes = 0;
    uint64_t _kept = 0, _kept_bytes = 0, _dropped = 0, _dropped_bytes = 0, _flushed_bytes = 0, _gop_flush = 0, _gop_overflow = 0;
    toolkit::Ticker _log_ticker;
};

} // namespace mediakit
#endif // ZLMEDIAKIT_RECORDTHINNER_H
