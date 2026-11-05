#pragma once

// Rejected: the server will never accept this file, so sending it again is
// pointless. Failed: anything else, so it's worth trying again later.
enum class UploadResult { Ok, Rejected, Failed };

// Upload a file from SD card to the OCI receiver, streaming in chunks.
// kind, if given, picks a different destination folder on the server
// (e.g. "trailcam").
UploadResult uploadFile(const char* sd_path, const char* kind = nullptr);
