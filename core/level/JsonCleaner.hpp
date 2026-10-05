#pragma once

#include <string>

// Clean ADOFAI JSON (fix missing commas, trailing commas, double commas)

namespace adofai {

std::string cleanJson(const std::string& raw);

}  // namespace adofai
