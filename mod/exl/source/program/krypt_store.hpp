#pragma once
// On-console storage for Krypt progression: one JSON value per save key, kept in memory and
// mirrored to sdmc:/mk11offline/krypt.dat (previous version kept as krypt.bak).

#include <cstddef>

namespace mk11::store {
    // Returns the stored JSON for `key`, or nullptr.
    const char* Get(const char* key);
    void Put(const char* key, const char* json);
    void Remove(const char* key);
    // Calls fn for every stored key starting with `prefix`.
    void ForEach(const char* prefix, void (*fn)(const char* key, const char* json, void* context), void* context);
    // Writes the store to the SD card if anything changed since the last flush. Inside a Batch it only
    // takes effect when the outermost Batch ends, so a reward batch rewrites the file once.
    void Flush();

    struct Batch {
        Batch();
        ~Batch();
        Batch(const Batch&) = delete;
    };
    size_t Count();

    // Appends one Krypt inventory batch (JSON) to sdmc:/mk11offline/inventory_batches.jsonl, so the
    // rewards the server would have granted are kept until they can be applied on the console.
    void RecordBatch(const char* json);
    // Appends `json` as one line to sdmc:/mk11offline/<file> (rotated to <file>.old above 1 MB).
    void AppendLine(const char* file, const char* json);

    // Test aid: returns the contents of sdmc:/mk11offline/replay_batch.json once (then deletes it),
    // or nullptr. The caller frees the result with game::Free.
    char* TakeReplayBatch();
}
