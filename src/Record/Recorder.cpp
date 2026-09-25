/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <atomic>
#include <climits>
#include "Recorder.h"
#include "BoundedRecordWorker.h"
#include "Common/config.h"
#include "Util/File.h"
#include "Common/MediaSource.h"
#include "MP4Recorder.h"
#include "AsyncMP4Recorder.h"
#include "HlsRecorder.h"
#include "FMP4/FMP4MediaSourceMuxer.h"
#include "TS/TSMediaSourceMuxer.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

string Recorder::getRecordPath(Recorder::type type, const MediaTuple& tuple, const string &customized_path) {
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    switch (type) {
        case Recorder::type_hls_fmp4:
        case Recorder::type_hls: {
            GET_CONFIG(string, hlsPath, Protocol::kHlsSavePath);
            string m3u8FilePath;
            auto tail =  type == Recorder::type_hls ? "/hls.m3u8" : "/hls.fmp4.m3u8";
            if (enableVhost) {
                m3u8FilePath = tuple.shortUrl() + tail;
            } else {
                m3u8FilePath = tuple.app + "/" + tuple.stream + tail;
            }
            //Here we use the customized file path.
            if (!customized_path.empty()) {
                return File::absolutePath(m3u8FilePath, customized_path);
            }
            return File::absolutePath(m3u8FilePath, hlsPath);
        }
        case Recorder::type_mp4: {
            GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
            GET_CONFIG(string, recordAppName, Record::kAppName);
            string mp4FilePath;
            if (enableVhost) {
                mp4FilePath = tuple.vhost + "/" + recordAppName + "/" + tuple.app + "/" + tuple.stream + "/";
            } else {
                mp4FilePath = recordAppName + "/" + tuple.app + "/" + tuple.stream + "/";
            }
            //Here we use the customized file path.
            if (!customized_path.empty()) {
                return File::absolutePath(mp4FilePath, customized_path);
            }
            return File::absolutePath(mp4FilePath, recordPath);
        }
        default: return "";
    }
}

std::shared_ptr<MediaSinkInterface> Recorder::createRecorder(type type, const MediaTuple& tuple, const ProtocolOption &option){
    switch (type) {
        case Recorder::type_hls: {
#if defined(ENABLE_HLS)
            auto path = Recorder::getRecordPath(type, tuple, option.hls_save_path);
            GET_CONFIG(bool, enable_vhost, General::kEnableVhost);
            auto ret = std::make_shared<HlsRecorder>(path, enable_vhost ? string(VHOST_KEY) + "=" + tuple.vhost : "", option);
            ret->setMediaSource(tuple);
            return ret;
#else
            throw std::invalid_argument("hls相关功能未打开，请开启ENABLE_HLS宏后编译再测试");
#endif
        }

        case Recorder::type_mp4: {
#if defined(ENABLE_MP4)
            auto path = Recorder::getRecordPath(type, tuple, option.mp4_save_path);
            return std::make_shared<AsyncMP4Recorder>(tuple, path, option.mp4_max_second);
#else
            throw std::invalid_argument("mp4相关功能未打开，请开启ENABLE_MP4宏后编译再测试");
#endif
        }

        case Recorder::type_hls_fmp4: {
#if defined(ENABLE_MP4)
            auto path = Recorder::getRecordPath(type, tuple, option.hls_save_path);
            GET_CONFIG(bool, enable_vhost, General::kEnableVhost);
            auto ret = std::make_shared<HlsFMP4Recorder>(path, enable_vhost ? string(VHOST_KEY) + "=" + tuple.vhost : "", option);
            ret->setMediaSource(tuple);
            return ret;
#else
            throw std::invalid_argument("hls.fmp4相关功能未打开，请开启ENABLE_MP4宏后编译再测试");
#endif
        }

        case Recorder::type_fmp4: {
#if defined(ENABLE_MP4)
            return std::make_shared<FMP4MediaSourceMuxer>(tuple, option);
#else
            throw std::invalid_argument("fmp4相关功能未打开，请开启ENABLE_MP4宏后编译再测试");
#endif
        }

        case Recorder::type_ts: {
#if defined(ENABLE_HLS) || defined(ENABLE_RTPPROXY)
            return std::make_shared<TSMediaSourceMuxer>(tuple, option);
#else
            throw std::invalid_argument("mpegts相关功能未打开，请开启ENABLE_HLS或ENABLE_RTPPROXY宏后编译再测试");
#endif
        }

        default: throw std::invalid_argument("未知的录制类型");
    }
}

RecordQueueLimits RecordQueueLimits::configured() {
    // 호출 스레드(녹화기 생성 지점)에서 읽는다. GET_CONFIG 는 설정 재적재를 구독한다.
    // 범위·기본값은 RecordQueueLimits(헤더)가 정본: queueMaxMB [4,256] 기본 96, stallToleranceMs [5000,600000] 기본 90000.
    //   queueMaxMB 상한 256: 전역 슬롯 32 x 256MiB = 8GiB 가 최악 상주량이라 그 이상은 설정 실수로 본다.
    //   stallToleranceMs 하한 5000: 예전 고정값. 더 짧으면 SMR 정지마다 녹화가 끊기던 원래 결함이 재발한다.
    //   stallToleranceMs 상한 600000(10분): 그 이상 멈춘 디스크는 사실상 고장이라 녹화기 교체 경로로 넘긴다.
    GET_CONFIG(int, queueMaxMB, Record::kQueueMaxMB);
    GET_CONFIG(int, stallToleranceMs, Record::kStallToleranceMs);
    std::string warning;
    auto limits = fromSettings(queueMaxMB, stallToleranceMs, &warning);
    if (!warning.empty()) {
        // 녹화기 생성마다 반복 출력하지 않도록 원래 설정값 조합이 바뀔 때만 로그.
        static std::atomic<unsigned long long> warned { ULLONG_MAX };
        const unsigned long long key = (static_cast<unsigned long long>(static_cast<unsigned>(queueMaxMB)) << 32) | static_cast<unsigned>(stallToleranceMs);
        if (warned.exchange(key) != key) WarnL << "[recording-queue] config clamped: " << warning;
    }
    return limits;
}

} /* namespace mediakit */
