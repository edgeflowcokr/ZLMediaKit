/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MP4MAKER_H_
#define MP4MAKER_H_

#include <mutex>
#include <memory>
#include "Common/MediaSink.h"
#include "Record/Recorder.h"
#include "MP4Muxer.h"

namespace mediakit {

#ifdef ENABLE_MP4
class MP4Muxer;

// 녹화 파일 mtime 을 지정 시각으로 맞춘다(실패 시 WarnL). 재생 타임라인은 mtime 을 세그먼트 End 로 쓰므로,
// 비동기 쓰기 큐가 밀려도 End 가 쓰기 완료 시각이 아니라 "시작 + 길이"(마지막 프레임 수신 시각)가 되게 한다.
void setRecordFileMtime(const std::string &path, time_t mtime);
// 녹화 파일 End 시각 = min(시작 + pts 길이, 마지막 프레임 수신 시각 + 1초, 현재 시각). 카메라 RTP 클럭이 실시간보다
// 빠르면 pts 길이가 실제보다 길어 End 가 다음 세그먼트 Start 를 넘거나 미래가 되므로 수신 시각으로 제한한다.
// last_ingest=0 이면 수신 제한 없이 현재 시각만 적용. 제한 폭이 5초 이상이면 경고 로그 1회(파일당).
time_t recordFileEndTime(const std::string &path, time_t start, float pts_seconds, time_t last_ingest);

class MP4Recorder final : public MediaSinkInterface {
public:
    using Ptr = std::shared_ptr<MP4Recorder>;

    MP4Recorder(const MediaTuple &tuple, const std::string &path, size_t max_second, bool inline_close = false);
    ~MP4Recorder() override;
    // Called on the bounded worker before releasing its final ownership.
    void finish() { try { flush(); } catch (...) { closeFile(); throw; } closeFile(); }

    /**
     * 重置所有Track
     * Reset all Tracks
     
     * [AUTO-TRANSLATED:8dd80826]
     */
    void resetTracks() override;

    /**
     * 输入frame
     * Input frame
     
     * [AUTO-TRANSLATED:3722ea0e]
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * 수신 벽시계(초)를 함께 넘기는 입력. 새 파일의 파일명·날짜 폴더·start_time 을 처리 시각이 아니라 이 값으로
     * 정한다(비동기 큐가 SMR 정지로 수십 초 밀려도 세그먼트 시각이 어긋나지 않게). 로테이션은 원래대로 pts 기준.
     */
    bool inputFrame(const Frame::Ptr &frame, time_t ingest_wall);

    /**
     * 刷新输出所有frame缓存
     * Refresh output all frame cache
     
     * [AUTO-TRANSLATED:adaea568]
     */
    void flush() override;

    /**
     * 添加ready状态的track
     * Add ready state track
     
     
     * [AUTO-TRANSLATED:2d8138b3]
     */
    bool addTrack(const Track::Ptr & track) override;

private:
    void createFile();
    void closeFile();
    void asyncClose();

private:
    bool _inline_close = false;
    bool _have_video = false;
    size_t _max_second;
    time_t _ingest_wall = 0; // 현재 입력 프레임의 수신 벽시계(0=모름 → 처리 시각)
    time_t _last_ingest_wall = 0; // 현재 파일의 마지막 프레임 수신 벽시계(End 시각 제한용, 0=모름)
    DeltaStamp _delta_stamp[TrackMax];
    std::atomic<uint64_t> _file_index { 0 };
    std::string _full_path_tmp;
    RecordInfo _info;
    MP4Muxer::Ptr _muxer;
    std::list<Track::Ptr> _tracks;
};

#endif ///ENABLE_MP4

} /* namespace mediakit */

#endif /* MP4MAKER_H_ */
