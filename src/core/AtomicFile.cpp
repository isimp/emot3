#include "AtomicFile.h"
#include "Logging.h"

#include <fstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

bool AtomicWriteFile(const std::string& path, const std::string& content,
                     bool binary) {
    const std::string tmp = path + ".tmp";
    {
        // Text mode (default): '\n' -> "\r\n" on Windows, matching the previous
        // direct-ofstream writers so the on-disk line endings don't change.
        // binary: bytes verbatim (the preset writer opened std::ios::binary).
        std::ios::openmode mode = std::ios::trunc | (binary ? std::ios::binary : std::ios::openmode(0));
        std::ofstream f(tmp, mode);
        if (!f.is_open()) {
            LOG_WARNING("Atomic write: could not open %s", tmp.c_str());
            return false;
        }
        f.write(content.data(), (std::streamsize)content.size());
        f.flush();
        if (!f.good()) {
            LOG_WARNING("Atomic write: write to %s failed (disk full?)", tmp.c_str());
            f.close();
            DeleteFileA(tmp.c_str());
            return false;
        }
    }  // close f before the rename
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOG_WARNING("Atomic write: replace %s failed (err %lu); left previous file intact",
                    path.c_str(), (unsigned long)GetLastError());
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}
