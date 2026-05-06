// =============================================================================
// modules/syntax_vex.vh - extension de syntax highlighting para Vex
// =============================================================================
//
// Tokenizer minimalista que reconoce las categorias del lenguaje Vex y las
// pinta con codigos ANSI inline:
//
//   * keywords    -> CYAN bold      (class fn if while for return ...)
//   * tipos       -> GREEN          (i32 i64 u8 ptr string void bool ...)
//   * literales   -> MAGENTA        (numeros 0..9, hex, bin, oct, true/false/null)
//   * strings     -> YELLOW         (entre comillas)
//   * comentarios -> DIM            (// hasta fin de linea)
//   * operadores  -> RESET          (no resaltado)
//   * identifs    -> RESET
//
// Diseño de la implementacion:
//   - Para evitar un bug del regalloc del frontend Vex con cascadas
//     profundas de `||` + CALL dentro de un loop body, las clasificaciones
//     de caracteres se hacen via funciones auxiliares CHARTYPE/categorize
//     que viven FUERA del while principal.  El render_line entonces solo
//     hace `switch` sobre el resultado entero (sin `||` ni CALL en
//     posicion de condicion-de-loop).
//
//   - El metodo se invoca como modulo compile-time (no via loadmodule)
//     porque el sistema de extensions runtime requiere `getMethod`,
//     `invoke` y `newInstance` que aun no son builtins de Vex.

// Categorias de caracter para el dispatch principal de render_line.
const i32 CHARTYPE_OTHER   = 0;
const i32 CHARTYPE_SLASH   = 1;   // '/' (posible inicio de comentario)
const i32 CHARTYPE_QUOTE   = 2;   // '"' (inicio de string literal)
const i32 CHARTYPE_DIGIT   = 3;   // 0..9
const i32 CHARTYPE_ALPHA   = 4;   // a..z A..Z _
const i32 CHARTYPE_AT      = 5;   // '@' (inicio de anotacion)

class SyntaxVex {
    public string keywords;
    public string types;
    public i32    enabled;        // 1=on, 0=off (toggle con F2 desde editor)

    public SyntaxVex() {
        // Cubre los keywords del lenguaje Vex tal como existen hoy.
        // El '|' al inicio y al final simplifica el match: buscamos
        // "|word|" sin casos especiales para los extremos.
        this.keywords = "|class|fn|if|else|while|for|do|return|break|continue|true|false|null|new|this|super|public|private|protected|static|final|const|let|import|using|typedef|struct|enum|match|case|interface|extends|implements|throw|try|catch|finally|synchronized|spawn|await|rspawn|here|on|nonnull|delete|free|sizeof|in|extern|@Override|@Inline|@Async|@Aspect|@Before|@After|@Around|@Module|@Export|@Generic|@Asm|";
        this.types    = "|i8|i16|i32|i64|u8|u16|u32|u64|f32|f64|bool|char|void|string|ptr|cstring|Optional|Result|Some|None|Ok|Err|ArrayList|HashMap|HashSet|Queue|Deque|TreeMap|TreeSet|Stack|VirtualPtr|";
        this.enabled  = 1;
    }

    // ----------------------------------------------------------------
    // Helpers de clasificacion de caracteres.
    // ----------------------------------------------------------------

    public i32 is_alpha(i32 c) {
        if (c >= 65 && c <= 90)  { return 1; }   // A..Z
        if (c >= 97 && c <= 122) { return 1; }   // a..z
        if (c == 95)             { return 1; }   // _
        return 0;
    }

    public i32 is_digit(i32 c) {
        if (c >= 48 && c <= 57) { return 1; }    // 0..9
        return 0;
    }

    public i32 is_alnum(i32 c) {
        if (this.is_alpha(c) == 1) { return 1; }
        if (this.is_digit(c) == 1) { return 1; }
        return 0;
    }

    // True si c es un digito hex o un sufijo de numero (x b o)
    public i32 is_numchar(i32 c) {
        if (this.is_digit(c) == 1)         { return 1; }
        if (c == 46)                       { return 1; }  // '.'
        if (c == 120 || c == 88)           { return 1; }  // x X
        if (c == 98  || c == 111)          { return 1; }  // b o
        if (c >= 97  && c <= 102)          { return 1; }  // a..f
        if (c >= 65  && c <= 70)           { return 1; }  // A..F
        return 0;
    }

    // Devuelve la CHARTYPE_* del caracter c.  Concentra todas las
    // cascadas de comparaciones aqui, en una funcion sin loop, donde
    // el regalloc tiene poco que rastrear.
    public i32 categorize(i32 c) {
        if (c == 47)                       { return CHARTYPE_SLASH; }
        if (c == 34)                       { return CHARTYPE_QUOTE; }
        if (this.is_digit(c) == 1)         { return CHARTYPE_DIGIT; }
        if (this.is_alpha(c) == 1)         { return CHARTYPE_ALPHA; }
        if (c == 64)                       { return CHARTYPE_AT;    }
        return CHARTYPE_OTHER;
    }

    // ----------------------------------------------------------------
    // Dictionary lookup: keyword o tipo.
    // ----------------------------------------------------------------

    // Compara `len` bytes de bdat[off..off+len] con la subcadena de
    // `dict` empezando en pos.  Devuelve 1 si coinciden EXACTAMENTE.
    public i32 match_at(u8* bdat, i32 off, i32 len, u8* draw, i32 dlen, i32 dict_pos) {
        i32 i = 0;
        while (i < len) {
            if (dict_pos + i >= dlen) { return 0; }
            if (bdat[off + i] != draw[dict_pos + i]) { return 0; }
            i = i + 1;
        }
        return 1;
    }

    // True si `bdat[off..off+len]` aparece como token completo en `dict`.
    public i32 in_dict(u8* bdat, i32 off, i32 len, string dict) {
        u8* draw = str_cstr(dict);
        i32 dlen = str_bytes(dict);
        i32 p = 0;
        while (p < dlen) {
            if (draw[p] == 124) {           // '|'
                p = p + 1;
                if (this.match_at(bdat, off, len, draw, dlen, p) == 1) {
                    if (p + len < dlen && draw[p + len] == 124) {
                        return 1;
                    }
                }
            } else {
                p = p + 1;
            }
        }
        return 0;
    }

    // ----------------------------------------------------------------
    // Sub-rutinas de render: cada una cubre una categoria de token.
    // Devuelven el nuevo offset `i` tras consumir el token.
    // ----------------------------------------------------------------

    // Imprime un comentario de linea ('//' hasta line_end).  Asume que
    // bdat[i] == '/' y bdat[i+1] == '/' (chequeado en render_line).
    public i32 emit_comment(u8* bdat, i32 i, i32 line_end) {
        print(DIM); print(WHITE);
        while (i < line_end) {
            print_char(bdat[i]);
            i = i + 1;
        }
        print(RESET);
        return i;
    }

    // Imprime un string literal ('"' bdat[i] hasta proximo '"' no escapado).
    public i32 emit_string(u8* bdat, i32 i, i32 line_end) {
        print(YELLOW);
        print_char(bdat[i]);     // '"' inicial
        i = i + 1;
        i32 done = 0;
        while (done == 0 && i < line_end) {
            i32 cc = bdat[i];
            if (cc == 92 && i + 1 < line_end) {
                // backslash escape: imprimir 2 chars
                print_char(cc);
                i = i + 1;
                print_char(bdat[i]);
                i = i + 1;
            } else if (cc == 34) {
                print_char(cc);
                i = i + 1;
                done = 1;
            } else {
                print_char(cc);
                i = i + 1;
            }
        }
        print(RESET);
        return i;
    }

    // Imprime un numero (digit run con sufijos hex/bin/oct/dot).
    public i32 emit_number(u8* bdat, i32 i, i32 line_end) {
        print(MAGENTA);
        i32 done = 0;
        while (done == 0 && i < line_end) {
            if (this.is_numchar(bdat[i]) == 1) {
                print_char(bdat[i]);
                i = i + 1;
            } else {
                done = 1;
            }
        }
        print(RESET);
        return i;
    }

    // Imprime un identificador / keyword / tipo.  Lee alnum, decide
    // categoria via diccionarios, aplica color y emite los bytes.
    public i32 emit_ident(u8* bdat, i32 i, i32 line_end) {
        i32 start = i;
        i32 done = 0;
        while (done == 0 && i < line_end) {
            if (this.is_alnum(bdat[i]) == 1) {
                i = i + 1;
            } else {
                done = 1;
            }
        }
        i32 tlen = i - start;
        i32 is_kw = this.in_dict(bdat, start, tlen, this.keywords);
        i32 is_ty = 0;
        if (is_kw == 0) {
            is_ty = this.in_dict(bdat, start, tlen, this.types);
        }
        if (is_kw == 1)      { print(BOLD); print(CYAN); }
        else if (is_ty == 1) { print(GREEN); }
        i32 j = start;
        while (j < i) {
            print_char(bdat[j]);
            j = j + 1;
        }
        if (is_kw == 1 || is_ty == 1) { print(RESET); }
        return i;
    }

    // Imprime una anotacion (@Word).
    public i32 emit_annot(u8* bdat, i32 i, i32 line_end) {
        i32 start = i;
        i = i + 1;       // saltar '@'
        i32 done = 0;
        while (done == 0 && i < line_end) {
            if (this.is_alnum(bdat[i]) == 1) {
                i = i + 1;
            } else {
                done = 1;
            }
        }
        i32 tlen = i - start;
        i32 is_kw = this.in_dict(bdat, start, tlen, this.keywords);
        if (is_kw == 1) { print(BOLD); print(CYAN); }
        i32 j = start;
        while (j < i) {
            print_char(bdat[j]);
            j = j + 1;
        }
        if (is_kw == 1) { print(RESET); }
        return i;
    }

    // ----------------------------------------------------------------
    // Loop principal: dispatch via categorize().  Sin cascadas de `||`
    // adentro del while -- todos los chequeos viven en categorize().
    // ----------------------------------------------------------------
    public void render_line(u8* bdat, i32 line_off, i32 line_end, i32 cols) {
        i32 i = line_off;
        i32 col = 0;
        while (i < line_end) {
            if (col >= cols) { i = line_end; }
            if (i < line_end) {
                i32 c   = bdat[i];
                i32 cat = this.categorize(c);
                i32 new_i = i;
                if (cat == CHARTYPE_SLASH) {
                    // Doble slash = comentario.
                    if (i + 1 < line_end && bdat[i + 1] == 47) {
                        new_i = this.emit_comment(bdat, i, line_end);
                    } else {
                        print_char(c);
                        new_i = i + 1;
                    }
                } else if (cat == CHARTYPE_QUOTE) {
                    new_i = this.emit_string(bdat, i, line_end);
                } else if (cat == CHARTYPE_DIGIT) {
                    new_i = this.emit_number(bdat, i, line_end);
                } else if (cat == CHARTYPE_ALPHA) {
                    new_i = this.emit_ident(bdat, i, line_end);
                } else if (cat == CHARTYPE_AT) {
                    new_i = this.emit_annot(bdat, i, line_end);
                } else {
                    print_char(c);
                    new_i = i + 1;
                }
                col = col + (new_i - i);
                i = new_i;
            }
        }
    }
}
