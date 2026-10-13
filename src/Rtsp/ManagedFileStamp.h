#pragma once
#include <string>

namespace mediakit {
// An explicit opt-in for the local managed-file publisher, not a generic URL
// override. Camera pushes/pulls and unmarked files retain their protocol policy.
inline bool managedFileRelativeStamp(const std::string &peer, const std::string &app,
                                     const std::string &stream, const std::string &params) {
    if (peer != "127.0.0.1" && peer != "::1" && peer != "::ffff:127.0.0.1") {
        return false;
    }
    if (app != "file" || params != "ef_file_stamp=relative" || stream.size() != 19 || stream.compare(0, 3, "vs-") != 0) {
        return false;
    }
    for (size_t i = 3; i < stream.size(); ++i) {
        auto c = stream[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}
} // namespace mediakit
