#ifndef LSP_JSON_RPC_H
#define LSP_JSON_RPC_H

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <variant>
#include <cstdint>

namespace lsp {

enum class JsonValueKind {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

struct JsonValue {
    JsonValueKind kind = JsonValueKind::Null;
    bool          bool_val = false;
    double        num_val = 0.0;
    int64_t       int_val = 0;
    bool          is_int = false;
    std::string   str_val;
    std::vector<JsonValue> arr_val;
    std::unordered_map<std::string, JsonValue> obj_val;

    JsonValue() = default;
    static JsonValue make_null();
    static JsonValue make_bool(bool v);
    static JsonValue make_number(double v);
    static JsonValue make_int(int64_t v);
    static JsonValue make_string(std::string v);
    static JsonValue make_array(std::vector<JsonValue> v);
    static JsonValue make_object(std::unordered_map<std::string, JsonValue> v);

    const JsonValue *get(const std::string &key) const;
    JsonValue *get_mut(const std::string &key);
    void set(const std::string &key, JsonValue val);
};

class JsonParser {
public:
    explicit JsonParser(std::string input);
    JsonValue parse();
    std::string error() const { return error_; }

private:
    char peek() const;
    char advance();
    void skip_ws();
    JsonValue parse_value();
    JsonValue parse_string();
    JsonValue parse_number();
    JsonValue parse_keyword();
    JsonValue parse_array();
    JsonValue parse_object();
    void error_at(const std::string &msg);

    std::string input_;
    size_t pos_ = 0;
    std::string error_;
};

std::string json_serialize(const JsonValue &v);

struct JsonRpcMessage {
    std::string jsonrpc = "2.0";
    std::string method;
    JsonValue params;
    JsonValue id;
    bool has_id = false;
    bool is_notification = false;
};

JsonRpcMessage parse_jsonrpc(const JsonValue &v);
JsonValue make_jsonrpc_request(const std::string &method, JsonValue params, JsonValue id);
JsonValue make_jsonrpc_notification(const std::string &method, JsonValue params);
JsonValue make_jsonrpc_response(JsonValue id, JsonValue result);
JsonValue make_jsonrpc_error(JsonValue id, int code, const std::string &message);

struct LspMessage {
    std::string body;
    size_t content_length = 0;

    bool read_from(const std::string &raw);
    static std::string encode(const std::string &body);
    static std::string serialize_and_encode(const JsonValue &msg);
};

} // namespace lsp

#endif
