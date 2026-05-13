#pragma once

#include <Arduino.h>

// Posts a sample of JPEG frames from an AVI on the SD card to the Claude API
// and returns the resulting description text.
class ClaudeClient {
public:
    // Samples sample_count frames evenly from the AVI at avi_path and asks
    // Claude to summarise the activity. Writes the response into out.
    // Returns true on a 200 OK with a parsable summary.
    bool summarise(const char* avi_path, uint8_t sample_count, String& out);
};
