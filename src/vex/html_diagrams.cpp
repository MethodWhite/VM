/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file html_diagrams.cpp
 * @brief Implementacion de los generadores HTML interactivos (ver html_diagrams.h).
 *
 * Flujo: DOT (de graphviz_from_*) -> modelo de grafo (grupos/nodos/aristas)
 * -> JSON embebido -> plantilla HTML autocontenida con motor de layout +
 * interaccion en JS.  El parser de DOT es tolerante pero asume el formato
 * REGULAR que emiten nuestros generadores Graphviz (no es un parser DOT
 * generico): clusters `subgraph cluster_X { label="..."; ... }`, nodos
 * `id [label="...", shape=..., fillcolor="...", tooltip="..."];` y aristas
 * `a -> b [label="...", color="...", style="..."];`.
 */

// IMPORTANTE: incluir los tipos reales ANTES de los headers de diagramas.
// `ast::ModuleNode` vive en `vex::ast` y `ir::IrModule` en `::ir`; los headers
// de diagramas hacen forward-decls que solo resuelven al namespace correcto
// si `vex::ast` ya es visible al parsearlos (mismo patron que compiler.cpp).
#include "vex/ast.h"
#include "ir/ssa_ir.h"

#include "vex/html_diagrams.h"
#include "vex/graphviz_diagrams.h"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace vex {

    namespace {

        // ------------------------------------------------------------------
        //  Modelo de grafo intermedio
        // ------------------------------------------------------------------

        /// Un nodo del grafo, derivado de un nodo DOT.
        struct HNode {
            std::string id;        ///< identificador unico (el del DOT).
            std::string group;     ///< cluster contenedor ("" si ninguno).
            std::string kind;      ///< clase semantica derivada del shape.
            std::string shape;     ///< shape DOT original.
            std::string header;    ///< primera linea / cabecera del nodo.
            std::vector<std::string> lines; ///< lineas de cuerpo (instrucciones).
            std::string tooltip;   ///< texto extra (line numbers, conteos).
            std::string fill;      ///< color de fondo (#hex).
            std::string fontcolor; ///< color de texto (#hex).
            std::string stroke;    ///< color de borde (#hex).
        };

        /// Una arista entre dos nodos.
        struct HEdge {
            std::string from;
            std::string to;
            std::string label;
            std::string type;   ///< flow|true|false|call|extends|implements|back|break|body
            std::string color;
            std::string style;  ///< solid|dotted|dashed
        };

        /// Un grupo (cluster) con su titulo.
        struct HGroup {
            std::string id;
            std::string title;
        };

        // ------------------------------------------------------------------
        //  Helpers de string
        // ------------------------------------------------------------------

        std::string trim(const std::string &s) {
            size_t a = 0, b = s.size();
            while (a < b && (unsigned char)s[a] <= ' ') ++a;
            while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
            return s.substr(a, b - a);
        }

        bool starts_with(const std::string &s, const char *p) {
            size_t n = 0;
            while (p[n]) ++n;
            return s.size() >= n && s.compare(0, n, p) == 0;
        }

        /// Deshace los escapes DOT de un label/tooltip a texto plano UTF-8.
        std::string unescape_dot(const std::string &s) {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char c = s[i + 1];
                    switch (c) {
                        case 'n': out += '\n'; break; // salto estandar
                        case 'l': out += '\n'; break; // left-justify de records
                        case 'r': break;              // ignorar
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '{': out += '{'; break;
                        case '}': out += '}'; break;
                        case '|': out += '|'; break;
                        case '<': out += '<'; break;
                        case '>': out += '>'; break;
                        default: out += c; break;
                    }
                    ++i;
                } else {
                    out += s[i];
                }
            }
            return out;
        }

        /// Escapa un string para incluirlo en un literal JSON.
        std::string json_escape(const std::string &s) {
            std::string out;
            out.reserve(s.size() + 8);
            for (unsigned char c : s) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out += (char)c;
                        }
                }
            }
            return out;
        }

        // ------------------------------------------------------------------
        //  Parser de atributos DOT
        // ------------------------------------------------------------------

        /// Parsea el contenido `k=v, k="v", ...` entre `[` y `]` a un mapa.
        std::unordered_map<std::string, std::string>
        parse_attrs(const std::string &body) {
            std::unordered_map<std::string, std::string> m;
            size_t i = 0, n = body.size();
            while (i < n) {
                // key
                while (i < n && (body[i] == ',' || (unsigned char)body[i] <= ' ')) ++i;
                size_t ks = i;
                while (i < n && body[i] != '=' && body[i] != ',') ++i;
                if (i >= n) break;
                std::string key = trim(body.substr(ks, i - ks));
                if (body[i] != '=') { continue; }
                ++i; // skip '='
                while (i < n && (unsigned char)body[i] <= ' ') ++i;
                std::string val;
                if (i < n && body[i] == '"') {
                    ++i;
                    std::string raw;
                    while (i < n) {
                        char c = body[i];
                        if (c == '\\' && i + 1 < n) { raw += c; raw += body[i + 1]; i += 2; continue; }
                        if (c == '"') { ++i; break; }
                        raw += c; ++i;
                    }
                    val = raw; // sigue escapado; el caller deshace si toca
                } else {
                    size_t vs = i;
                    while (i < n && body[i] != ',') ++i;
                    val = trim(body.substr(vs, i - vs));
                }
                if (!key.empty()) m[key] = val;
            }
            return m;
        }

        /// Divide un record label `{ a | b | c }` (escapado) en sus campos,
        /// respetando los `\|` escapados.  Devuelve los campos SIN deshacer
        /// (el caller deshace cada uno).
        std::vector<std::string> split_record(const std::string &raw) {
            std::string s = trim(raw);
            if (!s.empty() && s.front() == '{') s = s.substr(1);
            if (!s.empty() && s.back() == '}') s.pop_back();
            std::vector<std::string> out;
            std::string cur;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) { cur += s[i]; cur += s[i + 1]; i++; continue; }
                if (s[i] == '|') { out.push_back(trim(cur)); cur.clear(); continue; }
                cur += s[i];
            }
            out.push_back(trim(cur));
            return out;
        }

        // ------------------------------------------------------------------
        //  Clasificacion semantica
        // ------------------------------------------------------------------

        std::string kind_for_shape(const std::string &shape) {
            if (shape == "record")        return "block";
            if (shape == "diamond")       return "branch";
            if (shape == "doublecircle")  return "terminal";
            if (shape == "circle")        return "entry";
            if (shape == "box3d")         return "module";
            if (shape == "ellipse" || shape == "oval") return "terminal";
            if (shape == "octagon")       return "special";
            if (shape == "box")           return "decl";
            return "node";
        }

        std::string edge_type(const std::string &label, const std::string &style,
                              const std::string &color) {
            bool dotted = style.find("dotted") != std::string::npos;
            if (label == "body")        return "body";
            if (dotted && (label == "call" || label == "callvm" || label.empty()))
                return "call";
            if (label == "fall")        return "fall";
            if (label == "true")        return "true";
            if (label == "false")       return "false";
            if (label == "back" || label == "continue" || label == "loop")
                return "back";
            if (label == "break")       return "break";
            if (label == "extends")     return "extends";
            if (label == "implements")  return "implements";
            if (label == "next")        return "true";
            if (label == "done")        return "false";
            if (dotted)                 return "call";
            (void)color;
            return "flow";
        }

        // ------------------------------------------------------------------
        //  Parser DOT -> modelo
        // ------------------------------------------------------------------

        struct DotModel {
            std::vector<HGroup> groups;
            std::vector<HNode>  nodes;
            std::vector<HEdge>  edges;
        };

        DotModel parse_dot(const std::string &dot) {
            DotModel mdl;
            std::unordered_map<std::string, size_t> group_idx;

            // 1) Quitar lineas de comentario `//...` (solo a inicio de linea).
            std::string src;
            src.reserve(dot.size());
            {
                size_t i = 0, n = dot.size();
                while (i < n) {
                    size_t eol = dot.find('\n', i);
                    if (eol == std::string::npos) eol = n;
                    std::string line = dot.substr(i, eol - i);
                    std::string t = trim(line);
                    if (!starts_with(t, "//")) { src += line; src += '\n'; }
                    i = eol + 1;
                }
            }

            // 2) Scanner por statements respetando comillas y tracking de `{ }`.
            std::vector<std::string> group_stack;       // ids de cluster ("" = root/no-grupo)
            std::string pending_group;                  // grupo recien abierto sin titulo aun

            auto cur_group = [&]() -> std::string {
                for (auto it = group_stack.rbegin(); it != group_stack.rend(); ++it)
                    if (!it->empty()) return *it;
                return std::string();
            };

            auto handle_stmt = [&](const std::string &raw_stmt) {
                std::string st = trim(raw_stmt);
                if (st.empty()) return;

                // Titulo de grupo: `label="..."` suelto justo tras abrir cluster.
                if (!pending_group.empty() && starts_with(st, "label=")) {
                    auto m = parse_attrs(st);
                    auto it = m.find("label");
                    if (it != m.end()) {
                        size_t gi = group_idx[pending_group];
                        mdl.groups[gi].title = unescape_dot(it->second);
                    }
                    pending_group.clear();
                    return;
                }

                // Arista: contiene " -> ".
                size_t arrow = st.find("->");
                if (arrow != std::string::npos) {
                    std::string from = trim(st.substr(0, arrow));
                    std::string rest = trim(st.substr(arrow + 2));
                    std::string to, attrs;
                    size_t br = rest.find('[');
                    if (br != std::string::npos) {
                        to = trim(rest.substr(0, br));
                        size_t cb = rest.rfind(']');
                        if (cb != std::string::npos && cb > br)
                            attrs = rest.substr(br + 1, cb - br - 1);
                    } else {
                        to = trim(rest);
                    }
                    if (from.empty() || to.empty()) return;
                    HEdge e;
                    e.from = from;
                    e.to   = to;
                    auto m = parse_attrs(attrs);
                    if (m.count("label")) e.label = unescape_dot(m["label"]);
                    if (m.count("color")) e.color = m["color"];
                    if (m.count("style")) e.style = m["style"];
                    e.type = edge_type(e.label, e.style, e.color);
                    mdl.edges.push_back(std::move(e));
                    return;
                }

                // Nodo: `id [ ... ]`.
                size_t br = st.find('[');
                if (br == std::string::npos) return; // attr de grafo suelta -> ignorar
                std::string id = trim(st.substr(0, br));
                if (id.empty() || id == "node" || id == "edge" || id == "graph") return;
                // id valido: solo letra/digito/_.
                for (char c : id)
                    if (!(std::isalnum((unsigned char)c) || c == '_')) return;
                size_t cb = st.rfind(']');
                std::string attrs = (cb != std::string::npos && cb > br)
                                        ? st.substr(br + 1, cb - br - 1)
                                        : std::string();
                auto m = parse_attrs(attrs);

                HNode nd;
                nd.id    = id;
                nd.group = cur_group();
                nd.shape = m.count("shape") ? m["shape"] : "box";
                nd.kind  = kind_for_shape(nd.shape);
                nd.fill      = m.count("fillcolor") ? m["fillcolor"] : "#1e293b";
                nd.fontcolor = m.count("fontcolor") ? m["fontcolor"] : "#e5e7eb";
                nd.stroke    = m.count("color")     ? m["color"]     : "#475569";
                if (m.count("tooltip")) nd.tooltip = unescape_dot(m["tooltip"]);

                std::string label = m.count("label") ? m["label"] : id;
                if (nd.shape == "record") {
                    std::vector<std::string> fields = split_record(label);
                    if (!fields.empty()) {
                        nd.header = unescape_dot(fields[0]);
                        for (size_t k = 1; k < fields.size(); ++k) {
                            std::string ln = unescape_dot(fields[k]);
                            if (!ln.empty()) nd.lines.push_back(ln);
                        }
                    }
                } else {
                    std::string txt = unescape_dot(label);
                    size_t nl = txt.find('\n');
                    if (nl == std::string::npos) {
                        nd.header = txt;
                    } else {
                        nd.header = txt.substr(0, nl);
                        std::string rest = txt.substr(nl + 1);
                        size_t p = 0;
                        while (p < rest.size()) {
                            size_t q = rest.find('\n', p);
                            if (q == std::string::npos) q = rest.size();
                            std::string ln = trim(rest.substr(p, q - p));
                            if (!ln.empty()) nd.lines.push_back(ln);
                            p = q + 1;
                        }
                    }
                }
                mdl.nodes.push_back(std::move(nd));
            };

            size_t i = 0, n = src.size();
            std::string cur;
            while (i < n) {
                char c = src[i];
                if (c == '"') {
                    cur += c; ++i;
                    while (i < n) {
                        char cc = src[i];
                        cur += cc;
                        if (cc == '\\' && i + 1 < n) { cur += src[i + 1]; i += 2; continue; }
                        if (cc == '"') { ++i; break; }
                        ++i;
                    }
                    continue;
                }
                if (c == '{') {
                    std::string opener = trim(cur);
                    cur.clear();
                    std::string gid;
                    if (starts_with(opener, "subgraph")) {
                        // extraer el nombre del cluster
                        std::string nm = trim(opener.substr(8));
                        size_t sp = nm.find_first_of(" \t");
                        if (sp != std::string::npos) nm = nm.substr(0, sp);
                        if (starts_with(nm, "cluster_")) nm = nm.substr(8);
                        gid = nm;
                        if (!group_idx.count(gid)) {
                            group_idx[gid] = mdl.groups.size();
                            HGroup g; g.id = gid; g.title = gid;
                            mdl.groups.push_back(g);
                        }
                        pending_group = gid;
                    }
                    group_stack.push_back(gid); // "" si es digraph root
                    ++i; continue;
                }
                if (c == '}') {
                    if (!trim(cur).empty()) handle_stmt(cur);
                    cur.clear();
                    if (!group_stack.empty()) group_stack.pop_back();
                    ++i; continue;
                }
                if (c == ';') {
                    handle_stmt(cur);
                    cur.clear();
                    ++i; continue;
                }
                cur += c; ++i;
            }
            if (!trim(cur).empty()) handle_stmt(cur);
            return mdl;
        }

        // ------------------------------------------------------------------
        //  Modelo -> JSON
        // ------------------------------------------------------------------

        std::string model_to_json(const DotModel &mdl, const std::string &view) {
            std::ostringstream os;
            os << "{";
            os << "\"view\":\"" << json_escape(view) << "\",";

            os << "\"groups\":[";
            for (size_t i = 0; i < mdl.groups.size(); ++i) {
                if (i) os << ",";
                os << "{\"id\":\"" << json_escape(mdl.groups[i].id) << "\","
                   << "\"title\":\"" << json_escape(mdl.groups[i].title) << "\"}";
            }
            os << "],";

            os << "\"nodes\":[";
            for (size_t i = 0; i < mdl.nodes.size(); ++i) {
                const HNode &nd = mdl.nodes[i];
                if (i) os << ",";
                os << "{\"id\":\"" << json_escape(nd.id) << "\","
                   << "\"group\":\"" << json_escape(nd.group) << "\","
                   << "\"kind\":\"" << json_escape(nd.kind) << "\","
                   << "\"shape\":\"" << json_escape(nd.shape) << "\","
                   << "\"header\":\"" << json_escape(nd.header) << "\","
                   << "\"tooltip\":\"" << json_escape(nd.tooltip) << "\","
                   << "\"fill\":\"" << json_escape(nd.fill) << "\","
                   << "\"fc\":\"" << json_escape(nd.fontcolor) << "\","
                   << "\"stroke\":\"" << json_escape(nd.stroke) << "\","
                   << "\"lines\":[";
                for (size_t k = 0; k < nd.lines.size(); ++k) {
                    if (k) os << ",";
                    os << "\"" << json_escape(nd.lines[k]) << "\"";
                }
                os << "]}";
            }
            os << "],";

            os << "\"edges\":[";
            for (size_t i = 0; i < mdl.edges.size(); ++i) {
                const HEdge &e = mdl.edges[i];
                if (i) os << ",";
                os << "{\"from\":\"" << json_escape(e.from) << "\","
                   << "\"to\":\"" << json_escape(e.to) << "\","
                   << "\"type\":\"" << json_escape(e.type) << "\","
                   << "\"label\":\"" << json_escape(e.label) << "\","
                   << "\"color\":\"" << json_escape(e.color) << "\","
                   << "\"style\":\"" << json_escape(e.style) << "\"}";
            }
            os << "]";
            os << "}";
            return os.str();
        }

        // Plantilla HTML autocontenida (CSS + JS embebidos, sin CDN).  Los
        // tokens __GRAPH_DATA__ / __GRAPH_TITLE__ / __GRAPH_VIEW__ /
        // __TITLE_TEXT__ se sustituyen en build_page por el JSON del modelo
        // y el titulo.  El motor JS hace layout en capas (longest-path),
        // pan/zoom, panel de detalle, busqueda y filtros de aristas.
        static const char *kHtmlTemplate = R"VEXHTML(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE_TEXT__ - Vex diagrama interactivo</title>
<style>
  :root{
    --bg:#0b1020; --panel:#111827; --bar:#0f172a; --line:#1f2937;
    --txt:#e5e7eb; --muted:#94a3b8; --accent:#38bdf8;
  }
  *{box-sizing:border-box}
  html,body{margin:0;height:100%;background:var(--bg);color:var(--txt);
    font-family:Inter,Segoe UI,system-ui,sans-serif;font-size:13px}
  #bar{display:flex;align-items:center;justify-content:space-between;gap:12px;
    padding:8px 14px;background:var(--bar);border-bottom:1px solid var(--line);
    flex-wrap:wrap}
  #bar .title{display:flex;align-items:center;gap:10px;min-width:0}
  #bar h1{font-size:14px;margin:0;font-weight:600;white-space:nowrap;
    overflow:hidden;text-overflow:ellipsis;max-width:46vw}
  .badge{font-size:10px;text-transform:uppercase;letter-spacing:.08em;
    padding:2px 7px;border-radius:6px;background:#1e293b;color:var(--accent);
    border:1px solid #334155;font-weight:700}
  .counts{color:var(--muted);font-size:12px;white-space:nowrap}
  .controls{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
  .controls input[type=text],.controls select{background:#0b1222;color:var(--txt);
    border:1px solid var(--line);border-radius:7px;padding:5px 9px;font-size:12px;outline:none}
  .controls input[type=text]{width:200px}
  .controls input[type=text]:focus,.controls select:focus{border-color:var(--accent)}
  .controls button{background:#1e293b;color:var(--txt);border:1px solid var(--line);
    border-radius:7px;padding:5px 10px;font-size:12px;cursor:pointer}
  .controls button:hover{background:#243248;border-color:#3b4a63}
  .chk{display:flex;align-items:center;gap:5px;color:var(--muted);cursor:pointer;user-select:none}
  .muted{color:var(--muted);font-size:11px}
  #legend{display:flex;gap:14px;flex-wrap:wrap;padding:6px 14px;background:#0c1424;
    border-bottom:1px solid var(--line);font-size:11px}
  #legend label{display:flex;align-items:center;gap:6px;cursor:pointer;color:var(--muted)}
  #legend .sw{width:22px;height:0;border-top:3px solid;border-radius:2px}
  #viewport{position:relative;width:100%;height:calc(100vh - 92px);overflow:hidden;
    cursor:grab;background:
      radial-gradient(circle at 1px 1px, #16213a 1px, transparent 0) 0 0/26px 26px}
  #viewport.drag{cursor:grabbing}
  #world{position:absolute;left:0;top:0;transform-origin:0 0}
  #edges{position:absolute;left:0;top:0;overflow:visible;pointer-events:none}
  #nodes,#groups{position:absolute;left:0;top:0}
  .grp{position:absolute;border:1px dashed #334155;border-radius:12px;
    background:rgba(56,189,248,.04)}
  .grp .gt{position:absolute;top:-10px;left:12px;background:var(--bg);padding:0 8px;
    font-size:11px;color:#7dd3fc;font-weight:600;white-space:nowrap}
  .node{position:absolute;border:1.5px solid;border-radius:9px;padding:6px 9px;
    min-width:80px;max-width:440px;box-shadow:0 2px 8px rgba(0,0,0,.35);cursor:pointer;
    transition:box-shadow .1s,outline-color .1s,opacity .12s;outline:2px solid transparent}
  .node:hover{box-shadow:0 4px 16px rgba(0,0,0,.55)}
  .node.sel{outline-color:var(--accent);box-shadow:0 0 0 3px rgba(56,189,248,.35)}
  .node.dim{opacity:.14}
  .node.faded{opacity:.16}
  .node.match{outline-color:#fbbf24}
  .node .nh{font-weight:700;font-size:12px;white-space:pre-wrap;line-height:1.25}
  .node .nb{margin-top:5px;font-family:ui-monospace,Consolas,monospace;font-size:11px;
    white-space:pre;line-height:1.4;opacity:.96;border-top:1px solid rgba(255,255,255,.18);
    padding-top:4px;max-height:340px;overflow:hidden}
  .node .nmore{margin-top:4px;font-size:10px;opacity:.7}
  .k-branch{border-radius:9px}
  .epath{fill:none;stroke-width:1.5;opacity:.55;transition:opacity .1s,stroke-width .1s}
  .epath.thick{stroke-width:2.2;opacity:.7}
  .epath.hl{opacity:1;stroke-width:2.6}
  .epath.faded{opacity:.05}
  .elabel{font-size:10px;fill:#cbd5e1;paint-order:stroke;stroke:#0b1020;stroke-width:3px}
  #hint{position:absolute;bottom:10px;left:50%;transform:translateX(-50%);
    background:rgba(15,23,42,.85);border:1px solid var(--line);border-radius:8px;
    padding:5px 12px;color:var(--muted);font-size:11px;pointer-events:none}
  #minimap{position:absolute;right:12px;bottom:12px;background:rgba(8,14,28,.9);
    border:1px solid var(--line);border-radius:8px;overflow:hidden;z-index:15;
    box-shadow:0 4px 14px rgba(0,0,0,.5);cursor:pointer}
  #minimap svg{display:block}
  #mmview{position:absolute;border:1.5px solid var(--accent);
    background:rgba(56,189,248,.14);pointer-events:none}
  .controls button:disabled{opacity:.35;cursor:default}
  .controls button.navb{font-size:14px;line-height:1;padding:4px 9px}
  #panel{position:absolute;top:92px;right:0;width:380px;max-width:88vw;
    height:calc(100vh - 92px);background:var(--panel);border-left:1px solid var(--line);
    overflow:auto;padding:16px;z-index:20;box-shadow:-8px 0 24px rgba(0,0,0,.4)}
  #panel.hidden{display:none}
  #panelClose{position:absolute;top:10px;right:12px;background:none;border:none;
    color:var(--muted);font-size:22px;cursor:pointer;line-height:1}
  #panel h2{margin:0 24px 4px 0;font-size:14px;word-break:break-word}
  #panel .pk{display:inline-block;font-size:10px;text-transform:uppercase;
    letter-spacing:.06em;padding:2px 7px;border-radius:6px;background:#1e293b;
    color:var(--accent);margin-bottom:10px}
  #panel .sec{margin-top:14px}
  #panel .sec h3{font-size:11px;text-transform:uppercase;letter-spacing:.06em;
    color:var(--muted);margin:0 0 6px}
  #panel pre{margin:0;background:#0b1222;border:1px solid var(--line);border-radius:8px;
    padding:9px;font-family:ui-monospace,Consolas,monospace;font-size:11.5px;
    white-space:pre-wrap;word-break:break-word;line-height:1.5}
  #panel .erow{display:flex;align-items:center;gap:7px;padding:3px 0;font-size:12px}
  #panel .erow a{color:var(--accent);cursor:pointer;text-decoration:none}
  #panel .erow a:hover{text-decoration:underline}
  #panel .etag{font-size:9px;text-transform:uppercase;padding:1px 5px;border-radius:5px;
    border:1px solid;font-weight:700}
</style>
</head>
<body>
<header id="bar">
  <div class="title">
    <span class="badge" id="viewBadge"></span>
    <h1 id="hTitle">__TITLE_TEXT__</h1>
    <span class="counts" id="counts"></span>
  </div>
  <div class="controls">
    <input type="text" id="search" placeholder="Buscar (/)" autocomplete="off">
    <span id="searchInfo" class="muted"></span>
    <select id="groupSel" title="Enfocar una funcion/cuerpo"></select>
    <label class="chk"><input type="checkbox" id="compact"> Compacto</label>
    <button id="navBack" class="navb" title="Volver (Alt+Left / Backspace)" disabled>&larr;</button>
    <button id="navFwd" class="navb" title="Avanzar (Alt+Right)" disabled>&rarr;</button>
    <button id="zin" title="Zoom + (+)">+</button>
    <button id="zout" title="Zoom - (-)">&minus;</button>
    <button id="fit" title="Ajustar a pantalla (f)">Ajustar</button>
    <button id="dl" title="Descargar como SVG">SVG</button>
  </div>
</header>
<div id="legend"></div>
<main id="viewport">
  <div id="world">
    <svg id="edges"><defs>
      <marker id="arrow" markerWidth="9" markerHeight="9" refX="7" refY="3"
        orient="auto" markerUnits="userSpaceOnUse">
        <path d="M0,0 L7,3 L0,6 z" fill="context-stroke"></path>
      </marker>
    </defs><g id="gEdges"></g></svg>
    <div id="groups"></div>
    <div id="nodes"></div>
  </div>
  <div id="hint">Arrastra para mover &middot; rueda para zoom &middot; hover/click en un nodo resalta su flujo</div>
  <div id="minimap"><svg id="mmsvg"></svg><div id="mmview"></div></div>
</main>
<aside id="panel" class="hidden">
  <button id="panelClose" title="Cerrar (Esc)">&times;</button>
  <div id="panelBody"></div>
</aside>
<script>
"use strict";
const GRAPH = __GRAPH_DATA__;
const TITLE = __GRAPH_TITLE__;
const VIEW  = __GRAPH_VIEW__;

const EDGE_COLORS = { flow:'#64748b', true:'#22c55e', false:'#ef4444', call:'#a855f7',
  back:'#f59e0b', break:'#f97316', body:'#38bdf8', extends:'#60a5fa', implements:'#34d399',
  fall:'#94a3b8' };
const EDGE_LABELS = { flow:'flujo', true:'true', false:'false', call:'call',
  back:'back-edge', break:'break', body:'cuerpo', extends:'extends', implements:'implements',
  fall:'caida (fall-through)' };

const NS='http://www.w3.org/2000/svg';
const nodeById = new Map(GRAPH.nodes.map(n=>[n.id,n]));
const groupTitle = new Map(GRAPH.groups.map(g=>[g.id,g.title||g.id]));
// aristas incidentes por nodo (para resaltar el flujo de un nodo)
const incident = new Map();
GRAPH.edges.forEach((e,i)=>{
  if(!incident.has(e.from)) incident.set(e.from,[]);
  if(!incident.has(e.to)) incident.set(e.to,[]);
  incident.get(e.from).push(i);
  if(e.to!==e.from) incident.get(e.to).push(i);
});
const denseLabels = GRAPH.nodes.length>120;

const state = { scale:1, tx:60, ty:60, compact:false, focus:'', hidden:new Set(), sel:null };
const nav = { back:[], fwd:[] };
let hoverId=null;
let edgeGeom=[];   // [{from,to,type,base,path,label}]

const $ = id => document.getElementById(id);
const viewport=$('viewport'), world=$('world'), nodesLayer=$('nodes'),
      groupsLayer=$('groups'), svg=$('edges'), gEdges=$('gEdges');

function ecolor(e){ return (e.color && /^#/.test(e.color)) ? e.color : (EDGE_COLORS[e.type]||'#64748b'); }
function visibleN(n){ return !state.focus || n.group===state.focus; }
function curFocus(){ return hoverId || (state.sel?state.sel.id:null); }

// ---- construccion de nodos -------------------------------------------------
function buildNodes(){
  nodesLayer.innerHTML='';
  for(const n of GRAPH.nodes){
    const el=document.createElement('div');
    el.className='node k-'+(n.kind||'node');
    el.dataset.id=n.id;
    el.style.background=n.fill; el.style.color=n.fc; el.style.borderColor=n.stroke;
    const h=document.createElement('div'); h.className='nh'; h.textContent=n.header||n.id; el.appendChild(h);
    if(n.lines && n.lines.length){
      if(state.compact){ const m=document.createElement('div'); m.className='nmore';
        m.textContent=n.lines.length+' linea'+(n.lines.length>1?'s':''); el.appendChild(m); }
      else { const b=document.createElement('div'); b.className='nb'; b.textContent=n.lines.join('\n'); el.appendChild(b); }
    }
    el.addEventListener('mouseenter',()=>{ hoverId=n.id; applyHighlight(); });
    el.addEventListener('mouseleave',()=>{ if(hoverId===n.id){ hoverId=null; applyHighlight(); } });
    el.addEventListener('click', ev=>{ ev.stopPropagation(); selectNode(n.id); });
    n._el=el; nodesLayer.appendChild(el);
  }
}

// ---- layout: ranking + reduccion de cruces + enderezado --------------------
function layout(){
  const vis = GRAPH.nodes.filter(visibleN);
  for(const n of GRAPH.nodes) n._el.style.display = visibleN(n) ? '' : 'none';
  if(!vis.length){ edgeGeom=[]; gEdges.innerHTML=''; groupsLayer.innerHTML=''; return; }
  const idset = new Set(vis.map(n=>n.id));

  const succ=new Map(), pred=new Map();
  vis.forEach(n=>{ succ.set(n.id,[]); pred.set(n.id,[]); });
  const le = GRAPH.edges.filter(e=>e.from!==e.to && idset.has(e.from) && idset.has(e.to));
  le.forEach(e=>{ succ.get(e.from).push(e.to); pred.get(e.to).push(e.from); });

  // back-edges (DFS iterativo) para romper ciclos antes de rankear
  const col=new Map(); vis.forEach(n=>col.set(n.id,0));
  const back=new Set();
  function dfs(s){ const st=[{u:s,i:0}]; col.set(s,1);
    while(st.length){ const t=st[st.length-1], ch=succ.get(t.u);
      if(t.i<ch.length){ const v=ch[t.i++];
        if(col.get(v)===1) back.add(t.u+'->'+v);
        else if(col.get(v)===0){ col.set(v,1); st.push({u:v,i:0}); } }
      else { col.set(t.u,2); st.pop(); } } }
  vis.forEach(n=>{ if(col.get(n.id)===0) dfs(n.id); });

  // rank por longest-path sobre aristas forward
  const fsucc=new Map(), indeg=new Map();
  vis.forEach(n=>{ fsucc.set(n.id,[]); indeg.set(n.id,0); });
  le.forEach(e=>{ if(!back.has(e.from+'->'+e.to)){ fsucc.get(e.from).push(e.to); indeg.set(e.to,indeg.get(e.to)+1); } });
  const rank=new Map(); vis.forEach(n=>rank.set(n.id,0));
  const q=[]; vis.forEach(n=>{ if(indeg.get(n.id)===0) q.push(n.id); });
  for(let qi=0; qi<q.length; qi++){ const u=q[qi];
    for(const v of fsucc.get(u)){ if(rank.get(v)<rank.get(u)+1) rank.set(v,rank.get(u)+1);
      indeg.set(v,indeg.get(v)-1); if(indeg.get(v)===0) q.push(v); } }

  let maxR=0; vis.forEach(n=>maxR=Math.max(maxR,rank.get(n.id)));
  const rows=[]; for(let r=0;r<=maxR;r++) rows.push([]);
  vis.forEach(n=>rows[rank.get(n.id)].push(n));

  // orden inicial por (grupo, indice original)
  const gOrder=new Map(); let go=0; GRAPH.nodes.forEach(n=>{ if(!gOrder.has(n.group)) gOrder.set(n.group,go++); });
  const idx0=new Map(); GRAPH.nodes.forEach((n,i)=>idx0.set(n.id,i));
  rows.forEach(row=>row.sort((a,b)=>(gOrder.get(a.group)-gOrder.get(b.group))||(idx0.get(a.id)-idx0.get(b.id))));
  rows.forEach(row=>row.forEach((n,i)=>n._ord=i));

  // reduccion de cruces: barycenter (down + up, varias iteraciones)
  function meanOrd(ids){ if(!ids.length) return null; let s=0; ids.forEach(id=>s+=nodeById.get(id)._ord); return s/ids.length; }
  for(let it=0; it<4; it++){
    for(let r=1;r<rows.length;r++){
      const arr=rows[r].map(n=>{ const b=meanOrd(pred.get(n.id)); return [b==null?n._ord:b, n._ord, n]; });
      arr.sort((a,b)=>a[0]-b[0]||a[1]-b[1]); rows[r]=arr.map(x=>x[2]); rows[r].forEach((n,i)=>n._ord=i);
    }
    for(let r=rows.length-2;r>=0;r--){
      const arr=rows[r].map(n=>{ const b=meanOrd(succ.get(n.id)); return [b==null?n._ord:b, n._ord, n]; });
      arr.sort((a,b)=>a[0]-b[0]||a[1]-b[1]); rows[r]=arr.map(x=>x[2]); rows[r].forEach((n,i)=>n._ord=i);
    }
  }

  // medir tamanos reales
  vis.forEach(n=>{ n._w=n._el.offsetWidth; n._h=n._el.offsetHeight; });
  const HGAP=30, VGAP=58;
  // y por rank (altura maxima de la fila)
  let y=0;
  for(let r=0;r<rows.length;r++){ const hh=rows[r].reduce((m,n)=>Math.max(m,n._h),0)||0;
    rows[r].forEach(n=>n._y=y); y+=hh+VGAP; }
  // x inicial: empaquetado secuencial por fila
  rows.forEach(row=>{ let x=0; row.forEach(n=>{ n._x=x; x+=n._w+HGAP; }); });
  // enderezado: relajar cada nodo hacia el centro medio de sus vecinos
  const nb=new Map(); vis.forEach(n=>nb.set(n.id,[...pred.get(n.id),...succ.get(n.id)]));
  function relax(row){
    if(!row.length) return;
    const want=row.map(n=>{ const a=nb.get(n.id); if(!a.length) return n._x+n._w/2;
      let s=0; a.forEach(id=>{ const m=nodeById.get(id); s+=m._x+m._w/2; }); return s/a.length; });
    let prevR=-1e9;
    for(let i=0;i<row.length;i++){ let left=want[i]-row[i]._w/2; if(left<prevR+HGAP) left=prevR+HGAP; row[i]._x=left; prevR=left+row[i]._w; }
  }
  for(let it=0; it<8; it++){ for(let r=0;r<rows.length;r++) relax(rows[r]); for(let r=rows.length-1;r>=0;r--) relax(rows[r]); }

  // normalizar a x>=50 y aplicar posiciones
  let minx=1e9,maxx=-1e9; vis.forEach(n=>{ minx=Math.min(minx,n._x); maxx=Math.max(maxx,n._x+n._w); });
  const shift=50-minx;
  vis.forEach(n=>{ n._x+=shift; n._el.style.left=n._x+'px'; n._el.style.top=n._y+'px'; });
  const totalW=(maxx-minx)+100, totalH=y+50;

  // puertos: distribuir salidas/entradas a lo ancho del nodo (separa if/else)
  const outE=new Map(), inE=new Map(); vis.forEach(n=>{ outE.set(n.id,[]); inE.set(n.id,[]); });
  GRAPH.edges.forEach((e,i)=>{ if(e.from===e.to) return; if(!idset.has(e.from)||!idset.has(e.to)) return;
    outE.get(e.from).push(i); inE.get(e.to).push(i); });
  const cx=id=>{ const m=nodeById.get(id); return m._x+m._w/2; };
  const port={};
  outE.forEach((arr,id)=>{ const n=nodeById.get(id);
    arr.sort((a,b)=>cx(GRAPH.edges[a].to)-cx(GRAPH.edges[b].to));
    arr.forEach((ei,k)=>{ (port[ei]=port[ei]||{}); port[ei].sx=n._x+n._w*((k+1)/(arr.length+1));
      port[ei].sy=n._y+n._h; port[ei].smid=n._y+n._h/2; port[ei].sright=n._x+n._w; }); });
  inE.forEach((arr,id)=>{ const n=nodeById.get(id);
    arr.sort((a,b)=>cx(GRAPH.edges[a].from)-cx(GRAPH.edges[b].from));
    arr.forEach((ei,k)=>{ (port[ei]=port[ei]||{}); port[ei].ex=n._x+n._w*((k+1)/(arr.length+1));
      port[ei].ey=n._y; port[ei].emid=n._y+n._h/2; port[ei].eright=n._x+n._w; }); });

  drawGroups(vis);
  buildEdges(idset, port);
  applyHighlight();
  svg.setAttribute('width',totalW); svg.setAttribute('height',totalH);
  world.style.width=totalW+'px'; world.style.height=totalH+'px';
  buildMinimap(totalW,totalH);
  applyTransform();
}

function drawGroups(vis){
  groupsLayer.innerHTML='';
  const byG=new Map();
  vis.forEach(n=>{ if(!n.group) return; if(!byG.has(n.group)) byG.set(n.group,[]); byG.get(n.group).push(n); });
  const PAD=16;
  for(const [gid,members] of byG){
    let x0=1e9,y0=1e9,x1=-1e9,y1=-1e9;
    members.forEach(n=>{ x0=Math.min(x0,n._x); y0=Math.min(y0,n._y); x1=Math.max(x1,n._x+n._w); y1=Math.max(y1,n._y+n._h); });
    const box=document.createElement('div'); box.className='grp';
    box.style.left=(x0-PAD)+'px'; box.style.top=(y0-PAD-6)+'px';
    box.style.width=(x1-x0+PAD*2)+'px'; box.style.height=(y1-y0+PAD*2+6)+'px';
    const t=document.createElement('div'); t.className='gt'; t.textContent=groupTitle.get(gid)||gid; box.appendChild(t);
    groupsLayer.appendChild(box);
  }
}

function buildEdges(idset, port){
  gEdges.innerHTML=''; edgeGeom=[];
  GRAPH.edges.forEach((e,i)=>{
    if(state.hidden.has(e.type)) return;
    const a=nodeById.get(e.from), b=nodeById.get(e.to);
    if(!a||!b||!idset.has(a.id)||!idset.has(b.id)) return;
    const col=ecolor(e), p=port[i]||{};
    let d, lx, ly;
    if(a.id===b.id){ const sx=a._x+a._w, sy=a._y+a._h*0.34, ey=a._y+a._h*0.66;
      d='M'+sx+','+sy+' C'+(sx+50)+','+(sy-8)+' '+(sx+50)+','+(ey+8)+' '+sx+','+ey; lx=sx+44; ly=(sy+ey)/2; }
    else if(b._y<=a._y){
      const side=Math.max(a._x+a._w,b._x+b._w)+58;
      const ssx=p.sright!=null?p.sright:a._x+a._w, ssy=p.smid!=null?p.smid:a._y+a._h/2;
      const eex=p.eright!=null?p.eright:b._x+b._w, eey=p.emid!=null?p.emid:b._y+b._h/2;
      d='M'+ssx+','+ssy+' C'+side+','+ssy+' '+side+','+eey+' '+eex+','+eey; lx=side; ly=(ssy+eey)/2; }
    else {
      const sx=p.sx!=null?p.sx:a._x+a._w/2, sy=a._y+a._h;
      const ex=p.ex!=null?p.ex:b._x+b._w/2, ey=b._y;
      if(Math.abs(sx-ex)<1.5) d='M'+sx+','+sy+' L'+ex+','+ey;
      else { const my=(sy+ey)/2; d='M'+sx+','+sy+' C'+sx+','+my+' '+ex+','+my+' '+ex+','+ey; }
      lx=(sx+ex)/2; ly=(sy+ey)/2; }
    const path=document.createElementNS(NS,'path');
    path.setAttribute('d',d); path.setAttribute('stroke',col);
    path.setAttribute('class','epath'+((e.type==='call'||e.type==='back')?' thick':''));
    if(e.style && e.style.indexOf('dotted')>=0) path.setAttribute('stroke-dasharray','2,5');
    else if(e.style && e.style.indexOf('dashed')>=0) path.setAttribute('stroke-dasharray','7,5');
    path.setAttribute('marker-end','url(#arrow)');
    gEdges.appendChild(path);
    let label=null;
    if(e.label){ label=document.createElementNS(NS,'text'); label.setAttribute('x',lx); label.setAttribute('y',ly);
      label.setAttribute('text-anchor','middle'); label.setAttribute('class','elabel'); label.setAttribute('fill',col); label.textContent=e.label;
      gEdges.appendChild(label); }
    edgeGeom.push({from:e.from,to:e.to,type:e.type,base:'epath'+((e.type==='call'||e.type==='back')?' thick':''),path,label});
  });
}

// ---- resaltado de flujo (hover / seleccion) --------------------------------
function applyHighlight(){
  const fid=curFocus();
  if(!fid){
    GRAPH.nodes.forEach(n=>{ if(n._el) n._el.classList.remove('faded'); });
    edgeGeom.forEach(g=>{ g.path.setAttribute('class',g.base); if(g.label) g.label.style.display = denseLabels?'none':''; });
    return;
  }
  const keep=new Set([fid]);
  (incident.get(fid)||[]).forEach(ei=>{ const e=GRAPH.edges[ei]; keep.add(e.from); keep.add(e.to); });
  GRAPH.nodes.forEach(n=>{ if(n._el && visibleN(n)) n._el.classList.toggle('faded', !keep.has(n.id)); });
  edgeGeom.forEach(g=>{ const on=(g.from===fid||g.to===fid);
    g.path.setAttribute('class', g.base+(on?' hl':' faded'));
    if(g.label) g.label.style.display = on ? '' : 'none'; });
}

// ---- transform / pan / zoom ------------------------------------------------
function applyTransform(){
  world.style.transform='translate('+state.tx+'px,'+state.ty+'px) scale('+state.scale+')';
  updateMinimapView();
}
function zoomAt(factor, cx, cy){
  const ns=Math.min(3,Math.max(0.05,state.scale*factor));
  const r=viewport.getBoundingClientRect();
  const px=(cx-r.left-state.tx)/state.scale, py=(cy-r.top-state.ty)/state.scale;
  state.scale=ns; state.tx=cx-r.left-px*ns; state.ty=cy-r.top-py*ns; applyTransform();
}
function fit(){
  const w=parseFloat(world.style.width)||1000, h=parseFloat(world.style.height)||1000;
  const r=viewport.getBoundingClientRect();
  state.scale=Math.max(0.05,Math.min((r.width-60)/w,(r.height-60)/h,1.4));
  state.tx=(r.width-w*state.scale)/2; state.ty=Math.max(20,(r.height-h*state.scale)/2); applyTransform();
}
viewport.addEventListener('wheel',ev=>{ ev.preventDefault(); zoomAt(ev.deltaY<0?1.12:1/1.12, ev.clientX, ev.clientY); },{passive:false});
let drag=null;
viewport.addEventListener('mousedown',ev=>{ if(ev.target.closest('.node')||ev.target.closest('#minimap')) return;
  drag={x:ev.clientX,y:ev.clientY,tx:state.tx,ty:state.ty}; viewport.classList.add('drag'); });
window.addEventListener('mousemove',ev=>{ if(!drag) return;
  state.tx=drag.tx+(ev.clientX-drag.x); state.ty=drag.ty+(ev.clientY-drag.y); applyTransform(); });
window.addEventListener('mouseup',()=>{ drag=null; viewport.classList.remove('drag'); });

// ---- seleccion + panel + navegacion (back/fwd) -----------------------------
function updateNavBtns(){ $('navBack').disabled=!nav.back.length; $('navFwd').disabled=!nav.fwd.length; }
function viewSnap(){ return {tx:state.tx,ty:state.ty,scale:state.scale,sel:state.sel?state.sel.id:null}; }
function pushNav(){ nav.back.push(viewSnap()); nav.fwd.length=0; updateNavBtns(); }
function restoreView(v){ state.tx=v.tx; state.ty=v.ty; state.scale=v.scale; applyTransform();
  if(v.sel) selectNode(v.sel,false); else clearSel(); }
function goBack(){ if(!nav.back.length) return; nav.fwd.push(viewSnap()); restoreView(nav.back.pop()); updateNavBtns(); }
function goFwd(){ if(!nav.fwd.length) return; nav.back.push(viewSnap()); restoreView(nav.fwd.pop()); updateNavBtns(); }
function clearSel(){ if(state.sel){ state.sel._el.classList.remove('sel'); state.sel=null; } applyHighlight(); $('panel').classList.add('hidden'); }

function selectNode(id, openPanel){
  const n=nodeById.get(id); if(!n) return;
  if(state.sel) state.sel._el.classList.remove('sel');
  state.sel=n; n._el.classList.add('sel'); applyHighlight();
  const esc=s=>String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
  const ins=GRAPH.edges.filter(e=>e.to===id && e.from!==id);
  const outs=GRAPH.edges.filter(e=>e.from===id && e.to!==id);
  function erow(e,other){ const c=ecolor(e), lab=EDGE_LABELS[e.type]||e.type;
    return '<div class="erow"><span class="etag" style="color:'+c+';border-color:'+c+'">'+lab+'</span>'
      +'<a data-go="'+esc(other)+'">'+esc((nodeById.get(other)||{}).header||other)+'</a></div>'; }
  let html='<h2>'+esc(n.header||n.id)+'</h2><span class="pk">'+esc(n.kind||'node')+'</span>';
  if(n.group) html+='<div class="muted">grupo: '+esc(groupTitle.get(n.group)||n.group)+'</div>';
  html+='<div class="muted" style="margin-top:2px">id: '+esc(n.id)+' &middot; shape: '+esc(n.shape)+'</div>';
  if(n.lines && n.lines.length) html+='<div class="sec"><h3>Cuerpo ('+n.lines.length+')</h3><pre>'+esc(n.lines.join('\n'))+'</pre></div>';
  if(n.tooltip) html+='<div class="sec"><h3>Detalle</h3><pre>'+esc(n.tooltip)+'</pre></div>';
  if(ins.length) html+='<div class="sec"><h3>Entrantes ('+ins.length+')</h3>'+ins.map(e=>erow(e,e.from)).join('')+'</div>';
  if(outs.length) html+='<div class="sec"><h3>Salientes ('+outs.length+')</h3>'+outs.map(e=>erow(e,e.to)).join('')+'</div>';
  $('panelBody').innerHTML=html;
  $('panelBody').querySelectorAll('a[data-go]').forEach(a=>a.addEventListener('click',()=>{
    pushNav(); const t=a.getAttribute('data-go'); selectNode(t,true); centerNode(t); }));
  if(openPanel!==false) $('panel').classList.remove('hidden');
}
function centerNode(id){
  const n=nodeById.get(id); if(!n || n._x==null) return;
  const r=viewport.getBoundingClientRect();
  state.tx=r.width/2-(n._x+n._w/2)*state.scale-190;
  state.ty=r.height/2-(n._y+n._h/2)*state.scale; applyTransform();
}
$('panelClose').addEventListener('click',clearSel);
$('navBack').addEventListener('click',goBack);
$('navFwd').addEventListener('click',goFwd);
viewport.addEventListener('click',()=>{});

// ---- busqueda --------------------------------------------------------------
let matches=[], mi=0;
function runSearch(){
  const qv=$('search').value.trim().toLowerCase();
  matches=[];
  for(const n of GRAPH.nodes){
    n._el.classList.remove('match');
    if(!qv) continue;
    const hay=((n.header||'')+' '+(n.lines||[]).join(' ')+' '+(n.tooltip||'')).toLowerCase();
    if(hay.indexOf(qv)>=0){ n._el.classList.add('match'); matches.push(n); }
  }
  $('searchInfo').textContent = qv ? (matches.length+' coincidencia'+(matches.length!==1?'s':'')) : '';
  mi=0; if(matches.length){ pushNav(); centerNode(matches[0].id); }
}
$('search').addEventListener('input',runSearch);
$('search').addEventListener('keydown',ev=>{
  if(ev.key==='Enter' && matches.length){ mi=(mi+1)%matches.length; pushNav(); centerNode(matches[mi].id); }
  if(ev.key==='Escape'){ $('search').value=''; runSearch(); $('search').blur(); }
});

// ---- leyenda + grupo + compacto --------------------------------------------
function buildLegend(){
  const present=[...new Set(GRAPH.edges.map(e=>e.type))];
  const order=['flow','fall','true','false','call','back','break','body','extends','implements'];
  present.sort((a,b)=>order.indexOf(a)-order.indexOf(b));
  const leg=$('legend'); leg.innerHTML='';
  if(!present.length){ leg.style.display='none'; return; }
  for(const t of present){ const c=EDGE_COLORS[t]||'#64748b';
    const lab=document.createElement('label');
    lab.innerHTML='<input type="checkbox" checked data-t="'+t+'"><span class="sw" style="border-color:'+c+'"></span>'+(EDGE_LABELS[t]||t);
    lab.querySelector('input').addEventListener('change',e=>{ if(e.target.checked) state.hidden.delete(t); else state.hidden.add(t); layout(); });
    leg.appendChild(lab); }
}
function buildGroupSel(){
  const sel=$('groupSel'); sel.innerHTML='';
  const all=document.createElement('option'); all.value=''; all.textContent='Todo el modulo'; sel.appendChild(all);
  let any=false;
  for(const g of GRAPH.groups){ if(!GRAPH.nodes.some(n=>n.group===g.id)) continue; any=true;
    const o=document.createElement('option'); o.value=g.id; o.textContent=g.title||g.id; sel.appendChild(o); }
  if(!any) sel.style.display='none';
  sel.addEventListener('change',()=>{ state.focus=sel.value; clearSel(); layout(); fit(); });
}
$('compact').addEventListener('change',e=>{ state.compact=e.target.checked; buildNodes(); layout(); });

// ---- minimapa --------------------------------------------------------------
let mmInfo=null;
function buildMinimap(W,H){
  const mm=$('minimap'), mmsvg=$('mmsvg'); if(!mm) return;
  const MW=190, MH=Math.max(70,Math.min(240, MW*H/Math.max(1,W)));
  mm.style.width=MW+'px'; mm.style.height=MH+'px';
  mmsvg.setAttribute('width',MW); mmsvg.setAttribute('height',MH); mmsvg.setAttribute('viewBox','0 0 '+W+' '+H);
  let s='';
  GRAPH.nodes.forEach(n=>{ if(!visibleN(n)||n._x==null) return;
    s+='<rect x="'+n._x+'" y="'+n._y+'" width="'+n._w+'" height="'+n._h+'" rx="3" fill="'+n.fill+'" opacity="0.85"/>'; });
  mmsvg.innerHTML=s; mmInfo={W,H,MW,MH}; updateMinimapView();
}
function updateMinimapView(){
  const v=$('mmview'); if(!v||!mmInfo) return;
  const r=viewport.getBoundingClientRect();
  const sx=mmInfo.MW/mmInfo.W, sy=mmInfo.MH/mmInfo.H;
  const vx=(-state.tx)/state.scale, vy=(-state.ty)/state.scale;
  const vw=r.width/state.scale, vh=r.height/state.scale;
  v.style.left=(vx*sx)+'px'; v.style.top=(vy*sy)+'px'; v.style.width=Math.max(6,vw*sx)+'px'; v.style.height=Math.max(6,vh*sy)+'px';
}
$('minimap').addEventListener('mousedown',ev=>{
  if(!mmInfo) return; ev.stopPropagation();
  const rect=$('minimap').getBoundingClientRect();
  const wx=(ev.clientX-rect.left)/mmInfo.MW*mmInfo.W, wy=(ev.clientY-rect.top)/mmInfo.MH*mmInfo.H;
  const r=viewport.getBoundingClientRect();
  state.tx=r.width/2-wx*state.scale; state.ty=r.height/2-wy*state.scale; applyTransform();
});

// ---- descarga SVG ----------------------------------------------------------
$('dl').addEventListener('click',()=>{
  const w=parseFloat(world.style.width)||1000, h=parseFloat(world.style.height)||1000;
  const clone=svg.cloneNode(true);
  clone.setAttribute('xmlns','http://www.w3.org/2000/svg'); clone.setAttribute('width',w); clone.setAttribute('height',h);
  const fo=document.createElementNS(NS,'foreignObject'); fo.setAttribute('x',0); fo.setAttribute('y',0); fo.setAttribute('width',w); fo.setAttribute('height',h);
  const div=document.createElement('div'); div.setAttribute('xmlns','http://www.w3.org/1999/xhtml');
  div.innerHTML=groupsLayer.outerHTML+nodesLayer.outerHTML; fo.appendChild(div); clone.appendChild(fo);
  const css=document.querySelector('style').textContent;
  const styleEl=document.createElementNS(NS,'style'); styleEl.textContent=css; clone.insertBefore(styleEl,clone.firstChild);
  const blob=new Blob(['<?xml version="1.0" encoding="UTF-8"?>\n',new XMLSerializer().serializeToString(clone)],{type:'image/svg+xml'});
  const a=document.createElement('a'); a.href=URL.createObjectURL(blob); a.download=(VIEW||'diagrama')+'.svg'; a.click();
});

// ---- controles + teclado ---------------------------------------------------
$('zin').addEventListener('click',()=>{const r=viewport.getBoundingClientRect();zoomAt(1.2,r.left+r.width/2,r.top+r.height/2);});
$('zout').addEventListener('click',()=>{const r=viewport.getBoundingClientRect();zoomAt(1/1.2,r.left+r.width/2,r.top+r.height/2);});
$('fit').addEventListener('click',fit);
window.addEventListener('keydown',ev=>{
  if(document.activeElement===$('search')) return;
  if(ev.altKey && ev.key==='ArrowLeft'){ ev.preventDefault(); goBack(); return; }
  if(ev.altKey && ev.key==='ArrowRight'){ ev.preventDefault(); goFwd(); return; }
  if(ev.key==='Backspace'){ ev.preventDefault(); goBack(); }
  else if(ev.key==='+'||ev.key==='='){const r=viewport.getBoundingClientRect();zoomAt(1.2,r.left+r.width/2,r.top+r.height/2);}
  else if(ev.key==='-'){const r=viewport.getBoundingClientRect();zoomAt(1/1.2,r.left+r.width/2,r.top+r.height/2);}
  else if(ev.key==='f'){fit();}
  else if(ev.key==='c'){$('compact').click();}
  else if(ev.key==='/'){ev.preventDefault();$('search').focus();}
  else if(ev.key==='Escape'){ clearSel(); }
});

// ---- init ------------------------------------------------------------------
$('viewBadge').textContent=VIEW;
$('counts').textContent=GRAPH.nodes.length+' nodos, '+GRAPH.edges.length+' aristas'
  +(GRAPH.groups.length?(', '+GRAPH.groups.length+' grupos'):'');
if(GRAPH.nodes.length>80){ state.compact=true; $('compact').checked=true; }
buildLegend(); buildGroupSel(); buildNodes(); layout(); fit(); updateNavBtns();
window.addEventListener('resize',updateMinimapView);
</script>
</body>
</html>
)VEXHTML";

        std::string build_page(const std::string &json,
                               const std::string &title,
                               const std::string &view) {
            std::string page = kHtmlTemplate;
            auto replace_all = [&](const std::string &needle, const std::string &val) {
                size_t pos = 0;
                while ((pos = page.find(needle, pos)) != std::string::npos) {
                    page.replace(pos, needle.size(), val);
                    pos += val.size();
                }
            };
            replace_all("__GRAPH_DATA__", json);
            replace_all("__GRAPH_TITLE__", "\"" + json_escape(title) + "\"");
            replace_all("__GRAPH_VIEW__", "\"" + json_escape(view) + "\"");
            // Tambien el titulo visible en <title> y <h1> (texto plano).
            replace_all("__TITLE_TEXT__", json_escape(title));
            return page;
        }

    } // namespace

    // ----------------------------------------------------------------------
    //  API publica
    // ----------------------------------------------------------------------

    std::string html_from_dot(const std::string &dot_source,
                              const std::string &title,
                              const std::string &view_kind) {
        DotModel mdl = parse_dot(dot_source);
        std::string json = model_to_json(mdl, view_kind);
        return build_page(json, title, view_kind);
    }

    std::string html_from_ast(const ast::ModuleNode &mod) {
        return html_from_dot(graphviz_from_ast(mod),
                             "AST Vex (post type-check)", "ast");
    }

    std::string html_from_ir_module(const ir::IrModule &mod,
                                    const std::string &title) {
        std::string view = (title.find("post") != std::string::npos) ? "ir-post"
                                                                      : "ir-pre";
        return html_from_dot(graphviz_from_ir_module(mod, title), title, view);
    }

    std::string html_from_vel_text(const std::string &vel_text) {
        return html_from_dot(graphviz_from_vel_text(vel_text),
                             "Bytecode .vel", "vel");
    }

} // namespace vex
