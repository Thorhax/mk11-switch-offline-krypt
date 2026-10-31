#pragma once
// A small JSON document for the records the module keeps on the SD card (tower portal data).
// Supports parsing, dot-path assignment as Hydra update commands use it ("data.stages.2.isActive":
// object keys, or array indices for arrays) and compact serialisation.

#include <cstddef>

namespace mk11::json {

    struct Value;

    // Parses `text`; nullptr when it is not valid JSON. Free with Destroy.
    Value* Parse(const char* text);
    void Destroy(Value* v);

    // Replaces the value at the dot path with a copy of `value`, creating missing object keys.
    // Returns false when the path runs through a non-container or an out-of-range array index.
    bool Set(Value* root, const char* path, const Value* value);

    // The value at the dot path, or nullptr.
    Value* Find(Value* root, const char* path);
    // The string at `v` (nullptr unless it is a string).
    const char* String(const Value* v);
    // The integer at `v`; false unless it is a number.
    bool Integer(const Value* v, long long* out);
    // Replaces the value at the dot path with an integer (creating missing object keys).
    bool SetInteger(Value* root, const char* path, long long value);

    // Array elements: the count (0 unless `v` is an array) and the element at `i`.
    size_t Count(const Value* v);
    Value* At(const Value* v, size_t i);

    // Compact JSON text; free with std::free.
    char* Serialize(const Value* v);
}
