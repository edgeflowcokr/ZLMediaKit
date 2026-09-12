#pragma once
#ifdef ENABLE_MP4
#include "MP4Recorder.h"
#include "BoundedRecordWorker.h"
#include "Util/logger.h"
namespace mediakit {
class AsyncMP4Recorder final : public MediaSinkInterface {
    std::shared_ptr<MP4Recorder> _recorder;
    std::shared_ptr<std::atomic<uint64_t>> _frames = std::make_shared<std::atomic<uint64_t>>(0);
    BoundedRecordWorker _worker;
    bool _have_video = false;
    bool _waiting_keyframe = true;
public:
    AsyncMP4Recorder(const MediaTuple& tuple, const std::string& path, size_t seconds)
        : _recorder(std::make_shared<MP4Recorder>(tuple, path, seconds, true)),
          _worker([recorder = _recorder]() { recorder->finish(); },
                  [channel = tuple.stream, path](const char* reason) { ErrorL << "[recording-degraded] channel=" << channel << " path=" << path << " reason=" << reason; }) {}
    // First muxer-accepted frame is required; object creation alone is not recording.
    // This still does not claim physical disk flush or segment continuity.
    bool healthy() const { return _worker.healthy() && _frames->load() > 0; }
    bool canReplace() const { return _worker.canReplace(); }
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
        return _worker.submit([recorder = _recorder, frames = _frames, cached]() {
            if (recorder->inputFrame(cached)) ++(*frames);
        }, cached->size());
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
