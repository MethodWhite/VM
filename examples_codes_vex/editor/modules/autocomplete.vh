// =============================================================================
// modules/autocomplete.vh - Popup de autocompletado tipo vnano
// =============================================================================
//
// Funciona escaneando el buffer del editor para extraer todas las palabras
// (secuencias de [A-Za-z0-9_]) y proponiendolas como candidatas cuando el
// usuario escribe un prefijo alfanumerico de al menos un caracter.
//
// Layout del popup:
//   +--------------+
//   | candidate_1  |
//   | candidate_2  |  <- destacado el seleccionado con REVERSE
//   | candidate_3  |
//   +--------------+
//
// API publica:
//   trigger(buffer, cursor)      -> rescaneya el buffer y arma candidatos
//                                   filtrados por el prefijo actual
//   refresh(buffer, cursor)      -> idem (alias semantico)
//   move_up() / move_down()
//   accept() -> string           -> texto a insertar (prefijo + sufijo del
//                                   candidato seleccionado).  Devuelve "" si
//                                   no hay seleccion valida.
//   cancel()                     -> cierra el popup
//   is_active() -> i32
//   render(screen)               -> dibuja la caja flotante; el editor debe
//                                   llamarlo despues de place_cursor
//
// Limites internos:
//   - max_candidates = 32 (resto se descarta para no explotar memoria)
//   - max_show = 8 (filas visibles a la vez)
//   - max_word_len = 64 bytes
//
// El editor decide CUANDO mostrar el popup: tipicamente tras insertar un
// caracter alfanumerico, si el prefijo tiene >= 2 chars y hay candidatos.

class Autocomplete {
    public i32      active;            ///< 1 = visible y consumiendo teclas
    public i32      selected;          ///< indice del candidato seleccionado
    public i32      scroll;            ///< primer candidato visible
    public string   prefix;            ///< prefijo del cursor (lo que el user escribio)
    public i32      pos_row;           ///< fila ANSI 1-based donde dibujar
    public i32      pos_col;           ///< col ANSI 1-based donde dibujar
    public i32      max_show;
    public i32      n_candidates;
    public i32      candidate_count;   ///< alias mas claro

    // Candidatos: arrays paralelos de tamano fijo 32.
    public string   c0; public string c1; public string c2; public string c3;
    public string   c4; public string c5; public string c6; public string c7;
    public string   c8; public string c9; public string c10; public string c11;
    public string   c12; public string c13; public string c14; public string c15;
    public string   c16; public string c17; public string c18; public string c19;
    public string   c20; public string c21; public string c22; public string c23;
    public string   c24; public string c25; public string c26; public string c27;
    public string   c28; public string c29; public string c30; public string c31;

    public Autocomplete() {
        this.active           = 0;
        this.selected         = 0;
        this.scroll           = 0;
        this.prefix           = "";
        this.pos_row          = 0;
        this.pos_col          = 0;
        this.max_show         = 8;
        this.n_candidates     = 0;
        this.candidate_count  = 0;
    }

    // Setter helpers para evitar arrays dinamicos (Vex no los expone
    // todavia y mantener 32 ranuras fijas es suficiente para autocomplete).
    public void set_at(i32 i, string s) {
        if (i == 0)  { this.c0  = s; return; }
        if (i == 1)  { this.c1  = s; return; }
        if (i == 2)  { this.c2  = s; return; }
        if (i == 3)  { this.c3  = s; return; }
        if (i == 4)  { this.c4  = s; return; }
        if (i == 5)  { this.c5  = s; return; }
        if (i == 6)  { this.c6  = s; return; }
        if (i == 7)  { this.c7  = s; return; }
        if (i == 8)  { this.c8  = s; return; }
        if (i == 9)  { this.c9  = s; return; }
        if (i == 10) { this.c10 = s; return; }
        if (i == 11) { this.c11 = s; return; }
        if (i == 12) { this.c12 = s; return; }
        if (i == 13) { this.c13 = s; return; }
        if (i == 14) { this.c14 = s; return; }
        if (i == 15) { this.c15 = s; return; }
        if (i == 16) { this.c16 = s; return; }
        if (i == 17) { this.c17 = s; return; }
        if (i == 18) { this.c18 = s; return; }
        if (i == 19) { this.c19 = s; return; }
        if (i == 20) { this.c20 = s; return; }
        if (i == 21) { this.c21 = s; return; }
        if (i == 22) { this.c22 = s; return; }
        if (i == 23) { this.c23 = s; return; }
        if (i == 24) { this.c24 = s; return; }
        if (i == 25) { this.c25 = s; return; }
        if (i == 26) { this.c26 = s; return; }
        if (i == 27) { this.c27 = s; return; }
        if (i == 28) { this.c28 = s; return; }
        if (i == 29) { this.c29 = s; return; }
        if (i == 30) { this.c30 = s; return; }
        if (i == 31) { this.c31 = s; return; }
    }

    public string get_at(i32 i) {
        if (i == 0)  { return this.c0;  }
        if (i == 1)  { return this.c1;  }
        if (i == 2)  { return this.c2;  }
        if (i == 3)  { return this.c3;  }
        if (i == 4)  { return this.c4;  }
        if (i == 5)  { return this.c5;  }
        if (i == 6)  { return this.c6;  }
        if (i == 7)  { return this.c7;  }
        if (i == 8)  { return this.c8;  }
        if (i == 9)  { return this.c9;  }
        if (i == 10) { return this.c10; }
        if (i == 11) { return this.c11; }
        if (i == 12) { return this.c12; }
        if (i == 13) { return this.c13; }
        if (i == 14) { return this.c14; }
        if (i == 15) { return this.c15; }
        if (i == 16) { return this.c16; }
        if (i == 17) { return this.c17; }
        if (i == 18) { return this.c18; }
        if (i == 19) { return this.c19; }
        if (i == 20) { return this.c20; }
        if (i == 21) { return this.c21; }
        if (i == 22) { return this.c22; }
        if (i == 23) { return this.c23; }
        if (i == 24) { return this.c24; }
        if (i == 25) { return this.c25; }
        if (i == 26) { return this.c26; }
        if (i == 27) { return this.c27; }
        if (i == 28) { return this.c28; }
        if (i == 29) { return this.c29; }
        if (i == 30) { return this.c30; }
        if (i == 31) { return this.c31; }
        return "";
    }

    // True si byte forma parte de un identificador.
    public i32 is_word_byte(i32 c) {
        if (c >= 48 && c <= 57)  { return 1; }   // 0-9
        if (c >= 65 && c <= 90)  { return 1; }   // A-Z
        if (c >= 97 && c <= 122) { return 1; }   // a-z
        if (c == 95)             { return 1; }   // _
        return 0;
    }

    // True si byte es parte del PREFIJO inicial (no permitimos digito inicial,
    // pero como filtramos el prefijo extraido del cursor lo aceptamos).
    public i32 is_prefix_byte(i32 c) {
        return this.is_word_byte(c);
    }

    // Devuelve la palabra-prefijo justo antes del cursor: secuencia de bytes
    // alfanumericos terminada en el cursor.  Vacia si cursor esta tras un
    // delimitador o al inicio del buffer.
    public string extract_prefix(u8* bdat, i32 cursor) {
        if (cursor <= 0) { return ""; }
        i32 start = cursor;
        while (start > 0 && this.is_word_byte(bdat[start - 1]) == 1) {
            start = start - 1;
        }
        i32 len = cursor - start;
        if (len <= 0) { return ""; }
        return str_make(bdat + start, len);
    }

    // Compara dos strings y devuelve 1 si son iguales byte a byte.
    public i32 str_eq(string a, string b) {
        i32 la = str_bytes(a);
        i32 lb = str_bytes(b);
        if (la != lb) { return 0; }
        if (la == 0)  { return 1; }
        u8* pa = str_cstr(a);
        u8* pb = str_cstr(b);
        i32 i  = 0;
        while (i < la) {
            if (pa[i] != pb[i]) { return 0; }
            i = i + 1;
        }
        return 1;
    }

    // True si haystack comienza con needle (igualdad byte-a-byte).
    public i32 starts_with(string haystack, string needle) {
        i32 ln = str_bytes(needle);
        if (ln == 0) { return 1; }
        i32 lh = str_bytes(haystack);
        if (lh < ln) { return 0; }
        u8* ph = str_cstr(haystack);
        u8* pn = str_cstr(needle);
        i32 i  = 0;
        while (i < ln) {
            if (ph[i] != pn[i]) { return 0; }
            i = i + 1;
        }
        return 1;
    }

    // True si ya existe el candidato (evitar duplicados).
    public i32 contains(string s) {
        i32 i = 0;
        while (i < this.candidate_count) {
            if (this.str_eq(this.get_at(i), s) == 1) { return 1; }
            i = i + 1;
        }
        return 0;
    }

    // Anade candidato si no esta duplicado y no es identico al prefijo.
    public void add_candidate(string w) {
        if (this.candidate_count >= 32) { return; }
        if (this.str_eq(w, this.prefix) == 1) { return; }
        if (this.contains(w) == 1) { return; }
        this.set_at(this.candidate_count, w);
        this.candidate_count = this.candidate_count + 1;
        this.n_candidates    = this.candidate_count;
    }

    // Escanea bdat[0..blen) extrayendo palabras que empiezan con prefix.
    // Las palabras se identifican como sequencias maximales de chars
    // alfanumericos + '_'.  Skippea palabras que igualan exactamente al
    // prefijo (no es util sugerir lo que ya tengo).
    public void scan_buffer(u8* bdat, i32 blen) {
        this.candidate_count = 0;
        this.n_candidates    = 0;
        if (str_bytes(this.prefix) == 0) { return; }
        i32 i = 0;
        while (i < blen && this.candidate_count < 32) {
            if (this.is_word_byte(bdat[i]) == 1) {
                i32 start = i;
                while (i < blen && this.is_word_byte(bdat[i]) == 1) {
                    i = i + 1;
                }
                i32 wlen = i - start;
                if (wlen > 0 && wlen <= 64) {
                    string w = str_make(bdat + start, wlen);
                    if (this.starts_with(w, this.prefix) == 1) {
                        this.add_candidate(w);
                    }
                }
            } else {
                i = i + 1;
            }
        }
    }

    // Punto de entrada: el editor llama esto despues de cada insercion
    // alfanumerica.  Si el prefijo es >= 1 char y hay >= 1 candidato,
    // activa el popup.  Si no, lo desactiva.
    public void maybe_trigger(u8* bdat, i32 blen, i32 cursor, i32 row, i32 col) {
        this.prefix = this.extract_prefix(bdat, cursor);
        i32 pl = str_bytes(this.prefix);
        if (pl < 2) {
            this.active = 0;
            this.candidate_count = 0;
            this.n_candidates    = 0;
            return;
        }
        this.scan_buffer(bdat, blen);
        if (this.candidate_count == 0) {
            this.active = 0;
            return;
        }
        this.active   = 1;
        this.selected = 0;
        this.scroll   = 0;
        this.pos_row  = row;
        this.pos_col  = col;
    }

    public void cancel() {
        this.active          = 0;
        this.candidate_count = 0;
        this.n_candidates    = 0;
        this.selected        = 0;
        this.scroll          = 0;
    }

    public i32 is_active() {
        if (this.active == 1 && this.candidate_count > 0) { return 1; }
        return 0;
    }

    public void move_up() {
        if (this.candidate_count == 0) { return; }
        if (this.selected > 0) {
            this.selected = this.selected - 1;
        } else {
            this.selected = this.candidate_count - 1;
        }
        this.adjust_scroll();
    }

    public void move_down() {
        if (this.candidate_count == 0) { return; }
        this.selected = this.selected + 1;
        if (this.selected >= this.candidate_count) { this.selected = 0; }
        this.adjust_scroll();
    }

    public void adjust_scroll() {
        if (this.selected < this.scroll) { this.scroll = this.selected; }
        if (this.selected >= this.scroll + this.max_show) {
            this.scroll = this.selected - this.max_show + 1;
        }
    }

    // Devuelve la PARTE A INSERTAR (sufijo tras el prefijo).  Si selected
    // o el candidato no existen, devuelve "".  Cancela el popup tras
    // aceptar.
    public string accept_suffix() {
        if (this.candidate_count == 0 || this.selected < 0
         || this.selected >= this.candidate_count) {
            this.cancel();
            return "";
        }
        string full   = this.get_at(this.selected);
        i32    fl     = str_bytes(full);
        i32    pl     = str_bytes(this.prefix);
        if (fl <= pl) { this.cancel(); return ""; }
        u8*    pf     = str_cstr(full);
        string suffix = str_make(pf + pl, fl - pl);
        this.cancel();
        return suffix;
    }

    // Dibuja la caja en (pos_row, pos_col).  La caja es:
    //   line 0: +-----+
    //   line 1..N: | candidate |
    //   line bottom: +-----+
    // Ancho minimo 18, maximo 50.
    public void render(Screen screen) {
        if (this.is_active() == 0) { return; }
        // Calcular ancho como max(len(candidate)) + padding
        i32 max_text = 0;
        i32 i = 0;
        while (i < this.candidate_count) {
            i32 tl = str_bytes(this.get_at(i));
            if (tl > max_text) { max_text = tl; }
            i = i + 1;
        }
        i32 inner_w = max_text + 2;   // pad 1 a cada lado
        if (inner_w < 16) { inner_w = 16; }
        if (inner_w > 48) { inner_w = 48; }
        i32 n_show = this.candidate_count;
        if (n_show > this.max_show) { n_show = this.max_show; }
        // Caja: si excede el ancho de pantalla, recortar
        if (this.pos_col + inner_w + 2 > screen.cols) {
            inner_w = screen.cols - this.pos_col - 2;
            if (inner_w < 8) { inner_w = 8; }
        }
        // Top border
        screen.move_cursor(this.pos_row, this.pos_col);
        print(DIM);
        print("+");
        i32 t = 0;
        while (t < inner_w) { print("-"); t = t + 1; }
        print("+");
        print(RESET);
        // Filas
        i32 row = 0;
        while (row < n_show) {
            i32 idx = this.scroll + row;
            screen.move_cursor(this.pos_row + 1 + row, this.pos_col);
            print(DIM); print("|"); print(RESET);
            if (idx == this.selected) {
                print(REVERSE); print(BOLD);
            }
            print(" ");
            // Texto del candidato (recortado al inner_w-2 si excede)
            string tx = this.get_at(idx);
            i32 tl = str_bytes(tx);
            i32 avail = inner_w - 2;
            if (tl <= avail) {
                print(tx);
                i32 pad = avail - tl;
                while (pad > 0) { print(" "); pad = pad - 1; }
            } else {
                // recortar bytes
                u8* tp = str_cstr(tx);
                string clipped = str_make(tp, avail);
                print(clipped);
            }
            print(" ");
            if (idx == this.selected) { print(RESET); }
            print(DIM); print("|"); print(RESET);
            row = row + 1;
        }
        // Bottom border
        screen.move_cursor(this.pos_row + 1 + n_show, this.pos_col);
        print(DIM);
        print("+");
        t = 0;
        while (t < inner_w) { print("-"); t = t + 1; }
        print("+");
        print(RESET);
    }
}

// (las .vh no llevan main: forman parte del modulo del editor)
