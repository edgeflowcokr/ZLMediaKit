#pragma once
#ifdef ENABLE_MP4
#include "MP4Recorder.h"
#include "BoundedRecordWorker.h"
#include "Util/logger.h"
#include <atomic>
#include <chrono>
#include <ctime>
namespace mediakit {
class AsyncMP4Recorder final : public MediaSinkInterface {
    std::shared_ptr<MP4Recorder> _recorder;
    std::shared_ptr<std::atomic<uint64_t>> _frames = std::make_shared<std::atomic<uint64_t>>(0);
    BoundedRecordWorker _worker;
    bool _have_video = false;
    bool _waiting_keyframe = true;
    std::atomic<long long> _first_submit_ms{0}; // 첫 프레임을 작업자에 넘긴 시각(steady ms, 0=아직)
    static long long nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
public:
    AsyncMP4Recorder(const MediaTuple& tuple, const std::string& path, size_t seconds)
        : _recorder(std::make_shared<MP4Recorder>(tuple, path, seconds, true)),
          _worker([recorder = _recorder]() { recorder->finish(); },
                  [channel = tuple.stream, path](const char* reason) { ErrorL << "[recording-degraded] channel=" << channel << " path=" << path << " reason=" << reason; },
                  // 한도는 생성 스레드에서 config([record] queueMaxMB/stallToleranceMs)로 읽어 고정한다.
                  BoundedRecordWorker::Limits::configured(),
                  // [recording-queue]: 채널별 큐 최고수위·최장 디스크 호출(60초 창, 50/80% 돌파 시 즉시).
                  // 녹화 용량 한계(여유율) 산정과 입장제어의 측정 도구다. 필드명은 grep 대상이라 바꾸지 말 것.
                  [channel = tuple.stream](const std::string& fields, bool warn) {
                      if (warn) WarnL << "[recording-queue] channel=" << channel << " " << fields;
                      else InfoL << "[recording-queue] channel=" << channel << " " << fields;
                  }) {}
    // First muxer-accepted frame is required; object creation alone is not recording.
    // This still does not claim physical disk flush or segment continuity.
    // 예외(시작 대기): 첫 프레임을 이미 작업자에 넘겼고 작업자가 정상인데 아직 처리되지 않았으면(createFile/openMP4 가
    // 정지 허용시간 안에서 멈춤) 녹화 중으로 본다. 아니면 백엔드 record-watch 가 startRecord(code=-1)를 반복한다.
    // 이 유예는 첫 제출 후 허용시간까지로 한정한다(muxer 가 계속 거부하면 결국 false).
    bool healthy() const {
        if (!_worker.healthy()) return false;
        if (_frames->load() > 0) return true;
        const auto first = _first_submit_ms.load();
        return first && nowMs() - first <= _worker.limits().stall_tolerance_ms;
    }
    bool canReplace() const { return _worker.canReplace(); }
    BoundedRecordWorker::Stats queueStats() const { return _worker.stats(); }
    bool inputFrame(const Frame::Ptr& frame) override {
        if (!_worker.healthy()) return _worker.submit([](){});
        // No historical burst for continuous recording: start decodably at a
        // live keyframe. Track clones carry codec configuration; retain in-band
        // configuration frames too, but do not start with dependent P/B frames.
        if (_have_video && _waiting_keyframe) {
            if (frame->getTrackType() == TrackVideo && frame->keyFrame()) _waiting_keyframe = false;
            else if (!frame->configFrame()) return false;
        }
        auto cached = Frame::getCacheAbleFrame(frame);
        // 수신 벽시계: 새 세그먼트의 파일명·날짜 폴더·start_time 기준(처리 시각이 아니라 — 큐 지연만큼 밀리지 않게).
        const time_t ingest = ::time(NULL);
        const bool ok = _worker.submit([recorder = _recorder, frames = _frames, cached, ingest]() {
            if (recorder->inputFrame(cached, ingest)) ++(*frames);
        }, cached->size());
        long long unset = 0;
        if (ok) _first_submit_ms.compare_exchange_strong(unset, nowMs());
        return ok;
    }
    bool addTrack(const Track::Ptr& track) override {
        if (track->getTrackType() == TrackVideo) _have_video = true;
        auto copy = track->clone();
        return _worker.submit([recorder = _recorder, copy]() { recorder->addTrack(copy); });
    }
    void resetTracks() override {
        _have_video = false;
        _waiting_keyframe = true;
        _worker.submit([recorder = _recorder]() { recorder->resetTracks(); });
    }
    void flush() override { _worker.submit([recorder = _recorder]() { recorder->flush(); }); }
};
}
#endif
