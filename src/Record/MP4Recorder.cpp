/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifdef ENABLE_MP4
#include <cerrno>
#include <ctime>
#include <sys/stat.h>
#if defined(_WIN32)
#include <sys/utime.h>
#else
#include <utime.h>
#endif
#include "Util/File.h"
#include "Common/config.h"
#include "MP4Recorder.h"
#include "Thread/WorkThreadPool.h"
#include "MP4Muxer.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

void setRecordFileMtime(const std::string &path, time_t mtime) {
#if defined(_WIN32)
    struct _utimbuf times;
    times.actime = mtime;
    times.modtime = mtime;
    const int ret = _utime(path.data(), &times);
#else
    struct utimbuf times;
    times.actime = mtime;
    times.modtime = mtime;
    const int ret = utime(path.data(), &times);
#endif
    if (ret != 0) {
        // 파일은 이미 정상 게시됐다. mtime 보정만 실패하면 재생 End 가 쓰기 완료 시각으로 남는다(재생은 가능).
        WarnL << "[recording-mtime-failed] path=" << path << " mtime=" << mtime << " errno=" << errno;
    }
}

time_t recordFileEndTime(const std::string &path, time_t start, float pts_seconds, time_t last_ingest) {
    const time_t by_pts = start + static_cast<time_t>(pts_seconds + 0.5f);
    time_t end = by_pts;
    if (last_ingest && last_ingest + 1 < end) end = last_ingest + 1;
    const time_t now = ::time(NULL);
    if (end > now) end = now; // 어떤 경우에도 미래 시각은 쓰지 않는다
    if (end < start) end = start;
    if (by_pts - end >= 5) {
        WarnL << "[recording-end-clamped] path=" << path << " start=" << start << " pts_seconds=" << pts_seconds
              << " last_ingest=" << last_ingest << " end=" << end << " (pts 길이가 수신 시간보다 " << (by_pts - end)
              << "초 김 — 카메라 클럭이 실시간보다 빠름)";
    }
    return end;
}

MP4Recorder::MP4Recorder(const MediaTuple &tuple, const string &path, size_t max_second, bool inline_close) : _inline_close(inline_close) {
    // ///record 业务逻辑//////  [AUTO-TRANSLATED:2e78931a]
    // ///record Business Logic//////
    static_cast<MediaTuple &>(_info) = tuple;
    _info.folder = path;
    GET_CONFIG(uint32_t, s_max_second, Protocol::kMP4MaxSecond);
    _max_second = max_second ? max_second : s_max_second;
}

MP4Recorder::~MP4Recorder() {
    try {
        flush();
        closeFile();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MP4Recorder::createFile() {
    closeFile();
    // 파일명·날짜 폴더·start_time 은 첫 프레임의 "수신" 벽시계로 정한다. 처리 시각(::time)을 쓰면 비동기 쓰기 큐가
    // SMR 정지로 밀린 만큼 세그먼트가 늦게 표시되고(백엔드 재생은 파일명 앞 19자=Start), 자정 근처엔 날짜 폴더도 틀린다.
    const time_t start = _ingest_wall ? _ingest_wall : ::time(NULL);
    _last_ingest_wall = 0; // 새 파일(이전 파일 마감은 위 closeFile 에서 이미 값을 가져갔다)
    auto date = getTimeStr("%Y-%m-%d", start);
    auto file_name = date + "-" + getTimeStr("%H-%M-%S", start) + "-" + std::to_string(_file_index++) + ".mp4";
    auto full_path = _info.folder + date + "/" + file_name;
    auto full_path_tmp = _info.folder + date + "/." + file_name;

    // ///record 业务逻辑//////  [AUTO-TRANSLATED:2e78931a]
    // ///record Business Logic//////
    _info.start_time = start;
    _info.file_name = file_name;
    _info.file_path = full_path;
    GET_CONFIG(string, appName, Record::kAppName);
    _info.url = appName + "/" + _info.app + "/" + _info.stream + "/" + date + "/" + file_name;

    try {
        _muxer = std::make_shared<MP4Muxer>();
        TraceL << "Open tmp mp4 file: " << full_path_tmp;
        _muxer->openMP4(full_path_tmp);
        for (auto &track :_tracks) {
            // 添加track  [AUTO-TRANSLATED:80ae762a]
            // Add track
            _muxer->addTrack(track);
        }
        _full_path_tmp = full_path_tmp;
    } catch (std::exception &ex) {
        WarnL << ex.what();
        _muxer = nullptr;
        if (_inline_close) throw;
    }
}

void MP4Recorder::asyncClose() {
    auto muxer = std::move(_muxer);
    auto full_path_tmp = _full_path_tmp;
    auto info = _info;
    const time_t last_ingest = _last_ingest_wall;
    TraceL << "Start close tmp mp4 file: " << full_path_tmp;
    auto finish = [muxer, full_path_tmp, info, last_ingest]() mutable {
        info.time_len = muxer->getDuration() / 1000.0f;
        // 关闭mp4可能非常耗时，所以要放在后台线程执行  [AUTO-TRANSLATED:a7378a11]
        // Closing mp4 can be very time-consuming, so it should be executed in the background thread
        TraceL << "Closing tmp mp4 file: " << full_path_tmp;
        muxer->closeMP4();
        TraceL << "Closed tmp mp4 file: " << full_path_tmp;
        if (!full_path_tmp.empty()) {
            // 获取文件大小  [AUTO-TRANSLATED:7b90eb41]
            // Get file size
            info.file_size = File::fileSize(full_path_tmp);
            if (info.file_size < 1024) {
                // 录像文件太小，删除之  [AUTO-TRANSLATED:923d27c3]
                // The recording file is too small, delete it
                File::delete_file(full_path_tmp);
                return;
            }
            // 临时文件名改成正式文件名，防止mp4未完成时被访问  [AUTO-TRANSLATED:541a6f00]
            // Change the temporary file name to the official file name to prevent access to the mp4 before it is completed
            if (rename(full_path_tmp.data(), info.file_path.data()) != 0) {
                ErrorL << "[recording-finalize-failed] source=" << full_path_tmp << " destination=" << info.file_path;
                return; // Never announce a file that was not successfully published.
            }
            // End(mtime) = min(시작 + 길이, 마지막 수신 + 1초, 지금). 마감(쓰기 완료)이 큐 지연으로 늦어도 재생 타임라인
            // End 가 밀리지 않게, 카메라 클럭이 빨라도 다음 세그먼트 Start 를 넘거나 미래가 되지 않게.
            setRecordFileMtime(info.file_path, recordFileEndTime(info.file_path, info.start_time, info.time_len, last_ingest));
        }
        TraceL << "Emit mp4 record event: " << info.file_path;
        // 触发mp4录制切片生成事件  [AUTO-TRANSLATED:9959dcd4]
        // Trigger mp4 recording slice generation event
        NOTICE_EMIT(BroadcastRecordMP4Args, Broadcast::kBroadcastRecordMP4, info);
    };
    // The bounded recording worker already owns this call. Do not enqueue
    // unlimited finalizers on a shared pool if a storage device stops responding.
    if (_inline_close) finish();
    else WorkThreadPool::Instance().getExecutor()->async(std::move(finish));
}

void MP4Recorder::closeFile() {
    if (_muxer) {
        asyncClose();
        _muxer = nullptr;
    }
}

void MP4Recorder::flush() {
    if (_muxer) {
        _muxer->flush();
    }
}

bool MP4Recorder::inputFrame(const Frame::Ptr &frame, time_t ingest_wall) {
    _ingest_wall = ingest_wall;
    const bool ret = inputFrame(frame);
    if (_muxer && ingest_wall) _last_ingest_wall = ingest_wall; // 새 파일이 열렸다면 그 파일의 값
    _ingest_wall = 0; // 다음 일반 입력이 옛 수신 시각을 쓰지 않게
    return ret;
}

bool MP4Recorder::inputFrame(const Frame::Ptr &frame) {
    auto stamp_inc = _delta_stamp[frame->getTrackType()].relativeStamp(frame->pts(), false);
    if (!_muxer || (stamp_inc > int64_t(_max_second) * 1000 && (!_have_video || frame->keyFrame()))) {
        // 成立条件  [AUTO-TRANSLATED:8c9c6083]
        // Conditions for establishment
        // 1、_muxer为空  [AUTO-TRANSLATED:fa236097]
        // 1. _muxer is empty
        // 2、到了切片时间，并且只有音频  [AUTO-TRANSLATED:212e9d23]
        // 2. It's time to slice, and there is only audio
        // 3、到了切片时间，有视频并且遇到视频的关键帧  [AUTO-TRANSLATED:fa4a71ad]
        // 3. It's time to slice, there is video and a video keyframe is encountered
        createFile();
        for (auto &ref : _delta_stamp) {
            ref.reset();
        }
    }

    if (_muxer) {
        // 生成mp4文件  [AUTO-TRANSLATED:76a8d77c]
        // Generate mp4 file
        return _muxer->inputFrame(frame);
    }
    return false;
}

bool MP4Recorder::addTrack(const Track::Ptr &track) {
    // 保存所有的track，为创建MP4MuxerFile做准备  [AUTO-TRANSLATED:815c2486]
    // Save all tracks in preparation for creating MP4MuxerFile
    _tracks.emplace_back(track);
    if (track->getTrackType() == TrackVideo) {
        _have_video = true;
    }
    return true;
}

void MP4Recorder::resetTracks() {
    closeFile();
    _tracks.clear();
    _have_video = false;
}

} /* namespace mediakit */

#endif //ENABLE_MP4
