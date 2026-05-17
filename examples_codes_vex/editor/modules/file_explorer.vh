// =============================================================================
// modules/file_explorer.vh - listado de directorio via FFI a kernel32
// =============================================================================
//
// Wrapper minimo sobre Win32 FindFirstFileA / FindNextFileA / FindClose.
// Llena un linked list de DirEntry con los nombres de los entries del
// directorio.  Usado por Editor.cmd_open_file (Ctrl+O).
//
// La estructura WIN32_FIND_DATAA tiene 320 bytes; nos importa solo
// dwFileAttributes (offset 0, 4 bytes) y cFileName (offset 44, 260 bytes).
// FILE_ATTRIBUTE_DIRECTORY = 0x10.

class DirEntry {
    public string   name;
    public i32      is_dir;
    public DirEntry next;

    public DirEntry() {
        this.name   = "";
        this.is_dir = 0;
        this.next   = null;
    }
}

class FileExplorer {
    public i64 lib_kernel32;
    public i64 sym_findfirst;
    public i64 sym_findnext;
    public i64 sym_findclose;

    public FileExplorer() {
        this.lib_kernel32  = ffi_open("kernel32.dll");
        this.sym_findfirst = ffi_sym(this.lib_kernel32, "FindFirstFileA");
        this.sym_findnext  = ffi_sym(this.lib_kernel32, "FindNextFileA");
        this.sym_findclose = ffi_sym(this.lib_kernel32, "FindClose");
    }

    // list_current_dir: devuelve un linked list con los entries de "*"
    // (todo el directorio actual).  Filtramos "." y "..".
    public DirEntry list_current_dir() {
        u8* fd = malloc(320);
        i64 h = ffi_call(this.sym_findfirst, str_cstr("*"), fd);
        if (h == -1) {
            free(fd);
            return null;
        }
        DirEntry head = null;
        DirEntry tail = null;
        i32 done = 0;
        while (done == 0) {
            i32 attrs = fd[0] | (fd[1] << 8) | (fd[2] << 16) | (fd[3] << 24);
            i32 nlen = 0;
            while (nlen < 260 && fd[44 + nlen] != 0) {
                nlen = nlen + 1;
            }
            i32 skip = 0;
            if (nlen == 1 && fd[44] == 46) { skip = 1; }
            if (nlen == 2 && fd[44] == 46 && fd[45] == 46) { skip = 1; }
            if (skip == 0 && nlen > 0) {
                DirEntry e = new DirEntry();
                string nm = str_make(fd + 44, nlen);
                e.name   = nm;
                e.is_dir = 0;
                if ((attrs & 16) != 0) { e.is_dir = 1; }
                if (head == null) { head = e; tail = e; }
                else              { tail.next = e; tail = e; }
            }
            i64 rc = ffi_call(this.sym_findnext, h, fd);
            // BOOL en Win32 es int de 32 bits; ffi_call devuelve i64 con
            // posible basura en bytes altos.  Mascarar a 32 bits.
            if ((rc & 0xFFFFFFFF) == 0) { done = 1; }
        }
        ffi_call(this.sym_findclose, h);
        free(fd);
        return head;
    }
}
