/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "HlsMediaSource.h"
#include "Common/config.h"

using namespace toolkit;

namespace mediakit {

class SockInfoImp : public Session {
public:
    using Ptr = std::shared_ptr<SockInfoImp>;
    // Session 생성자는 소켓 종류만 확인하면 되므로 받은 뒤 바로 놓는다. 쿠키(kHlsCookieSecond=60)가 소켓을 쥐고 있으면
    // HttpSession 이 keepAlive(30초)로 끝나도 소켓이 닫히지 않아, 그 연결에 온 다음 요청이 처리되지 않고 매달렸다
    // (EdgeFlowAI .5 2026-11-21: 백엔드가 30~60초 쉰 연결을 재사용해 HLS·WHEP·API 가 시간 초과 → 502, CloseWait 수십 개 누적).
    // 식별자·주소는 아래 필드에 복사해 두므로 getMediaPlayerList·유량 보고는 그대로 동작한다.
    SockInfoImp(const Socket::Ptr &sock) : Session(sock) { setSock(nullptr); }

    std::string get_local_ip() override { return _local_ip; }

    uint16_t get_local_port() override { return _local_port; }

    std::string get_peer_ip() override { return _peer_ip; }

    uint16_t get_peer_port() override { return _peer_port; }

    std::string getIdentifier() const override { return _identifier; }

    void onRecv(const Buffer::Ptr &buf) override {}
    void onError(const SockException &err) override {}
    void onManager() override {}

    std::string _local_ip;
    std::string _peer_ip;
    std::string _identifier;
    uint16_t _local_port;
    uint16_t _peer_port;
};

HlsCookieData::HlsCookieData(const MediaInfo &info, const std::shared_ptr<Session> &session) {
    _info = info;
    auto sock_info = std::make_shared<SockInfoImp>(session->getSock());
    sock_info->_identifier = session->getIdentifier();
    sock_info->_peer_ip = session->get_peer_ip();
    sock_info->_peer_port = session->get_peer_port();
    sock_info->_local_ip = session->get_local_ip();
    sock_info->_local_port = session->get_local_port();
    _sock_info = sock_info;
    _added = std::make_shared<bool>(false);
    addReaderCount();
}

void HlsCookieData::addReaderCount() {
    if (!*_added) {
        auto src = getMediaSource();
        if (src) {
            *_added = true;
            _ring_reader = src->getRing()->attach(EventPollerPool::Instance().getPoller());
            auto added = _added;
            _ring_reader->setDetachCB([added]() {
                // HlsMediaSource已经销毁  [AUTO-TRANSLATED:bedb0385]
                // HlsMediaSource has been destroyed
                *added = false;
            });
            auto sock_info = _sock_info;
            _ring_reader->setGetInfoCB([sock_info]() {
                Any ret;
                ret.set(std::static_pointer_cast<Session>(sock_info));
                return ret;
            });
        }
    }
}

HlsCookieData::~HlsCookieData() {
    if (*_added) {
        uint64_t duration = (_ticker.createdTime() - _ticker.elapsedTime()) / 1000;
        WarnL << _sock_info->getIdentifier() << "(" << _sock_info->get_peer_ip() << ":" << _sock_info->get_peer_port()
              << ") " << "HLS播放器(" << _info.shortUrl() << ")断开,耗时(s):" << duration;

        GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);
        uint64_t bytes = _bytes.load();
        if (bytes >= iFlowThreshold * 1024) {
            try {
                NOTICE_EMIT(BroadcastFlowReportArgs, Broadcast::kBroadcastFlowReport, _info, bytes, duration, true, *_sock_info);
            } catch (std::exception &ex) {
                WarnL << "Exception occurred: " << ex.what();
            }
        }
    }
}

void HlsCookieData::addByteUsage(size_t bytes) {
    addReaderCount();
    _bytes += bytes;
    _ticker.resetTime();
}

void HlsCookieData::setMediaSource(const HlsMediaSource::Ptr &src) {
    _src = src;
}

HlsMediaSource::Ptr HlsCookieData::getMediaSource() const {
    return _src.lock();
}

void HlsMediaSource::setIndexFile(std::string index_file)
{
    if (!_ring) {
        std::weak_ptr<HlsMediaSource> weakSelf = std::static_pointer_cast<HlsMediaSource>(shared_from_this());
        auto lam = [weakSelf](int size) {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                return;
            }
            strongSelf->onReaderChanged(size);
        };
        _ring = std::make_shared<RingType>(0, std::move(lam));
        regist();
    }

    // 赋值m3u8索引文件内容  [AUTO-TRANSLATED:c11882b5]
    // Assign m3u8 index file content
    std::lock_guard<std::mutex> lck(_mtx_index);
    _index_file = std::move(index_file);

    if (!_index_file.empty()) {
        _list_cb.for_each([&](const std::function<void(const std::string& str)>& cb) { cb(_index_file); });
        _list_cb.clear();
    }
}

void HlsMediaSource::getIndexFile(std::function<void(const std::string& str)> cb)
{
    std::lock_guard<std::mutex> lck(_mtx_index);
    if (!_index_file.empty()) {
        cb(_index_file);
        return;
    }
    // 等待生成m3u8文件  [AUTO-TRANSLATED:c3ae3286]
    // Waiting for m3u8 file generation
    _list_cb.emplace_back(std::move(cb));
}

} // namespace mediakit
