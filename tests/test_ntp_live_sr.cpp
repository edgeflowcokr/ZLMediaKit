#include <iostream>
#include <limits>
#include <array>
#include <thread>
#include <chrono>
#include "Common/Stamp.h"
#include "Rtsp/RtpReceiver.h"
using namespace mediakit;
static int failures=0;
static void expect(bool ok,const char*name) {
    std::cout<<name<<" result="<<(ok?"PASS":"FAIL")<<'\n';failures+=!ok;
}
static NtpStamp anchored() {
    NtpStamp c;c.setNtpStampContinuous(90000,100000);c.getNtpStamp(90000,90000);return c;
}
static void advance(NtpStamp&c) {
    for(uint32_t stamp=180000;stamp<=540000;stamp+=90000)c.getNtpStamp(stamp,90000);
}
int main() {
    auto c=anchored();
    expect(c.getNtpStamp(92970,90000)==100033,"first_anchor_and_normal_cadence");
    c=anchored();advance(c);c.setNtpStampContinuous(540000,105345);
    expect(c.getNtpStamp(542970,90000)>=105033 && c.getNtpStamp(542970,90000)<=105038,"positive_coarse_report_is_bounded");
    c=anchored();advance(c);c.setNtpStampContinuous(540000,104543);
    expect(c.getNtpStamp(542970,90000)>=105028 && c.getNtpStamp(542970,90000)<=105033,"negative_coarse_report_is_bounded");
    c=anchored();advance(c);c.setNtpStampContinuous(540000,105002);
    for(uint32_t step=1;step<=2000;++step)c.getNtpStamp(540000+step*90,90000);
    std::cout<<"small_clock_result_ms="<<c.getNtpStamp(720000,90000)<<" expected_ms=107002\n";
    expect(c.getNtpStamp(720000,90000)==107002,"small_clock_drift_converges_without_jump");
    c=anchored();advance(c);c.setNtpStampContinuous(540000,105000);
    auto before=c.getNtpStamp(542970,90000);c.setNtpStampContinuous(540000,105000);
    expect(c.getNtpStamp(542970,90000)==before,"duplicate_sender_report_ignored");
    c.setNtpStampContinuous(90000,100000);
    expect(c.getNtpStamp(542970,90000)==before,"stale_sender_report_ignored");
    for(uint64_t ntp=105001;ntp<105100;++ntp)c.setNtpStampContinuous(540000,ntp);
    expect(c.getNtpStamp(542970,90000)==before,"same_rtp_report_burst_has_no_slew_budget");
    c.setNtpStampContinuous(9000000,199000);
    expect(c.getNtpStamp(542970,90000)==before,"unrelated_rtp_anchor_rejected");
    c.setNtpStampContinuous(0,0);
    expect(c.getNtpStamp(542970,90000)==before,"invalid_zero_report_ignored");
    NtpStamp legacy;legacy.setNtpStamp(90000,100000);legacy.getNtpStamp(540000,90000);
    legacy.setNtpStamp(540000,105345);
    expect(legacy.getNtpStamp(542970,90000)==105378,"legacy_and_vod_exact_mapping_unchanged");
    NtpStamp idle;idle.setNtpStampContinuous(90000,100000);idle.setNtpStampContinuous(180000,101000);
    expect(idle.getNtpStamp(180000,90000)==101000,"reports_before_first_packet_keep_exact_anchor");
    NtpStamp wrap;const auto wrap_start=UINT32_MAX-1000;
    wrap.setNtpStampContinuous(wrap_start,100000);wrap.getNtpStamp(wrap_start,90000);
    expect(wrap.getNtpStamp(uint32_t(wrap_start+2970),90000)>=100032 && wrap.getNtpStamp(uint32_t(wrap_start+2970),90000)<=100034,"rtp_wrap_still_supported");
    NtpStamp audio;audio.setNtpStampContinuous(48000,100000);
    for(uint32_t stamp=48000;stamp<=288000;stamp+=48000)audio.getNtpStamp(stamp,48000);
    audio.setNtpStampContinuous(288000,105345);
    expect(audio.getNtpStamp(289584,48000)>=105033 && audio.getNtpStamp(289584,48000)<=105038,"audio_sample_rate_uses_same_slew_policy");
    c=anchored();uint64_t previous=100000;uint64_t max_gap=0;
    for(int frame=1;frame<=900;++frame) {
        const uint32_t rtp=90000+frame*3000;
        if(frame%150==0)c.setNtpStampContinuous(rtp,100000+frame*1000/30+(frame%300?345:-457));
        const auto now=c.getNtpStamp(rtp,90000);
        if(now<previous){++failures;break;}
        max_gap=(std::max)(max_gap,now-previous);previous=now;
    }
    expect(max_gap<=39 && previous>=129970 && previous<=130030,"repeated_bad_reports_cannot_inject_playout_gaps");
    c=anchored();advance(c);c.setNtpStampContinuous(540000,104543);
    previous=c.getNtpStamp(540000,90000);bool tiny_monotonic=true;
    for(uint32_t step=1;step<=10000;++step) {
        const auto now=c.getNtpStamp(540000+step,90000);
        tiny_monotonic=tiny_monotonic && now>=previous;previous=now;
    }
    expect(tiny_monotonic,"negative_slew_never_reverses_one_rtp_tick_cadence");
    c=anchored();advance(c);c.getNtpStamp(542970,90000);
    before=c.getNtpStamp(542970,90000);c.setNtpStampContinuous(540000,104543);
    expect(c.getNtpStamp(542970,90000)==before && c.getNtpStamp(543060,90000)>=before,"delayed_report_does_not_rebase_current_packet");
    NtpStamp exact;exact.setNtpStampContinuous(90000,100000);
    for(uint32_t step=1;step<=10000;++step)exact.getNtpStamp(90000+step*90,90000);
    expect(exact.getNtpStamp(990000,90000)==110000,"one_millisecond_rtp_ticks_do_not_accumulate_float_truncation");
    auto av_video=anchored();NtpStamp av_audio;
    av_audio.setNtpStampContinuous(48000,100000);av_audio.getNtpStamp(48000,48000);
    for(uint32_t frame=1;frame<=300;++frame) {
        if(frame==150)av_video.setNtpStampContinuous(90000+frame*3000,105345);
        av_video.getNtpStamp(90000+frame*3000,90000);
    }
    for(uint32_t frame=1;frame<=500;++frame) {
        if(frame==250)av_audio.setNtpStampContinuous(48000+frame*960,104543);
        av_audio.getNtpStamp(48000+frame*960,48000);
    }
    const auto av_offset=int64_t(av_video.getNtpStamp(990000,90000))-int64_t(av_audio.getNtpStamp(528000,48000));
    expect(av_offset>=-12 && av_offset<=12,"audio_video_phase_changes_are_slow_and_bounded");
    c=anchored();advance(c);c.resetContinuousAnchor();
    c.setNtpStampContinuous(1700000,200000);
    expect(c.getNtpStamp(1702970,90000)==200033,"new_source_can_establish_random_rtp_anchor");
    NtpStamp keep_legacy;keep_legacy.setNtpStamp(90000,100000);keep_legacy.resetContinuousAnchor();
    expect(keep_legacy.getNtpStamp(90000,90000)==100000,"continuous_reset_does_not_change_legacy_state");
    RtpTrackImp track;uint64_t received_stamp=0;
    track.setOnSorted([&](RtpPacket::Ptr packet){received_stamp=packet->ntp_stamp;});
    auto packet=[&](uint16_t seq,uint32_t stamp,uint32_t ssrc) {
        std::array<uint8_t,14> bytes{{0x80,0xE0,0,0,0,0,0,0,0,0,0,0,0x65,0x80}};
        const auto wire_seq=htons(seq);const auto wire_stamp=htonl(stamp);const auto wire_ssrc=htonl(ssrc);
        memcpy(bytes.data()+2,&wire_seq,2);memcpy(bytes.data()+4,&wire_stamp,4);memcpy(bytes.data()+8,&wire_ssrc,4);
        track.inputRtp(TrackVideo,90000,bytes.data(),bytes.size());
    };
    track.setNtpStamp(90000,100000,true);packet(1,90000,1);
    track.clear();track.setNtpStamp(1700000,200000,true);packet(1,1700000,2);
    expect(received_stamp==200000,"rtp_track_clear_reanchors_new_session");
    std::this_thread::sleep_for(std::chrono::milliseconds(3050));
    packet(2,3100000,3);track.setNtpStamp(3100000,300000,true);packet(3,3102970,3);
    expect(received_stamp==300033,"accepted_new_ssrc_gets_new_sender_report_anchor");
    return failures?1:0;
}
