#include "JsonCleaner.hpp"


namespace adofai {

std::string cleanJson(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());

    bool inString = false;
    bool escaped = false;
    char lastOut = 0;

    for (size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            if (c == '\r') continue;
            lastOut = c;
            out.push_back(c);
            continue;
        }

        if (c == '"') {
            inString = true;
            if (lastOut == '}' || lastOut == ']' || lastOut == '"' || (lastOut >= '0' && lastOut <= '9')) {
                out.push_back(',');
            }
            lastOut = '"';
            out.push_back(c);
            continue;
        }

        if (c == '\r') continue;

        if ((c == '{' || c == '[') && (lastOut == '}' || lastOut == ']' || lastOut == '"' || (lastOut >= '0' && lastOut <= '9'))) {
            out.push_back(',');
        }

        if (c == ',') {
            size_t j = i + 1;
            while (j < raw.size() && (raw[j] == ' ' || raw[j] == '\t' || raw[j] == '\n')) j++;
            if (j < raw.size()) {
                char nc = raw[j];
                if (nc == '}' || nc == ']') continue;
                if (nc == ',') continue;
            }
        }

        if (c != ' ' && c != '\t' && c != '\n') lastOut = c;
        out.push_back(c);
    }

    // Post-pass: remove double commas and leading commas after { or [
    std::string out2;
    out2.reserve(out.size());
    bool afterBrace = false;
    bool justSkippedComma = false;
    for (char c : out) {
        if (c == '{' || c == '[') {
            afterBrace = true;
            justSkippedComma = false;
            out2.push_back(c);
        } else if (c == ',' && afterBrace) {
            continue;
        } else if (c == ',' && justSkippedComma) {
            continue;
        } else if (c == ',') {
            justSkippedComma = true;
            out2.push_back(c);
        } else if (c != ' ' && c != '\t' && c != '\n') {
            afterBrace = false;
            justSkippedComma = false;
            out2.push_back(c);
        } else {
            out2.push_back(c);
        }
    }

    return out2;
}

}  // namespace adofai
