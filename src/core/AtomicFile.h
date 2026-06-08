#pragma once

// Crash-safe file write: write `content` to a sibling ".tmp" file, then
// atomically rename it over `path`. The live file is never truncated until the
// complete new content is on disk, so a crash / disk-full / exception mid-write
// leaves the PREVIOUS good file intact (a direct std::ofstream(path) truncates
// the live file the moment it opens it - how a mid-save crash wiped emotes.json).
// Returns false (and logs) on failure, leaving `path` untouched.
//
// `path` is expected to be ASCII (the addon's config files under addons/emot3/),
// so the ANSI rename is fine; not for arbitrary Unicode paths.

#include <string>

// binary=false (default): text mode, so '\n' -> "\r\n" on Windows, matching the
// hand-rolled settings/catalog writers. binary=true: bytes written verbatim (for
// the preset writer, which controls its own line endings via the JSON dump).
bool AtomicWriteFile(const std::string& path, const std::string& content,
                     bool binary = false);
