#pragma once

// Coalesced, off-thread config writer (v1.2.2 save-cost work; see emot3.md).
//
// Before: every UI change wrote its whole config file synchronously on the render
// thread, with a forced physical-disk flush — so a checkbox toggle or category
// switch hitched, and a collapsed Quickbar re-saved every frame. Now:
//
//  * UI mutation sites call RequestSave(kind) — O(1), no I/O — to mark a config
//    dirty instead of writing.
//  * PumpSaves(), called once per frame from AddonRender, waits until a config's
//    changes have SETTLED (a short debounce), then serializes it on the render
//    thread and hands the bytes to a background writer thread. A burst of edits, a
//    slider drag, or a stuck per-frame dirtier all collapse to ONE write.
//  * Disk I/O (the temp+rename in AtomicWriteFile) happens on the writer thread,
//    so even the larger emotes.json / me_motes.json never stall a frame.
//
// Pure navigation state (active QB category, section/category collapse) marks
// NOTHING dirty — it rides along the next real write and the unload flush, so
// switching categories costs zero disk I/O.

enum class SaveKind { Settings, Emotes, MeMotes };

void StartSaveWriter();        // AddonLoad: spawn the writer thread
void RequestSave(SaveKind k);  // render thread: mark this config dirty (debounced)
void PumpSaves();              // AddonRender, once/frame: flush settled-dirty configs
void FlushSavesBlocking();     // AddonUnload: write pending + settings, join the writer
