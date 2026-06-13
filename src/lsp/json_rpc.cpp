#include "lsp/json_rpc.h"
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <cstring>

namespace lsp {

// --- JsonValue ---

JsonValue JsonValue::make_null() { return {}; }
JsonValue JsonValue::make_bool(bool v) { JsonValue j; j.kind = JsonValueKind::Bool; j.bool_val = v; return j; }
JsonValue JsonValue::make_number(double v) { JsonValue j; j.kind = JsonValueKind::Number; j.num_val = v; return j; }
JsonValue JsonValue::make_int(int64_t v) { JsonValue j; j.kind = JsonValueKind::Number; j.is_int = true; j.int_val = v; j.num_val = (double)v; return j; }
JsonValue JsonValue::make_string(std::string v) { JsonValue j; j.kind = JsonValueKind::String; j.str_val = std::move(v); return j; }
JsonValue JsonValue::make_array(std::vector<JsonValue> v) { JsonValue j; j.kind = JsonValueKind::Array; j.arr_val = std::move(v); return j; }

JsonValue JsonValue::make_object(std::unordered_map<std::string, JsonValue> v) {
    JsonValue j; j.kind = JsonValueKind::Object; j.obj_val = std::move(v); return j;
}

const JsonValue *JsonValue::get(const std::string &key) const {
    if (kind != JsonValueKind::Object) return nullptr;
    auto it = obj_val.find(key);
    return it != obj_val.end() ? &it->second : nullptr;
}

JsonValue *JsonValue::get_mut(const std::string &key) {
    if (kind != JsonValueKind::Object) return nullptr;
    auto it = obj_val.find(key);
    return it != obj_val.end() ? &it->second : nullptr;
}

void JsonValue::set(const std::string &key, JsonValue val) {
    if (kind != JsonValueKind::Object) {
        kind = JsonValueKind::Object;
        obj_val.clear();
    }
    obj_val[key] = std::move(val);
}

// --- JsonParser ---

JsonParser::JsonParser(std::string input) : input_(std::move(input)) {}

char JsonParser::peek() const {
    return pos_ < input_.size() ? input_[pos_] : '\0';
}

char JsonParser::advance() {
    return pos_ < input_.size() ? input_[pos_++] : '\0';
}

void JsonParser::skip_ws() {
    while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t' ||
           input_[pos_] == '\n' || input_[pos_] == '\r'))
        pos_++;
}

void JsonParser::error_at(const std::string &msg) {
    if (error_.empty()) error_ = msg + " at pos " + std::to_string(pos_);
}

JsonValue JsonParser::parse() {
    skip_ws();
    if (pos_ >= input_.size()) {
        error_at("empty input");
        return {};
    }
    auto v = parse_value();
    skip_ws();
    return v;
}

JsonValue JsonParser::parse_value() {
    skip_ws();
    char c = peek();
    if (c == '"') return parse_string();
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
    if (c == 't' || c == 'f' || c == 'n') return parse_keyword();
    error_at(std::string("unexpected char: ") + c);
    return {};
}

JsonValue JsonParser::parse_string() {
    if (advance() != '"') { error_at("expected '\"'"); return {}; }
    std::string s;
    while (pos_ < input_.size()) {
        char c = advance();
        if (c == '"') return JsonValue::make_string(s);
        if (c == '\\') {
            if (pos_ >= input_.size()) break;
            char esc = advance();
            switch (esc) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    if (pos_ + 4 > input_.size()) break;
                    std::string hex = input_.substr(pos_, 4);
                    pos_ += 4;
                    char32_t cp = std::stoul(hex, nullptr, 16);
                    if (cp < 0x80) s += (char)cp;
                    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
                    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: s += esc; break;
            }
        } else {
            s += c;
        }
    }
    error_at("unterminated string");
    return JsonValue::make_string(s);
}

JsonValue JsonParser::parse_number() {
    size_t start = pos_;
    if (peek() == '-') advance();
    while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') advance();
    bool is_float = false;
    if (pos_ < input_.size() && input_[pos_] == '.') {
        is_float = true;
        advance();
        while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') advance();
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
        is_float = true;
        advance();
        if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) advance();
        while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') advance();
    }
    std::string num_str = input_.substr(start, pos_ - start);
    if (is_float) return JsonValue::make_number(std::stod(num_str));
    return JsonValue::make_int((int64_t)std::stoll(num_str));
}

JsonValue JsonParser::parse_keyword() {
    if (input_.substr(pos_, 4) == "true") { pos_ += 4; return JsonValue::make_bool(true); }
    if (input_.substr(pos_, 5) == "false") { pos_ += 5; return JsonValue::make_bool(false); }
    if (input_.substr(pos_, 4) == "null") { pos_ += 4; return JsonValue::make_null(); }
    error_at("invalid keyword");
    return {};
}

JsonValue JsonParser::parse_array() {
    if (advance() != '[') { error_at("expected '['"); return {}; }
    std::vector<JsonValue> arr;
    skip_ws();
    if (peek() == ']') { advance(); return JsonValue::make_array(arr); }
    while (true) {
        arr.push_back(parse_value());
        skip_ws();
        char c = advance();
        if (c == ']') return JsonValue::make_array(std::move(arr));
        if (c != ',') { error_at("expected ',' or ']'"); return {}; }
        skip_ws();
    }
}

JsonValue JsonParser::parse_object() {
    if (advance() != '{') { error_at("expected '{'"); return {}; }
    std::unordered_map<std::string, JsonValue> obj;
    skip_ws();
    if (peek() == '}') { advance(); return JsonValue::make_object(obj); }
    while (true) {
        skip_ws();
        if (peek() != '"') { error_at("expected string key"); return {}; }
        auto key_val = parse_string();
        if (!key_val.str_val.empty() || key_val.kind == JsonValueKind::String) {}
        skip_ws();
        if (advance() != ':') { error_at("expected ':'"); return {}; }
        skip_ws();
        obj[key_val.str_val] = parse_value();
        skip_ws();
        char c = advance();
        if (c == '}') return JsonValue::make_object(std::move(obj));
        if (c != ',') { error_at("expected ',' or '}'"); return {}; }
    }
}

// --- JSON serializer ---

static void json_serialize_internal(std::ostream &os, const JsonValue &v) {
    switch (v.kind) {
        case JsonValueKind::Null:   os << "null"; break;
        case JsonValueKind::Bool:   os << (v.bool_val ? "true" : "false"); break;
        case JsonValueKind::Number:
            if (v.is_int) os << v.int_val;
            else os << v.num_val;
            break;
        case JsonValueKind::String:
            os << '"';
            for (char c : v.str_val) {
                switch (c) {
                    case '"': os << "\\\""; break;
                    case '\\': os << "\\\\"; break;
                    case '\b': os << "\\b"; break;
                    case '\f': os << "\\f"; break;
                    case '\n': os << "\\n"; break;
                    case '\r': os << "\\r"; break;
                    case '\t': os << "\\t"; break;
                    default:
                        if ((unsigned char)c < 0x20) {
                            char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                            os << buf;
                        } else os << c;
                }
            }
            os << '"';
            break;
        case JsonValueKind::Array:
            os << '[';
            for (size_t i = 0; i < v.arr_val.size(); ++i) {
                if (i) os << ',';
                json_serialize_internal(os, v.arr_val[i]);
            }
            os << ']';
            break;
        case JsonValueKind::Object:
            os << '{';
            bool first = true;
            for (const auto &[key, val] : v.obj_val) {
                if (!first) os << ',';
                first = false;
                JsonValue k; k.kind = JsonValueKind::String; k.str_val = key;
                json_serialize_internal(os, k);
                os << ':';
                json_serialize_internal(os, val);
            }
            os << '}';
            break;
    }
}

std::string json_serialize(const JsonValue &v) {
    std::ostringstream os;
    json_serialize_internal(os, v);
    return os.str();
}

// --- JSON-RPC ---

JsonRpcMessage parse_jsonrpc(const JsonValue &v) {
    JsonRpcMessage msg;
    if (v.kind != JsonValueKind::Object) return msg;
    auto *method = v.get("method");
    if (method && method->kind == JsonValueKind::String) msg.method = method->str_val;
    auto *params = v.get("params");
    if (params) msg.params = *params;
    auto *id = v.get("id");
    if (id && id->kind != JsonValueKind::Null) {
        msg.id = *id;
        msg.has_id = true;
    }
    auto *jsonrpc = v.get("jsonrpc");
    if (jsonrpc && jsonrpc->kind == JsonValueKind::String) msg.jsonrpc = jsonrpc->str_val;
    msg.is_notification = !msg.has_id;
    return msg;
}

JsonValue make_jsonrpc_request(const std::string &method, JsonValue params, JsonValue id) {
    auto obj = JsonValue::make_object({});
    obj.set("jsonrpc", JsonValue::make_string("2.0"));
    obj.set("method", JsonValue::make_string(method));
    obj.set("params", std::move(params));
    obj.set("id", std::move(id));
    return obj;
}

JsonValue make_jsonrpc_notification(const std::string &method, JsonValue params) {
    auto obj = JsonValue::make_object({});
    obj.set("jsonrpc", JsonValue::make_string("2.0"));
    obj.set("method", JsonValue::make_string(method));
    obj.set("params", std::move(params));
    return obj;
}

JsonValue make_jsonrpc_response(JsonValue id, JsonValue result) {
    auto obj = JsonValue::make_object({});
    obj.set("jsonrpc", JsonValue::make_string("2.0"));
    obj.set("id", std::move(id));
    obj.set("result", std::move(result));
    return obj;
}

JsonValue make_jsonrpc_error(JsonValue id, int code, const std::string &message) {
    auto obj = JsonValue::make_object({});
    obj.set("jsonrpc", JsonValue::make_string("2.0"));
    obj.set("id", std::move(id));
    auto err = JsonValue::make_object({});
    err.set("code", JsonValue::make_int(code));
    err.set("message", JsonValue::make_string(message));
    obj.set("error", std::move(err));
    return obj;
}

// --- LspMessage (Content-Length framing) ---

bool LspMessage::read_from(const std::string &raw) {
    static const char HEADER_END[] = "\r\n\r\n";
    auto pos = raw.find(HEADER_END);
    if (pos == std::string::npos) return false;

    std::string header = raw.substr(0, pos);
    static const char CL[] = "Content-Length: ";
    auto clpos = header.find(CL);
    if (clpos == std::string::npos) {
        static const char CL_LOWER[] = "content-length: ";
        clpos = header.find(CL_LOWER);
        if (clpos == std::string::npos) return false;
        content_length = (size_t)std::stoul(header.substr(clpos + strlen(CL_LOWER)));
    } else {
        content_length = (size_t)std::stoul(header.substr(clpos + strlen(CL)));
    }

    size_t body_start = pos + 4;
    if (raw.size() < body_start + content_length) return false;

    body = raw.substr(body_start, content_length);
    return true;
}

std::string LspMessage::encode(const std::string &body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string LspMessage::serialize_and_encode(const JsonValue &msg) {
    return LspMessage::encode(json_serialize(msg));
}

} // namespace lsp
