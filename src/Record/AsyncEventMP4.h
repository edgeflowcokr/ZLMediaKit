#pragma once
#ifdef ENABLE_MP4
#include "MP4Muxer.h"
#include "BoundedRecordWorker.h"
#include <list>

namespace mediakit {
// All disk calls, including destructor/close, belong to the bounded worker.
// History is admitted as one byte-accounted batch, never as a burst of tasks.
class AsyncEventMP4 {
    struct State {
        MP4Muxer muxer;
        bool closed = false;
        void close() { if (!closed) { closed = true; muxer.flush(); muxer.closeMP4(); } }
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    BoundedRecordWorker worker_;
    void submit(std::function<void()> task, size_t bytes = 0) {
        if (!worker_.submit(std::move(task), bytes))
            throw std::runtime_error("event clip storage queue unavailable");
    }
public:
    explicit AsyncEventMP4(std::function<void(const char*)> report)
        : worker_([state = state_] { state->close(); }, std::move(report)) {}
    void openMP4(const std::string& path) {
        submit([state = state_, path] { state->muxer.openMP4(path); });
    }
    void addTrack(const Track::Ptr& track) {
        auto copy = track->clone();
        submit([state = state_, copy] { state->muxer.addTrack(copy); });
    }
    void addTrackCompleted() { submit([state = state_] { state->muxer.addTrackCompleted(); }); }
    bool inputFrame(const Frame::Ptr& frame) {
        auto cached = Frame::getCacheAbleFrame(frame);
        return worker_.submit([state = state_, cached] { state->muxer.inputFrame(cached); }, cached->size());
    }
    void inputHistory(const std::list<Frame::Ptr>& history) {
        std::list<Frame::Ptr> cached;
        size_t bytes = 0;
        for (const auto& frame : history) {
            if (frame->size() > BoundedRecordWorker::max_bytes - bytes)
                throw std::runtime_error("event history exceeds bounded 16MiB admission");
            bytes += frame->size();
            cached.push_back(Frame::getCacheAbleFrame(frame));
        }
        submit([state = state_, cached = std::move(cached)] {
            for (const auto& frame : cached) state->muxer.inputFrame(frame);
        }, bytes);
    }
    bool finish(std::function<void(float)> completed) {
        // Rejection is already reported by the worker. Do not throw through a
        // ring-reader callback: it must always release its reader ownership.
        return worker_.submit([state = state_, completed = std::move(completed)] {
            // H.264/H.265 mergers retain the final access unit until flushed.
            // Without this, every completed clip silently loses its last frame.
            state->muxer.flush();
            const auto seconds = state->muxer.getDuration() / 1000.0f;
            state->close();
            completed(seconds);
        });
    }
};
}
#endif
