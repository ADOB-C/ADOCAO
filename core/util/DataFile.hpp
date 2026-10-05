#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Read a data file from ADOCAO-data.zip, ADOCAO-data/ folder, or direct path.
// Returns empty vector if not found.

namespace adofai {

std::vector<uint8_t> readDataFile(const std::string& relativePath);

}  // namespace adofai
