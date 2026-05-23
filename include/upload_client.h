#pragma once

// Upload a file from SD card to the OCI receiver, streaming in chunks.
// Returns true on HTTP 200/201. Silent fail — caller should not treat
// upload failure as fatal.
bool uploadFile(const char* sd_path);
