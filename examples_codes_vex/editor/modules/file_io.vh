// =============================================================================
// modules/file_io.vh - I/O de fichero via FFI a msvcrt
// =============================================================================
//
// Encapsula la apertura/escritura/lectura de ficheros usando msvcrt.dll
// directamente con ffi_call (mismo backend que vesta_io::vio_fopen pero
// expuesto a Vex sin pasar por el plugin nativo).  Los simbolos se
// cachean en el constructor para evitar lookup repetido.
//
// La eleccion de msvcrt sobre vio_fopen es deliberada: la API msvcrt es
// directa (tres ffi_calls), bien conocida (man fopen / man fwrite) y
// permite leer y escribir en cualquier modo (wb, rb, ab, ...).

class FileIO {
    public i64 lib;
    public i64 sym_fopen;
    public i64 sym_fwrite;
    public i64 sym_fread;
    public i64 sym_fclose;
    public i64 sym_fseek;
    public i64 sym_ftell;
    public i64 sym_rewind;
    public i64 sym_fflush;
    public i64 sym_chdir;       // _chdir: cambiar directorio actual (Win32 msvcrt)
    public i64 sym_getcwd;      // _getcwd: obtener directorio actual

    public FileIO() {
        this.lib         = ffi_open("msvcrt.dll");
        this.sym_fopen   = ffi_sym(this.lib, "fopen");
        this.sym_fwrite  = ffi_sym(this.lib, "fwrite");
        this.sym_fread   = ffi_sym(this.lib, "fread");
        this.sym_fclose  = ffi_sym(this.lib, "fclose");
        this.sym_fseek   = ffi_sym(this.lib, "fseek");
        this.sym_ftell   = ffi_sym(this.lib, "ftell");
        this.sym_rewind  = ffi_sym(this.lib, "rewind");
        this.sym_fflush  = ffi_sym(this.lib, "fflush");
        this.sym_chdir   = ffi_sym(this.lib, "_chdir");
        this.sym_getcwd  = ffi_sym(this.lib, "_getcwd");
    }

    // chdir: cambiar el directorio actual del proceso.  Devuelve 0 si OK,
    // -1 si error (path no existe).  Usado por el editor para fijar la
    // carpeta del proyecto al arrancar (FileExplorer lista "*" del cwd).
    public i32 chdir(string path) {
        if (this.sym_chdir == 0) { return -1; }
        i64 r = ffi_call(this.sym_chdir, str_cstr(path));
        return (i32) r;
    }

    // getcwd: rellena `buf` (tamano `size`) con el path absoluto del cwd
    // null-terminated.  Devuelve buf en exito, 0 en error.
    public i64 getcwd(u8* buf, i32 size) {
        if (this.sym_getcwd == 0) { return 0; }
        return ffi_call(this.sym_getcwd, buf, size);
    }

    // Flush del buffer interno del FILE* al disco.  Importante para
    // logs de debug que deben sobrevivir a un crash.
    public i64 flush_fp(i64 fp) {
        return ffi_call(this.sym_fflush, fp);
    }

    // open: devuelve FILE* (i64) o 0 si falla.  mode = "wb", "rb", "ab".
    public i64 open(string path, string mode) {
        return ffi_call(this.sym_fopen, str_cstr(path), str_cstr(mode));
    }

    // write_bytes: escribe `len` bytes desde host_ptr `buf` al fichero
    // `fp`.  Devuelve numero de bytes escritos (deberia == len).
    public i64 write_bytes(i64 fp, u8* buf, i32 len) {
        return ffi_call(this.sym_fwrite, buf, 1, len, fp);
    }

    // write_string: escribe `len` bytes desde host_ptr `buf` (sirve para
    // tanto el data de Line como str_cstr de un literal Vex).
    public i64 write_str(i64 fp, i64 ptr, i32 len) {
        return ffi_call(this.sym_fwrite, ptr, 1, len, fp);
    }

    // close: cierra el fichero (devuelve 0 si OK).
    public i64 close(i64 fp) {
        return ffi_call(this.sym_fclose, fp);
    }

    // file_size: tamano en bytes del fichero abierto en `fp` (via fseek
    // a SEEK_END + ftell + rewind).  SEEK_END = 2.
    public i64 file_size(i64 fp) {
        ffi_call(this.sym_fseek, fp, 0, 2);
        i64 sz = ffi_call(this.sym_ftell, fp);
        ffi_call(this.sym_rewind, fp);
        return sz;
    }

    // read_bytes: lee `len` bytes desde `fp` al buffer host `buf`.
    // Devuelve numero de bytes leidos.
    public i64 read_bytes(i64 fp, u8* buf, i32 len) {
        return ffi_call(this.sym_fread, buf, 1, len, fp);
    }
}
