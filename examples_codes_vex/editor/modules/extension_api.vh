// =============================================================================
// modules/extension_api.vh - registro global de extensiones del editor
// =============================================================================
//
// Una extension es un .velb separado que se carga via loadmodule(path)
// en runtime.  Al cargarse, su `main` registra comandos en el
// ExtensionRegistry global del editor mediante calls a register_command.
//
// Cada comando se identifica por (clase, slot) en lugar de un closure
// (closures cross-modulo no estan soportados todavia).  El editor
// invoca el comando via reflexion: findclass + findmethod + callm.
//
// Para el MVP exponemos solo el "hook on_key" (callback que recibe la
// tecla) y "hook on_render" (callback tras render).  Suficiente para
// demostrar la arquitectura de plugins.

class ExtensionRegistry {
    // Slot 0..3 (MVP).  Cada slot referencia el nombre de la clase de la
    // extension; el editor llama findclass(name).getMethod("on_key") y
    // luego callm para invocar.  Si name es la cadena vacia, el slot
    // esta libre.
    public string ext0_name;
    public string ext1_name;
    public string ext2_name;
    public string ext3_name;
    public i32    ext_count;

    public ExtensionRegistry() {
        this.ext_count  = 0;
    }

    // register_extension: anade una extension al primer slot libre.
    // class_name debe ser una clase con metodos publicos
    //   `void on_key(i32 k)`  (hook tras tecla, opcional)
    //   `void on_render()`    (hook tras render, opcional)
    // Devuelve 1 si OK, 0 si no hay slot libre.
    public i32 register_extension(string class_name) {
        if (this.ext_count == 0) { this.ext0_name = class_name; this.ext_count = 1; return 1; }
        if (this.ext_count == 1) { this.ext1_name = class_name; this.ext_count = 2; return 1; }
        if (this.ext_count == 2) { this.ext2_name = class_name; this.ext_count = 3; return 1; }
        if (this.ext_count == 3) { this.ext3_name = class_name; this.ext_count = 4; return 1; }
        return 0;
    }

    public string get_ext(i32 i) {
        if (i == 0) { return this.ext0_name; }
        if (i == 1) { return this.ext1_name; }
        if (i == 2) { return this.ext2_name; }
        if (i == 3) { return this.ext3_name; }
        return "";
    }
}
