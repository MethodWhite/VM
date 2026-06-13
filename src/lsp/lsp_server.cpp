#include "lsp/lsp_server.h"
#include "vex/compiler.h"
#include "vex/diagnostic.h"
#include "vex/lexer.h"
#include "vex/parser.h"
#include "vex/type_checker.h"
#include "vex/token.h"
#include "vex/types.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>

namespace lsp {

// ============================================================================
// Constructor / Destructor
// ============================================================================

LspServer::LspServer() {
    opts_.module_name = "lsp_input";
    opts_.opt_level = 0;  // fast compile for LSP
}

LspServer::~LspServer() = default;

void LspServer::set_compile_options(const vex::CompileOptions &opts) {
    opts_ = opts;
}

// ============================================================================
// Entry point: handle a raw message (Content-Length framed)
// ============================================================================

void LspServer::handle_message(const std::string &raw) {
    if (shutdown_) return;

    input_buffer_ += raw;

    while (true) {
        LspMessage msg;
        if (!msg.read_from(input_buffer_)) break;

        // Remove the consumed portion from buffer
        size_t header_end = input_buffer_.find("\r\n\r\n");
        size_t msg_len = header_end + 4 + msg.content_length;
        if (msg_len <= input_buffer_.size()) {
            input_buffer_ = input_buffer_.substr(msg_len);
        } else {
            input_buffer_.clear();
        }

        JsonParser parser(msg.body);
        JsonValue json = parser.parse();

        JsonRpcMessage rpc = parse_jsonrpc(json);

        if (rpc.has_id && !rpc.is_notification) {
            handle_request(rpc, json);
        } else {
            handle_notification(rpc, json);
        }
    }
}

// ============================================================================
// Request / Notification dispatch
// ============================================================================

void LspServer::handle_request(const JsonRpcMessage &msg, const JsonValue &raw) {
    if (msg.method == "initialize") {
        send_response(msg.id, handle_initialize(msg.params, msg.id));
    } else if (msg.method == "shutdown") {
        send_response(msg.id, handle_shutdown(msg.id));
    } else if (msg.method == "textDocument/completion") {
        send_response(msg.id, handle_completion(msg.params, msg.id));
    } else if (msg.method == "textDocument/definition") {
        send_response(msg.id, handle_definition(msg.params, msg.id));
    } else if (msg.method == "textDocument/hover") {
        send_response(msg.id, handle_hover(msg.params, msg.id));
    } else if (msg.method == "textDocument/diagnostic") {
        send_response(msg.id, handle_diagnostic(msg.params, msg.id));
    } else {
        send_response(msg.id, JsonValue::make_object({}));
    }
}

void LspServer::handle_notification(const JsonRpcMessage &msg, const JsonValue &raw) {
    if (msg.method == "initialized") {
        // no-op
    } else if (msg.method == "textDocument/didOpen") {
        handle_did_open(msg.params);
    } else if (msg.method == "textDocument/didChange") {
        handle_did_change(msg.params);
    } else if (msg.method == "textDocument/didClose") {
        handle_did_close(msg.params);
    } else if (msg.method == "exit") {
        shutdown_ = true;
    }
}

// ============================================================================
// Initialize
// ============================================================================

JsonValue LspServer::handle_initialize(const JsonValue &params, const JsonValue &id) {
    initialized_ = true;

    auto capabilities = JsonValue::make_object({});
    auto text_document_sync = JsonValue::make_object({});
    text_document_sync.set("openClose", JsonValue::make_bool(true));
    text_document_sync.set("change", JsonValue::make_int(1)); // full sync
    capabilities.set("textDocumentSync", std::move(text_document_sync));

    auto completion_provider = JsonValue::make_object({});
    completion_provider.set("triggerCharacters", JsonValue::make_array({
        JsonValue::make_string("."),
        JsonValue::make_string(":")
    }));
    capabilities.set("completionProvider", std::move(completion_provider));

    capabilities.set("definitionProvider", JsonValue::make_bool(true));
    capabilities.set("hoverProvider", JsonValue::make_bool(true));

    auto diagnostic_provider = JsonValue::make_object({});
    diagnostic_provider.set("interFileDependencies", JsonValue::make_bool(false));
    diagnostic_provider.set("workspaceDiagnostics", JsonValue::make_bool(false));
    capabilities.set("diagnosticProvider", std::move(diagnostic_provider));

    auto result = JsonValue::make_object({});
    result.set("capabilities", std::move(capabilities));
    result.set("serverInfo", JsonValue::make_object({}));
    result.get_mut("serverInfo")->set("name", JsonValue::make_string("vex-lsp"));
    result.get_mut("serverInfo")->set("version", JsonValue::make_string("0.1.0"));
    return result;
}

JsonValue LspServer::handle_shutdown(const JsonValue &id) {
    shutdown_ = true;
    return JsonValue::make_null();
}

// ============================================================================
// didOpen / didChange / didClose
// ============================================================================

void LspServer::handle_did_open(const JsonValue &params) {
    auto *text_document = params.get("textDocument");
    if (!text_document) return;

    auto *uri = text_document->get("uri");
    auto *language_id = text_document->get("languageId");
    auto *text = text_document->get("text");
    auto *version = text_document->get("version");

    if (!uri || !text) return;

    DocumentState doc;
    doc.uri = uri->str_val;
    doc.content = text->str_val;
    doc.language_id = language_id ? language_id->str_val : "vex";
    doc.version = version ? (int64_t)version->int_val : 0;

    documents_[doc.uri] = std::move(doc);
    compile_document(uri->str_val);
    publish_diagnostics(uri->str_val);
}

void LspServer::handle_did_change(const JsonValue &params) {
    auto *text_document = params.get("textDocument");
    auto *content_changes = params.get("contentChanges");
    if (!text_document || !content_changes) return;

    auto *uri = text_document->get("uri");
    auto *version = text_document->get("version");
    if (!uri) return;

    if (content_changes->kind == JsonValueKind::Array && !content_changes->arr_val.empty()) {
        auto &change = content_changes->arr_val.back();
        auto *text = change.get("text");
        if (text) {
            documents_[uri->str_val].content = text->str_val;
            documents_[uri->str_val].version = version ? (int64_t)version->int_val : 0;
        }
    }

    compile_document(uri->str_val);
    publish_diagnostics(uri->str_val);
}

void LspServer::handle_did_close(const JsonValue &params) {
    auto *text_document = params.get("textDocument");
    if (!text_document) return;
    auto *uri = text_document->get("uri");
    if (!uri) return;
    documents_.erase(uri->str_val);
    last_compile_results_.erase(uri->str_val);
    last_ast_.erase(uri->str_val);
    last_type_checkers_.erase(uri->str_val);
}

// ============================================================================
// Compile and extract diagnostics
// ============================================================================

void LspServer::compile_document(const std::string &uri) {
    auto it = documents_.find(uri);
    if (it == documents_.end()) return;

    std::string filename = uri;
    if (filename.find("file://") == 0) {
        filename = filename.substr(7);
    }

    vex::CompileResult result;
    result.ok = true;

    vex::Diagnostics &diags = result.diagnostics;
    vex::Lexer lexer(it->second.content, filename, diags);
    vex::Parser parser(lexer, diags);
    auto ast = parser.parse_program();

    std::unique_ptr<vex::TypeChecker> type_checker;
    if (ast && !diags.has_errors()) {
        type_checker = std::make_unique<vex::TypeChecker>(*ast, diags);
        type_checker->run();
    }

    if (diags.has_errors()) result.ok = false;

    last_compile_results_[uri] = std::move(result);
    last_ast_[uri] = std::move(ast);
    last_type_checkers_[uri] = std::move(type_checker);
}

// ============================================================================
// Publish Diagnostics
// ============================================================================

void LspServer::publish_diagnostics(const std::string &uri) {
    auto it = last_compile_results_.find(uri);
    if (it == last_compile_results_.end()) return;

    DiagnosticsBridge bridge;
    auto lsp_diags = bridge.convert(it->second.diagnostics, uri);
    auto params = bridge.to_lsp_params(lsp_diags, uri);

    send_notification("textDocument/publishDiagnostics", std::move(params));
}

// ============================================================================
// Completion
// ============================================================================

static bool is_keyword_token(vex::TokenKind k) {
    return (uint16_t)k >= (uint16_t)vex::TokenKind::KW_VOID &&
           (uint16_t)k <= (uint16_t)vex::TokenKind::KW_CASE;
}

JsonValue LspServer::handle_completion(const JsonValue &params, const JsonValue &id) {
    auto *text_document = params.get("textDocument");
    auto *position = params.get("position");
    if (!text_document || !position) {
        return JsonValue::make_null();
    }

    auto *uri = text_document->get("uri");
    if (!uri) return JsonValue::make_null();

    std::string uri_str = uri->str_val;

    auto items = JsonValue::make_array({});

    // Keyword completions
    auto keywords = get_keyword_completions();
    for (const auto &kw : keywords) {
        auto item = JsonValue::make_object({});
        item.set("label", JsonValue::make_string(kw.name));
        item.set("kind", JsonValue::make_int(14)); // Keyword
        item.set("detail", JsonValue::make_string(kw.kind));
        items.arr_val.push_back(std::move(item));
    }

    // Symbol completions (from TypeChecker scopes)
    auto symbols = get_symbol_completions(uri_str);
    for (const auto &sym : symbols) {
        auto item = JsonValue::make_object({});
        item.set("label", JsonValue::make_string(sym.name));
        // kind: 6=Function, 5=Variable, 9=Module, etc.
        int kind = 13; // default: Value
        if (sym.kind == "Function") kind = 6;
        else if (sym.kind == "Variable") kind = 5;
        else if (sym.kind == "Constant") kind = 4;
        else if (sym.kind == "Class") kind = 7;
        else if (sym.kind == "Struct") kind = 7;
        item.set("kind", JsonValue::make_int(kind));
        if (!sym.detail.empty())
            item.set("detail", JsonValue::make_string(sym.detail));
        items.arr_val.push_back(std::move(item));
    }

    auto result = JsonValue::make_object({});
    result.set("isIncomplete", JsonValue::make_bool(false));
    result.set("items", std::move(items));
    return result;
}

std::vector<SymbolInfo> LspServer::get_keyword_completions() const {
    static const char *kw_list[] = {
        "void", "bool", "char", "i8", "i16", "i32", "i64",
        "u8", "u16", "u32", "u64", "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "f32", "f64", "float", "double", "string",
        "ArrayList", "HashMap", "HashSet", "Queue", "Deque", "TreeMap", "TreeSet", "Stack",
        "unique", "shared", "borrow", "borrow_mut",
        "const", "static", "final", "nonnull",
        "typedef", "using", "namespace", "struct", "class", "interface", "enum",
        "fn", "public", "private", "protected", "import", "extern",
        "if", "else", "while", "do", "for", "in", "break", "continue",
        "goto", "return", "try", "catch", "finally", "throw",
        "new", "delete", "this", "super",
        "synchronized", "await", "spawn", "rspawn",
        "match", "case", "true", "false", "null",
        "comptime", "var", "auto", "lend", "move",
    };

    std::vector<SymbolInfo> result;
    for (const char *kw : kw_list) {
        SymbolInfo info;
        info.name = kw;
        info.kind = "Keyword";
        info.detail = "keyword";
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<SymbolInfo> LspServer::get_symbol_completions(const std::string &uri) const {
    std::vector<SymbolInfo> result;
    auto tc_it = last_type_checkers_.find(uri);
    if (tc_it == last_type_checkers_.end() || !tc_it->second) return result;

    const auto &tc = *tc_it->second;

    // Get symbols from struct layouts
    for (const auto &[name, _] : tc.struct_layouts()) {
        SymbolInfo info;
        info.name = name;
        info.kind = "Struct";
        result.push_back(std::move(info));
    }

    // Class layouts
    for (const auto &[name, _] : tc.class_layouts()) {
        SymbolInfo info;
        info.name = name;
        info.kind = "Class";
        result.push_back(std::move(info));
    }

    // Enum layouts
    for (const auto &[name, _] : tc.enum_layouts()) {
        SymbolInfo info;
        info.name = name;
        info.kind = "Enum";
        result.push_back(std::move(info));
    }

    // Comptime constants
    for (const auto &[name, c] : tc.comptime_const_values()) {
        SymbolInfo info;
        info.name = name;
        info.kind = "Constant";
        info.detail = c.is_str ? c.str_value : std::to_string(c.value);
        result.push_back(std::move(info));
    }

    return result;
}

// ============================================================================
// Definition (go-to-definition)
// ============================================================================

JsonValue LspServer::handle_definition(const JsonValue &params, const JsonValue &id) {
    auto *text_document = params.get("textDocument");
    auto *position = params.get("position");
    if (!text_document || !position) return JsonValue::make_null();

    auto *uri = text_document->get("uri");
    auto *line_val = position->get("line");
    auto *char_val = position->get("character");
    if (!uri || !line_val || !char_val) return JsonValue::make_null();

    std::string uri_str = uri->str_val;
    uint32_t line = (uint32_t)(line_val->int_val + 1);      // LSP 0-based -> 1-based
    uint32_t col = (uint32_t)(char_val->int_val + 1);

    // Look through the AST for a node at this position
    auto ast_it = last_ast_.find(uri_str);
    if (ast_it == last_ast_.end() || !ast_it->second) return JsonValue::make_null();

    FindResult found = find_node_at(*ast_it->second, line, col);
    if (!found.node) return JsonValue::make_null();

    // Return the location of the found node itself
    auto loc = JsonValue::make_object({});
    loc.set("uri", JsonValue::make_string(uri_str));

    auto range = JsonValue::make_object({});
    auto start = JsonValue::make_object({});
    auto end = JsonValue::make_object({});
    start.set("line", JsonValue::make_int((int64_t)(found.node->loc.line - 1)));
    start.set("character", JsonValue::make_int((int64_t)(found.node->loc.column - 1)));
    end.set("line", JsonValue::make_int((int64_t)(found.node->loc.line - 1)));
    end.set("character", JsonValue::make_int((int64_t)(found.node->loc.column + 
        (found.node->loc.length > 0 ? found.node->loc.length - 1 : 0))));
    range.set("start", std::move(start));
    range.set("end", std::move(end));
    loc.set("range", std::move(range));

    return loc;
}

// ============================================================================
// Hover
// ============================================================================

JsonValue LspServer::handle_hover(const JsonValue &params, const JsonValue &id) {
    auto *text_document = params.get("textDocument");
    auto *position = params.get("position");
    if (!text_document || !position) return JsonValue::make_null();

    auto *uri = text_document->get("uri");
    auto *line_val = position->get("line");
    auto *char_val = position->get("character");
    if (!uri || !line_val || !char_val) return JsonValue::make_null();

    std::string uri_str = uri->str_val;
    uint32_t line = (uint32_t)(line_val->int_val + 1);
    uint32_t col = (uint32_t)(char_val->int_val + 1);

    auto ast_it = last_ast_.find(uri_str);
    if (ast_it == last_ast_.end() || !ast_it->second) return JsonValue::make_null();

    FindResult found = find_node_at(*ast_it->second, line, col);
    if (!found.node) return JsonValue::make_null();

    std::string hover_text;
    hover_text += "Node kind: " + std::to_string((int)found.node->kind) + "\n";

    // If it's an expression with a result type, show it
    if (found.node->kind >= vex::ast::NodeKind::IntLitExpr &&
        found.node->kind <= vex::ast::NodeKind::CastExpr) {
        const auto *expr = static_cast<const vex::ast::Expr *>(found.node);
        hover_text += "Type: " + vex::type_to_string(expr->result_type) + "\n";
    }

    // If we have a TypeChecker, try to get more type info
    auto tc_it = last_type_checkers_.find(uri_str);
    if (tc_it != last_type_checkers_.end() && tc_it->second) {
        if (found.node->kind == vex::ast::NodeKind::IdentExpr) {
            const auto *ident = static_cast<const vex::ast::IdentExpr *>(found.node);
            hover_text += "Identifier: `" + ident->name + "`\n";
            if (!found.scope_name.empty())
                hover_text += "Enclosing: `" + found.scope_name + "`\n";
        }
    }

    auto result = JsonValue::make_object({});
    auto contents = JsonValue::make_object({});
    contents.set("kind", JsonValue::make_string("markdown"));
    contents.set("value", JsonValue::make_string(hover_text));
    result.set("contents", std::move(contents));
    return result;
}

// ============================================================================
// Diagnostic (pull diagnostics - LSP 3.17+)
// ============================================================================

JsonValue LspServer::handle_diagnostic(const JsonValue &params, const JsonValue &id) {
    auto *text_document = params.get("textDocument");
    if (!text_document) {
        // Return empty result for workspace diagnostic
        return JsonValue::make_null();
    }

    auto *uri = text_document->get("uri");
    if (!uri) return JsonValue::make_null();

    std::string uri_str = uri->str_val;

    // Re-compile if not already compiled
    if (last_compile_results_.find(uri_str) == last_compile_results_.end()) {
        compile_document(uri_str);
    }

    auto cr_it = last_compile_results_.find(uri_str);
    if (cr_it == last_compile_results_.end()) {
        return JsonValue::make_null();
    }

    DiagnosticsBridge bridge;
    auto lsp_diags = bridge.convert(cr_it->second.diagnostics, uri_str);

    auto items = JsonValue::make_array({});
    for (const auto &d : lsp_diags) {
        items.arr_val.push_back(d.to_json());
    }

    auto result = JsonValue::make_object({});
    auto kind = JsonValue::make_object({});
    kind.set("diagnostics", std::move(items));
    result.set("kind", JsonValue::make_string("full"));
    result.set("items", JsonValue::make_array({std::move(kind)}));
    return result;
}

// ============================================================================
// AST traversal
// ============================================================================

static bool loc_contains(const vex::SourceLoc &loc, uint32_t line, uint32_t column) {
    if (line < loc.line) return false;
    if (line == loc.line && column < loc.column) return false;
    if (line == loc.line && loc.length > 0 && column > loc.column + loc.length) return false;
    if (line > loc.line && loc.line != 1 && line > loc.line + 1) return false;
    return true;
}

void LspServer::find_node_at_rec(const vex::ast::Node &node, uint32_t line, uint32_t column,
                                  FindResult &result, const std::string &scope) const {
    if (!loc_contains(node.loc, line, column)) return;

    // This node contains the point; update result
    result.node = &node;
    result.scope_name = scope;

    // Recurse into children based on node kind
    using NK = vex::ast::NodeKind;

    switch (node.kind) {
        case NK::Module: {
            const auto &mod = static_cast<const vex::ast::ModuleNode &>(node);
            for (const auto &decl : mod.decls) {
                if (decl) find_node_at_rec(*decl, line, column, result, scope);
            }
            break;
        }
        case NK::FunctionDecl: {
            const auto &fn = static_cast<const vex::ast::FunctionDecl &>(node);
            // Check params
            for (const auto &p : fn.params) {
                if (p) find_node_at_rec(*p, line, column, result, fn.name);
            }
            // Check body
            if (fn.body) find_node_at_rec(*fn.body, line, column, result, fn.name);
            break;
        }
        case NK::BlockStmt: {
            const auto &block = static_cast<const vex::ast::BlockStmt &>(node);
            for (const auto &stmt : block.body) {
                if (stmt) find_node_at_rec(*stmt, line, column, result, scope);
            }
            break;
        }
        case NK::VarDeclStmt: {
            const auto &vd = static_cast<const vex::ast::VarDeclStmt &>(node);
            if (vd.init) find_node_at_rec(*vd.init, line, column, result, scope);
            break;
        }
        case NK::ExprStmt: {
            const auto &es = static_cast<const vex::ast::ExprStmt &>(node);
            if (es.expr) find_node_at_rec(*es.expr, line, column, result, scope);
            break;
        }
        case NK::IfStmt: {
            const auto &ifs = static_cast<const vex::ast::IfStmt &>(node);
            if (ifs.cond) find_node_at_rec(*ifs.cond, line, column, result, scope);
            if (ifs.then_branch) find_node_at_rec(*ifs.then_branch, line, column, result, scope);
            if (ifs.else_branch) find_node_at_rec(*ifs.else_branch, line, column, result, scope);
            break;
        }
        case NK::WhileStmt: {
            const auto &ws = static_cast<const vex::ast::WhileStmt &>(node);
            if (ws.cond) find_node_at_rec(*ws.cond, line, column, result, scope);
            if (ws.body) find_node_at_rec(*ws.body, line, column, result, scope);
            break;
        }
        case NK::ReturnStmt: {
            const auto &rs = static_cast<const vex::ast::ReturnStmt &>(node);
            if (rs.value) find_node_at_rec(*rs.value, line, column, result, scope);
            break;
        }
        case NK::BinaryExpr: {
            const auto &be = static_cast<const vex::ast::BinaryExpr &>(node);
            if (be.lhs) find_node_at_rec(*be.lhs, line, column, result, scope);
            if (be.rhs) find_node_at_rec(*be.rhs, line, column, result, scope);
            break;
        }
        case NK::UnaryExpr: {
            const auto &ue = static_cast<const vex::ast::UnaryExpr &>(node);
            if (ue.operand) find_node_at_rec(*ue.operand, line, column, result, scope);
            break;
        }
        case NK::AssignExpr: {
            const auto &ae = static_cast<const vex::ast::AssignExpr &>(node);
            if (ae.target) find_node_at_rec(*ae.target, line, column, result, scope);
            if (ae.value) find_node_at_rec(*ae.value, line, column, result, scope);
            break;
        }
        case NK::CallExpr: {
            const auto &ce = static_cast<const vex::ast::CallExpr &>(node);
            if (ce.callee) find_node_at_rec(*ce.callee, line, column, result, scope);
            for (const auto &arg : ce.args) {
                if (arg) find_node_at_rec(*arg, line, column, result, scope);
            }
            break;
        }
        case NK::IdentExpr:
        case NK::IntLitExpr:
        case NK::FloatLitExpr:
        case NK::BoolLitExpr:
        case NK::NullLitExpr:
        case NK::CharLitExpr:
        case NK::StringLitExpr:
        case NK::ThisExpr:
            // Leaf nodes - no recursion needed
            break;
        case NK::FieldAccessExpr: {
            const auto &fa = static_cast<const vex::ast::FieldAccessExpr &>(node);
            if (fa.base) find_node_at_rec(*fa.base, line, column, result, scope);
            break;
        }
        case NK::IndexExpr: {
            const auto &ie = static_cast<const vex::ast::IndexExpr &>(node);
            if (ie.base) find_node_at_rec(*ie.base, line, column, result, scope);
            if (ie.index) find_node_at_rec(*ie.index, line, column, result, scope);
            break;
        }
        case NK::NewExpr: {
            const auto &ne = static_cast<const vex::ast::NewExpr &>(node);
            for (const auto &arg : ne.args) {
                if (arg) find_node_at_rec(*arg, line, column, result, scope);
            }
            break;
        }
        case NK::CastExpr: {
            const auto &ce = static_cast<const vex::ast::CastExpr &>(node);
            if (ce.operand) find_node_at_rec(*ce.operand, line, column, result, scope);
            break;
        }
        case NK::LambdaExpr: {
            const auto &le = static_cast<const vex::ast::LambdaExpr &>(node);
            for (const auto &p : le.params) {
                if (p) find_node_at_rec(*p, line, column, result, scope);
            }
            if (le.body) find_node_at_rec(*le.body, line, column, result, scope);
            break;
        }
        case NK::MatchExpr: {
            const auto &me = static_cast<const vex::ast::MatchExpr &>(node);
            if (me.scrutinee) find_node_at_rec(*me.scrutinee, line, column, result, scope);
            // Arms contain body statements
            for (const auto &arm : me.arms) {
                if (arm.body) find_node_at_rec(*arm.body, line, column, result, scope);
            }
            break;
        }
        case NK::ForStmt: {
            const auto &fs = static_cast<const vex::ast::ForStmt &>(node);
            if (fs.init) find_node_at_rec(*fs.init, line, column, result, scope);
            if (fs.cond) find_node_at_rec(*fs.cond, line, column, result, scope);
            if (fs.step) find_node_at_rec(*fs.step, line, column, result, scope);
            if (fs.body) find_node_at_rec(*fs.body, line, column, result, scope);
            break;
        }
        case NK::ForEachStmt: {
            const auto &fe = static_cast<const vex::ast::ForEachStmt &>(node);
            if (fe.iter_expr) find_node_at_rec(*fe.iter_expr, line, column, result, scope);
            if (fe.body) find_node_at_rec(*fe.body, line, column, result, scope);
            break;
        }
        case NK::ParamDecl:
        case NK::GlobalVarDecl: {
            const auto &gv = static_cast<const vex::ast::GlobalVarDecl &>(node);
            if (gv.init) find_node_at_rec(*gv.init, line, column, result, scope);
            break;
        }
        case NK::TypeAliasDecl:
        case NK::StructDecl: {
            const auto &sd = static_cast<const vex::ast::StructDecl &>(node);
            break;
        }
        case NK::ClassDecl: {
            const auto &cls = static_cast<const vex::ast::ClassDecl &>(node);
            for (const auto &f : cls.fields) {
                if (f.init) find_node_at_rec(*f.init, line, column, result, cls.name);
            }
            for (const auto &m : cls.methods) {
                if (m) find_node_at_rec(*m, line, column, result, cls.name);
            }
            break;
        }
        case NK::EnumDecl:
        case NK::ExternFnDecl:
        case NK::ImportDecl:
        case NK::NamespaceDecl: {
            const auto &ns = static_cast<const vex::ast::NamespaceDecl &>(node);
            for (const auto &d : ns.decls) {
                if (d) find_node_at_rec(*d, line, column, result, ns.name);
            }
            break;
        }
        case NK::DoWhileStmt: {
            const auto &dw = static_cast<const vex::ast::DoWhileStmt &>(node);
            if (dw.body) find_node_at_rec(*dw.body, line, column, result, scope);
            if (dw.cond) find_node_at_rec(*dw.cond, line, column, result, scope);
            break;
        }
        case NK::TernaryExpr: {
            const auto &te = static_cast<const vex::ast::TernaryExpr &>(node);
            if (te.cond) find_node_at_rec(*te.cond, line, column, result, scope);
            if (te.then_expr) find_node_at_rec(*te.then_expr, line, column, result, scope);
            if (te.else_expr) find_node_at_rec(*te.else_expr, line, column, result, scope);
            break;
        }
        case NK::TryStmt: {
            const auto &ts = static_cast<const vex::ast::TryStmt &>(node);
            if (ts.body) find_node_at_rec(*ts.body, line, column, result, scope);
            for (const auto &c : ts.catches) {
                if (c.body) find_node_at_rec(*c.body, line, column, result, scope);
            }
            if (ts.finally_body) find_node_at_rec(*ts.finally_body, line, column, result, scope);
            break;
        }
        case NK::ThrowStmt: {
            const auto &ts = static_cast<const vex::ast::ThrowStmt &>(node);
            if (ts.value) find_node_at_rec(*ts.value, line, column, result, scope);
            break;
        }
        case NK::SpawnExpr: {
            const auto &se = static_cast<const vex::ast::SpawnExpr &>(node);
            if (se.sched_idx) find_node_at_rec(*se.sched_idx, line, column, result, scope);
            if (se.body) find_node_at_rec(*se.body, line, column, result, scope);
            break;
        }
        case NK::InitListExpr: {
            const auto &il = static_cast<const vex::ast::InitListExpr &>(node);
            for (const auto &e : il.elements) {
                if (e) find_node_at_rec(*e, line, column, result, scope);
            }
            break;
        }
        case NK::PrimitiveTypeNode:
        case NK::NamedTypeNode:
        case NK::PointerTypeNode:
        case NK::ArrayTypeNode:
        case NK::FunctionTypeNode:
            break;
        case NK::BreakStmt:
        case NK::ContinueStmt:
        case NK::GotoStmt:
        case NK::LabelStmt:
        case NK::SynchronizedStmt: {
            const auto &ss = static_cast<const vex::ast::SynchronizedStmt &>(node);
            if (ss.target) find_node_at_rec(*ss.target, line, column, result, scope);
            if (ss.body) find_node_at_rec(*ss.body, line, column, result, scope);
            break;
        }
        case NK::ComptimeBlockStmt: {
            const auto &cb = static_cast<const vex::ast::ComptimeBlockStmt &>(node);
            for (const auto &s : cb.stmts) {
                if (s) find_node_at_rec(*s, line, column, result, scope);
            }
            break;
        }
        case NK::ComptimeForStmt: {
            const auto &cf = static_cast<const vex::ast::ComptimeForStmt &>(node);
            if (cf.lo_expr) find_node_at_rec(*cf.lo_expr, line, column, result, scope);
            if (cf.hi_expr) find_node_at_rec(*cf.hi_expr, line, column, result, scope);
            if (cf.body) find_node_at_rec(*cf.body, line, column, result, scope);
            break;
        }
        case NK::SuperCallExpr:
        case NK::SuperMethodCallExpr:
            break;
        case NK::RSpawnExpr: {
            const auto &rs = static_cast<const vex::ast::RSpawnExpr &>(node);
            if (rs.node_idx) find_node_at_rec(*rs.node_idx, line, column, result, scope);
            if (rs.body) find_node_at_rec(*rs.body, line, column, result, scope);
            break;
        }
        case NK::TryExpr:
            break;
        default:
            break;
    }
}

LspServer::FindResult LspServer::find_node_at(const vex::ast::Node &node,
                                                uint32_t line, uint32_t column) const {
    FindResult result;
    find_node_at_rec(node, line, column, result, "");
    return result;
}

// ============================================================================
// Send / Output helpers
// ============================================================================

void LspServer::send_notification(const std::string &method, JsonValue params) {
    auto msg = make_jsonrpc_notification(method, std::move(params));
    write_output(LspMessage::serialize_and_encode(msg));
}

void LspServer::send_response(JsonValue id, JsonValue result) {
    auto msg = make_jsonrpc_response(std::move(id), std::move(result));
    write_output(LspMessage::serialize_and_encode(msg));
}

void LspServer::write_output(const std::string &data) {
    std::cout << data << std::flush;
}

std::string LspServer::read_input() {
    std::string line;
    std::getline(std::cin, line);
    return line;
}

void LspServer::run() {
    std::string buf;
    while (!shutdown_) {
        char chunk[4096];
        std::cin.read(chunk, sizeof(chunk));
        auto nread = std::cin.gcount();
        if (nread <= 0) {
            if (std::cin.eof()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        buf.append(chunk, (size_t)nread);
        handle_message(buf);
        // input_buffer_ inside handle_message holds unprocessed data
        buf.clear();
    }
}

} // namespace lsp
