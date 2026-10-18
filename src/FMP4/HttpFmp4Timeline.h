#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mediakit {

// HTTP progressive playback has no MSE timestampOffset. A late reader must
// not have to seek across the shared live muxer's entire uptime before it can
// decode. Only a single video track is supported here: multi-track A/V needs
// a common media-time anchor and deliberately retains its existing timeline.
// This is a connection-local translation, never a cadence/SPS/payload rewrite.
class HttpFmp4Timeline {
public:
    bool configure(const std::string &init) {
        _active = false;
        _anchored = false;
        _base = 0;
        _track = 0;
        if (init.size() > 1024 * 1024) return false;
        std::vector<Box> top, moov, trak, mdia;
        if (!boxes(init, 0, init.size(), top)) return false;
        const auto *movie = one(top, "moov");
        if (!movie || !children(init, *movie, moov)) return false;
        const auto *track = one(moov, "trak");
        if (!track || !children(init, *track, trak) || one(trak, "edts")) return false;
        const auto *tkhd = one(trak, "tkhd");
        const auto *media = one(trak, "mdia");
        if (!tkhd || !media || !children(init, *media, mdia)) return false;
        const auto *hdlr = one(mdia, "hdlr");
        if (!hdlr || hdlr->end - hdlr->payload < 12
            || init.compare(hdlr->payload + 8, 4, "vide") != 0) return false;
        if (tkhd->end - tkhd->payload < 4) return false;
        const auto version = uint8_t(init[tkhd->payload]);
        const size_t id_relative = version == 0 ? 12 : 20;
        if (version > 1 || tkhd->end - tkhd->payload < id_relative + 4) return false;
        const auto id_offset = tkhd->payload + id_relative;
        _track = read(init, id_offset, 4);
        const auto *mdhd = one(mdia, "mdhd");
        if (!mdhd || mdhd->end - mdhd->payload < 4) return false;
        const auto media_version = uint8_t(init[mdhd->payload]);
        const size_t scale_relative = media_version == 0 ? 12 : 20;
        if (media_version > 1 || mdhd->end - mdhd->payload < scale_relative + 4) return false;
        _scale = read(init, mdhd->payload + scale_relative, 4);
        _active = _track != 0 && _scale != 0;
        return _active;
    }

    bool active() const { return _active; }
    uint64_t base() const { return _base; }

    // Validate the entire packet before copying or advancing connection state.
    // Bad metadata stops this reader instead of mixing translated and absolute
    // timestamps. Packet limit is 32 MiB, with at most 1024 boxes per container.
    bool translate(const std::string &packet, std::string &out) {
        if (!_active) { out = packet; return true; }
        if (packet.size() > 32 * 1024 * 1024) return false;
        std::vector<Box> top;
        if (!boxes(packet, 0, packet.size(), top)) return false;
        std::vector<Field> fields;
        bool found = false;
        uint64_t candidate = _base;
        for (const auto &box : top) {
            if (box.type == "moof") {
                std::vector<Box> moof, traf;
                if (!children(packet, box, moof)) return false;
                const auto *fragment = one(moof, "traf");
                if (!fragment || !children(packet, *fragment, traf)) return false;
                const auto *tfhd = one(traf, "tfhd");
                const auto *tfdt = one(traf, "tfdt");
                if (!tfhd || !tfdt || tfhd->end - tfhd->payload < 8
                    || read(packet, tfhd->payload + 4, 4) != _track
                    || (read(packet, tfhd->payload, 4) & 0x000001)
                    || !(read(packet, tfhd->payload, 4) & 0x020000)) return false;
                Field time;
                if (!timeField(packet, *tfdt, 4, time)) return false;
                if (!_anchored && !found) candidate = time.value;
                found = true;
                fields.push_back(time);
            } else if (box.type == "sidx") {
                // The muxer writes a segment index as well as tfdt. Translate
                // its presentation epoch by the same amount; durations, byte
                // offsets, trun CTS and all mdat bytes remain untouched.
                if (box.end - box.payload < 12
                    || read(packet, box.payload + 4, 4) != _track
                    || read(packet, box.payload + 8, 4) != _scale) return false;
                Field time;
                if (!timeField(packet, box, 12, time)) return false;
                fields.push_back(time);
            } else if (box.type != "styp" && box.type != "mdat" && box.type != "free") {
                return false; // no silent handling of other time-bearing boxes
            }
        }
        if (!found) return false;
        if (!_anchored) for (const auto &field : fields) if (field.value < candidate) candidate = field.value;
        for (const auto &field : fields) if (field.value < candidate) return false;
        out = packet;
        for (const auto &field : fields) write(out, field.offset, field.width, field.value - candidate);
        _base = candidate;
        _anchored = true;
        return true;
    }

private:
    struct Box { size_t payload, end; std::string type; };
    struct Field { size_t offset, width; uint64_t value; };
    static uint64_t read(const std::string &s, size_t pos, size_t width) {
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i) value = (value << 8) | uint8_t(s[pos + i]);
        return value;
    }
    static void write(std::string &s, size_t pos, size_t width, uint64_t value) {
        for (size_t i = width; i; --i) { s[pos + i - 1] = char(value & 255); value >>= 8; }
    }
    static bool boxes(const std::string &s, size_t pos, size_t end, std::vector<Box> &out) {
        while (pos < end) {
            if (end - pos < 8 || out.size() >= 1024) return false;
            auto size = read(s, pos, 4);
            size_t header = 8;
            if (size == 1) {
                if (end - pos < 16) return false;
                size = read(s, pos + 8, 8); header = 16;
            }
            if (size < header || size > end - pos) return false;
            out.push_back({pos + header, pos + size_t(size), s.substr(pos + 4, 4)});
            pos += size_t(size);
        }
        return true;
    }
    static bool children(const std::string &s, const Box &box, std::vector<Box> &out) {
        return boxes(s, box.payload, box.end, out);
    }
    static const Box *one(const std::vector<Box> &list, const char *type) {
        const Box *result = nullptr;
        for (const auto &box : list) if (box.type == type) {
            if (result) return nullptr;
            result = &box;
        }
        return result;
    }
    static bool timeField(const std::string &s, const Box &box, size_t relative, Field &field) {
        if (box.end - box.payload < 4) return false;
        const auto version = uint8_t(s[box.payload]);
        const size_t width = version == 0 ? 4 : 8;
        if (version > 1 || box.end - box.payload < relative + width) return false;
        field = {box.payload + relative, width, read(s, box.payload + relative, width)};
        return true;
    }
    bool _active = false, _anchored = false;
    uint64_t _base = 0, _track = 0, _scale = 0;
};

} // namespace mediakit
