// A small JSON value, for the files the engine keeps beside its caches (a file's facts, a picture's
// note, a folder's last listing) and the cases tests share with the browser. Not a general
// library: numbers are doubles, objects keep their order, and nothing throws.
#pragma once

#include "base/types.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace geo::json {

class Value {
public:
    enum class Kind : u8 { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Value>;

    Value() = default;
    Value(bool value) : kind_(Kind::Bool), number_(value ? 1.0 : 0.0) {}
    Value(double value) : kind_(Kind::Number), number_(value) {}
    Value(int value) : kind_(Kind::Number), number_(value) {}
    Value(u32 value) : kind_(Kind::Number), number_(value) {}
    Value(u64 value) : kind_(Kind::Number), number_(double(value)) {}
    Value(i64 value) : kind_(Kind::Number), number_(double(value)) {}
    Value(std::string value) : kind_(Kind::String), text_(std::move(value)) {}
    Value(std::string_view value) : kind_(Kind::String), text_(value) {}
    Value(const char* value) : kind_(Kind::String), text_(value) {}

    static Value array() {
        Value v;
        v.kind_ = Kind::Array;
        return v;
    }
    static Value object() {
        Value v;
        v.kind_ = Kind::Object;
        return v;
    }

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Kind::Null; }
    bool isNumber() const { return kind_ == Kind::Number; }
    bool isString() const { return kind_ == Kind::String; }
    bool isArray() const { return kind_ == Kind::Array; }
    bool isObject() const { return kind_ == Kind::Object; }

    /// The value as each type, or `fallback` when it is another.
    bool asBool(bool fallback = false) const {
        return kind_ == Kind::Bool ? number_ != 0.0 : fallback;
    }
    double asNumber(double fallback = 0.0) const {
        return kind_ == Kind::Number ? number_ : fallback;
    }
    const std::string& asString() const;

    /// An array's items or an object's members; empty for anything else.
    const std::vector<Value>& items() const;
    const std::vector<Member>& members() const;

    /// A member by name, or a null value when there is none.
    const Value& operator[](std::string_view name) const;
    /// An item by index, or a null value past the end.
    const Value& operator[](usize index) const;
    usize size() const;

    /// Appends to an array, or sets a member of an object (replacing one of the same name).
    Value& push(Value value);
    Value& set(std::string_view name, Value value);

    /// Compact JSON text. Numbers that are whole print without a fraction.
    std::string write() const;

private:
    Kind kind_ = Kind::Null;
    double number_ = 0.0;
    std::string text_;
    std::vector<Value> items_;
    std::vector<Member> members_;
};

/// Parses `text`. Returns false and sets `error` if it is not one JSON value; nesting deeper than
/// 64 is refused, so untrusted text cannot exhaust the stack.
bool parse(std::string_view text, Value& value, std::string& error);

/// Appends `text` as a JSON string literal.
void writeString(std::string& out, std::string_view text);

} // namespace geo::json
