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
    // MOVEFILE_REPLACE_EXISTING only — deliberately NOT MOVEFILE_WRITE_THROUGH.
    // The two guarantees are independent: the temp+rename gives ATOMICITY (readers
    // never see a torn file; a crash leaves the complete new or the complete
    // previous file — what this writer exists for, since NTFS journals the rename
    // metadata), while WRITE_THROUGH would add DURABILITY by blocking the caller on
    // a physical-media flush every save. That device flush is the multi-millisecond
    // stall that made casual saves (toggle a checkbox, switch a category) hitch; for
    // an addon config it isn't worth it. Worst case after a hard power-loss seconds
    // after a save: the config reverts to the previous valid version (and the
    // loaders heal/default a bad file anyway). The cheap f.flush() above still
    // pushes the bytes into the OS cache before the rename — no device flush.
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        LOG_WARNING("Atomic write: replace %s failed (err %lu); left previous file intact",
                    path.c_str(), (unsigned long)GetLastError());
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

bool DirExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}
