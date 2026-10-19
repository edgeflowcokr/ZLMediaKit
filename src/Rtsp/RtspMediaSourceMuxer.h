/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_RTSPMEDIASOURCEMUXER_H
#define ZLMEDIAKIT_RTSPMEDIASOURCEMUXER_H

#include "RtspMuxer.h"
#include "Rtsp/RtspMediaSource.h"

namespace mediakit {

class RtspMediaSourceMuxer final : public RtspMuxer, public MediaSourceEventInterceptor,
                                   public std::enable_shared_from_this<RtspMediaSourceMuxer> {
public:
    using Ptr = std::shared_ptr<RtspMediaSourceMuxer>;

    RtspMediaSourceMuxer(const MediaTuple& tuple,
                         const ProtocolOption &option,
                         const TitleSdp::Ptr &title = nullptr) : RtspMuxer(title) {
        _option = option;
        _media_src = std::make_shared<RtspMediaSource>(tuple);
        getRtpRing()->setDelegate(_media_src);
    }

    ~RtspMediaSourceMuxer() override {
        try {
            RtspMuxer::flush();
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

    void addTrackCompleted() override {
        RtspMuxer::addTrackCompleted();
        _media_src->setSdp(getSdp());
    }

    void onReaderChanged(MediaSource &sender, int size) override {
        // 독자 poller의 늦은 알림이 현재 재생 중인 RTP 출력을 끄지 않게 한다.
        const auto readers = _media_src->readerCount();
        if (size != readers && !_stale_reader_logged) {
            _stale_reader_logged = true;
            WarnL << "RTSP demand ignored stale reader count: stream=" << _media_src->getMediaTuple().stream
                  << ", notified=" << size << ", current=" << readers;
        }
        refreshDemand(readers);
        MediaSourceEventInterceptor::onReaderChanged(sender, readers);
    }

    bool inputFrame(const Frame::Ptr &frame) override {
        refreshDemand(_media_src->readerCount());
        if (_clear_cache && _option.rtsp_demand) {
            _clear_cache = false;
            _media_src->clearCache();
        }
        if (_enabled || !_option.rtsp_demand) {
            return RtspMuxer::inputFrame(frame);
        }
        return false;
    }

    bool isEnabled() {
        refreshDemand(_media_src->readerCount());
        // 缓存尚未清空时，还允许触发inputFrame函数，以便及时清空缓存  [AUTO-TRANSLATED:7cfd4d49]
        // The inputFrame function is still allowed to be triggered when the cache has not been cleared, so that the cache can be cleared in time.
        return _option.rtsp_demand ? (_clear_cache ? true : _enabled) : true;
    }

    MediaSource::Ptr getMediaSource() const {
        return _media_src;
    }

private:
    void refreshDemand(int readers) {
        // RTSP는 첫 RTP 패킷에서 ring/등록이 생성된다. 최초 생성까지 막으면
        // 독자가 매체를 발견할 수 없어 영구 대기가 되므로 이 구간은 허용한다.
        const bool enabled = !_option.rtsp_demand || !_media_src->getRing() || readers > 0;
        if (_option.rtsp_demand && _enabled && !enabled) _clear_cache = true;
        _enabled = enabled;
    }
    bool _enabled = true;
    bool _clear_cache = false;
    bool _stale_reader_logged = false; // 스트림 인스턴스당 한 번만 진단한다.
    ProtocolOption _option;
    RtspMediaSource::Ptr _media_src;
};


}//namespace mediakit
#endif //ZLMEDIAKIT_RTSPMEDIASOURCEMUXER_H
