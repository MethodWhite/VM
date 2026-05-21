/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file debug_info.cpp
 * @brief Implementacion de la clase DebugInfo para consultar info de debug.
 *
 * Carga la seccion de depuracion (DebugSectionHeader + tablas) desde el blob
 * de bytes proporcionado por el caller, valida el magic/version y expone
 * lookups rapidos:
 *
 *   - lookup_line(bytecode_offset) -> LineInfo (archivo, linea, columna).
 *     Implementado con busqueda binaria sobre la tabla ordenada de
 *     DebugLineEntry / DebugLineEntry3.  Coste O(log N).
 *
 *   - lookup_vars(bytecode_offset) -> vector<VarInfo>.
 *     Recorre la tabla de variables y devuelve las visibles en el rango
 *     [scope_start, scope_end).  Coste O(N) -- aceptable porque una funcion
 *     tiene tipicamente <50 variables locales.
 *
 *   - lookup_scope(bytecode_offset) -> string.
 *     Solo nivel 3.  Devuelve el scope mas interno que contiene el offset.
 *
 * Tambien provee la API inversa @c lookup_offset_for_line para que el
 * debugger traduzca breakpoints "file.vex:42" a un bytecode_offset
 * concreto.  Implementado con busqueda lineal por (file, line) en la
 * tabla de lineas.  Coste O(N) por lookup, lo cual es aceptable porque
 * los breakpoints se setean al arrancar (no en hot path).
 */

#include "debug/debug_info.h"
#include <algorithm>
#include <cstring>

namespace debug {

    DebugInfo::DebugInfo(const uint8_t *data, size_t size) {
        if (!data || size < sizeof(DebugSectionHeader)) {
            return; // valid_ se queda en false
        }

        // Copia defensiva del blob: el caller puede liberar el buffer
        // original; mantenemos `raw_` como propietario para que los
        // punteros lines1_/lines3_/vars_/scopes_/strings_ apunten a
        // memoria estable durante la vida de DebugInfo.
        raw_.assign(data, data + size);

        const auto *hdr = reinterpret_cast<const DebugSectionHeader *>(raw_.data());

        if (hdr->magic != DEBUG_SECTION_MAGIC) return;
        if (hdr->version != DEBUG_FORMAT_VERSION) return;
        if (hdr->level < DEBUG_LEVEL_LINES || hdr->level > DEBUG_LEVEL_FULL) return;

        level_       = hdr->level;
        line_count_  = hdr->line_count;
        var_count_   = hdr->var_count;
        scope_count_ = hdr->scope_count;

        // Layout en el blob:
        //   [DebugSectionHeader  (24 B)]
        //   [DebugLineEntry[]    (N * 12 B) o [DebugLineEntry3[] (N * 16 B)]
        //   [DebugVarEntry[]     (V * 20 B)]   (solo nivel >= 2)
        //   [DebugScopeEntry[]   (S * 16 B)]   (solo nivel 3)
        //   [strings blob        (strings_size bytes)]
        const uint8_t *cursor = raw_.data() + sizeof(DebugSectionHeader);

        const size_t line_entry_sz = (level_ >= DEBUG_LEVEL_FULL)
                                         ? sizeof(DebugLineEntry3)
                                         : sizeof(DebugLineEntry);
        const size_t lines_total = static_cast<size_t>(line_count_) * line_entry_sz;
        if (cursor + lines_total > raw_.data() + size) return;

        if (level_ >= DEBUG_LEVEL_FULL) {
            lines3_ = reinterpret_cast<const DebugLineEntry3 *>(cursor);
        } else {
            lines1_ = reinterpret_cast<const DebugLineEntry *>(cursor);
        }
        cursor += lines_total;

        // Tabla de variables solo si nivel >= 2.
        if (level_ >= DEBUG_LEVEL_VARS && var_count_ > 0) {
            const size_t vars_total = static_cast<size_t>(var_count_) * sizeof(DebugVarEntry);
            if (cursor + vars_total > raw_.data() + size) return;
            vars_ = reinterpret_cast<const DebugVarEntry *>(cursor);
            cursor += vars_total;
        }

        // Tabla de scopes solo si nivel == 3.
        if (level_ >= DEBUG_LEVEL_FULL && scope_count_ > 0) {
            const size_t scopes_total = static_cast<size_t>(scope_count_) * sizeof(DebugScopeEntry);
            if (cursor + scopes_total > raw_.data() + size) return;
            scopes_ = reinterpret_cast<const DebugScopeEntry *>(cursor);
            cursor += scopes_total;
        }

        // Strings blob al final.
        if (hdr->strings_size > 0) {
            if (cursor + hdr->strings_size > raw_.data() + size) return;
            strings_ = reinterpret_cast<const char *>(cursor);
        }

        valid_ = true;
    }

    LineInfo DebugInfo::lookup_line(uint32_t bytecode_offset) const {
        LineInfo info{};
        info.found = false;
        if (!valid_ || line_count_ == 0) return info;

        // Busqueda binaria: encontrar la entrada con el mayor
        // bytecode_offset <= bytecode_offset.  La tabla esta ordenada
        // por bytecode_offset asi que upper_bound + retroceso es O(log N).
        if (level_ >= DEBUG_LEVEL_FULL && lines3_) {
            const DebugLineEntry3 *first = lines3_;
            const DebugLineEntry3 *last  = lines3_ + line_count_;
            // upper_bound: primer elemento con offset > bytecode_offset.
            auto it = std::upper_bound(first, last, bytecode_offset,
                [](uint32_t value, const DebugLineEntry3 &e) {
                    return value < e.bytecode_offset;
                });
            if (it == first) return info;
            const DebugLineEntry3 &e = *(it - 1);
            info.file   = strings_ ? (strings_ + e.file_offset) : "";
            info.line   = e.line_number;
            info.column = e.column;
            info.found  = true;
        } else if (lines1_) {
            const DebugLineEntry *first = lines1_;
            const DebugLineEntry *last  = lines1_ + line_count_;
            auto it = std::upper_bound(first, last, bytecode_offset,
                [](uint32_t value, const DebugLineEntry &e) {
                    return value < e.bytecode_offset;
                });
            if (it == first) return info;
            const DebugLineEntry &e = *(it - 1);
            info.file   = strings_ ? (strings_ + e.file_offset) : "";
            info.line   = e.line_number;
            info.column = 0;
            info.found  = true;
        }
        return info;
    }

    /**
     * @brief Traduce (file, line) -> bytecode_offset.
     *
     * Busqueda lineal por la tabla.  Devuelve el menor bytecode_offset
     * cuyo line_number sea >= line_target Y cuyo file_offset apunte a
     * @p file_target.  Devuelve UINT32_MAX si no encuentra match.
     *
     * Util para resolver breakpoints "file.vex:42" emitidos por el
     * cliente del debugger.  Puede haber multiples instrucciones VM
     * para una misma linea Vex (e.g. ALLOCA + CONST + STORE); elegimos
     * la PRIMERA de ese rango (menor offset), que corresponde al
     * "inicio" de la linea desde el punto de vista del usuario.
     */
    uint32_t DebugInfo::lookup_offset_for_line(const std::string &file_target,
                                                uint32_t           line_target) const {
        if (!valid_ || line_count_ == 0 || !strings_) return UINT32_MAX;

        // Match por file: comparamos contenido de strings_ + file_offset
        // con file_target.  Para velocidad cacheamos el offset del file
        // en un map en una mejora futura; por ahora linear scan basta.
        // line_target == 0 significa "primera linea conocida del file".
        uint32_t best_off  = UINT32_MAX;
        uint32_t best_line = UINT32_MAX;

        // Normalizamos un path para comparacion: backslashes -> forward y
        // toda en minusculas.  Asi `F:\C\VM\file.vex` matchea con
        // `f:/c/vm/file.vex` y `F:/C/VM/file.vex`.  Necesario porque el
        // IDE Electron envia paths en estilo forward-slash mientras que
        // el frontend Vex incrusta el path original (Windows backslash)
        // en la debug section del .velb.
        auto norm_path = [](const char *p, size_t len) {
            std::string out;
            out.reserve(len);
            for (size_t i = 0; i < len; i++) {
                char c = p[i];
                if (c == '\\') c = '/';
                else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
                out.push_back(c);
            }
            return out;
        };
        const std::string nt = norm_path(file_target.data(), file_target.size());

        // Path match helper.  file_target puede ser basename o path
        // completo; hacemos match por sufijo CASE/SLASH-INSENSITIVE.
        auto path_matches = [&](uint32_t file_off) -> bool {
            const char *fname = strings_ + file_off;
            const size_t flen = std::strlen(fname);
            const size_t tlen = nt.size();
            if (tlen == 0) return false;
            if (tlen > flen) return false;
            const std::string nf = norm_path(fname, flen);
            return nf.compare(nf.size() - tlen, tlen, nt) == 0;
        };

        // PASE 1: buscar MATCH EXACTO de linea.  Si existe una entrada
        // con ln == line_target, devolvemos el menor bc_off entre ellas.
        // Esto evita que un BP en la linea N (sin entry directo, e.g.
        // linea de comentario o firma `i32 main() {`) se solape con el
        // BP en la linea N+1 (que SI tiene entries), causando que ambos
        // BPs queden registrados con el MISMO addr.  Dos BPs al mismo
        // addr provocan que solo el primero registrado dispare (el
        // segundo queda enmascarado por last_bp_pc tras el continue),
        // sintoma reportado: "BP en linea de println no triggea pero
        // BP en return SI triggea".
        if (line_target != 0) {
            uint32_t exact_off = UINT32_MAX;
            if (level_ >= DEBUG_LEVEL_FULL && lines3_) {
                for (uint32_t i = 0; i < line_count_; ++i) {
                    if (lines3_[i].line_number == line_target
                     && path_matches(lines3_[i].file_offset)
                     && lines3_[i].bytecode_offset < exact_off) {
                        exact_off = lines3_[i].bytecode_offset;
                    }
                }
            } else if (lines1_) {
                for (uint32_t i = 0; i < line_count_; ++i) {
                    if (lines1_[i].line_number == line_target
                     && path_matches(lines1_[i].file_offset)
                     && lines1_[i].bytecode_offset < exact_off) {
                        exact_off = lines1_[i].bytecode_offset;
                    }
                }
            }
            if (exact_off != UINT32_MAX) return exact_off;
        }

        // PASE 2: fallback "siguiente linea conocida >= target".  Solo
        // se usa cuando la linea target NO tiene entries (e.g. comentario,
        // linea en blanco, firma de funcion).  En este caso resolvemos
        // a la primera instruccion despues del target -- comportamiento
        // tradicional de GDB.
        auto check_match = [&](uint32_t bc_off, uint32_t ln, uint32_t file_off) {
            if (!path_matches(file_off)) return;
            if (line_target == 0) {
                if (bc_off < best_off) {
                    best_off  = bc_off;
                    best_line = ln;
                }
                return;
            }
            if (ln > line_target) {
                if (ln < best_line || (ln == best_line && bc_off < best_off)) {
                    best_off  = bc_off;
                    best_line = ln;
                }
            }
        };

        if (level_ >= DEBUG_LEVEL_FULL && lines3_) {
            for (uint32_t i = 0; i < line_count_; ++i) {
                check_match(lines3_[i].bytecode_offset,
                            lines3_[i].line_number,
                            lines3_[i].file_offset);
            }
        } else if (lines1_) {
            for (uint32_t i = 0; i < line_count_; ++i) {
                check_match(lines1_[i].bytecode_offset,
                            lines1_[i].line_number,
                            lines1_[i].file_offset);
            }
        }
        return best_off;
    }

    std::vector<VarInfo> DebugInfo::lookup_vars(uint32_t bytecode_offset) const {
        std::vector<VarInfo> out;
        if (!valid_ || level_ < DEBUG_LEVEL_VARS || !vars_ || var_count_ == 0) {
            return out;
        }
        out.reserve(8);
        for (uint32_t i = 0; i < var_count_; ++i) {
            const DebugVarEntry &e = vars_[i];
            if (bytecode_offset >= e.scope_start && bytecode_offset < e.scope_end) {
                VarInfo vi{};
                vi.name         = strings_ ? (strings_ + e.name_offset) : "";
                vi.stack_offset = e.stack_offset;
                vi.type_kind    = e.type_kind;
                vi.var_flags    = e.var_flags;
                out.push_back(vi);
            }
        }
        return out;
    }

    std::string DebugInfo::lookup_scope(uint32_t bytecode_offset) const {
        if (!valid_ || level_ < DEBUG_LEVEL_FULL || !scopes_ || scope_count_ == 0) {
            return std::string();
        }
        // Buscar el scope mas interno que contiene el offset.  Como puede
        // haber scopes anidados, elegimos el de menor (end - start), que
        // corresponde al mas profundo de la jerarquia.
        const DebugScopeEntry *best = nullptr;
        uint32_t best_size = UINT32_MAX;
        for (uint32_t i = 0; i < scope_count_; ++i) {
            const DebugScopeEntry &e = scopes_[i];
            if (bytecode_offset >= e.start_offset
             && bytecode_offset <  e.end_offset) {
                const uint32_t sz = e.end_offset - e.start_offset;
                if (sz < best_size) {
                    best_size = sz;
                    best      = &e;
                }
            }
        }
        if (!best || !strings_) return std::string();
        return std::string(strings_ + best->name_offset);
    }

} // namespace debug
