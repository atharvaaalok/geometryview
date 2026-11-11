#include "base/json.h"

#include <cmath>
#include <cstdlib>
#include <format>

namespace geo::json {
namespace {

const Value kNull;
const std::string kEmptyString;
const std::vector<Value> kNoItems;
const std::vector<Value::Member> kNoMembers;

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool parseDocument(Value& value, std::string& error) {
        if (!parseValue(value, 0)) {
            error = error_.empty() ? std::format("Unexpected text at {}.", at_) : error_;
            return false;
        }
        skipSpace();
        if (at_ != text_.size()) {
            error = std::format("Unexpected text after the value at {}.", at_);
            return false;
        }
        return true;
    }

private:
    void skipSpace() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\n' ||
                                      text_[at_] == '\r' || text_[at_] == '\t')) {
            ++at_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) {
            return false;
        }
        at_ += word.size();
        return true;
    }

    bool parseValue(Value& value, int depth) {
        if (depth > kMaxDepth) {
            error_ = "The JSON is nested too deeply.";
            return false;
        }
        skipSpace();
        if (at_ >= text_.size()) {
            return false;
        }
        switch (text_[at_]) {
        case '{':
            return parseObject(value, depth);
        case '[':
            return parseArray(value, depth);
        case '"': {
            std::string text;
            if (!parseString(text)) {
                return false;
            }
            value = Value(std::move(text));
            return true;
        }
        case 't':
            value = Value(true);
            return literal("true");
        case 'f':
            value = Value(false);
            return literal("false");
        case 'n':
            value = Value();
            return literal("null");
        default:
            return parseNumber(value);
        }
    }

    bool parseObject(Value& value, int depth) {
        ++at_; // {
        value = Value::object();
        skipSpace();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return true;
        }
        while (true) {
            skipSpace();
            std::string name;
            if (at_ >= text_.size() || text_[at_] != '"' || !parseString(name)) {
                return false;
            }
            skipSpace();
            if (at_ >= text_.size() || text_[at_] != ':') {
                return false;
            }
            ++at_;
            Value member;
            if (!parseValue(member, depth + 1)) {
                return false;
            }
            value.set(name, std::move(member));
            skipSpace();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return true;
            }
            return false;
        }
    }

    bool parseArray(Value& value, int depth) {
        ++at_; // [
        value = Value::array();
        skipSpace();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return true;
        }
        while (true) {
            Value item;
            if (!parseValue(item, depth + 1)) {
                return false;
            }
            value.push(std::move(item));
            skipSpace();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return true;
            }
            return false;
        }
    }

    bool parseNumber(Value& value) {
        const usize start = at_;
        if (at_ < text_.size() && text_[at_] == '-') {
            ++at_;
        }
        const auto digits = [&] {
            const usize from = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
                ++at_;
            }
            return at_ > from;
        };
        if (!digits()) {
            return false;
        }
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            if (!digits()) {
                return false;
            }
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) {
                ++at_;
            }
            if (!digits()) {
                return false;
            }
        }
        const std::string copy(text_.substr(start, at_ - start));
        value = Value(std::strtod(copy.c_str(), nullptr));
        return true;
    }

    bool hex4(u32& code) {
        if (text_.size() - at_ < 4) {
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code |= u32(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= u32(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= u32(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    static void append(std::string& out, u32 code) {
        if (code < 0x80) {
            out += char(code);
        } else if (code < 0x800) {
            out += char(0xC0 | (code >> 6));
            out += char(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += char(0xE0 | (code >> 12));
            out += char(0x80 | ((code >> 6) & 0x3F));
            out += char(0x80 | (code & 0x3F));
        } else {
            out += char(0xF0 | (code >> 18));
            out += char(0x80 | ((code >> 12) & 0x3F));
            out += char(0x80 | ((code >> 6) & 0x3F));
            out += char(0x80 | (code & 0x3F));
        }
    }

    bool parseString(std::string& out) {
        ++at_; // "
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') {
                return true;
            }
            if (u8(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) {
                return false;
            }
            const char e = text_[at_++];
            switch (e) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'u': {
                u32 code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code < 0xDC00 && text_.substr(at_, 2) == "\\u") {
                    at_ += 2;
                    u32 low = 0;
                    if (!hex4(low) || low < 0xDC00 || low >= 0xE000) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                append(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    std::string_view text_;
    usize at_ = 0;
    std::string error_;
};

void writeValue(std::string& out, const Value& value) {
    switch (value.kind()) {
    case Value::Kind::Null:
        out += "null";
        break;
    case Value::Kind::Bool:
        out += value.asBool() ? "true" : "false";
        break;
    case Value::Kind::Number: {
        const double number = value.asNumber();
        if (!std::isfinite(number)) {
            out += "null";
        } else if (number == std::trunc(number) && std::fabs(number) < 9007199254740992.0) {
            out += std::format("{}", i64(number));
        } else {
            out += std::format("{}", number);
        }
        break;
    }
    case Value::Kind::String:
        writeString(out, value.asString());
        break;
    case Value::Kind::Array: {
        out += '[';
        bool first = true;
        for (const Value& item : value.items()) {
            if (!first) {
                out += ',';
            }
            first = false;
            writeValue(out, item);
        }
        out += ']';
        break;
    }
    case Value::Kind::Object: {
        out += '{';
        bool first = true;
        for (const auto& [name, member] : value.members()) {
            if (!first) {
                out += ',';
            }
            first = false;
            writeString(out, name);
            out += ':';
            writeValue(out, member);
        }
        out += '}';
        break;
    }
    }
}

} // namespace

const std::string& Value::asString() const {
    return kind_ == Kind::String ? text_ : kEmptyString;
}

const std::vector<Value>& Value::items() const {
    return kind_ == Kind::Array ? items_ : kNoItems;
}

const std::vector<Value::Member>& Value::members() const {
    return kind_ == Kind::Object ? members_ : kNoMembers;
}

const Value& Value::operator[](std::string_view name) const {
    for (const auto& [key, member] : members()) {
        if (key == name) {
            return member;
        }
    }
    return kNull;
}

const Value& Value::operator[](usize index) const {
    return index < items().size() ? items_[index] : kNull;
}

usize Value::size() const {
    return kind_ == Kind::Array ? items_.size() : kind_ == Kind::Object ? members_.size() : 0;
}

Value& Value::push(Value value) {
    if (kind_ != Kind::Array) {
        *this = array();
    }
    items_.push_back(std::move(value));
    return items_.back();
}

Value& Value::set(std::string_view name, Value value) {
    if (kind_ != Kind::Object) {
        *this = object();
    }
    for (auto& [key, member] : members_) {
        if (key == name) {
            member = std::move(value);
            return member;
        }
    }
    members_.emplace_back(std::string(name), std::move(value));
    return members_.back().second;
}

std::string Value::write() const {
    std::string out;
    writeValue(out, *this);
    return out;
}

bool parse(std::string_view text, Value& value, std::string& error) {
    Parser parser(text);
    return parser.parseDocument(value, error);
}

void writeString(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (u8(c) < 0x20) {
                out += std::format("\\u{:04x}", unsigned(u8(c)));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

} // namespace geo::json
