/*
* Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
*
* This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
*
* Use of this source code is governed by MIT-like license that can be found in the
* LICENSE file in the root of the source tree. All contributing project authors
* may be found in the AUTHORS file in the root of the source tree.
*/

#include <math.h>
#include "Common/config.h"
#include "MultiMediaSourceMuxer.h"
#include "Thread/WorkThreadPool.h"
#include "Util/File.h"
#include "Record/AsyncMP4Recorder.h"
#include "Record/AsyncEventMP4.h"

using namespace std;
using namespace toolkit;

namespace toolkit {
    StatisticImp(mediakit::MultiMediaSourceMuxer);
}

namespace mediakit {

namespace {
class MediaSourceForMuxer : public MediaSource {
public:
    MediaSourceForMuxer(const MultiMediaSourceMuxer::Ptr &muxer)
        : MediaSource("muxer", muxer->getMediaTuple()) {
        MediaSource::setListener(muxer);
    }
    int readerCount() override { return 0; }
};

#if defined(ENABLE_MP4)
// 이벤트 클립(startRecord back/forward)이 닫힌(closeMP4) 직후, 연속녹화(MP4Recorder)와 동일한
// kBroadcastRecordMP4 훅을 emit 한다. 연속녹화 경로는 MP4Recorder::asyncClose 에서 이미 emit 하지만
// startRecord(이벤트 클립) 경로는 지금까지 아무 훅도 쏘지 않아 노드가 클립 완료를 알 수 없던 갭을 메운다.
// 노드는 file_path 로 연속녹화와 이벤트 클립을 구분한다(클립 경로 = startRecord 호출 시 넘긴 path).
// 주의: time_len 은 반드시 muxer->closeMP4() 호출 전에 muxer->getDuration() 으로 구해 넘겨야 한다
//       (closeMP4 가 _tracks 를 비워 이후 getDuration()==0). file_size 는 close 후 파일이 확정된 뒤 읽는다.
//       이 순서는 MP4Recorder::asyncClose 와 동일하다.
static void emitEventClipRecorded(const std::string &path, const MediaTuple &tuple, float time_len) {
    RecordInfo info;
    static_cast<MediaTuple &>(info) = tuple;
    info.file_path = path;
    auto pos = path.find_last_of('/');
    if (pos != std::string::npos) {
        info.file_name = path.substr(pos + 1);
        info.folder = path.substr(0, pos + 1);
    } else {
        info.file_name = path;
    }
    info.time_len = time_len;
    info.file_size = toolkit::File::fileSize(path);
    // 클립 첫 프레임의 대략적 벽시계(GMT, 초) = 종료 시각 - 클립 길이.
    info.start_time = ::time(NULL) - (time_t)time_len;
    // 연속녹화(MP4Recorder.cpp)와 동일한 이벤트를 쏜다. 노드는 경로로 클립/연속을 판별.
    NOTICE_EMIT(BroadcastRecordMP4Args, Broadcast::kBroadcastRecordMP4, info);
}
#endif
} // namespace

class FramePacedSender : public FrameWriterInterface, public std::enable_shared_from_this<FramePacedSender> {
public:
    using OnFrame = std::function<void(const Frame::Ptr &frame)>;

    // Buffer depth thresholds (ms)
    static constexpr int kLowMS    = 100;  // Below this → slow down
    static constexpr int kTargetMS = 200;  // Upper bound of normal-speed zone
    static constexpr int kHighMS   = 500;  // Above this → max speed + force flush
    // Speed bounds
    static constexpr float kMinSpeed = 0.5f;
    static constexpr float kMaxSpeed = 1.5f;
    // Keep the cache bounded even when publisher timestamps are identical or tightly clustered.
    static constexpr size_t kMaxCacheSize = 25 * 5;

    FramePacedSender(uint32_t paced_sender_ms, OnFrame cb)
        : _paced_sender_ms(paced_sender_ms), _cb(std::move(cb)) {}

    void resetTimer(const EventPoller::Ptr &poller) {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        std::weak_ptr<FramePacedSender> weak_self = shared_from_this();
        _timer = std::make_shared<Timer>(_paced_sender_ms / 1000.0f, [weak_self]() {
            if (auto strong_self = weak_self.lock()) {
                strong_self->onTick();
                return true;
            }
            return false;
        }, poller);
    }

    bool inputFrame(const Frame::Ptr &frame) override {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        if (!_timer) {
            setCurrentStamp(frame->dts());
            resetTimer(EventPoller::getCurrentPoller());
        }
        auto &last_dts = _last_dts[frame->getTrackType()];
        if (last_dts > frame->dts()) {
            WarnL << "Dts decrease: " << last_dts << "->" << frame->dts()
                  << ", flush paced sender cache: " << _cache.size();
            flushCache(frame->dts());
        }
        _cache.emplace(frame->dts(), Frame::getCacheAbleFrame(frame));
        last_dts = frame->dts();
        if (_cache.size() > kMaxCacheSize) {
            WarnL << "Force flush paced sender cache: size=" << _cache.size();
            flushCache(frame->dts());
        }
        return true;
    }

private:
    /**
     * Compute playback speed multiplier from buffer depth (ms).
     *
     *   buf < kLowMS       → slow down  (kMinSpeed … 1.0)
     *   kLowMS ≤ buf ≤ kTargetMS → normal     (1.0)
     *   kTargetMS < buf ≤ kHighMS → speed up   (1.0 … kMaxSpeed)
     *   buf > kHighMS      → max speed  (kMaxSpeed) + force flush
     */
    float calcSpeed(int buf_ms) const {
        if (buf_ms < kLowMS) {
            float t = (float)buf_ms / kLowMS;
            return kMinSpeed + t * (1.0f - kMinSpeed);
        }
        if (buf_ms <= kTargetMS) {
            return 1.0f;
        }
        if (buf_ms <= kHighMS) {
            float t = (float)(buf_ms - kTargetMS) / (kHighMS - kTargetMS);
            return 1.0f + t * (kMaxSpeed - 1.0f);
        }
        return kMaxSpeed;
    }

    void onTick() {
        std::lock_guard<std::recursive_mutex> lck(_mtx);

        uint64_t wall_ms = _ticker.elapsedTime();
        _ticker.resetTime();

        // Buffer depth = newest dts − oldest dts in cache
        int buf_ms = 0;
        if (_cache.size() >= 2) {
            buf_ms = (int)(_cache.rbegin()->first - _cache.begin()->first);
        }

        float speed = calcSpeed(buf_ms);
        _virtual_pos += (uint64_t)(wall_ms * speed);

        // Consume frames whose dts ≤ virtual position
        while (!_cache.empty()) {
            auto front = _cache.begin();
            if (front->first > _virtual_pos) break;
            _cb(front->second);
            _cache.erase(front);
        }

        // Safety flush when buffer grows too deep
        if (buf_ms > kHighMS * 2) {
            WarnL << "Force flush paced sender cache: buf=" << buf_ms << "ms";
            flushCache(_cache.empty() ? _virtual_pos : _cache.rbegin()->first);
        }
    }

    void flushCache(uint64_t dts) {
        while (!_cache.empty()) {
            auto front = _cache.begin();
            _cb(front->second);
            _cache.erase(front);
        }
        setCurrentStamp(dts);
    }

    void setCurrentStamp(uint64_t stamp) {
        _virtual_pos = stamp;
        _ticker.resetTime();
    }

private:
    uint32_t _paced_sender_ms;
    uint64_t _virtual_pos = 0;
    uint64_t _last_dts[2] = {0, 0};
    OnFrame _cb;
    Ticker _ticker;
    Timer::Ptr _timer;
    std::recursive_mutex _mtx;
    std::multimap<uint64_t, Frame::Ptr> _cache;
};

std::shared_ptr<MediaSinkInterface> MultiMediaSourceMuxer::makeRecorder(Recorder::type type) {
    auto recorder = Recorder::createRecorder(type, getMediaTuple(), _option);
    for (auto &track : getTracks()) {
        recorder->addTrack(track);
    }
    recorder->addTrackCompleted();
    if (_ring && type == Recorder::type_mp4) {
        // Continuous recording starts at the next live keyframe. The shared
        // ring can contain minutes of event pre-roll, not just one GOP; replaying
        // it here floods the bounded disk queue. Event startRecord keeps its
        // separate, explicitly requested historical clip path unchanged.
        size_t history_frames = 0;
        _ring->flushGop([&](const Frame::Ptr &) { ++history_frames; });
        InfoL << "[recording-start] channel=" << getMediaTuple().stream
              << " history_frames_skipped=" << history_frames << " start=next_live_keyframe";
    } else if (_ring) {
        _ring->flushGop([&](const Frame::Ptr &frame) {
            recorder->inputFrame(frame);
        });
    }
    return recorder;
}

static string getTrackInfoStr(const TrackSource *track_src){
    _StrPrinter codec_info;
    auto tracks = track_src->getTracks(true);
    for (auto &track : tracks) {
        track->update();
        auto codec_type = track->getTrackType();
        codec_info << track->getCodecName();
        switch (codec_type) {
            case TrackAudio : {
                auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
                codec_info << "["
                           << audio_track->getAudioSampleRate() << "/"
                           << audio_track->getAudioChannel() << "/"
                           << audio_track->getAudioSampleBit() << "] ";
                break;
            }
            case TrackVideo : {
                auto video_track = dynamic_pointer_cast<VideoTrack>(track);
                codec_info << "["
                           << video_track->getVideoWidth() << "/"
                           << video_track->getVideoHeight() << "/"
                           << round(video_track->getVideoFps()) << "] ";
                break;
            }
            default:
                break;
        }
    }
    return codec_info;
}

const ProtocolOption &MultiMediaSourceMuxer::getOption() const {
    return _option;
}

const MediaTuple &MultiMediaSourceMuxer::getMediaTuple() const {
    return _tuple;
}

std::string MultiMediaSourceMuxer::shortUrl() const {
    auto ret = getOriginUrl(MediaSource::NullMediaSource());
    if (!ret.empty()) {
        return ret;
    }
    return _tuple.shortUrl();
}
#if defined(ENABLE_RTPPROXY)
void MultiMediaSourceMuxer::forEachRtpSender(const std::function<void(const std::string &ssrc, const RtpSender &sender)> &cb) const {
    for (auto &pr : _rtp_sender) {
        auto sender = std::get<1>(pr.second).lock();
        if (sender) {
            cb(pr.first, *sender);
        }
    }
}
#endif // ENABLE_RTPPROXY
MultiMediaSourceMuxer::MultiMediaSourceMuxer(const MediaTuple& tuple, float dur_sec, const ProtocolOption &option): _tuple(tuple) {
    if (!option.stream_replace.empty()) {
        // 支持在on_publish hook中替换stream_id  [AUTO-TRANSLATED:375eb2ff]
        // Support replacing stream_id in on_publish hook
        _tuple.stream = option.stream_replace;
    }
    _poller = EventPollerPool::Instance().getPoller();
    _create_in_poller = _poller->isCurrentThread();
    _option = option;
    _dur_sec = dur_sec;
    setMaxTrackCount(option.max_track);

    if (option.enable_rtmp) {
        _rtmp = std::make_shared<RtmpMediaSourceMuxer>(_tuple, option, std::make_shared<TitleMeta>(dur_sec));
    }
    if (option.enable_rtsp) {
        _rtsp = std::make_shared<RtspMediaSourceMuxer>(_tuple, option, std::make_shared<TitleSdp>(dur_sec));
    }
    if (option.enable_hls) {
        _hls = dynamic_pointer_cast<HlsRecorder>(Recorder::createRecorder(Recorder::type_hls, _tuple, option));
    }
    if (option.enable_hls_fmp4) {
        _hls_fmp4 = dynamic_pointer_cast<HlsFMP4Recorder>(Recorder::createRecorder(Recorder::type_hls_fmp4, _tuple, option));
    }
    if (option.enable_mp4) {
        _mp4 = Recorder::createRecorder(Recorder::type_mp4, _tuple, option);
    }
    if (option.enable_ts) {
        _ts = dynamic_pointer_cast<TSMediaSourceMuxer>(Recorder::createRecorder(Recorder::type_ts, _tuple, option));
    }
    if (option.enable_fmp4) {
        _fmp4 = dynamic_pointer_cast<FMP4MediaSourceMuxer>(Recorder::createRecorder(Recorder::type_fmp4, _tuple, option));
    }

    // 音频相关设置  [AUTO-TRANSLATED:6ee58d57]
    // Audio related settings
    enableAudio(option.enable_audio);
    enableMuteAudio(option.add_mute_audio);

    NOTICE_EMIT(BroadcastCreateMuxerArgs, Broadcast::kBroadcastCreateMuxer, _delegate, *this);
}

void MultiMediaSourceMuxer::setMediaListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    setDelegate(listener);

    auto self = shared_from_this();
    // 拦截事件  [AUTO-TRANSLATED:100ca068]
    // Intercept events
    if (_rtmp) {
        _rtmp->setListener(self);
    }
    if (_rtsp) {
        _rtsp->setListener(self);
    }
    if (_ts) {
        _ts->setListener(self);
    }
    if (_fmp4) {
        _fmp4->setListener(self);
    }
    if (_hls_fmp4) {
        _hls_fmp4->setListener(self);
    }
    if (_hls) {
        _hls->setListener(self);
    }
}

void MultiMediaSourceMuxer::setTrackListener(const std::weak_ptr<Listener> &listener) {
    _track_listener = listener;
}

int MultiMediaSourceMuxer::totalReaderCount() const {
    return (_rtsp ? _rtsp->readerCount() : 0) +
           (_rtmp ? _rtmp->readerCount() : 0) +
           (_ts ? _ts->readerCount() : 0) +
           (_fmp4 ? _fmp4->readerCount() : 0) +
           (_mp4 ? _option.mp4_as_player : 0) +
           (_hls ? _hls->readerCount() : 0) +
           (_hls_fmp4 ? _hls_fmp4->readerCount() : 0) +
           (_ring ? _ring->readerCount() : 0);
}

int MultiMediaSourceMuxer::totalReaderCount(MediaSource &sender) {
    auto listener = getDelegate();
    if (!listener) {
        return totalReaderCount();
    }
    try {
        return listener->totalReaderCount(sender);
    } catch (MediaSourceEvent::NotImplemented &) {
        // listener未重载totalReaderCount  [AUTO-TRANSLATED:f098007e]
        // Listener did not reload totalReaderCount
        return totalReaderCount();
    }
}

// 此函数可能跨线程调用  [AUTO-TRANSLATED:e8c5f74d]
// This function may be called across threads
bool MultiMediaSourceMuxer::setupRecord(MediaSource &sender, Recorder::type type, bool start, const string &custom_path, size_t max_second) {
    CHECK(getOwnerPoller(MediaSource::NullMediaSource())->isCurrentThread(), "Can only call setupRecord in it's owner poller");
    onceToken token(nullptr, [&]() {
        if (_option.mp4_as_player && type == Recorder::type_mp4) {
            // 开启关闭mp4录制，触发观看人数变化相关事件  [AUTO-TRANSLATED:b63a8deb]
            // Turn on/off mp4 recording, trigger events related to changes in the number of viewers
            onReaderChanged(sender, totalReaderCount());
        }
    });
    switch (type) {
        case Recorder::type_hls : {
            if (start && !_hls) {
                // 开始录制  [AUTO-TRANSLATED:36d99250]
                // Start recording
                _option.hls_save_path = custom_path;
                auto hls = dynamic_pointer_cast<HlsRecorder>(makeRecorder(type));
                if (hls) {
                    // 设置HlsMediaSource的事件监听器  [AUTO-TRANSLATED:69990c92]
                    // Set the event listener for HlsMediaSource
                    hls->setListener(shared_from_this());
                }
                _hls = hls;
            } else if (!start && _hls) {
                // 停止录制  [AUTO-TRANSLATED:3dee9292]
                // Stop recording
                _hls = nullptr;
            }
            return true;
        }
        case Recorder::type_mp4 : {
#if defined(ENABLE_MP4)
            if (start && _mp4) {
                auto previous = std::dynamic_pointer_cast<AsyncMP4Recorder>(_mp4);
                if (previous && !previous->healthy()) {
                    if (!previous->canReplace()) return false;
                    WarnL << "[recording-recovery] channel=" << _tuple.stream
                          << " action=replace_failed_recorder old_writer_finished=1";
                    _mp4 = nullptr;
                }
            }
#endif
            if (start && !_mp4) {
                // 开始录制  [AUTO-TRANSLATED:36d99250]
                // Start recording
                _option.mp4_save_path = custom_path;
                _option.mp4_max_second = max_second;
                _mp4 = makeRecorder(type);
            } else if (!start && _mp4) {
                // 停止录制  [AUTO-TRANSLATED:3dee9292]
                // Stop recording
                _mp4 = nullptr;
            }
            return true;
        }
        case Recorder::type_hls_fmp4: {
            if (start && !_hls_fmp4) {
                // 开始录制  [AUTO-TRANSLATED:36d99250]
                // Start recording
                _option.hls_save_path = custom_path;
                auto hls = dynamic_pointer_cast<HlsFMP4Recorder>(makeRecorder(type));
                if (hls) {
                    // 设置HlsMediaSource的事件监听器  [AUTO-TRANSLATED:69990c92]
                    // Set the event listener for HlsMediaSource
                    hls->setListener(shared_from_this());
                }
                _hls_fmp4 = hls;
            } else if (!start && _hls_fmp4) {
                // 停止录制  [AUTO-TRANSLATED:3dee9292]
                // Stop recording
                _hls_fmp4 = nullptr;
            }
            return true;
        }
        case Recorder::type_fmp4: {
            if (start && !_fmp4) {
                auto fmp4 = dynamic_pointer_cast<FMP4MediaSourceMuxer>(makeRecorder(type));
                if (fmp4) {
                    fmp4->setListener(shared_from_this());
                }
                _fmp4 = fmp4;
            } else if (!start && _fmp4) {
                _fmp4 = nullptr;
            }
            return true;
        }
        case Recorder::type_ts: {
            if (start && !_ts) {
                auto ts = dynamic_pointer_cast<TSMediaSourceMuxer>(makeRecorder(type));
                if (ts) {
                    ts->setListener(shared_from_this());
                }
                _ts = ts;
            } else if (!start && _ts) {
                _ts = nullptr;
            }
            return true;
        }
        default : return false;
    }
}

std::string MultiMediaSourceMuxer::startRecord(const std::string &file_path, int back_time_ms, int forward_time_ms) {
#if !defined(ENABLE_MP4)
    throw std::invalid_argument("mp4相关功能未打开，请开启ENABLE_MP4宏后编译再测试");
#else
    if (!_ring) {
        throw std::runtime_error("frame gop cache disabled, start record event video failed");
    }
    std::string path;
    if (!start_with(file_path, "/")) {
        path = Recorder::getRecordPath(Recorder::type_mp4, _tuple, _option.mp4_save_path);
        path += file_path;
    } else {
        path = file_path;
    }
    TraceL << "mp4 save path: " << path;

    auto muxer = std::make_shared<AsyncEventMP4>([path](const char* reason) {
        ErrorL << "[event-recording-degraded] path=" << path << " reason=" << reason;
    });
    muxer->openMP4(path);
    for (auto &track : MediaSink::getTracks()) {
        muxer->addTrack(track);
    }
    muxer->addTrackCompleted();

    bool have_history = false;
    if (back_time_ms > 0) {
        // 回溯录制
        std::list<Frame::Ptr> history;
        _ring->flushGop([&](const Frame::Ptr &frame) { history.emplace_back(frame); });
        if (!history.empty()) {
            auto now_dts = history.back()->dts();

            decltype(history)::iterator pos = history.end();
            for (auto it = history.rbegin(); it != history.rend(); ++it) {
                auto &frame = *it;
                if (frame->getTrackType() != TrackVideo || (!frame->configFrame() && !frame->keyFrame())) {
                    continue;
                }
                // 如果视频关键帧到末尾的时长超过一定的时间，那前面的数据应该全部删除
                if (frame->dts() + back_time_ms < now_dts) {
                    pos = it.base();
                    --pos;
                    break;
                }
            }
            if (pos != history.end()) {
                // 移除历史视频前面过多的数据
                DebugL << "clear history front video: " << history.front()->dts() << " -> " << (*pos)->dts();
                history.erase(history.begin(), pos);
            }

            if (forward_time_ms < 0) {
                // 如果后向录制时长为负，说明回溯录制要截取一段尾部
                pos = history.end();
                for (auto it = history.rbegin(); it != history.rend(); ++it) {
                    auto &frame = *it;
                    if (frame->getTrackType() != TrackVideo) {
                        continue;
                    }
                    if (frame->dts() < now_dts + forward_time_ms) {
                        pos = it.base();
                        ++pos;
                        break;
                    }
                }

                if (pos != history.end()) {
                    // 移除历史视频后面过多的数据
                    DebugL << "clear history tail video: " << (*pos)->dts() << " -> " << now_dts;
                    history.erase(pos, history.end());
                }
            }

            if (!history.empty()) {
                auto &front = history.front();
                InfoL << "start record: " << path
                      << ", start_dts: " << front->dts() << ", key_frame: " << front->keyFrame() << ", config_frame: " << front->configFrame()
                      << ", now_dts: " << now_dts;
                have_history = true;
            }

            muxer->inputHistory(history);
        }
    }

    if (forward_time_ms > 0) {
        if (!have_history) {
            InfoL << "start record: " << path << ", back_time_ms: " << back_time_ms << ", forward_time_ms: " << forward_time_ms;
        }

        weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();
        MediaTuple tuple = _tuple; // 완료 훅에 넣을 vhost/app/stream (RecordInfo 기반 클립 식별용)
        auto lam = [weak_self, muxer, forward_time_ms, have_history, path, tuple]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            uint64_t now_dts = 0;
            int selected_index = -1;
            Ticker ticker;
            bool is_live_stream = strong_self->_dur_sec < 0.01;
            auto reader = strong_self->_ring->attach(strong_self->MultiMediaSourceMuxer::getOwnerPoller(MediaSource::NullMediaSource()), !have_history, 1);
            // All callbacks below run on this reader's owner poller. A separate
            // deadline is essential: a stopped camera cannot trigger a frame callback.
            auto finalized = std::make_shared<bool>(false);
            auto finish = std::make_shared<std::function<void(const char*)>>(
                [muxer, path, tuple, finalized](const char* reason) {
                    if (*finalized) return;
                    *finalized = true;
                    if (std::string(reason) != "duration")
                        WarnL << "[event-recording-truncated] stream=" << tuple.shortUrl() << " path=" << path << " reason=" << reason;
                    if (!muxer->finish([path, tuple](float seconds) { emitEventClipRecorded(path, tuple, seconds); }))
                        ErrorL << "[event-recording-finalize-rejected] stream=" << tuple.shortUrl() << " path=" << path;
                });
            reader->setReadCB([muxer, now_dts, selected_index, forward_time_ms, reader, path, tuple, ticker, is_live_stream, finish](const Frame::Ptr &frame) mutable {
                if (!reader) {
                    // 已经关闭录制
                    return;
                }
                // 循环引用自身
                if (!now_dts) {
                    now_dts = frame->dts();
                    selected_index = frame->getIndex();
                }
                // 新增兜底机制，如果直播录制任务时长超过预期时间3秒，不管数据时间戳是否增长是否达到预期，都强制停止录制
                if ((frame->getIndex() == selected_index && now_dts + forward_time_ms < frame->dts())
                    || (is_live_stream && ticker.createdTime() > forward_time_ms + 3000ULL)) {
                    InfoL << "stop record: " << path << ", end dts: " << frame->dts();
                    // closeMP4 전에 길이를 구하고, close 후 이벤트 클립 완료 훅을 emit (연속녹화와 동일 이벤트, 노드는 경로로 구분).
                    (*finish)("duration");
                    reader = nullptr;
                    return;
                }
                if (!muxer->inputFrame(frame)) reader = nullptr;
            });
            std::weak_ptr<RingType::RingReader> weak_reader = reader;
            reader->setDetachCB([weak_reader, finish]() {
                (*finish)("stream_detached");
                if (auto strong_reader = weak_reader.lock()) {
                    // 防止循环引用
                    strong_reader->setReadCB(nullptr);
                }
            });
            // Weak captures avoid retaining a completed writer/worker until the deadline.
            std::weak_ptr<std::function<void(const char*)>> weak_finish = finish;
            if (is_live_stream) {
                strong_self->MultiMediaSourceMuxer::getOwnerPoller(MediaSource::NullMediaSource())->doDelayTask(
                    static_cast<uint64_t>(forward_time_ms) + 3000ULL, [weak_reader, weak_finish]() -> uint64_t {
                        if (auto complete = weak_finish.lock()) (*complete)("wall_clock_deadline");
                        if (auto active = weak_reader.lock()) active->setReadCB(nullptr);
                        return 0;
                    });
            }
        };
        if (back_time_ms >= 0) {
            // 立即前向录制
            lam();
        } else {
            // 延时启动录制
            MultiMediaSourceMuxer::getOwnerPoller(MediaSource::NullMediaSource())->doDelayTask(-back_time_ms, [lam]() {
                lam();
                return 0;
            });
        }
    } else if (have_history) {
        // forward 없는 순수 back(pre-only) 클립: 히스토리만 기록됐으므로 지금 닫고 완료 훅을 emit.
        // (forward>0 경로는 위에서 reader 종료 시 emit. history 가 없으면 빈 파일이므로 emit 안 함.)
        MediaTuple tuple = _tuple;
        muxer->finish([path, tuple](float time_len) {
            emitEventClipRecorded(path, tuple, time_len);
        });
    }

    return path;
#endif
}

// 此函数可能跨线程调用  [AUTO-TRANSLATED:e8c5f74d]
// This function may be called across threads
bool MultiMediaSourceMuxer::isRecording(Recorder::type type) {
    switch (type) {
        case Recorder::type_hls: return !!_hls;
        case Recorder::type_mp4:
#if defined(ENABLE_MP4)
            if (auto recorder = std::dynamic_pointer_cast<AsyncMP4Recorder>(_mp4)) return recorder->healthy();
#endif
            return !!_mp4;
        case Recorder::type_hls_fmp4: return !!_hls_fmp4;
        case Recorder::type_fmp4: return !!_fmp4;
        case Recorder::type_ts: return !!_ts;
        default: return false;
    }
}

void MultiMediaSourceMuxer::startSendRtp(const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t, const toolkit::SockException &)> cb) {
#if defined(ENABLE_RTPPROXY)
    createGopCacheIfNeed();

    auto ring = _ring;
    auto ssrc = args.ssrc;
    auto ssrc_multi_send = args.ssrc_multi_send;
    auto tracks = getTracks(false);
    auto poller = getOwnerPoller(MediaSource::NullMediaSource());
    auto rtp_sender = std::make_shared<RtpSender>(poller);

    weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();

    rtp_sender->setOnClose([weak_self, ssrc](const toolkit::SockException &ex) {
        if (auto strong_self = weak_self.lock()) {
            // 可能归属线程发生变更  [AUTO-TRANSLATED:2b379e30]
            // The owning thread may change
            strong_self->getOwnerPoller(MediaSource::NullMediaSource())->async([=]() {
                WarnL << "stream:" << strong_self->shortUrl() << " stop send rtp:" << ssrc << ", reason:" << ex;
                strong_self->_rtp_sender.erase(ssrc);
                NOTICE_EMIT(BroadcastSendRtpStoppedArgs, Broadcast::kBroadcastSendRtpStopped, *strong_self, ssrc, ex);
            });
        }
    });

    rtp_sender->startSend(*this, args, [ssrc,ssrc_multi_send, weak_self, rtp_sender, cb, tracks, ring, poller](uint16_t local_port, const SockException &ex) mutable {
        cb(local_port, ex);
        auto strong_self = weak_self.lock();
        if (!strong_self || ex) {
            return;
        }

        for (auto &track : tracks) {
            rtp_sender->addTrack(track);
        }
        rtp_sender->addTrackCompleted();

        auto reader = ring->attach(poller);
        reader->setReadCB([rtp_sender](const Frame::Ptr &frame) {
            rtp_sender->inputFrame(frame);
        });

        // 可能归属线程发生变更  [AUTO-TRANSLATED:2b379e30]
        // The owning thread may change
        strong_self->getOwnerPoller(MediaSource::NullMediaSource())->async([=]() {
            if (!ssrc_multi_send) {
                strong_self->_rtp_sender.erase(ssrc);
            }
            std::weak_ptr<RtpSender> sender = rtp_sender;
            strong_self->_rtp_sender.emplace(ssrc, make_tuple(reader, sender));
        });
    });
#else
    cb(0, SockException(Err_other, "该功能未启用，编译时请打开ENABLE_RTPPROXY宏"));
#endif//ENABLE_RTPPROXY
}

bool MultiMediaSourceMuxer::stopSendRtp(const string &ssrc) {
#if defined(ENABLE_RTPPROXY)
    if (ssrc.empty()) {
        // 关闭全部  [AUTO-TRANSLATED:ffaadfda]
        // Close all
        auto size = _rtp_sender.size();
        _rtp_sender.clear();
        return size;
    }
    // 关闭特定的  [AUTO-TRANSLATED:2286322a]
    // Close specific
    return _rtp_sender.erase(ssrc);
#else
    return false;
#endif//ENABLE_RTPPROXY
}

MultiMediaSourceMuxer::RingType::RingReader::Ptr MultiMediaSourceMuxer::getFrameReader() {
    auto poller = getOwnerPoller(MediaSource::NullMediaSource());
    CHECK(poller->isCurrentThread());
    createGopCacheIfNeed();
    return _ring->attach(poller);
}

EventPoller::Ptr MultiMediaSourceMuxer::getOwnerPoller(MediaSource &sender) {
    auto listener = getDelegate();
    if (!listener) {
        return _poller;
    }
    try {
        auto ret = listener->getOwnerPoller(sender);
        if (ret != _poller) {
            WarnL << "OwnerPoller changed " << _poller->getThreadName() << " -> " << ret->getThreadName() << " : " << shortUrl();
            _poller = ret;
            if (_paced_sender) {
                _paced_sender->resetTimer(_poller);
            }
        }
        return ret;
    } catch (MediaSourceEvent::NotImplemented &) {
        // listener未重载getOwnerPoller  [AUTO-TRANSLATED:0ebf2e53]
        // Listener did not reload getOwnerPoller
        return _poller;
    }
}

bool MultiMediaSourceMuxer::close(MediaSource &sender) {
    MediaSourceEventInterceptor::close(sender);
    _rtmp = nullptr;
    _rtsp = nullptr;
    _fmp4 = nullptr;
    _ts = nullptr;
    _mp4 = nullptr;
    _hls = nullptr;
    _hls_fmp4 = nullptr;
#if defined(ENABLE_RTPPROXY)
    _rtp_sender.clear();
#endif // ENABLE_RTPPROXY
    return true;
}

std::shared_ptr<MultiMediaSourceMuxer> MultiMediaSourceMuxer::getMuxer(MediaSource &sender) const {
    return const_cast<MultiMediaSourceMuxer*>(this)->shared_from_this();
}

bool MultiMediaSourceMuxer::onTrackReady(const Track::Ptr &track) {
    auto &stamp = _stamps[track->getIndex()];
    if (_dur_sec > 0.01) {
        // 点播  [AUTO-TRANSLATED:f0b0f74a]
        // On-demand
        stamp.setPlayBack();
    }

    bool ret = false;
    if (_rtmp) {
        ret = _rtmp->addTrack(track) ? true : ret;
    }
    if (_rtsp) {
        ret = _rtsp->addTrack(track) ? true : ret;
    }
    if (_ts) {
        ret = _ts->addTrack(track) ? true : ret;
    }
    if (_fmp4) {
        ret = _fmp4->addTrack(track) ? true : ret;
    }
    if (_hls) {
        ret = _hls->addTrack(track) ? true : ret;
    }
    if (_hls_fmp4) {
        ret = _hls_fmp4->addTrack(track) ? true : ret;
    }
    if (_mp4) {
        ret = _mp4->addTrack(track) ? true : ret;
    }
    if (_delegate) {
        _delegate->addTrack(track);
    }
    return ret;
}

void MultiMediaSourceMuxer::onAllTrackReady() {
    CHECK(!_create_in_poller || getOwnerPoller(MediaSource::NullMediaSource())->isCurrentThread());

    if (_option.paced_sender_ms) {
        std::weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();
        _paced_sender = std::make_shared<FramePacedSender>(_option.paced_sender_ms, [weak_self](const Frame::Ptr &frame) {
            if (auto strong_self = weak_self.lock()) {
                strong_self->onTrackFrame_l(frame);
            }
        });
    }

    setMediaListener(getDelegate());

    if (_rtmp) {
        _rtmp->addTrackCompleted();
    }
    if (_rtsp) {
        _rtsp->addTrackCompleted();
    }
    if (_ts) {
        _ts->addTrackCompleted();
    }
    if (_mp4) {
        _mp4->addTrackCompleted();
    }
    if (_fmp4) {
        _fmp4->addTrackCompleted();
    }
    if (_hls) {
        _hls->addTrackCompleted();
    }
    if (_hls_fmp4) {
        _hls_fmp4->addTrackCompleted();
    }

    auto listener = _track_listener.lock();
    if (listener) {
        listener->onAllTrackReady();
    }

    createGopCacheIfNeed();
    Stamp *first = nullptr;
    for (auto &pr : _stamps) {
        if (!first) {
            first = &pr.second;
        } else {
            pr.second.syncTo(*first);
        }
    }
    if (_delegate) {
        _delegate->addTrackCompleted();
    }
    InfoL << "stream: " << shortUrl() << " , codec info: " << getTrackInfoStr(this);
}

void MultiMediaSourceMuxer::createGopCacheIfNeed() {
    if (_ring) {
        return;
    }
    GET_CONFIG(size_t, gop_cache, RtpProxy::kGopCache);
    // 프리버퍼(링버퍼) 최대 프레임 수: 예전엔 1024 하드코딩. 이벤트 클립 pre-event 캐시가 이 링을 재사용하므로 설정값으로 뺀다.
    // 실제 보관 시간 = min(이 프레임 상한, gop_cache 개수만큼의 GOP). pre-event N초 확보하려면 둘 다 키워야 함(config.ini 참고).
    GET_CONFIG(size_t, event_pre_buffer_frames, Record::kEventPreBufferFrames);
    auto ring_max_size = std::max<size_t>(event_pre_buffer_frames, 1);
    weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();
    auto src = std::make_shared<MediaSourceForMuxer>(weak_self.lock());
    _ring = std::make_shared<RingType>(ring_max_size, [weak_self, src](int size) {
        if (auto strong_self = weak_self.lock()) {
            // 切换到归属线程  [AUTO-TRANSLATED:abcf859b]
            // Switch to the owning thread
            strong_self->getOwnerPoller(MediaSource::NullMediaSource())->async([=]() {
                strong_self->onReaderChanged(*src, strong_self->totalReaderCount());
            });
        }
    }, std::max<size_t>(gop_cache, 1));
}

void MultiMediaSourceMuxer::resetTracks() {
    MediaSink::resetTracks();

    if (_rtmp) {
        _rtmp->resetTracks();
    }
    if (_rtsp) {
        _rtsp->resetTracks();
    }
    if (_ts) {
        _ts->resetTracks();
    }
    if (_fmp4) {
        _fmp4->resetTracks();
    }
    if (_hls_fmp4) {
        _hls_fmp4->resetTracks();
    }
    if (_hls) {
        _hls->resetTracks();
    }
    if (_mp4) {
        _mp4->resetTracks();
    }
}

void MultiMediaSourceMuxer::addProbe(uint32_t probe_ms, const std::function<void(const std::list<FrameInfo> &info_list)> &cb) {
    CHECK(getOwnerPoller(MediaSource::NullMediaSource())->isCurrentThread());
    auto info_list = std::make_shared<std::list<FrameInfo>>();
    Ticker ticker;
    _on_frame = [info_list, ticker](const Frame::Ptr &frame) mutable {
        FrameInfo info;
        info.codec_id = frame->getCodecId();
        info.dts = frame->dts();
        info.pts = frame->pts();
        info.recv_stamp = ticker.createdTime();
        info.frame_size = frame->size();
        info.index = frame->getIndex();
        info.key_frame = frame->keyFrame();
        info.config_frame = frame->configFrame();
        info_list->emplace_back(info);
    };
    std::weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();
    getOwnerPoller(MediaSource::NullMediaSource())->doDelayTask(probe_ms, [weak_self, cb, info_list]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->_on_frame = nullptr;
        }
        cb(*info_list);
        return 0;
    });
}

bool MultiMediaSourceMuxer::onTrackFrame(const Frame::Ptr &frame_in) {
    if (_on_frame) {
        _on_frame(frame_in);
    }
    auto frame = frame_in;
    if (_option.modify_stamp != ProtocolOption::kModifyStampOff) {
        // 时间戳不采用原始的绝对时间戳  [AUTO-TRANSLATED:8beb3bf7]
        // Timestamp does not use the original absolute timestamp
        frame = std::make_shared<FrameStamp>(frame, _stamps[frame->getIndex()], _option.modify_stamp);
    }
    return _paced_sender ? _paced_sender->inputFrame(frame) : onTrackFrame_l(frame);
}

bool MultiMediaSourceMuxer::onTrackFrame_l(const Frame::Ptr &frame_in) {
    auto frame = frame_in;
    bool ret = false;
    if (_rtmp) {
        ret = _rtmp->inputFrame(frame) ? true : ret;
    }
    if (_rtsp) {
        ret = _rtsp->inputFrame(frame) ? true : ret;
    }
    if (_ts) {
        ret = _ts->inputFrame(frame) ? true : ret;
    }

    if (_hls) {
        ret = _hls->inputFrame(frame) ? true : ret;
    }

    if (_hls_fmp4) {
        ret = _hls_fmp4->inputFrame(frame) ? true : ret;
    }

    if (_mp4) {
        ret = _mp4->inputFrame(frame) ? true : ret;
    }
    if (_fmp4) {
        ret = _fmp4->inputFrame(frame) ? true : ret;
    }
    if (_delegate) {
        _delegate->inputFrame(frame);
    }
    if (_ring) {
        // 此场景由于直接转发，可能存在切换线程引起的数据被缓存在管道，所以需要CacheAbleFrame  [AUTO-TRANSLATED:528afbb7]
        // In this scenario, due to direct forwarding, there may be data cached in the pipeline due to thread switching, so CacheAbleFrame is needed
        frame = Frame::getCacheAbleFrame(frame);
        if (frame->getTrackType() == TrackVideo) {
            // 视频时，遇到第一帧配置帧或关键帧则标记为gop开始处  [AUTO-TRANSLATED:66247aa8]
            // When it is a video, if the first frame configuration frame or key frame is encountered, it is marked as the beginning of the GOP
            auto video_key_pos = frame->keyFrame() || frame->configFrame();
            _ring->write(frame, video_key_pos && !_video_key_pos);
            if (!frame->dropAble()) {
                _video_key_pos = video_key_pos;
            }
        } else {
            // 没有视频时，设置is_key为true，目的是关闭gop缓存  [AUTO-TRANSLATED:f3223755]
            // When there is no video, set is_key to true to disable gop caching
            _ring->write(frame, !haveVideo());
        }
    }
    return ret;
}

bool MultiMediaSourceMuxer::isEnabled(){
    GET_CONFIG(uint32_t, stream_none_reader_delay_ms, General::kStreamNoneReaderDelayMS);
    if (!_is_enable || _last_check.elapsedTime() > stream_none_reader_delay_ms) {
        // 无人观看时，每次检查是否真的无人观看  [AUTO-TRANSLATED:48bc59c6]
        // When no one is watching, check each time if there is really no one watching
        // 有人观看时，则延迟一定时间检查一遍是否无人观看了(节省性能)  [AUTO-TRANSLATED:a7dfddc4]
        // When someone is watching, check again after a certain delay to see if no one is watching (save performance)
        _is_enable = (_rtmp ? _rtmp->isEnabled() : false) ||
                     (_rtsp ? _rtsp->isEnabled() : false) ||
                     (_ts ? _ts->isEnabled() : false) ||
                     (_fmp4 ? _fmp4->isEnabled() : false) ||
                     (_ring ? (bool)_ring->readerCount() : false)  ||
                     (_hls ? _hls->isEnabled() : false) ||
                     (_hls_fmp4 ? _hls_fmp4->isEnabled() : false) ||
                     _mp4;

        if (_is_enable) {
            // 无人观看时，不刷新计时器,因为无人观看时每次都会检查一遍，所以刷新计数器无意义且浪费cpu  [AUTO-TRANSLATED:03ab47cf]
            // When no one is watching, do not refresh the timer, because each time no one is watching, it will be checked, so refreshing the counter is meaningless and wastes cpu
            _last_check.resetTime();
        }
    }
    return _is_enable;
}

}//namespace mediakit
