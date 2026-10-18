#include "FMP4/HttpFmp4Timeline.h"
#include <fstream>
#include <iostream>
#include <iterator>
using namespace mediakit;
static int failures = 0;
static void check(bool ok, const char *name) { std::cout << name << " result=" << (ok ? "PASS" : "FAIL") << '\n'; failures += !ok; }
static std::string num(uint64_t n, size_t bytes) {
    std::string s(bytes, 0);
    while (bytes) { s[--bytes] = char(n & 255); n >>= 8; }
    return s;
}
static std::string box(const char *type, const std::string &body) { return num(body.size() + 8, 4) + type + body; }
static std::string init(bool audio = false, bool version1 = false, bool edit = false) {
    const auto tkhd = box("tkhd", num(version1 ? 0x01000000 : 0, 4) + num(0, version1 ? 16 : 8) + num(1, 4));
    const auto mdhd = box("mdhd", num(version1 ? 0x01000000 : 0, 4) + num(0, version1 ? 16 : 8) + num(1000, 4));
    const auto hdlr = box("hdlr", num(0, 8) + "vide");
    auto track = box("trak", tkhd + box("mdia", mdhd + hdlr) + (edit ? box("edts", "") : ""));
    return box("ftyp", "isom") + box("moov", track + (audio ? track : ""));
}
static std::string fragment(uint64_t time, bool wide = false, bool index = true, uint64_t pts = UINT64_MAX) {
    const auto tfhd = box("tfhd", num(0x020000, 4) + num(1, 4));
    const auto tfdt = box("tfdt", num(wide ? 0x01000000 : 0, 4) + num(time, wide ? 8 : 4));
    const auto trun = box("trun", num(0, 4) + num(1, 4) + num(33, 4));
    const auto sidx = index ? box("sidx", num(wide ? 0x01000000 : 0, 4) + num(1, 4) + num(1000, 4) + num(pts == UINT64_MAX ? time : pts, wide ? 8 : 4) + num(0, 16)) : "";
    return box("styp", "isom") + sidx + box("moof", box("traf", tfhd + tfdt + trun)) + box("mdat", "exact-video-payload");
}
int main(int argc, char **argv) {
    HttpFmp4Timeline t; std::string out;
    check(t.configure(init()), "single_video_enabled");
    const auto old = fragment(853098);
    check(t.translate(old, out) && out == fragment(0) && t.base() == 853098, "late_connection_first_fragment_and_index_start_zero");
    check(old == fragment(853098), "shared_input_not_mutated");
    check(t.translate(fragment(853131), out) && out == fragment(33), "second_fragment_keeps_actual_delta");
    check(t.translate(fragment(853731), out) && out == fragment(633), "long_real_gap_is_not_compressed");
    const auto saved = out;
    check(!t.translate(fragment(853000), out) && out == saved && t.base() == 853098, "backward_epoch_rejected_atomically");
    check(t.translate(fragment(853732), out) && out == fragment(634), "bad_packet_does_not_change_anchor");
    HttpFmp4Timeline second;
    check(second.configure(init()) && second.translate(fragment(990000), out) && out == fragment(0), "independent_reader_has_independent_epoch");
    HttpFmp4Timeline wide;
    check(wide.configure(init(false, true)) && wide.translate(fragment(UINT64_C(1) << 40, true), out) && out == fragment(0, true), "version_one_uint64_epoch_and_init_supported");
    check(wide.translate(fragment((UINT64_C(1) << 40) + 40, true), out) && out == fragment(40, true), "version_one_delta_preserved");
    HttpFmp4Timeline bframe;
    check(bframe.configure(init()) && bframe.translate(fragment(1000, false, true, 960), out) && out == fragment(40, false, true, 0), "negative_composition_offset_preserves_decode_presentation_relationship");
    HttpFmp4Timeline no_index;
    check(no_index.configure(init()) && no_index.translate(fragment(1000, false, false), out) && out == fragment(0, false, false), "fragment_without_index_supported");
    HttpFmp4Timeline av;
    check(!av.configure(init(true)) && !av.active(), "multiple_tracks_keep_existing_av_timeline");
    check(av.translate(old, out) && out == old, "unsupported_init_is_byte_exact_passthrough");
    check(!av.configure(init(false, false, true)), "edit_list_not_silently_rebased");
    check(!av.configure(box("moov", box("trak", box("tkhd", num(0x01000000, 4))))), "short_version_one_header_rejected_without_underflow");
    const auto good = fragment(853800);
    const auto previous = out;
    check(!t.translate(good.substr(0, good.size() - 1), out) && out == previous, "truncated_packet_no_partial_mutation");
    check(!t.translate(num(0, 4) + "moof", out), "open_ended_size_rejected");
    check(!t.translate(num(1, 4) + "moof" + num(UINT64_MAX, 8), out), "extended_size_overflow_rejected");
    check(!t.translate(box("sidx", ""), out), "short_index_rejected");
    auto wrong = good; auto pos = wrong.find("tfhd"); wrong[pos + 11] = 2;
    check(!t.translate(wrong, out), "different_track_id_rejected");
    wrong = good; pos = wrong.find("sidx"); wrong[pos + 15] = 0;
    check(!t.translate(wrong, out), "different_index_timescale_rejected");
    wrong = good; pos = wrong.find("tfdt"); wrong[pos + 4] = 2;
    check(!t.translate(wrong, out), "unknown_tfdt_version_rejected");
    check(!t.translate(box("moof", box("traf", "")), out), "missing_tfhd_tfdt_rejected");
    check(!t.translate(box("mfra", ""), out), "unknown_time_bearing_metadata_not_silently_changed");
    check(!t.translate(std::string(32 * 1024 * 1024 + 1, 0), out), "packet_memory_bound");
    check(!t.configure(std::string(1024 * 1024 + 1, 0)), "init_memory_bound");
    // Optional offline real fMP4 inspection uses exactly the production helper.
    // It does not serve files or open sockets. Split the init from all media
    // segments and validate mdat/trun bytes separately with the existing harness.
    if (argc == 3) {
        std::ifstream in(argv[1], std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {}), header, media, translated;
        HttpFmp4Timeline real;
        bool ok = in.is_open() && bytes.size() <= 64 * 1024 * 1024;
        size_t at = 0;
        while (ok && at + 8 <= bytes.size()) {
            uint32_t n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | uint8_t(bytes[at + i]);
            if (n < 8 || n > bytes.size() - at) break;
            auto type = bytes.substr(at + 4, 4);
            if (type == "ftyp" || type == "moov") {
                header += bytes.substr(at, n);
                if (type == "moov") ok = real.configure(header);
            }
            else media += bytes.substr(at, n);
            at += n;
            if (type == "mdat") {
                ok = real.active() && real.translate(media, out);
                if (ok) translated += out;
                media.clear();
            }
        }
        ok = ok && at == bytes.size() && media.empty() && !translated.empty();
        check(ok, "offline_real_file_translate");
        if (ok) { std::ofstream file(argv[2], std::ios::binary); file << header << translated; check(bool(file), "offline_real_file_written"); }
    }
    return failures ? 1 : 0;
}
