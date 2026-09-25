#pragma once
#ifdef ENABLE_MP4
#include "MP4Muxer.h"
#include "MP4Recorder.h"
#include "BoundedRecordWorker.h"
#include "Util/File.h"
#include "Util/logger.h"
#include <atomic>
#include <ctime>
#include <list>
#include <mutex>
#include <vector>

namespace mediakit {
// All disk calls, including destructor/close, belong to the bounded worker.
// History is admitted as one byte-accounted batch, never as a burst of tasks.
//
// 시각(2026-09-25): 클립의 start_time 은 첫 프레임의 "수신" 벽시계(이력이면 요청 시점에서 dts 차로 역산, 실시간이면
// 첫 inputFrame 시각)이고, 마감 후 파일 mtime 을 시작 + 길이로 맞춘다. 처리(마감) 시각에서 역산하면 쓰기 큐가
// 밀린 만큼 어긋난다.
// 작업자 실패(정지 초과·예산 초과) 후에도 받은 프레임은 drain 되어 파일이 정상 마감되므로, finish 가 거부되더라도
// 마감된 파일이 유효하면 degraded 로 완료를 알린다(백엔드가 클립 경로를 붙일 수 있게).
class AsyncEventMP4 {
public:
    // 클립 마감 결과. 디스크 호출(close·stat·utime)은 전부 작업자에서 끝내고 여기엔 값만 담는다
    // (그래서 poller 에서 완료 콜백을 불러도 디스크 호출이 없다).
    struct Result { float seconds = 0; uint64_t file_size = 0; time_t start_time = 0; bool degraded = false; };
    using Completed = std::function<void(const Result&)>;
    // 이력 적재 계획. planHistory 가 openMP4 제출 전에 검증·자르기를 끝낸다(표현 불가면 빈 mp4 를 만들지 않는다).
    struct HistoryPlan { std::list<Frame::Ptr> frames; size_t bytes = 0; time_t start_time = 0; };

private:
    struct State {
        MP4Muxer muxer;
        std::string path;
        bool closed = false;
        std::atomic<bool> broken{false};      // 작업 중 muxer 예외 → 마감된 파일을 정상으로 알리지 않는다
        std::atomic<long long> start_wall{0}; // 첫 프레임 수신 벽시계(초, 0=아직)
        std::atomic<long long> last_wall{0};  // 마지막 프레임 수신 벽시계(End 시각 제한용)
        std::mutex mutex;                     // 아래 마감 상태(작업자 ↔ poller)
        bool finalized = false;
        bool finish_accepted = false;         // finish 작업이 대기열에 들어감(예외로 버려지면 통지 없이 사라질 수 있음)
        bool finish_ran = false;              // finish 작업이 실제로 실행됨
        std::string error;                    // 첫 muxer 예외 요약(미통지 사유 로그용)
        Result result;
        Completed late;                       // 작업자 실패로 finish 가 거부됐을 때: 마감 뒤 알림
        // 작업자 전용·멱등: flush → 길이 → close → 크기 → mtime(시작 + 길이).
        Result closeClip() {
            Result r;
            if (closed) return r;
            closed = true;
            muxer.flush();
            // closeMP4 가 트랙을 비우므로 길이는 반드시 close 전에 구한다.
            r.seconds = muxer.getDuration() / 1000.0f;
            muxer.closeMP4();
            r.file_size = path.empty() ? 0 : toolkit::File::fileSize(path);
            r.start_time = static_cast<time_t>(start_wall.load());
            // End(mtime) = min(시작 + 길이, 마지막 수신 + 1초, 지금) — 카메라 클럭이 빨라도 미래·과장 End 를 쓰지 않는다.
            if (r.file_size && r.start_time)
                setRecordFileMtime(path, recordFileEndTime(path, r.start_time, r.seconds, static_cast<time_t>(last_wall.load())));
            return r;
        }
        void markBroken(const std::string& what) {
            std::lock_guard<std::mutex> lock(mutex);
            if (error.empty()) error = what;
            broken = true;
        }
        // 통지하지 못한 사유(작업자 마감 단계 로그용). mutex 보유 상태에서 부른다.
        std::string invalidReason(const Result& r) const {
            if (broken) return "muxer_exception: " + error;
            if (r.seconds <= 0) return "empty_clip";
            return "file_too_small size=" + std::to_string(r.file_size);
        }
        // 알릴 만한 파일인가: muxer 예외 없음 + 길이 > 0 + 연속 녹화와 같은 최소 크기(1KiB).
        bool valid(const Result& r) const { return !broken && r.seconds > 0 && r.file_size >= 1024; }
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    BoundedRecordWorker::Observer observe_;
    BoundedRecordWorker worker_;
    // muxer 예외를 표시하고 그대로 던진다(작업자가 남은 큐를 버리고 보고한다).
    std::function<void()> guard(std::function<void()> task) {
        return [state = state_, task = std::move(task)] {
            try { task(); }
            catch (const std::exception& ex) { state->markBroken(ex.what()); throw; }
            catch (...) { state->markBroken("non-standard exception"); throw; }
        };
    }
    void submit(std::function<void()> task, size_t bytes = 0,
                BoundedRecordWorker::Admission admission = BoundedRecordWorker::Admission::Live) {
        if (!worker_.submit(guard(std::move(task)), bytes, admission))
            throw std::runtime_error("event clip storage queue unavailable");
    }
public:
    explicit AsyncEventMP4(std::function<void(const char*)> report,
                           BoundedRecordWorker::Observer observe = nullptr)
        : observe_(observe),
          worker_([state = state_] {
                      // 작업자 마감(정상 종료·실패 후 drain 완료 모두). finish 작업이 이미 닫았으면 그 결과를 쓴다.
                      auto r = state->closeClip();
                      Completed late;
                      std::string lost; // 받아들였거나 보류한 완료 콜백을 통지하지 못한 사유(비면 정상)
                      {
                          std::lock_guard<std::mutex> lock(state->mutex);
                          // finish 작업이 받아들여졌는데 실행되지 못했으면 앞선 muxer 예외로 대기열이 비워진 것이다.
                          // (수락 표시가 이 구간 뒤에 오면 finish() 쪽이 같은 판정으로 로그한다 — 정확히 한쪽만.)
                          if (state->finish_accepted && !state->finish_ran)
                              lost = "finish_dropped_after_" + state->invalidReason(r);
                          if (!state->finalized) { state->finalized = true; state->result = r; }
                          r = state->result;
                          late = std::move(state->late);
                          if (late && !state->valid(r)) lost = state->invalidReason(r);
                      }
                      if (!lost.empty()) {
                          // 조용한 실패 금지: 완료를 기다리는 쪽(백엔드 클립 부착)이 영영 통지를 받지 못한다.
                          ErrorL << "[event-recording-not-announced] path=" << state->path << " reason=" << lost;
                      } else if (late) {
                          r.degraded = true;
                          late(r);
                      }
                  },
                  std::move(report), BoundedRecordWorker::Limits::configured(), std::move(observe)) {}
    void openMP4(const std::string& path) {
        state_->path = path; // 작업자는 이후 제출(큐 mutex)로 이 값을 본다
        submit([state = state_, path] { state->muxer.openMP4(path); });
    }
    void addTrack(const Track::Ptr& track) {
        auto copy = track->clone();
        submit([state = state_, copy] { state->muxer.addTrack(copy); });
    }
    void addTrackCompleted() { submit([state = state_] { state->muxer.addTrackCompleted(); }); }
    bool inputFrame(const Frame::Ptr& frame) {
        // 이력 없이 시작한 클립: 첫 실시간 프레임 수신 시각이 시작 시각.
        const long long now = static_cast<long long>(::time(NULL));
        long long unset = 0;
        state_->start_wall.compare_exchange_strong(unset, now);
        state_->last_wall = now;
        auto cached = Frame::getCacheAbleFrame(frame);
        return worker_.submit(guard([state = state_, cached] { state->muxer.inputFrame(cached); }), cached->size());
    }
    // 프리버퍼(pre-event) 일괄 허용 상한 = 녹화기 예산의 절반. 나머지 절반은 이력을 쓰는 동안(SMR 정지 시 수십 초)
    // 들어오는 실시간 프레임 몫이다. 넘으면 클립 전체를 실패시키지 않고 가장 오래된 GOP 부터 잘라
    // 키프레임(SPS/PPS 포함) 경계에서 시작한다. 계상 = 프레임 페이로드 + 프레임당 오버헤드(작업자와 같은 단위).
    static size_t historyCap(size_t budget) { return budget / 2; }
    // GOP 하나가 historyCap 을 넘을 때 받아 줄 한계 = 예산의 3/4. 실시간 몫으로 최소 1/4 을 남긴다.
    // (이력을 버리고 실시간 키프레임부터 시작하는 대안은 채택하지 않았다: 이력이 없으면 reader 가 GOP 캐시를 다시
    //  재생해 같은 큰 GOP 가 실시간 프레임으로 한꺼번에 들어온다. 또 예산이 차도 이제 받은 프레임은 drain 되고
    //  완료가 degraded 로 통지되므로 클립이 사라지지 않는다.)
    static size_t historyHardLimit(size_t budget) { return budget / 4 * 3; }
    // 이력 검증·자르기(디스크 호출 없음). openMP4 제출 전에 부른다: 표현 불가면 여기서 던져 빈 파일을 만들지 않는다.
    HistoryPlan planHistory(const std::list<Frame::Ptr>& history) const {
        HistoryPlan plan;
        if (history.empty()) return plan;
        const size_t per_frame = BoundedRecordWorker::kItemOverheadBytes;
        const size_t budget = worker_.limits().max_bytes;
        const size_t cap = historyCap(budget);
        std::vector<Frame::Ptr> frames(history.begin(), history.end());
        std::vector<RecordHistoryFrame> info;
        info.reserve(frames.size());
        for (const auto& frame : frames)
            info.push_back({frame->size(), frame->getTrackType() == TrackVideo, frame->configFrame() || frame->keyFrame(), frame->keyFrame()});
        const auto trim = recordHistoryTrim(info, cap, historyHardLimit(budget), per_frame);
        if (!trim.ok)
            throw std::runtime_error("event history cannot fit record budget as keyframe-aligned GOPs: history_bytes="
                                     + std::to_string(trim.total_bytes) + " budget_bytes=" + std::to_string(budget));
        if (trim.start && observe_) {
            observe_("trigger=history_trim requested_frames=" + std::to_string(frames.size())
                     + " kept_frames=" + std::to_string(frames.size() - trim.start)
                     + " dropped_bytes=" + std::to_string(trim.total_bytes - trim.kept_bytes)
                     + " kept_bytes=" + std::to_string(trim.kept_bytes)
                     + " cap_bytes=" + std::to_string(cap) + " budget_bytes=" + std::to_string(budget), true);
        }
        for (size_t i = trim.start; i < frames.size(); ++i) plan.frames.push_back(Frame::getCacheAbleFrame(frames[i]));
        plan.bytes = trim.kept_bytes;
        // 시작 시각 = 지금(가장 최근 이력 ≈ 방금 수신) - (최신 dts - 유지 첫 프레임 dts).
        const auto span_ms = frames.back()->dts() > frames[trim.start]->dts() ? frames.back()->dts() - frames[trim.start]->dts() : 0;
        plan.start_time = ::time(NULL) - static_cast<time_t>(span_ms / 1000);
        return plan;
    }
    void inputHistory(HistoryPlan plan) {
        if (plan.frames.empty()) return;
        state_->start_wall = static_cast<long long>(plan.start_time);
        state_->last_wall = static_cast<long long>(::time(NULL)); // 가장 최근 이력 ≈ 방금 수신
        // 일괄 선적재(Baseline): 이 수위는 사용률 돌파(cross50/80) 경고 대상이 아니다.
        submit([state = state_, cached = std::move(plan.frames)] {
            for (const auto& frame : cached) state->muxer.inputFrame(frame);
        }, plan.bytes, BoundedRecordWorker::Admission::Baseline);
    }
    void inputHistory(const std::list<Frame::Ptr>& history) { inputHistory(planHistory(history)); }
    // true = 완료가 통지될 예정(정상 마감 또는 drain 후 degraded 통지 대기), false = 알릴 파일이 없음.
    bool finish(Completed completed) {
        auto state = state_;
        // Rejection is already reported by the worker. Do not throw through a
        // ring-reader callback: it must always release its reader ownership.
        const bool accepted = worker_.submit(guard([state, completed] {
            // H.264/H.265 mergers retain the final access unit until flushed.
            // Without this, every completed clip silently loses its last frame.
            auto r = state->closeClip();
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->finalized = true;
                state->finish_ran = true;
                state->result = r;
            }
            completed(r);
        }));
        if (accepted) {
            std::string lost;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->finish_accepted = true;
                // 작업자가 이미 마감했는데 finish 가 실행되지 않았다면(앞선 muxer 예외로 버려짐) 여기서 알린다.
                if (state->finalized && !state->finish_ran) lost = "finish_dropped_after_" + state->invalidReason(state->result);
            }
            if (!lost.empty()) ErrorL << "[event-recording-not-announced] path=" << state->path << " reason=" << lost;
            return true;
        }
        // 작업자 실패로 거부: 받은 프레임은 drain 후 작업자 마감에서 닫힌다. 아직이면 그때 알리고, 이미 끝났으면
        // 저장된 결과(값만)로 지금 알린다.
        Result r;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->finalized) { state->late = std::move(completed); return true; }
            r = state->result;
        }
        if (!state->valid(r)) return false;
        r.degraded = true;
        completed(r);
        return true;
    }
};
}
#endif
