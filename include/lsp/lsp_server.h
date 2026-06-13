#ifndef LSP_SERVER_H
#define LSP_SERVER_H

#include "lsp/json_rpc.h"
#include "lsp/lsp_diagnostics.h"
#include "vex/compiler.h"
#include "vex/diagnostic.h"
#include "vex/ast.h"
#include "vex/lexer.h"
#include "vex/parser.h"
#include "vex/type_checker.h"
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include <memory>

namespace lsp {

struct DocumentState {
    std::string uri;
    std::string content;
    int64_t     version = 0;
    std::string language_id;
};

struct SymbolInfo {
    std::string name;
    std::string kind;       // "Variable", "Function", "Keyword", etc.
    std::string detail;     // type info or signature
};

struct DefinitionLocation {
    std::string uri;
    uint32_t line = 1;
    uint32_t column = 1;
};

class LspServer {
public:
    LspServer();
    ~LspServer();

    void set_compile_options(const vex::CompileOptions &opts);
    const vex::CompileOptions &compile_options() const { return opts_; }

    void handle_message(const std::string &raw);
    void run();

private:
    // --- Protocol handlers ---
    void handle_request(const JsonRpcMessage &msg, const JsonValue &raw);
    void handle_notification(const JsonRpcMessage &msg, const JsonValue &raw);

    // --- LSP handlers ---
    JsonValue handle_initialize(const JsonValue &params, const JsonValue &id);
    JsonValue handle_shutdown(const JsonValue &id);
    void handle_did_open(const JsonValue &params);
    void handle_did_change(const JsonValue &params);
    void handle_did_close(const JsonValue &params);
    JsonValue handle_completion(const JsonValue &params, const JsonValue &id);
    JsonValue handle_definition(const JsonValue &params, const JsonValue &id);
    JsonValue handle_hover(const JsonValue &params, const JsonValue &id);
    JsonValue handle_diagnostic(const JsonValue &params, const JsonValue &id);

    // --- Compiler helpers ---
    void compile_document(const std::string &uri);
    std::vector<vex::Diagnostic> get_last_diagnostics(const std::string &uri) const;

    // --- AST traversal helpers ---
    struct FindResult {
        const vex::ast::Node *node = nullptr;
        std::string scope_name;   // enclosing function/var name
    };
    FindResult find_node_at(const vex::ast::Node &node, uint32_t line, uint32_t column) const;
    void find_node_at_rec(const vex::ast::Node &node, uint32_t line, uint32_t column,
                          FindResult &result, const std::string &scope) const;

    // --- Completion helpers ---
    std::vector<SymbolInfo> get_keyword_completions() const;
    std::vector<SymbolInfo> get_symbol_completions(const std::string &uri) const;

    // --- Send helpers ---
    void send_notification(const std::string &method, JsonValue params);
    void send_response(JsonValue id, JsonValue result);
    void publish_diagnostics(const std::string &uri);

    // --- I/O ---
    void write_output(const std::string &data);
    std::string read_input();

    // --- State ---
    vex::CompileOptions opts_;
    std::unordered_map<std::string, DocumentState> documents_;
    std::unordered_map<std::string, vex::CompileResult> last_compile_results_;
    std::unordered_map<std::string, std::unique_ptr<vex::ast::ModuleNode>> last_ast_;
    std::unordered_map<std::string, std::unique_ptr<vex::TypeChecker>> last_type_checkers_;
    bool initialized_ = false;
    bool shutdown_ = false;

    // Input buffer for partial messages
    std::string input_buffer_;
};

} // namespace lsp

#endif
