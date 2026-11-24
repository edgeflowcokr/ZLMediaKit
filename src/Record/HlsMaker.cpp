/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <cstdio>
#include <ctime>
#include <iomanip>
#include "HlsMaker.h"
#include "Common/config.h"

using namespace std;

namespace mediakit {

HlsMaker::HlsMaker(bool is_fmp4, float seg_duration, uint32_t seg_number, bool seg_keep) {
    _is_fmp4 = is_fmp4;
    // 最小允许设置为0，0个切片代表点播  [AUTO-TRANSLATED:19235e8e]
    // Minimum allowed setting is 0, 0 slices represent on-demand
    _seg_number = seg_number;
    _seg_duration = seg_duration;
    _seg_keep = seg_keep;
}

// UTC epoch ms -> ISO 8601(밀리초, Z). HLS EXT-X-PROGRAM-DATE-TIME 형식(EdgeFlowAI).
static std::string formatProgramDate(int64_t ms) {
    time_t sec = (time_t)(ms / 1000);
    struct tm t {};
#if defined(_WIN32)
    gmtime_s(&t, &sec);
#else
    gmtime_r(&sec, &t);
#endif
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
             t.tm_sec, (int)(ms % 1000));
    return buf;
}

void HlsMaker::noteOriginStamp(uint64_t stamp, uint64_t origin) {
    if (!_pdt_base_ms) {
        return;
    }
    if (!_pdt_has_first) {
        _pdt_has_first = true;
        _pdt_first_origin = origin;
    }
    _pdt_anchors.emplace_back(stamp, origin);
    while (_pdt_anchors.size() > 16) {
        _pdt_anchors.pop_front();
    }
}

int64_t HlsMaker::programDateFor(uint64_t stamp) const {
    if (!_pdt_base_ms || _pdt_anchors.empty()) {
        return -1;
    }
    // 이 조각 키프레임의 짝(없으면 그 직전 짝)에서 원본 시간표를 구한다. 출력 시간표 차이는 짝 사이 보간(대개 0).
    auto anchor = _pdt_anchors.front();
    for (auto it = _pdt_anchors.rbegin(); it != _pdt_anchors.rend(); ++it) {
        if (it->first <= stamp) {
            anchor = *it;
            break;
        }
    }
    int64_t origin = (int64_t)anchor.second + ((int64_t)stamp - (int64_t)anchor.first);
    int64_t offset = origin - (int64_t)_pdt_first_origin;
    if (offset < 0) {
        // 원본 시간표가 되감기면(녹화기 재시작 등) 틀린 시각을 쓰지 않고 표기를 뺀다.
        return -1;
    }
    return (int64_t)_pdt_base_ms + offset;
}

void HlsMaker::makeIndexFile(bool include_delay, bool eof) {
    GET_CONFIG(uint32_t, segDelay, Hls::kSegmentDelay);
    GET_CONFIG(uint32_t, segRetain, Hls::kSegmentRetain);
    std::deque<std::tuple<int, std::string>> temp(_seg_dur_list);
    if (!include_delay && _seg_number) {
        while (temp.size() > _seg_number) {
            temp.pop_front();
        }
    }
    int maxSegmentDuration = 0;
    for (auto &tp : temp) {
        int dur = std::get<0>(tp);
        if (dur > maxSegmentDuration) {
            maxSegmentDuration = dur;
        }
    }
    uint64_t index_seq;
    if (_seg_number) {
        if (include_delay) {
            if (_file_index > _seg_number + segDelay) {
                index_seq = _file_index - _seg_number - segDelay;
            } else {
                index_seq = 0LL;
            }
        } else {
            if (_file_index > _seg_number) {
                index_seq = _file_index - _seg_number;
            } else {
                index_seq = 0LL;
            }
        }
    } else {
        index_seq = 0LL;
    }

    string index_str;
    index_str.reserve(2048);
    index_str += "#EXTM3U\n";
    index_str += (_is_fmp4 ? "#EXT-X-VERSION:7\n" : "#EXT-X-VERSION:4\n");
    if (_seg_number == 0) {
        index_str += "#EXT-X-PLAYLIST-TYPE:EVENT\n";
    } else {
        index_str += "#EXT-X-ALLOW-CACHE:NO\n";
    }
    index_str += "#EXT-X-TARGETDURATION:" + std::to_string((maxSegmentDuration + 999) / 1000) + "\n";
    index_str += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(index_seq) + "\n";
    if (_is_fmp4) {
        index_str += "#EXT-X-MAP:URI=\"init.mp4\"\n";
    }

    stringstream ss;
    // _seg_pdt_list 는 _seg_dur_list 와 길이가 같고, temp 는 그 뒤쪽 부분이다(EdgeFlowAI 녹화 시각 표기).
    size_t pdt_index = _seg_pdt_list.size() >= temp.size() ? _seg_pdt_list.size() - temp.size() : _seg_pdt_list.size();
    for (auto &tp : temp) {
        if (pdt_index < _seg_pdt_list.size()) {
            auto pdt = _seg_pdt_list[pdt_index++];
            if (pdt >= 0) {
                ss << "#EXT-X-PROGRAM-DATE-TIME:" << formatProgramDate(pdt) << "\n";
            }
        }
        ss << "#EXTINF:" << std::setprecision(3) << std::get<0>(tp) / 1000.0 << ",\n" << std::get<1>(tp) << "\n";
    }
    index_str += ss.str();

    if (eof) {
        index_str += "#EXT-X-ENDLIST\n";
    }
    onWriteHls(index_str, include_delay);
}

void HlsMaker::inputInitSegment(const char *data, size_t len) {
    if (!_is_fmp4) {
        throw std::invalid_argument("Only fmp4-hls can input init segment");
    }
    onWriteInitSegment(data, len);
}

void HlsMaker::inputData(const char *data, size_t len, uint64_t timestamp, bool is_idr_fast_packet) {
    if (data && len) {
        if (timestamp < _last_timestamp) {
            // 时间戳回退了，切片时长重新计时  [AUTO-TRANSLATED:fe91bd7f]
            // Timestamp has been rolled back, slice duration is recalculated
            WarnL << "Timestamp reduce: " << _last_timestamp << " -> " << timestamp;
            _last_seg_timestamp = _last_timestamp = timestamp;
        }
        if (is_idr_fast_packet) {
            // 尝试切片ts  [AUTO-TRANSLATED:62264109]
            // Attempt to slice ts
            addNewSegment(timestamp);
        }
        if (!_last_file_name.empty()) {
            // 存在切片才写入ts数据  [AUTO-TRANSLATED:ddd46115]
            // Write ts data only if there are slices
            onWriteSegment(data, len);
            _last_timestamp = timestamp;
        }
    } else {
        // resetTracks时触发此逻辑  [AUTO-TRANSLATED:0ba915ed]
        // This logic is triggered when resetTracks is called
        flushLastSegment(false);
    }
}

void HlsMaker::delOldSegment() {
    GET_CONFIG(uint32_t, segDelay, Hls::kSegmentDelay);
    if (_seg_number == 0 || _seg_keep) {
        // 如果设置为保留0个切片，则认为是保存为点播；或者设置为一直保存，就不删除  [AUTO-TRANSLATED:5bf20108]
        // If set to keep 0 or all slices, it is considered to be saved as on-demand
        return;
    }
    // 在hls m3u8索引文件中,我们保存的切片个数跟_seg_number相关设置一致  [AUTO-TRANSLATED:b14b5b98]
    // In the hls m3u8 index file, the number of slices we save is consistent with the _seg_number setting
    if (_file_index > _seg_number + segDelay) {
        _seg_dur_list.pop_front();
        if (!_seg_pdt_list.empty()) {
            _seg_pdt_list.pop_front();
        }
    }
    GET_CONFIG(uint32_t, segRetain, Hls::kSegmentRetain);
    // 但是实际保存的切片个数比m3u8所述多若干个,这样做的目的是防止播放器在切片删除前能下载完毕  [AUTO-TRANSLATED:1688f857]
    // However, the actual number of slices saved is a few more than what is stated in the m3u8, this is done to prevent the player from downloading the slices before they are deleted
    if (_file_index > _seg_number + segDelay + segRetain) {
        onDelSegment(_file_index - _seg_number - segDelay - segRetain - 1);
    }
}

void HlsMaker::addNewSegment(uint64_t stamp) {
    GET_CONFIG(bool, fastRegister, Hls::kFastRegister);
    if (_file_index > fastRegister  && stamp - _last_seg_timestamp < _seg_duration * 1000) {
        // 确保序号为0的切片立即open，如果开启快速注册功能，序号为1的切片也应该遇到关键帧立即生成；否则需要等切片时长够长  [AUTO-TRANSLATED:d81d1a1c]
        // Ensure that the slice with sequence number 0 is opened immediately, if the fast registration function is enabled, the slice with sequence number 1 should also be generated immediately when it encounters a keyframe; otherwise, it needs to wait until the slice duration is long enough
        return;
    }
    // 关闭并保存上一个切片，如果_seg_number==0,那么是点播。  [AUTO-TRANSLATED:14076b61]
    // Close and save the previous slice, if _seg_number==0, then it is on-demand.
    flushLastSegment(false);
    // 新增切片  [AUTO-TRANSLATED:b8623419]
    // Add a new slice
    _last_file_name = onOpenSegment(_file_index++);
    // 记录本次切片的起始时间戳  [AUTO-TRANSLATED:8eb776e9]
    // Record the starting timestamp of this slice
    _last_seg_timestamp = _last_timestamp ? _last_timestamp : stamp;
    _last_seg_pdt = programDateFor(stamp);
}

void HlsMaker::flushLastSegment(bool eof){
    GET_CONFIG(uint32_t, segDelay, Hls::kSegmentDelay);
    if (_last_file_name.empty()) {
        // 不存在上个切片  [AUTO-TRANSLATED:d81fe08e]
        // There is no previous slice
        return;
    }
    // 文件创建到最后一次数据写入的时间即为切片长度  [AUTO-TRANSLATED:1f85739c]
    // The time from file creation to the last data write is the slice length
    auto seg_dur = _last_timestamp - _last_seg_timestamp;
    if (seg_dur <= 0) {
        seg_dur = 100;
    }
    _seg_dur_list.emplace_back(seg_dur, std::move(_last_file_name));
    _seg_pdt_list.emplace_back(_last_seg_pdt);
    delOldSegment();
    // 先flush ts切片，否则可能存在ts文件未写入完毕就被访问的情况  [AUTO-TRANSLATED:f8d6dc87]
    // Flush the ts slice first, otherwise there may be a situation where the ts file is not written completely before it is accessed
    onFlushLastSegment(seg_dur);
    // 然后写m3u8文件  [AUTO-TRANSLATED:67200ce1]
    // Then write the m3u8 file
    makeIndexFile(false, eof);
    // 写入切片延迟的m3u8文件  [AUTO-TRANSLATED:b1f12e43]
    // Write the m3u8 file with slice delay
    if (segDelay) {
        makeIndexFile(true, eof);
    }
}

bool HlsMaker::isLive() const {
    return _seg_number != 0;
}

bool HlsMaker::isKeep() const {
    return _seg_keep;
}

bool HlsMaker::isFmp4() const {
    return _is_fmp4;
}

void HlsMaker::clear() {
    _file_index = 0;
    _last_timestamp = 0;
    _last_seg_timestamp = 0;
    _seg_dur_list.clear();
    _seg_pdt_list.clear();
    _last_seg_pdt = -1;
    _last_file_name.clear();
}

}//namespace mediakit
