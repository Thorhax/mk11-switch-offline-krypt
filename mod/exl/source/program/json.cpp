#include "json.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mk11::json {

    enum class Type { Null, Bool, Number, String, Array, Object };

    struct Value {
        Type type = Type::Null;
        char* text = nullptr;     // number literal or string contents
        bool flag = false;
        Value** items = nullptr;  // array elements / object values
        char** keys = nullptr;    // object keys
        size_t count = 0, capacity = 0;
    };

    namespace {
        Value* New(Type type) {
            auto v = static_cast<Value*>(calloc(1, sizeof(Value)));
            if (v) v->type = type;
            return v;
        }

        char* Dup(const char* s, size_t n) {
            auto out = static_cast<char*>(malloc(n + 1));
            if (out) { memcpy(out, s, n); out[n] = 0; }
            return out;
        }

        bool Append(Value* c, char* key, Value* item) {
            if (c->count == c->capacity) {
                size_t cap = c->capacity ? c->capacity * 2 : 4;
                auto items = static_cast<Value**>(realloc(c->items, cap * sizeof(Value*)));
                if (!items) return false;
                c->items = items;
                if (c->type == Type::Object) {
                    auto keys = static_cast<char**>(realloc(c->keys, cap * sizeof(char*)));
                    if (!keys) return false;
                    c->keys = keys;
                }
                c->capacity = cap;
            }
            if (c->type == Type::Object) c->keys[c->count] = key;
            c->items[c->count++] = item;
            return true;
        }

        struct Parser {
            const char* p;

            void Skip() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }

            // String contents are kept escaped exactly as written, so serialising gives them back.
            char* RawString() {
                if (*p != '"') return nullptr;
                const char* start = ++p;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                if (*p != '"') return nullptr;
                char* s = Dup(start, p - start);
                p++;
                return s;
            }

            Value* Parse() {
                Skip();
                if (*p == '{') {
                    p++;
                    Value* v = New(Type::Object);
                    Skip();
                    if (*p == '}') { p++; return v; }
                    for (;;) {
                        Skip();
                        char* key = RawString();
                        Skip();
                        if (!key || *p != ':') { free(key); Destroy(v); return nullptr; }
                        p++;
                        Value* item = Parse();
                        if (!item || !Append(v, key, item)) { free(key); Destroy(item); Destroy(v); return nullptr; }
                        Skip();
                        if (*p == ',') { p++; continue; }
                        if (*p == '}') { p++; return v; }
                        Destroy(v);
                        return nullptr;
                    }
                }
                if (*p == '[') {
                    p++;
                    Value* v = New(Type::Array);
                    Skip();
                    if (*p == ']') { p++; return v; }
                    for (;;) {
                        Value* item = Parse();
                        if (!item || !Append(v, nullptr, item)) { Destroy(item); Destroy(v); return nullptr; }
                        Skip();
                        if (*p == ',') { p++; continue; }
                        if (*p == ']') { p++; return v; }
                        Destroy(v);
                        return nullptr;
                    }
                }
                if (*p == '"') {
                    Value* v = New(Type::String);
                    v->text = RawString();
                    if (!v->text) { Destroy(v); return nullptr; }
                    return v;
                }
                if (!strncmp(p, "true", 4)) { p += 4; Value* v = New(Type::Bool); v->flag = true; return v; }
                if (!strncmp(p, "false", 5)) { p += 5; return New(Type::Bool); }
                if (!strncmp(p, "null", 4)) { p += 4; return New(Type::Null); }
                const char* start = p;
                while (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9')) p++;
                if (p == start) return nullptr;
                Value* v = New(Type::Number);
                v->text = Dup(start, p - start);
                return v;
            }
        };

        Value* Copy(const Value* v) {
            Value* c = New(v->type);
            c->flag = v->flag;
            if (v->text) c->text = Dup(v->text, strlen(v->text));
            for (size_t i = 0; i < v->count; i++)
                Append(c, v->type == Type::Object ? Dup(v->keys[i], strlen(v->keys[i])) : nullptr, Copy(v->items[i]));
            return c;
        }

        void Replace(Value* dst, const Value* src) {
            Value* c = Copy(src);
            Value old = *dst;
            *dst = *c;
            free(c);
            // free the old contents
            Value* tmp = New(old.type);
            *tmp = old;
            Destroy(tmp);
        }

        // The child named by one path segment; with `create`, a missing object key is added as null.
        Value* Child(Value* v, const char* seg, size_t len, bool create) {
            if (v->type == Type::Object) {
                for (size_t i = 0; i < v->count; i++)
                    if (strlen(v->keys[i]) == len && !strncmp(v->keys[i], seg, len)) return v->items[i];
                if (!create) return nullptr;
                Value* item = New(Type::Null);
                Append(v, Dup(seg, len), item);
                return item;
            }
            if (v->type == Type::Array) {
                char* end;
                char buf[16];
                if (len >= sizeof(buf)) return nullptr;
                memcpy(buf, seg, len);
                buf[len] = 0;
                unsigned long i = strtoul(buf, &end, 10);
                return (*end || i >= v->count) ? nullptr : v->items[i];
            }
            return nullptr;
        }

        Value* Walk(Value* root, const char* path, bool create) {
            Value* v = root;
            while (v && *path) {
                const char* dot = strchr(path, '.');
                size_t len = dot ? (size_t)(dot - path) : strlen(path);
                v = Child(v, path, len, create);
                path += len + (dot ? 1 : 0);
            }
            return v;
        }

        struct Writer {
            char* buf = nullptr;
            size_t len = 0, cap = 0;
            void Put(const char* s, size_t n) {
                if (len + n + 1 > cap) {
                    size_t c = cap ? cap * 2 : 256;
                    while (c < len + n + 1) c *= 2;
                    auto b = static_cast<char*>(realloc(buf, c));
                    if (!b) return;
                    buf = b;
                    cap = c;
                }
                memcpy(buf + len, s, n);
                len += n;
                buf[len] = 0;
            }
            void Put(const char* s) { Put(s, strlen(s)); }
            void Write(const Value* v) {
                switch (v->type) {
                    case Type::Null: Put("null"); break;
                    case Type::Bool: Put(v->flag ? "true" : "false"); break;
                    case Type::Number: Put(v->text); break;
                    case Type::String: Put("\""); Put(v->text); Put("\""); break;
                    case Type::Array:
                        Put("[");
                        for (size_t i = 0; i < v->count; i++) { if (i) Put(","); Write(v->items[i]); }
                        Put("]");
                        break;
                    case Type::Object:
                        Put("{");
                        for (size_t i = 0; i < v->count; i++) {
                            if (i) Put(",");
                            Put("\""); Put(v->keys[i]); Put("\":");
                            Write(v->items[i]);
                        }
                        Put("}");
                        break;
                }
            }
        };
    }

    Value* Parse(const char* text) {
        if (!text) return nullptr;
        Parser parser{ text };
        Value* v = parser.Parse();
        parser.Skip();
        if (v && *parser.p) { Destroy(v); return nullptr; }
        return v;
    }

    void Destroy(Value* v) {
        if (!v) return;
        for (size_t i = 0; i < v->count; i++) {
            Destroy(v->items[i]);
            if (v->keys) free(v->keys[i]);
        }
        free(v->items);
        free(v->keys);
        free(v->text);
        free(v);
    }

    bool Set(Value* root, const char* path, const Value* value) {
        Value* target = Walk(root, path, true);
        if (!target) return false;
        Replace(target, value);
        return true;
    }

    Value* Find(Value* root, const char* path) { return Walk(root, path, false); }

    const char* String(const Value* v) { return v && v->type == Type::String ? v->text : nullptr; }

    size_t Count(const Value* v) { return v && v->type == Type::Array ? v->count : 0; }
    Value* At(const Value* v, size_t i) { return i < Count(v) ? v->items[i] : nullptr; }

    bool Integer(const Value* v, long long* out) {
        if (!v || v->type != Type::Number) return false;
        *out = strtoll(v->text, nullptr, 10);
        return true;
    }

    bool SetInteger(Value* root, const char* path, long long value) {
        char text[32];
        snprintf(text, sizeof(text), "%lld", value);
        Value number{};
        number.type = Type::Number;
        number.text = text;
        return Set(root, path, &number);
    }

    char* Serialize(const Value* v) {
        Writer w;
        w.Write(v);
        return w.buf;
    }
}
