#pragma once
#include "Player/PlayerProxy.h"
#include "Rtsp/RtspPlayer.h"
#include "json/json.h"

namespace mediakit {
// URL/status 원문을 만들지 않는 정제된 owner-poller 스냅샷.
inline Json::Value liveStreamHealthSample(const PlayerProxy::Ptr &proxy,uint64_t now) {
    Json::Value row(Json::objectValue),source(Json::objectValue),relay(Json::objectValue);
    row["observation"]="unsupported";row["generation"]=Json::nullValue;row["sourceRoute"]="unknown";
    row["ownerPollerDelayMs"]=Json::nullValue;
    for(auto key:{"videoRtpPackets","videoRtpAgeMs","videoFrames","videoFrameAgeMs","lastTcpAgeMs","controlOkAgeMs","reconnecting","expected"})source[key]=Json::nullValue;
    relay["kind"]="fmp4";
    for(auto key:{"demanded","readers","inputFrames","inputAgeMs","fragments","fragmentAgeMs","ringBatches","ringAgeMs","muxStage"})relay[key]=Json::nullValue;
    auto rtsp=proxy?std::dynamic_pointer_cast<RtspPlayer>(proxy->liveHealthPlayer()):nullptr;
    if(rtsp) {
        row["observation"]="observed";
        const auto &p=rtsp->liveInputProgress();
        const auto &generation=rtsp->liveInputGeneration();
        if(!generation.empty())row["generation"]=generation;
        row["sourceRoute"]=proxy->getTransportUrl()==proxy->getUrl()?"upstream":"shared_mirror";
        source["expected"]=rtsp->liveInputExpected();
        source["reconnecting"]=proxy->liveHealthRetryPending()||rtsp->liveInputConnecting();
        if(p.video_rtp){source["videoRtpPackets"]=std::to_string(p.video_rtp);source["videoRtpAgeMs"]=Json::UInt64(RtspInputProgress::age(now,p.video_at));}
        if(p.tcp_reads)source["lastTcpAgeMs"]=Json::UInt64(RtspInputProgress::age(now,p.tcp_at));
        if(p.control_ok)source["controlOkAgeMs"]=Json::UInt64(RtspInputProgress::age(now,p.control_at));
        if(auto muxer=proxy->liveHealthMuxer())if(auto diagnostic=muxer->liveFmp4Progress()) {
            const auto d=diagnostic->snapshot();
            if(d.video_frames){source["videoFrames"]=std::to_string(d.video_frames);source["videoFrameAgeMs"]=Json::UInt64(RtspInputProgress::age(now,d.video_at));}
            if(auto src=muxer->liveFmp4Source()) {
                const auto readers=src->readerCount();relay["readers"]=readers;
                relay["demanded"]=!proxy->getOption().fmp4_demand||readers>0;
            }
            if(d.input){relay["inputFrames"]=std::to_string(d.input);relay["inputAgeMs"]=Json::UInt64(RtspInputProgress::age(now,d.input_at));}
            if(d.fragments){relay["fragments"]=std::to_string(d.fragments);relay["fragmentAgeMs"]=Json::UInt64(RtspInputProgress::age(now,d.fragment_at));}
            if(d.ring_batches){relay["ringBatches"]=std::to_string(d.ring_batches);relay["ringAgeMs"]=Json::UInt64(RtspInputProgress::age(now,d.ring_at));}
            relay["muxStage"]=Json::UInt64(d.stage);
        }
    }
    row["source"]=std::move(source);row["relay"]=std::move(relay);return row;
}
} // namespace mediakit
