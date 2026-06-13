#include "lsp/lsp_diagnostics.h"
#include <algorithm>

namespace lsp {

static std::string diag_level_to_lsp(vex::DiagLevel level) {
    switch (level) {
        case vex::DiagLevel::ERR:  return "Error";
        case vex::DiagLevel::WARN: return "Warning";
        case vex::DiagLevel::NOTE: return "Information";
    }
    return "Hint";
}

static int lsp_severity_number(const std::string &sev) {
    if (sev == "Error") return 1;
    if (sev == "Warning") return 2;
    if (sev == "Information") return 3;
    return 4;
}

lsp::JsonValue LspDiagnostic::to_json() const {
    auto d = lsp::JsonValue::make_object({});
    auto range = lsp::JsonValue::make_object({});
    auto start = lsp::JsonValue::make_object({});
    auto end = lsp::JsonValue::make_object({});
    start.set("line", lsp::JsonValue::make_int((int64_t)(line > 0 ? line - 1 : 0)));
    start.set("character", lsp::JsonValue::make_int((int64_t)(column > 0 ? column - 1 : 0)));
    end.set("line", lsp::JsonValue::make_int((int64_t)(end_line > 0 ? end_line - 1 : 0)));
    end.set("character", lsp::JsonValue::make_int((int64_t)(end_column > 0 ? end_column - 1 : 0)));
    range.set("start", std::move(start));
    range.set("end", std::move(end));
    d.set("range", std::move(range));
    d.set("severity", lsp::JsonValue::make_int(lsp_severity_number(severity)));
    d.set("message", lsp::JsonValue::make_string(message));
    if (!code.empty()) d.set("code", lsp::JsonValue::make_string(code));
    return d;
}

std::vector<LspDiagnostic> DiagnosticsBridge::convert(
        const vex::Diagnostics &diags, const std::string &file_uri) {
    std::vector<LspDiagnostic> result;
    for (const auto &d : diags.all()) {
        LspDiagnostic ld;
        ld.file       = file_uri;
        ld.line       = d.loc.line;
        ld.column     = d.loc.column;
        ld.end_line   = d.loc.line;
        ld.end_column = d.loc.column + std::max(d.loc.length, (uint32_t)1);
        ld.severity   = diag_level_to_lsp(d.level);
        ld.message    = d.message;
        ld.code       = d.code;
        result.push_back(std::move(ld));
    }
    return result;
}

lsp::JsonValue DiagnosticsBridge::to_lsp_params(
        const std::vector<LspDiagnostic> &diags,
        const std::string &file_uri,
        const std::string &diagnostic_version) {
    auto params = lsp::JsonValue::make_object({});
    params.set("uri", lsp::JsonValue::make_string(file_uri));
    if (!diagnostic_version.empty())
        params.set("version", lsp::JsonValue::make_int((int64_t)std::stoll(diagnostic_version)));

    auto items = lsp::JsonValue::make_array({});
    for (const auto &d : diags)
        items.arr_val.push_back(d.to_json());
    params.set("diagnostics", std::move(items));
    return params;
}

} // namespace lsp
