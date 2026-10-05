#pragma once

#include <string>
#include <vector>

// Pure data type describing a group of hitsound timestamps sharing a type and
// volume. It lives in core/timeline so the pure timeline layer can produce it
// without depending on audio/ or any platform/device headers.

namespace adofai {

struct HitsoundTimestampGroup {
    std::string type;               // "Kick", "Snare", etc.
    float volume = 100.0f;          // 0-100
    std::vector<double> timestamps; // seconds
};

}  // namespace adofai
