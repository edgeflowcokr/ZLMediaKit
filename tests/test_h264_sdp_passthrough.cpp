#include <iostream>
#include "webrtc/Sdp.h"
#include "Util/logger.h"
#include "Common/config.h"
#include "ext-codec/H264Rtp.h"
using namespace mediakit;
using namespace toolkit;
static int failures=0;
static void check(bool ok,const char *name){std::cout<<name<<" result="<<(ok?"PASS":"FAIL")<<'\n';failures+=!ok;}
static RtcCodecPlan h264(uint8_t pt,const std::string &profile,const std::string &mode="1",const std::string &maximum="") {
    RtcCodecPlan p;p.pt=pt;p.codec="H264";p.sample_rate=90000;
    if(!profile.empty())p.fmtp["profile-level-id"]=profile;
    if(!mode.empty())p.fmtp["packetization-mode"]=mode;
    if(!maximum.empty())p.fmtp["max-recv-level"]=maximum;
    p.fmtp["level-asymmetry-allowed"]="1";return p;
}
static void run(const char *name,const std::string &source,const std::string &receiver,bool accepted,
                const std::string &mode="1",const std::string &maximum="",const std::string &asymmetry="1") {
    RtcConfigure cfg;SdpAttrFingerprint fp;
    fp.algorithm="sha-256";fp.hash="00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF";
    cfg.setDefaultSetting("testUfrag","testPassword1234567890",RtpDirection::sendrecv,fp);
    cfg.setPlayRtspInfo("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=unit\r\nt=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1"
        +(source.empty()?std::string():";profile-level-id="+source)+"\r\n");
    RtcSession offer{};offer.version=0;RtcMedia video;
    video.type=TrackVideo;video.mid="0";video.port=9;video.proto="UDP/TLS/RTP/SAVPF";
    video.role=DtlsRole::actpass;video.direction=RtpDirection::recvonly;video.rtcp_mux=true;
    RtcCodecPlan vp8;vp8.pt=100;vp8.codec="VP8";vp8.sample_rate=90000;video.plan.push_back(vp8);
    // Reproduce the former Main -> Baseline fallback before the compatible PT.
    if(source.substr(0,2)=="4d" || source.substr(0,2)=="4D")video.plan.push_back(h264(102,"42001f"));
    video.plan.push_back(h264(116,receiver,mode,maximum));
    if(asymmetry.empty())video.plan.back().fmtp.erase("level-asymmetry-allowed");
    else video.plan.back().fmtp["level-asymmetry-allowed"]=asymmetry;
    offer.media.push_back(video);
    auto answer=cfg.createAnswer(offer);auto media=answer->getMedia(TrackVideo);
    bool active=media&&media->port&&media->direction!=RtpDirection::inactive;
    bool ok=accepted?active&&!media->plan.empty()&&media->plan.front().pt==116
        :media&&!active&&media->port==0&&media->direction==RtpDirection::inactive;
    if(accepted&&ok){
        const auto &out=media->plan.front();
        ok=media->direction==RtpDirection::sendonly
            &&out.getFmtp("packetization-mode")=="1"
            &&out.getFmtp("profile-level-id")== (source.empty()?"42000a":source)
            &&out.getFmtp("max-recv-level").empty();
    }
    std::cout<<name<<" result="<<(ok?"PASS":"FAIL")<<'\n';failures+=!ok;
}
static void legacyPublish(bool audio){
    RtcConfigure cfg;SdpAttrFingerprint fp;fp.algorithm="sha-256";
    fp.hash="00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF";
    cfg.setDefaultSetting("testUfrag","testPassword1234567890",RtpDirection::recvonly,fp);
    RtcSession offer{};offer.version=0;RtcMedia m;m.type=audio?TrackAudio:TrackVideo;
    m.mid="0";m.port=9;m.proto="UDP/TLS/RTP/SAVPF";m.role=DtlsRole::actpass;m.direction=RtpDirection::sendonly;m.rtcp_mux=true;
    auto p=h264(116,"42e01f");if(audio){p.codec="opus";p.sample_rate=48000;p.channel=2;p.fmtp.clear();}
    m.plan.push_back(p);offer.media.push_back(m);auto answer=cfg.createAnswer(offer);auto selected=answer->getMedia(m.type);
    check(selected&&selected->port&&selected->direction==RtpDirection::recvonly&&!selected->plan.empty()
        &&selected->plan.front().codec==p.codec, audio?"legacy_audio_publish_unchanged":"legacy_h264_publish_unchanged");
}
struct FragmentCapture: toolkit::RingDelegate<RtpPacket::Ptr>{
    int fua=0;
    void onWrite(RtpPacket::Ptr p,bool) override{if(p->getPayloadSize()&&H264_TYPE(p->getPayload()[0])==28)++fua;}
};
static void disabledStapStillFragments(){
    toolkit::mINI::Instance()[Rtp::kH264StapA]=0;
    NOTICE_EMIT(BroadcastReloadConfigArgs,Broadcast::kBroadcastReloadConfig);
    auto capture=std::make_shared<FragmentCapture>();auto ring=std::make_shared<RtpRing::RingType>();ring->setDelegate(capture);
    H264RtpEncoder encoder;encoder.setRtpInfo(1,1400,90000,96);encoder.setRtpRing(ring);
    auto f=FrameImp::create<H264Frame>();f->_prefix_size=4;f->_buffer.assign("\0\0\0\1\x65\x80",6);
    f->_buffer.append(std::string(6000,'a'));f->_pts=f->_dts=1000;encoder.inputFrame(f);encoder.flush();
    check(capture->fua>1,"actual_encoder_stap_disabled_still_emits_fu_a");
}
int main(){
    auto logger=toolkit::Logger::Instance().shared_from_this();
    run("exact_main","4d001f","4d001f",true);
    run("higher_receiver_level","4d001f","4d0029",true);
    run("main_cannot_fall_back_to_baseline","4d0033","42001f",false);
    run("source_exceeds_receiver","4d0033","4d001f",false);
    run("profile_hex_case","4D001F","4d001f",true);
    run("constrained_baseline_equivalent_ids","42e01f","4d801f",true);
    run("baseline_is_not_constrained_baseline","42001f","42e01f",false);
    run("high_is_not_main","640029","4d0029",false);
    run("constrained_high_is_not_high","640c29","640029",false);
    run("level_1b_below_1_1","42f00b","42e00b",true);
    run("level_1b_above_1","42f00b","42e00a",false);
    run("missing_source_profile_defaults_baseline_1","","42000a",true);
    run("missing_receiver_profile_is_not_main","4d001f","",false);
    run("missing_receiver_defaults_baseline_1","42000a","",true);
    run("invalid_profile_hex","4dZZ1f","4d001f",false);
    run("unknown_profile_rejected","12001f","12001f",false);
    run("unknown_level_rejected","4d00ff","4d00ff",false);
    run("explicit_receive_level_extension","4d0033","4d001f",true,"1","0033");
    run("extension_below_source","4d0033","4d001f",false,"1","0029");
    run("invalid_extension","4d0033","4d001f",false,"1","nope");
    run("extension_must_exceed_default","4d001f","4d001f",false,"1","001f");
    run("packetization_zero_not_advertised_as_one","4d001f","4d001f",false,"0");
    run("missing_mode_defaults_zero","4d001f","4d001f",false,"");
    run("interleaved_packetization_unsupported","4d001f","4d001f",false,"2");
    run("no_asymmetry_level_upgrade_forbidden","4d0033","4d001f",false,"1","0033","0");
    run("missing_asymmetry_level_upgrade_forbidden","4d0033","4d001f",false,"1","0033","");
    run("no_asymmetry_lower_source_allowed","4d001e","4d001f",true,"1","","0");
    run("invalid_asymmetry_rejected","4d001f","4d001f",false,"1","","2");
    toolkit::mINI::Instance()[Rtp::kH264StapA]=0;
    run("stap_disabled_still_accepts_mode_1_for_fu_a","4d001f","4d001f",true,"1");
    run("stap_disabled_cannot_advertise_single_nal_only","4d001f","4d001f",false,"0");
    disabledStapStillFragments();
    legacyPublish(false);legacyPublish(true);
    return failures?1:0;
}
