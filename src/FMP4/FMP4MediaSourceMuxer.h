/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_FMP4MEDIASOURCEMUXER_H
#define ZLMEDIAKIT_FMP4MEDIASOURCEMUXER_H

#include "FMP4MediaSource.h"
#include "Record/MP4Muxer.h"

namespace mediakit {

class FMP4MediaSourceMuxer final : public MP4MuxerMemory, public MediaSourceEventInterceptor,
                                   public std::enable_shared_from_this<FMP4MediaSourceMuxer> {
public:
    using Ptr = std::shared_ptr<FMP4MediaSourceMuxer>;

    FMP4MediaSourceMuxer(const MediaTuple& tuple, const ProtocolOption &option) {
        _option = option;
        setSingleH264SampleLookahead(true);
        _media_src = std::make_shared<FMP4MediaSource>(tuple);
    }

    ~FMP4MediaSourceMuxer() override {
        try {
            MP4MuxerMemory::flush();
        } catch (std::exception &ex) {
            WarnL << ex.what();
        }
    }

    void setListener(const std::weak_ptr<MediaSourceEvent> &listener){
        setDelegate(listener);
        _media_src->setListener(shared_from_this());
    }

    int readerCount() const{
        return _media_src->readerCount();
    }

    void onReaderChanged(MediaSource &sender, int size) override {
        // 독자별 poller에서 보낸 size는 소유 poller에 도착할 때 이미 오래된 값일 수 있다.
        // 늦은 0명 알림으로 새 HTTP 독자의 mux를 끄거나 활성 GOP를 지우지 않는다.
        const auto readers = _media_src->readerCount();
        if (size != readers && !_stale_reader_logged) {
            _stale_reader_logged = true;
            WarnL << "fMP4 demand ignored stale reader count: stream=" << _media_src->getMediaTuple().stream
                  << ", notified=" << size << ", current=" << readers;
        }
        refreshDemand(readers);
        MediaSourceEventInterceptor::onReaderChanged(sender, readers);
    }

    bool inputFrame(const Frame::Ptr &frame) override {
        refreshDemand(_media_src->readerCount());
        if (_clear_cache && _option.fmp4_demand) {
            _clear_cache = false;
            _media_src->clearCache();
        }
        if (_enabled || !_option.fmp4_demand) {
            return MP4MuxerMemory::inputFrame(frame);
        }
        return false;
    }

    bool isEnabled() {
        refreshDemand(_media_src->readerCount());
        // 缓存尚未清空时，还允许触发inputFrame函数，以便及时清空缓存  [AUTO-TRANSLATED:7cfd4d49]
        // The inputFrame function is still allowed to be triggered when the cache has not been cleared, so that the cache can be cleared in time.
        return _option.fmp4_demand ? (_clear_cache ? true : _enabled) : true;
    }

    void addTrackCompleted() override {
        MP4MuxerMemory::addTrackCompleted();
        _media_src->setInitSegment(getInitSegment());
    }

    MediaSource::Ptr getMediaSource() const {
        return _media_src;
    }

protected:
    void onSegmentData(std::string string, uint64_t stamp, bool key_frame) override {
        if (string.empty()) {
            return;
        }
        FMP4Packet::Ptr packet = std::make_shared<FMP4Packet>(std::move(string));
        packet->time_stamp = stamp;
        _media_src->onWrite(std::move(packet), key_frame);
    }

private:
    void refreshDemand(int readers) {
        const bool enabled = !_option.fmp4_demand || readers > 0;
        if (_option.fmp4_demand && _enabled && !enabled) _clear_cache = true;
        _enabled = enabled;
    }
    bool _enabled = true;
    bool _clear_cache = false;
    bool _stale_reader_logged = false; // 스트림 인스턴스당 한 번만 진단한다.
    ProtocolOption _option;
    FMP4MediaSource::Ptr _media_src;
};

}//namespace mediakit

#endif //ZLMEDIAKIT_FMP4MEDIASOURCEMUXER_H
