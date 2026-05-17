// =============================================================================
// modules/extension_api.vh - registro de extensiones runtime via reflexion
// =============================================================================
//
// Una extension es un .velb separado que se carga via loadmodule(path) en
// runtime.  Su `main` retorna un indicador de exito; la clase de la
// extension queda registrada en el ClassRegistry global del Loader.
//
// El editor crea UNA instancia de ExtensionRegistry que mantiene hasta 4
// extensiones activas.  Cada slot guarda los handles cacheados (cls, inst,
// metodos hook) para evitar lookups en cada frame.
//
// API: el caller (main del editor) hace forName/newInstance/getMethod
// explicitamente (con strings literales, como exige el lenguaje), y pasa
// los handles ya resueltos al register_handles.  Esto evita necesitar
// `forName_dyn(string_var)` (no soportado todavia).
//
// Hooks soportados (todos opcionales):
//   - i32 on_render_line(u8* bdat, i32 line_off, i32 line_end, i32 cols)
//       Llamado para cada linea visible.  Devuelve 1 si la extension
//       imprimio la linea (el editor saltara su render por defecto).
//   - i32 on_keypress(i32 key)
//       Llamado tras leer cada tecla.  Devuelve 1 si la consumio (el
//       editor no procesara la tecla por defecto).
//
// Cuando un hook no existe en la clase, getMethod devuelve 0 y el
// dispatcher salta el slot sin invocarlo.

class ExtensionRegistry {
    // Slot 0..3: 4 extensiones cargables simultaneamente.
    public i64 ext0_cls; public i64 ext0_inst; public i64 ext0_render; public i64 ext0_key;
    public i64 ext1_cls; public i64 ext1_inst; public i64 ext1_render; public i64 ext1_key;
    public i64 ext2_cls; public i64 ext2_inst; public i64 ext2_render; public i64 ext2_key;
    public i64 ext3_cls; public i64 ext3_inst; public i64 ext3_render; public i64 ext3_key;
    public i32 ext_count;

    public ExtensionRegistry() {
        this.ext_count   = 0;
        this.ext0_cls = 0; this.ext0_inst = 0; this.ext0_render = 0; this.ext0_key = 0;
        this.ext1_cls = 0; this.ext1_inst = 0; this.ext1_render = 0; this.ext1_key = 0;
        this.ext2_cls = 0; this.ext2_inst = 0; this.ext2_render = 0; this.ext2_key = 0;
        this.ext3_cls = 0; this.ext3_inst = 0; this.ext3_render = 0; this.ext3_key = 0;
    }

    // Registra una extension via handles ya resueltos por el caller.
    // Devuelve 1 si OK, 0 si no hay slot libre.
    // m_render y m_key pueden ser 0 (hook no presente en la clase); el
    // dispatcher los saltara.
    public i32 register_handles(i64 cls, i64 inst, i64 m_render, i64 m_key) {
        if (cls == 0 || inst == 0) { return 0; }
        if (this.ext_count == 0) {
            this.ext0_cls = cls; this.ext0_inst = inst;
            this.ext0_render = m_render; this.ext0_key = m_key;
            this.ext_count = 1; return 1;
        }
        if (this.ext_count == 1) {
            this.ext1_cls = cls; this.ext1_inst = inst;
            this.ext1_render = m_render; this.ext1_key = m_key;
            this.ext_count = 2; return 1;
        }
        if (this.ext_count == 2) {
            this.ext2_cls = cls; this.ext2_inst = inst;
            this.ext2_render = m_render; this.ext2_key = m_key;
            this.ext_count = 3; return 1;
        }
        if (this.ext_count == 3) {
            this.ext3_cls = cls; this.ext3_inst = inst;
            this.ext3_render = m_render; this.ext3_key = m_key;
            this.ext_count = 4; return 1;
        }
        return 0;
    }

    // Recorre los slots invocando on_render_line(line_no, bdat, line_off, line_end, cols).
    // Cada extension puede:
    //   - imprimir un prefijo (e.g. line number gutter) y devolver 0 para
    //     que la siguiente extension continue (composicion);
    //   - imprimir el contenido completo y devolver 1 (consumida la linea,
    //     corta el dispatch).
    // Si ninguna extension consume la linea, retorna 0 -> render por defecto.
    public i32 dispatch_render_line(i32 line_no, u8* bdat, i32 line_off, i32 line_end, i32 cols) {
        if (this.ext_count >= 1 && this.ext0_render != 0) {
            i64 r = invoke(this.ext0_render, this.ext0_inst, line_no, bdat, line_off, line_end, cols);
            if (r != 0) { return 1; }
        }
        if (this.ext_count >= 2 && this.ext1_render != 0) {
            i64 r = invoke(this.ext1_render, this.ext1_inst, line_no, bdat, line_off, line_end, cols);
            if (r != 0) { return 1; }
        }
        if (this.ext_count >= 3 && this.ext2_render != 0) {
            i64 r = invoke(this.ext2_render, this.ext2_inst, line_no, bdat, line_off, line_end, cols);
            if (r != 0) { return 1; }
        }
        if (this.ext_count >= 4 && this.ext3_render != 0) {
            i64 r = invoke(this.ext3_render, this.ext3_inst, line_no, bdat, line_off, line_end, cols);
            if (r != 0) { return 1; }
        }
        return 0;
    }

    // Recorre los slots invocando on_keypress.  Si alguna extension
    // devuelve 1, retorna 1 (tecla consumida).
    public i32 dispatch_keypress(i32 key) {
        if (this.ext_count >= 1 && this.ext0_key != 0) {
            i64 r = invoke(this.ext0_key, this.ext0_inst, key);
            if (r != 0) { return 1; }
        }
        if (this.ext_count >= 2 && this.ext1_key != 0) {
            i64 r = invoke(this.ext1_key, this.ext1_inst, key);
            if (r != 0) { return 1; }
        }
        if (this.ext_count >= 3 && this.ext2_key != 0) {
            i64 r = invoke(this.ext2_key, this.ext2_inst, key);
            if (r != 0) { return 1; }
        }
        if (this.ext_count >= 4 && this.ext3_key != 0) {
            i64 r = invoke(this.ext3_key, this.ext3_inst, key);
            if (r != 0) { return 1; }
        }
        return 0;
    }
}
