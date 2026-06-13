#ifndef LSP_DIAGNOSTICS_H
#define LSP_DIAGNOSTICS_H

#include "vex/diagnostic.h"
#include "lsp/json_rpc.h"
#include <string>
#include <vector>

namespace lsp {

struct LspDiagnostic {
    lsp::JsonValue to_json() const;

    std::string file;
    uint32_t    line      = 1;  // 1-based, LSP is 0-based internally
    uint32_t    column    = 1;
    uint32_t    end_line  = 1;
    uint32_t    end_column= 1;
    std::string severity;       // "Error", "Warning", "Information", "Hint"
    std::string message;
    std::string code;
};

class DiagnosticsBridge {
public:
    std::vector<LspDiagnostic> convert(const vex::Diagnostics &diags,
                                        const std::string &file_uri);
    lsp::JsonValue to_lsp_params(const std::vector<LspDiagnostic> &diags,
                                  const std::string &file_uri,
                                  const std::string &diagnostic_version = "");
};

} // namespace lsp

#endif
