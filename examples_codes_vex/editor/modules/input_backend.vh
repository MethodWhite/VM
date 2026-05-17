// =============================================================================
// modules/input_backend.vh - backend de lectura de teclas via FFI
// =============================================================================
//
// Replica el patron de vnano.vsh InputBackend._setup adaptado a Vex:
//   * Windows : msvcrt._kbhit + msvcrt._getch (no bloqueante).
//   * user32  : GetAsyncKeyState para detectar Ctrl/Shift/Alt fisicas.
//
// El metodo poll_key() devuelve la siguiente tecla pulsada o KEY_NONE
// si no hay.  read_key_blocking() hace busy-wait + flush sobre poll.

class InputBackend {
    public i64 lib_msvcrt;
    public i64 lib_user32;
    public i64 sym_kbhit;
    public i64 sym_getch;
    public i64 sym_gaks;       // GetAsyncKeyState (puede ser 0 si user32 falla)

    public InputBackend() {
        this.lib_msvcrt = ffi_open("msvcrt.dll");
        this.lib_user32 = ffi_open("user32.dll");
        this.sym_kbhit  = ffi_sym(this.lib_msvcrt, "_kbhit");
        this.sym_getch  = ffi_sym(this.lib_msvcrt, "_getch");
        if (this.lib_user32 != 0) {
            this.sym_gaks = ffi_sym(this.lib_user32, "GetAsyncKeyState");
        } else {
            this.sym_gaks = 0;
        }
    }

    // poll_key: tecla actual o KEY_NONE.  Las teclas extendidas (flechas,
    // F1..F12, Home/End, etc.) llegan como secuencia 0x00 / 0xE0 + scancode.
    // Si SHIFT esta pulsado fisicamente durante una flecha/nav, emitimos
    // KEY_SHIFT_* en vez del KEY_* normal para que el editor extienda la
    // seleccion.  Capturamos el estado del shift JUSTO antes del
    // win_special para minimizar ventanas de carrera.
    public i32 poll_key() {
        i64 n = ffi_call(this.sym_kbhit);
        if (n == 0) {
            return KEY_NONE;
        }
        i64 c = ffi_call(this.sym_getch);
        if (c == 0 || c == 224) {
            i64 c2 = ffi_call(this.sym_getch);
            i32 shift_now = this.is_key_down(VK_SHIFT);
            i32 base = this.win_special(c2);
            if (shift_now == 1) {
                if (base == KEY_UP)    { return KEY_SHIFT_UP;    }
                if (base == KEY_DOWN)  { return KEY_SHIFT_DOWN;  }
                if (base == KEY_LEFT)  { return KEY_SHIFT_LEFT;  }
                if (base == KEY_RIGHT) { return KEY_SHIFT_RIGHT; }
                if (base == KEY_HOME)  { return KEY_SHIFT_HOME;  }
                if (base == KEY_END)   { return KEY_SHIFT_END;   }
            }
            return base;
        }
        i32 ic = c;
        return ic;
    }

    public i32 read_key_blocking() {
        flush();
        while (true) {
            i32 k = this.poll_key();
            if (k != KEY_NONE) {
                return k;
            }
        }
        return KEY_NONE;
    }

    // Mapeo de scancodes Windows tras prefijo 0x00 / 0xE0.  Tomado
    // directamente de vnano.vsh._win_special.
    public i32 win_special(i64 c2) {
        if (c2 == 72) { return KEY_UP;    }
        if (c2 == 80) { return KEY_DOWN;  }
        if (c2 == 75) { return KEY_LEFT;  }
        if (c2 == 77) { return KEY_RIGHT; }
        if (c2 == 71) { return KEY_HOME;  }
        if (c2 == 79) { return KEY_END;   }
        if (c2 == 73) { return KEY_PGUP;  }
        if (c2 == 81) { return KEY_PGDN;  }
        if (c2 == 83) { return KEY_DEL;   }
        if (c2 == 59) { return KEY_F1;    }
        if (c2 == 60) { return KEY_F2;    }
        if (c2 == 61) { return KEY_F3;    }
        if (c2 == 62) { return KEY_F4;    }
        if (c2 == 63) { return KEY_F5;    }
        if (c2 == 64) { return KEY_F6;    }
        if (c2 == 65) { return KEY_F7;    }
        if (c2 == 66) { return KEY_F8;    }
        if (c2 == 67) { return KEY_F9;    }
        if (c2 == 68) { return KEY_F10;   }
        if (c2 == 133){ return KEY_F11;   }   // VK_F11 extended
        if (c2 == 134){ return KEY_F12;   }   // VK_F12 extended
        return KEY_NONE;
    }

    // True si la tecla virtual esta fisicamente pulsada AHORA mismo.
    // Devuelve 0 si no hay user32 o la tecla no esta abajo.
    public i32 is_key_down(i32 vkey) {
        if (this.sym_gaks == 0) { return 0; }
        i64 r = ffi_call(this.sym_gaks, vkey);
        if ((r & 0x8000) != 0) { return 1; }
        return 0;
    }
}
