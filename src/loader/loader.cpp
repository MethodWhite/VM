/**
 * @file loader.cpp
 * @brief Implementacion del loader de ejecutables VELB para VestaVM.
 *
 * Implementa @c loader::Loader: @c parse_velb_header(), @c parse_table_spaces(),
 * @c parser_table_sections(), @c parser_import_table(), @c parse_velb(),
 * @c load_executable() y @c create_vm_instance().
 * Valida el header, reserva memoria en el @c ArenaManager y crea el @c ProcessVM
 * con el PC inicializado segun @c init_pc del ejecutable.
 */
#include "loader/loader.h"

#include <cstring>
#include "emmit/bytereader.h"
#include "emmit/struct_context.h"
#include "runtime/manager_runtime.h"
#include "runtime/decode_table.h"   // A.9: rebase_bytecode_addresses usa decode tables
#include "runtime/decode_instruction.h" // A.9: InstrFormat para size_from_mode
#include "ffi/vesta_plugin.h"
#include "cli/sync_io.h"

/*
 *  Loader
 *  ??? load_executable(path)
 *  ?     ??? parse_velb_header()
 *  ?     ??? load_spaces()
 *  ?     ??? load_sections()
 *  ?     ??? resolve_labels()
 *  ?     ??? build_runtime_context()
 *  ??? create_vm_instance()
 */
namespace loader {
    Loader::Loader(
        runtime::ManageVM &instance_manager
    ): instance_manager(instance_manager) {}

    std::string Loader::read_string_at(const std::vector<uint8_t> &blob, uint64_t offset) {
        std::string result;

        while (offset < blob.size() && blob[offset] != 0) {
            result.push_back(static_cast<char>(blob[offset]));
            offset++;
        }

        return result;
    }


    std::vector<std::string> Loader::read_all_strings(const std::vector<uint8_t> &blob) {
        std::vector<std::string> result;
        uint64_t                 i = 0;

        while (i < blob.size()) {
            std::string s;

            while (i < blob.size() && blob[i] != 0) {
                s.push_back(static_cast<char>(blob[i]));
                i++;
            }

            result.push_back(s);

            // saltar el '\0'
            i++;
        }

        return result;
    }

    std::string Loader::read_string_at(ByteReader &reader, uint64_t offset) {
        // Guardamos el offset original
        uint64_t original = reader.offset;

        // Nos movemos al offset solicitado
        reader.seek(offset);

        // Leemos la cadena
        std::string result;
        while (!reader.eof() && reader.input[reader.offset] != 0) {
            result.push_back(static_cast<char>(reader.input[reader.offset]));
            reader.offset++;
        }

        // Restauramos el offset original
        reader.offset = original;

        return result;
    }


    void Loader::parse_velb_header(Executable &exe, ByteReader &reader) {
        if (reader.input.size() < sizeof(HeaderVELB)) {
            throw_error_at(ErrorKind::TruncatedHeader,
                           "El ejecutable es demasiado pequeno para contener un header VELB", reader);
        }

        exe.header.magic.firma = reader.read32();
        if (exe.header.magic.firma != MAGIC_NUMBER_VELB) {
            throw_error_at(ErrorKind::InvalidMagic,
                           "Magic number VELB invalido", reader);
        }

        exe.header.format_v = reader.read32();
        if (VERSION_VELB != exe.header.format_v) {
            throw_error_at(ErrorKind::InvalidVersion,
                           "Vesrion invalida de bytecode, la version actual es " +
                           std::to_string(VERSION_VELB) +
                           " pero la encontrada fue: " + std::to_string(exe.header.format_v),
                           reader
            );
        }

        // La version de la VM debe estar dentro del rango [min_v, max_v] exigido por el ejecutable.
        exe.header.max_v = reader.read32();
        exe.header.min_v = reader.read32();
        if (exe.header.max_v < VERSION_VM || exe.header.min_v > VERSION_VM) {
            throw_error_at(ErrorKind::InvalidVersion,
                           "Vesrion invalida de maquina virtual, la version actual es " +
                           std::to_string(VERSION_VM) +
                           " pero la encontrada exigida por el codigo es: " + std::to_string(exe.header.min_v) + " - "
                           + std::to_string(exe.header.max_v),
                           reader
            );
        }

        exe.header.checksum  = reader.read64();
        exe.header.flags     = reader.read64();
        exe.header.timestamp = reader.read64();
        exe.header.arch      = reader.read32();

        // cantidad de espacios de direcciones
        exe.header.count = reader.read32();

        // offset a la tabla de secciones
        exe.header.table_offset = reader.read64();

        // cantidad de espacios de direcciones que viene despues del header.
        exe.header.n_spaces = reader.read64();
        if (exe.header.n_spaces == 0) {
            throw_error_at(ErrorKind::NotFoundSpacesAddress,
                           "no se a definido la cantidad de espacios de direcciones.",
                           reader
            );
        }
        exe.header.address_spaces = new table_spaces_address[exe.header.count];

        // offset a la tabla de strings
        exe.header.offset_section_strings = reader.read64();

        // PC por el que empezar la ejecuccion
        exe.init_pc = exe.header.start_pc = reader.read64();

        // offset a la tabla de importacion
        exe.header.offset_import_table = reader.read64();

        // offset a la tabla de etiquetas o labels
        exe.header.offset_label_table = reader.read64();

        // obtener cuantas entradas tiene la tabla de importacion
        exe.header.size_import_table = reader.read32();

        // obtener cuantas labels tiene la tabla de labels
        exe.header.size_label_table = reader.read32();

        // campos de depuracion (deben leerse para mantener sincronizacion con el writer)
        exe.header.offset_debug_section = reader.read64();
        exe.header.size_debug_section   = reader.read32();
        exe.header.debug_level          = reader.read8();
        (void) reader.read8(); // _debug_pad[0]
        (void) reader.read8(); // _debug_pad[1]
        (void) reader.read8(); // _debug_pad[2]

        // A.9: campos nuevos de la tabla de relocations.  Bumped VERSION_VELB
        // a 0x2 para indicar el cambio de layout.  Si VERSION_VELB esta en
        // este file ya cumple, podemos leer estos campos sin riesgo.
        exe.header.offset_reloc_table = reader.read64();
        exe.header.size_reloc_table   = reader.read32();
        for (int i = 0; i < 12; ++i) (void)reader.read8(); // _reloc_pad[12]

        // el header siempre debe estar alineado a 16 bytes
        while (reader.offset % 16 != 0) {
            (void) reader.read8();
        }
    }

    void Loader::parse_table_spaces(Executable &exe, ByteReader &reader) {
        for (size_t i = 0; i < exe.header.n_spaces; i++) {
            if (exe.header.address_spaces == nullptr) {
                throw_error_at(ErrorKind::InvalidFormat,
                               "No se a podido encontrar los espacios de direcciones por algun motivo desconocido.",
                               reader
                );
            }

            // direccion de inicio del espacio de direcciones
            exe.header.address_spaces[i].address.address_init = reader.read64();

            // direccion final del espacio de direcciones
            exe.header.address_spaces[i].address.address_final = reader.read64();

            // offser a la seccion metada de strings
            exe.header.address_spaces[i].offset_section_strings = reader.read64();

            // offset al bytecode para el espacio de direcciones
            exe.header.address_spaces[i].offset_bytecode = reader.read64();


            Space space{};

            // indicamos el espacio de direcciones
            space.range.address_init  = exe.header.address_spaces[i].address.address_init;
            space.range.address_final = exe.header.address_spaces[i].address.address_final;

            // offset en el bytecode
            space.file_offset = exe.header.address_spaces[i].offset_bytecode;

            // obtenemos el nombre del espacio de direcciones:
            space.name_section = read_string_at(reader, exe.header.address_spaces[i].offset_section_strings);

            exe.spaces.push_back(space);
        }

        // la tabla de espacios siempre debe estar alineado a 16 bytes para que se pueda encontrar
        // la tabla de secciones.
        while (reader.offset % 16 != 0) {
            (void) reader.read8();
        }
    }

    Space *Loader::find_space_for_section(Executable &exe, const Section &sec) {
        for (auto &space: exe.spaces) {
            if (sec.memory.address_init >= space.range.address_init &&
                sec.memory.address_final <= space.range.address_final) {
                return &space;
            }
        }
        return nullptr;
    }


    void Loader::parser_table_sections(Executable &exe, ByteReader &reader) {
        // realizamos un punto de control completo para parsear la tabla de secciones.
        ByteReader reader_child = reader.subreader(
            exe.header.count * sizeof(section_range_memory)
        );

        // mover el cursor a la tabla de secciones
        reader_child.seek(exe.header.table_offset);

        for (size_t i = 0; i < exe.header.count; i++) {
            Section sec;
            sec.memory.address_init  = reader_child.read64();
            sec.memory.address_final = reader_child.read64();
            uint64_t offset_string   = reader_child.read64();
            try {
                // aqui usamos al padre, por que el offset de la tabla de strings no puede accederse
                // con el cursor hijo, ya que el cursor hijo solo puede acceder a la tabla de seeciones
                // mientras que el cursor padre puede acceder a cualquier parte del bytecode
                sec.name = read_string_at(reader, offset_string);
            } catch (const ByteReaderError &e) {
                throw_error_at(
                    ErrorKind::InvalidFormat,
                    std::string("Ha ocurrido un error de lectura al leer la tabla de strings: '") + e.what() + "'",
                    reader_child
                );
            }
            sec.size_real = sec.memory.address_final - sec.memory.address_init;

            // Buscar el espacio al que pertenece
            Space *space = find_space_for_section(exe, sec);

            if (!space) {
                throw_error_at(
                    ErrorKind::InvalidFormat,
                    "La seccion '" + sec.name + "' no pertenece a ningun espacio de direcciones",
                    reader_child
                );
            }

            // calcular file offset de la seccion
            sec.file_offset = space->file_offset + (sec.memory.address_init - space->range.address_init);

            // Insertar la seccion en el espacio correspondiente
            space->add_section(sec);
            exe.sections.push_back(&space->table_section[sec.name]);
        }

        // es muy importante saber donde empieza el bytecode real generado por el ensamblador
        // para que el loader pueda parchearlo y cargarlo de forma correcta.
        exe.offset_real_bytecode = (
            exe.header.table_offset +
            exe.sections.size() * sizeof(section_range_memory)
            // debemos calcular el final de la tabla de secciones que es
            // donde empieza el bytecode real dentro del archivo
        );
    }

    void Loader::parser_import_table(Executable &exe, ByteReader &reader) {
        /**
         * Si el offset es 0 entonces no hay tabla de importacion.
         */
        if (exe.header.offset_import_table == 0) {
            return;
        }

        // nos desplazamos hasta la tabla de importacion moviendo el cursor
        // al offset indicado
        reader.seek(exe.header.offset_import_table);

        std::vector<entry_import_table> import_table_from_file;

        // obtener la tabla de importacion del archivo:
        for (size_t i = 0; i < exe.header.size_import_table; i++) {
            entry_import_table entry;
            entry.offset_module_string    = reader.read32();
            entry.offset_function_string  = reader.read32();
            entry.offset_signature_string = reader.read32();
            entry.offset_bytecode         = reader.read32();
            import_table_from_file.push_back(entry);
        }

        for (auto &imp: import_table_from_file) {
            uint32_t module_idx   = ffi_loader.intern_module(read_string_at(reader, imp.offset_module_string));
            uint32_t function_idx = ffi_loader.intern_function(read_string_at(reader, imp.offset_function_string));

            // si el metodo no tiene firma no se usa firma
            std::string sig = "";
            //if (imp.offset_signature_string != 0) { // aun no se usa por que no se a implementado
            //    sig = read_string_at(reader, imp.offset_signature_string);
            //}
            uint32_t sig_idx = ffi_loader.intern_signature(sig);

            ffi::NativeImportKey key{module_idx, function_idx, sig_idx};

            auto &entry = ffi_loader.imports[key]; // crea si no existe

            if (entry.patch_sites.empty()) {
                entry.key.module_idx    = module_idx;
                entry.key.function_idx  = function_idx;
                entry.key.signature_idx = sig_idx;
            }

            entry.patch_sites.push_back(imp.offset_bytecode);
        }

        // resolver todas las instrucciones que usan la tabla de importacion
        // nativa.
        this->ffi_loader.resolve_all(exe.bytecode.data(), exe.offset_real_bytecode);

        // construir la tabla de callbacks de la API del plugin y notificar
        // a los modulos que exporten "vesta_init".
        this->plugin_api = {};
        this->plugin_api.api_version = VESTA_PLUGIN_API_VERSION;
        this->plugin_api.manager     = static_cast<VestaManager *>(&instance_manager);

        this->plugin_api.create_vm  = [](VestaManager *mgr, uint32_t n) -> uint64_t {
            return static_cast<runtime::ManageVM *>(mgr)->create_vm(static_cast<size_t>(n));
        };
        this->plugin_api.destroy_vm = [](VestaManager *mgr, uint64_t vm_id) -> int {
            return static_cast<runtime::ManageVM *>(mgr)->destroy_vm(vm_id) ? 1 : 0;
        };
        this->plugin_api.get_vm     = [](VestaManager *mgr, uint64_t vm_id) -> VestaVM_t * {
            return static_cast<VestaVM_t *>(
                static_cast<runtime::ManageVM *>(mgr)->get_vm(vm_id));
        };
        this->plugin_api.has_vm     = [](VestaManager *mgr, uint64_t vm_id) -> int {
            return static_cast<runtime::ManageVM *>(mgr)->has_vm(vm_id) ? 1 : 0;
        };
        this->plugin_api.vm_count   = [](VestaManager *mgr) -> uint64_t {
            return static_cast<uint64_t>(
                static_cast<runtime::ManageVM *>(mgr)->vm_count());
        };
        this->plugin_api.start_vm   = [](VestaVM_t *vm) {
            static_cast<runtime::VM *>(vm)->start();
        };
        this->plugin_api.stop_vm    = [](VestaVM_t *vm) {
            static_cast<runtime::VM *>(vm)->stop();
        };
        this->plugin_api.wait_vm    = [](VestaVM_t *vm) {
            static_cast<runtime::VM *>(vm)->wait();
        };
        this->plugin_api.spawn_process = [](VestaVM_t *vm, uint32_t *out_sched, uint64_t *out_pid) {
            GlobalPID gid = static_cast<runtime::VM *>(vm)->spawn_process();
            if (out_sched) *out_sched = gid.scheduler_id;
            if (out_pid)   *out_pid   = gid.local_pid;
        };
        this->plugin_api.make_ready = [](VestaVM_t *vm, uint32_t sched_id, uint64_t local_pid) {
            GlobalPID pid{sched_id, local_pid};
            static_cast<runtime::VM *>(vm)->make_ready(pid);
        };
        this->plugin_api.vm_read_bytes = [](uint64_t proc_ptr, uint64_t vm_addr,
                                      void *dst, uint64_t len) -> uint64_t {
            auto *proc = reinterpret_cast<runtime::ProcessVM *>(proc_ptr);
            proc->vm_mem.read_bytes(vm_addr, dst, static_cast<size_t>(len));
            return len;
        };
        this->plugin_api.vm_write_bytes = [](uint64_t proc_ptr, uint64_t vm_addr,
                                       const void *src, uint64_t len) -> uint64_t {
            auto *proc = reinterpret_cast<runtime::ProcessVM *>(proc_ptr);
            proc->vm_mem.write_bytes(vm_addr, src, static_cast<size_t>(len));
            return len;
        };
        this->plugin_api.log = [](const char *msg) {
            vesta::print_threadsafe(std::string(msg));
        };

        // GC roots externos via write-barrier.  Las colecciones nativas
        // que retienen GcHandles (e.g. ArrayList<string>) llaman a estas
        // APIs para que el GC del proceso activo no colecte los objetos
        // referenciados desde fuera de su HandleTable de bytecode.
        this->plugin_api.gc_addref = [](uint64_t proc_ptr, uint64_t gc_handle) {
            if (proc_ptr == 0 || gc_handle == 0) return;
            auto *proc = reinterpret_cast<runtime::ProcessVM *>(proc_ptr);
            proc->gc_heap.gc_addref(static_cast<gc::GcHandle>(gc_handle));
        };
        this->plugin_api.gc_release = [](uint64_t proc_ptr, uint64_t gc_handle) {
            if (proc_ptr == 0 || gc_handle == 0) return;
            auto *proc = reinterpret_cast<runtime::ProcessVM *>(proc_ptr);
            proc->gc_heap.gc_release(static_cast<gc::GcHandle>(gc_handle));
        };
        this->plugin_api.api_version = VESTA_PLUGIN_API_VERSION;

        this->ffi_loader.call_plugin_inits(&this->plugin_api);
    }

    std::unique_ptr<Executable> Loader::parse_velb(std::vector<uint8_t> bytecode) {
        auto exe      = std::make_unique<Executable>();
        exe->bytecode = bytecode;

        ByteReader reader(exe->bytecode);

        // parseamos el header
        parse_velb_header(*exe, reader);

        // parseamos la tabla de espacio de direcciones que siempre va despues del header
        parse_table_spaces(*exe, reader);

        // parseamos la tabla de secciones, requiere haber parseado previamente la tabla
        // de espacios de direcciones, ya que se va a anadir a estos.
        parser_table_sections(*exe, reader);

        // parseamos la tabla de importacion.
        parser_import_table(*exe, reader);

        // parsear la tabla de relocations al final del archivo (si existe).
        // Cada entry son 24 bytes packed: bytecode_offset (u64) + target_value
        // (u64) + type (u8) + pad (u8x7).  El header indica offset y count.
        if (exe->header.offset_reloc_table != 0
         && exe->header.size_reloc_table > 0
         && exe->header.offset_reloc_table + 24ULL * exe->header.size_reloc_table
            <= exe->bytecode.size()) {
            const size_t off = exe->header.offset_reloc_table;
            const size_t count = exe->header.size_reloc_table;
            exe->velb_relocations.reserve(count);
            ByteReader rr(exe->bytecode);
            rr.seek(off);
            for (size_t i = 0; i < count; ++i) {
                entry_relocation_table e{};
                e.bytecode_offset = rr.read64();
                e.target_value    = rr.read64();
                e.type            = rr.read8();
                for (int p = 0; p < 7; ++p) e._pad[p] = rr.read8();
                exe->velb_relocations.push_back(e);
            }
        }

        return exe;
    }

    runtime::ProcessVM *Loader::load_executable(runtime::VM &vm, std::string path) {
        // Leer archivo completo
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("No se pudo abrir el ejecutable: " + path);
        }

        std::vector<uint8_t> bytecode(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>()
        );

        // Delegar en la version bytecode
        return load_executable(vm, bytecode);
    }



    runtime::ProcessVM *Loader::load_executable(runtime::VM &vm, std::vector<uint8_t> raw_bytecode_file) {
        if (raw_bytecode_file.empty()) {
            throw std::runtime_error(
                "Loader::load_executable: Se intento cargar un ejecutable con raw_bytecode_file vacio");
        }
        auto exe = parse_velb(raw_bytecode_file);

        GlobalPID           pid      = vm.spawn_process();
        runtime::ProcessVM *proccess = vm.get_process(pid);

        // configuramos RIP (PC tambien llamado)
        proccess->registers.rip.qword(exe->init_pc);

        // Inicializar RSP/RBP del proceso main al stack base convencional
        // (mismo esquema que exec_instr_spawn para procesos hijo: 0x10000000
        // + (local_pid % 0x1000) * 0x100000).  Sin esta inicializacion el
        // primer `enter N` con RSP=0 hace `push rbp` -> escribir en VA
        // 0xFFFFFFFFFFFFFFF8 -> mapeo de pagina enorme y segfault.  La
        // pila crece hacia abajo desde stack_base.
        //
        // Sentinel HLT para entry-points con TCO: si main termina con
        // `leave; tailcall X` y la funcion final hace `leave; ret`, el ret
        // pop'eara la cima de la pila esperando una direccion de retorno
        // del caller.  Como main no tiene caller, escribimos un par de
        // bytes HLT (0x00 0x03 = extended hlt) en una VA fija, y empujamos
        // esa VA inicialmente como "direccion de retorno" de main.  El ret
        // saltara a ese HLT y la VM terminara limpiamente.  El sentinel
        // vive justo por encima del stack_base (la pila crece hacia abajo,
        // asi que el espacio por encima esta libre).
        const uint64_t stack_base_main =
            0x10000000ULL + (pid.local_pid % 0x1000ULL) * 0x100000ULL;
        const uint64_t hlt_sentinel_va = stack_base_main + 16;
        const uint8_t  hlt_bytes[2] = { 0x00, 0x03 };
        proccess->vm_mem.vm_to_host_memcpy(hlt_sentinel_va, hlt_bytes, sizeof(hlt_bytes));
        // Empujar el sentinel como retorno inicial de main.
        const uint64_t initial_rsp = stack_base_main - 8;
        proccess->vm_mem.vm_to_host_memcpy(initial_rsp, &hlt_sentinel_va, sizeof(hlt_sentinel_va));
        proccess->registers.stack_pointer.qword(initial_rsp);
        proccess->registers.base_pointer.qword(initial_rsp);
        // fix8 - el GC stack scan necesita conocer el limite superior
        // del stack (stack_high) para iterar [rsp, stack_high) buscando
        // handles vivos.  stack_low_water arranca igual a stack_high (no
        // hay slot usado todavia).  Se actualiza en subsp; reset post-GC.
        proccess->stack_high      = initial_rsp;
        proccess->stack_low_water = initial_rsp;

        // copiamos cada seccion del ejecutable a la memoria virtual
        // de la VM
        for (auto *sec: exe->sections) {
            uint64_t vm_addr = sec->memory.address_init;
            uint64_t offset  = sec->file_offset;

            // no se debe usar raw_bytecode_file ya que no contiene simbolos
            // resueltos; usar exe->bytecode que contiene los mismos datos
            // pero con las instrucciones parcheadas.
            //
            // sec->file_offset YA es absoluto al inicio del archivo (lo
            // computa parser_table_sections como
            // space->file_offset + delta_va, y space->file_offset =
            // address_spaces[i].offset_bytecode que el linker patchea con
            // base_bytecode_offset absoluto).  Por tanto NO sumar
            // exe->offset_real_bytecode (tambien absoluto), o se produce
            // double-counting + lectura OOB del vector exe->bytecode.
            // Bug introducido en a0e3098; logica correcta original en
            // commit dff2bc7 (`bytecode.data() + offset`).
            if (offset >= exe->bytecode.size()) continue;
            const uint8_t *src    = exe->bytecode.data() + offset;
            const size_t   avail  = exe->bytecode.size() - offset;
            proccess->vm_mem.vm_to_host_memcpy(vm_addr, src, avail);
        }
        // poner ejecutable a la pila de ejecutuables
        executables.push_back(std::move(exe));

        //vm->vm_mem[0x10] = 1;
        return proccess;
    }

    // ---------------------------------------------------------------------
    // rebase preciso usando la tabla de relocations.
    //
    // Para cada entry de la tabla de relocations del .velb:
    //   - bytecode_offset apunta al slot DENTRO del bytecode (relativo a
    //     bytecode start, no a archivo).
    //   - target_value es la direccion absoluta original escrita por el
    //     linker.
    //   - Si target_value esta en el rango [orig_base, orig_end) (es decir,
    //     apunta al code section del modulo), reescribimos el slot a
    //     (target_value - orig_base + new_base).
    //   - Si esta fuera (e.g. apunta a static_data en otra seccion o a
    //     codigo importado), no se patchea.
    //
    // La gran ventaja vs scan brute-force: NO false positives.  El linker
    // sabe exactamente que slots son addresses; los datos numericos
    // (frame sizes en `enter`, constantes literales en `mov reg, N`, etc.)
    // NO aparecen en la tabla de relocations.
    static void apply_relocations_for_rebase(
            std::vector<uint8_t> &data,
            size_t bytecode_base_in_file,   // exe->offset_real_bytecode
            const std::vector<entry_relocation_table> &relocs,
            uint64_t orig_base, uint64_t orig_end, uint64_t new_base) {
        if (orig_base == new_base) return;
        const int64_t delta = static_cast<int64_t>(new_base)
                            - static_cast<int64_t>(orig_base);
        for (const auto &rel : relocs) {
            if (rel.type != static_cast<uint8_t>(RelocTypeVELB::ABSOLUTE64))
                continue; // por ahora solo Absolute64
            // Solo reescribir si target original esta en el rango del code
            // section que estamos relocando.
            if (rel.target_value < orig_base || rel.target_value >= orig_end)
                continue;
            const uint64_t patched = static_cast<uint64_t>(
                static_cast<int64_t>(rel.target_value) + delta);
            const size_t file_off = bytecode_base_in_file + rel.bytecode_offset;
            if (file_off + 8 > data.size()) continue; // safety
            std::memcpy(&data[file_off], &patched, 8);
        }
    }

    // ---------------------------------------------------------------------
    // helper: parchea el PRIMER `hlt` (extended `0x00 0x03`) que
    // aparezca en el code section del modulo, reemplazandolo por `ret`
    // (`0xC3`) seguido de un byte de relleno (0x00).  Esto convierte el
    // epilogo de `main_ret` (default `leave hlt` para Vex standalone) en
    // `leave ret`, haciendo el main LLAMABLE via CALLVM desde loadmod del
    // caller.  Para plugins que no usan spawn, el primer hlt es siempre
    // el de main; plugins con spawn helpers tendrian otros hlts mas
    // adelante (no afectados, solo el PRIMERO se patchea).
    static void patch_first_hlt_to_ret(std::vector<uint8_t> &data,
                                        size_t code_offset, size_t code_size) {
        if (code_offset + code_size > data.size()) return;
        for (size_t i = code_offset; i + 1 < code_offset + code_size; ++i) {
            if (data[i] == 0x00 && data[i + 1] == 0x03) {
                data[i]     = 0xC3; // RET (primary FIXED_1)
                data[i + 1] = 0x00; // padding (no se ejecuta tras el RET)
                return;
            }
        }
    }

    // carga dinamica de un modulo (.velb) en una VM ya corriendo.
    // Si la VA original del modulo solapa con executables ya cargados,
    // reasigna a `next_dyn_base` y reescribe direcciones absolutas en el
    // bytecode (rebase automatico).  Tambien convierte el `hlt` final del
    // main del modulo en `ret` para que sea callable via CALLVM.  El caller
    // (instruccion bytecode loadmod) salta a init_pc tras push de return
    // address; el main del modulo ejecuta __module_init en su prologo,
    // registra clases en el ClassRegistry global y RETs de vuelta al caller.
    uint64_t Loader::load_module_dynamic(runtime::VM &vm,
                                          std::vector<uint8_t> raw_bytecode_file) {
        if (raw_bytecode_file.empty()) {
            return 0; // archivo vacio -> failure
        }
        auto exe = parse_velb(std::move(raw_bytecode_file));
        if (!exe) return 0;

        // ---- Detectar solapamiento de VA y reasignar si es necesario ----
        // Encontrar el code section.  Conviccion del emisor Vex: la primera
        // seccion en el .velb es siempre "code".  El emisor (annotations.cpp)
        // deja sec->memory.address_init/final = 0 -> sec->size_real = 0, asi
        // que NO podemos descartar secciones vacias; tomamos directamente la
        // primera seccion (la convencion).
        Section *code_sec = nullptr;
        for (auto *sec : exe->sections) {
            if (sec && sec->name == "code") { code_sec = sec; break; }
        }
        if (!code_sec) {
            // fallback: primera seccion existente
            for (auto *sec : exe->sections) { if (sec) { code_sec = sec; break; } }
        }
        // Si no hay code section, no hay nada que cargar; devolver el init_pc
        // tal cual (puede ser 0).
        uint64_t init_pc = exe->init_pc;
        if (code_sec) {
            const uint64_t orig_base = code_sec->memory.address_init;
            // Como sec->size_real esta a 0 con el formato actual, derivar
            // code_size del tamano efectivo del bytecode en el archivo.
            // Tomamos desde el inicio de la seccion (file_offset) hasta el
            // final del area de bytecode, que es:
            //   - el inicio de la tabla de relocations si existe
            //   - o el inicio de la tabla de imports si existe
            //   - o el final del archivo
            uint64_t bytecode_end = exe->bytecode.size();
            if (exe->header.offset_reloc_table != 0
             && exe->header.offset_reloc_table < bytecode_end) {
                bytecode_end = exe->header.offset_reloc_table;
            }
            if (exe->header.offset_import_table != 0
             && exe->header.size_import_table > 0
             && exe->header.offset_import_table < bytecode_end) {
                bytecode_end = exe->header.offset_import_table;
            }
            const uint64_t code_size = (code_sec->file_offset < bytecode_end)
                                     ? (bytecode_end - code_sec->file_offset)
                                     : 0;

            // Comprobar overlap contra todos los executables ya cargados.
            // size_real esta a 0 con el formato actual: derivar el tamano
            // efectivo de cada seccion existente con el mismo razonamiento
            // (file_offset hasta el primer end-of-bytecode marker).
            bool conflict = false;
            for (const auto &existing : executables) {
                if (!existing) continue;
                // calcular bytecode_end del executable existente
                uint64_t e_bc_end = existing->bytecode.size();
                if (existing->header.offset_reloc_table != 0
                 && existing->header.offset_reloc_table < e_bc_end) {
                    e_bc_end = existing->header.offset_reloc_table;
                }
                if (existing->header.offset_import_table != 0
                 && existing->header.size_import_table > 0
                 && existing->header.offset_import_table < e_bc_end) {
                    e_bc_end = existing->header.offset_import_table;
                }
                for (const auto *esec : existing->sections) {
                    if (!esec) continue;
                    const uint64_t e_start = esec->memory.address_init;
                    const uint64_t e_size_eff = esec->size_real
                        ? esec->size_real
                        : (esec->file_offset < e_bc_end
                           ? (e_bc_end - esec->file_offset) : 0);
                    if (e_size_eff == 0) continue;
                    const uint64_t e_end = e_start + e_size_eff;
                    if (orig_base < e_end && (orig_base + code_size) > e_start) {
                        conflict = true; break;
                    }
                }
                if (conflict) break;
            }

            if (conflict) {
                // solapamiento detectado.  Si el .velb tiene tabla de
                // relocations (formato VERSION_VELB >= 2), podemos hacer
                // rebase preciso a una VA libre sin necesitar flags.  Si
                // no hay tabla, reportamos error.
                if (exe->velb_relocations.empty()) {
                    std::fprintf(stderr,
                        "[loadmodule] error: la VA del modulo (0x%llX..0x%llX) "
                        "solapa con codigo ya cargado y el .velb no contiene "
                        "tabla de relocations (compilado con linker viejo).  "
                        "Recompila para que el rebase transparente funcione.\n",
                        (unsigned long long)orig_base,
                        (unsigned long long)(orig_base + code_size));
                    std::fflush(stderr);
                    return 0;
                }
                // Asignar nueva VA alineada a 4 KiB.
                const uint64_t aligned_size =
                    (code_size + 0xFFFULL) & ~0xFFFULL;
                const uint64_t new_base = next_dyn_base;
                next_dyn_base += aligned_size;
                // Rebase preciso: aplicar las relocations a la nueva VA.
                apply_relocations_for_rebase(
                    exe->bytecode,
                    exe->offset_real_bytecode,   // base de bytecode dentro del .velb
                    exe->velb_relocations,
                    orig_base, orig_base + code_size, new_base);

                // Patch init_pc al nuevo base.
                init_pc = init_pc - orig_base + new_base;

                // Reasignar la VA de TODAS las secciones cuyo rango
                // [address_init, address_init + size_eff) cae dentro del
                // bloque rebaseado [orig_base, orig_base + code_size).
                // Sin esto las secciones secundarias (e.g. "strings"
                // sintetica del MetaSpace, mapeada a VA 0) seguirian
                // copiandose a VA 0 y machacarian el code section del
                // caller en su mismo VA.
                for (auto *sec : exe->sections) {
                    if (!sec) continue;
                    const uint64_t s_va_orig = sec->memory.address_init;
                    if (s_va_orig >= orig_base && s_va_orig < orig_base + code_size) {
                        const uint64_t delta = new_base - orig_base;
                        sec->memory.address_init  = s_va_orig + delta;
                        sec->memory.address_final = sec->memory.address_final + delta;
                    } else if (s_va_orig == 0 && sec != code_sec) {
                        // Caso comun: secciones MetaSpace o auxiliares con
                        // address_init=0 (no fueron asignadas al rango
                        // original del code).  Las re-mapeamos al inicio
                        // del nuevo bloque para evitar colision con VA 0
                        // del caller.  Como su size_real==0 y solo se
                        // copian si tienen contenido, esto es seguro: si
                        // estan vacias no destruyen nada, y si tienen
                        // datos los datos van a una VA libre.
                        sec->memory.address_init  = new_base;
                        sec->memory.address_final = new_base;
                    }
                }
            }

            // Convertir el primer `hlt` del code section en `ret` para que
            // el main del modulo sea callable via CALLVM (tras esto el modulo
            // ya no funciona standalone -- aceptable porque es un plugin).
            // code_sec->file_offset YA es absoluto en el archivo.
            patch_first_hlt_to_ret(exe->bytecode,
                code_sec->file_offset,
                static_cast<size_t>(code_size));
        }

        // Replicar la copia de secciones a TODOS los procesos vivos de la
        // VM (cada ProcessVM tiene vm_mem privado).  Iteramos schedulers y
        // procesos.  Procesos creados DESPUES heredan el codigo via
        // copy_executables_to invocado en exec_instr_spawn.
        //
        // CRITICO: a diferencia de load_executable (que carga en
        // un proceso fresco con vm_mem vacio), aqui el proceso destino YA
        // tiene codigo del caller cargado.  No podemos copiar
        // exe->bytecode.size() bytes desde cada sec->memory.address_init
        // porque secciones vacias (e.g. 'strings' con VA 0..0) destruirian
        // el codigo del caller en VA 0.  Usamos sec->size_real (el tamano
        // real de la seccion) en su lugar.  Para secciones con size_real=0
        // simplemente no copiamos nada.
        for (auto &sched : vm.schedulers) {
            for (auto &proc : sched->processes) {
                if (!proc) continue;
                for (const auto *sec : exe->sections) {
                    if (!sec) continue;
                    const uint64_t vm_addr = sec->memory.address_init;
                    const uint64_t offset  = sec->file_offset;
                    if (offset >= exe->bytecode.size()) continue;
                    const uint8_t *src    = exe->bytecode.data() + offset;
                    const size_t   avail  = exe->bytecode.size() - offset;
                    // Si la seccion declara un tamano (size_real > 0), respetarlo
                    // para no destruir codigo del caller en VAs colindantes.
                    // Si size_real == 0 (formato actual sin rangos populados),
                    // copiar todo lo restante del archivo hasta el final.
                    const size_t sec_size = sec->size_real
                                          ? std::min<size_t>(sec->size_real, avail)
                                          : avail;
                    proc->vm_mem.vm_to_host_memcpy(vm_addr, src, sec_size);
                }
            }
        }

        // Anadir al pool de executables; copy_executables_to se encarga de
        // los procesos futuros (e.g. spawn).
        executables.push_back(std::move(exe));
        return init_pc;
    }

    void Loader::copy_executables_to(runtime::ProcessVM &dest) {
        // Replica la copia que hace load_executable() pero apuntando al
        // vm_mem del proceso destino.  Itera todos los executables cargados
        // (puede ser >1 si se usa loadmodule) y todas sus secciones,
        // copiando el bytecode al rango de VAs original.
        for (const auto &exe_ptr : executables) {
            const Executable *exe = exe_ptr.get();
            if (!exe) continue;
            for (const auto *sec : exe->sections) {
                if (!sec) continue;
                const uint64_t vm_addr = sec->memory.address_init;
                const uint64_t offset  = sec->file_offset;
                if (offset >= exe->bytecode.size()) continue;
                const uint8_t *src    = exe->bytecode.data() + offset;
                const size_t   avail  = exe->bytecode.size() - offset;
                const size_t sec_size = sec->size_real
                                      ? std::min<size_t>(sec->size_real, avail)
                                      : avail;
                dest.vm_mem.vm_to_host_memcpy(vm_addr, src, sec_size);
            }
        }
    }

    void Loader::resolve_labels(Assembly::Bytecode::Section &section) {}

    void Loader::load_sections(Assembly::Bytecode::Label &label) {}

    void Loader::load_spaces(Assembly::Bytecode::Space &space) {}

    void Loader::build_runtime_context() {}

    Executable &Loader::get_last_instance_unlocked() {
        return *executables.back();
    }

    Executable &Loader::get_last_instance() {
        std::lock_guard lock(loader_mutex);
        return get_last_instance_unlocked();
    }


    runtime::VM *Loader::create_vm_instance(size_t num_schedulers) {
        // bloqueamos el acceso si otro hilo intenta entrar
        std::lock_guard lock(loader_mutex);

        // cramos una instancia VM y configuramos el PC
        uint64_t     id = instance_manager.create_vm(num_schedulers);
        runtime::VM *vm = instance_manager.get_vm(id);

        return vm;
    }

    /**
     * @brief Resuelve los campos de un FieldInfo[] clonado sustituyendo parametros de tipo.
     *
     * Para cada FieldInfo con is_type_param == true, busca el tipo concreto en
     * params[type_param_idx].concrete y lo asigna a type_class, marcando el campo
     * como resuelto (is_type_param = false).
     *
     * @param fields  Array de FieldInfo clonado; modificado en sitio.
     * @param count   Numero de entradas en fields.
     * @param params  Array de GenericParam con los tipos concretos ya asignados.
     * @param np      Numero de parametros de tipo.
     */
    static void resolve_generic_fields(FieldInfo *fields, size_t count,
                                       const GenericParam *params, size_t np) {
        for (size_t i = 0; i < count; i++) {
            if (!fields[i].is_type_param) continue; // tipo ya concreto, no tocar
            uint16_t idx = fields[i].type_param_idx;
            if (static_cast<size_t>(idx) < np && params[idx].concrete != nullptr) {
                fields[i].type_class    = params[idx].concrete; // sustituir tipo
                fields[i].is_type_param = false;                // marcar resuelto
            }
        }
    }

    /**
     * @brief Instancia una clase generica con tipos concretos (monomorphization en runtime).
     *
     * Construye la clave "ClassName<T1,T2,...>" y busca en generic_cache_.  Si no
     * existe, clona el ClassInfo original y realiza resolucion completa de tipos:
     *
     *   1. GenericParam[].concrete = tipo concreto (constraint se preserva para bounds).
     *   2. FieldInfo[] de campos de instancia: type_class resuelto via type_param_idx.
     *   3. MethodInfo[].args y .return_type: misma resolucion para cada metodo.
     *
     * Toda la memoria auxiliar se almacena en generic_store_* para duracion de vida
     * automatica: se libera al destruir el Loader.
     *
     * Thread-safe: protegido por loader_mutex.
     *
     * @param generic    ClassInfo de la clase generica (CLASS_FLAG_GENERIC).
     * @param type_args  Array de ClassInfo* con los tipos concretos.
     * @param count      Numero de elementos en type_args.
     * @return Puntero al ClassInfo especializado (valido de por vida del Loader).
     */
    ClassInfo *Loader::specialize_class(ClassInfo *generic,
                                        ClassInfo **type_args,
                                        size_t count) {
        if (generic == nullptr || type_args == nullptr || count == 0) return generic;

        // --- construir clave de cache: "ClassName<T1,T2,...>" ---
        std::string key(reinterpret_cast<const char *>(generic->name.data),
                        generic->name.size);
        key += '<';
        for (size_t i = 0; i < count; i++) {
            if (i > 0) key += ',';
            if (type_args[i] != nullptr) {
                key.append(reinterpret_cast<const char *>(type_args[i]->name.data),
                           type_args[i]->name.size);
            } else {
                key += '?'; // tipo desconocido: clave degenerada
            }
        }
        key += '>';

        std::lock_guard<std::mutex> lock(loader_mutex);

        // camino rapido: especializacion ya cacheada
        auto it = generic_cache_.find(key);
        if (it != generic_cache_.end()) return it->second;

        // --- clonar ClassInfo base ---
        auto clone = std::make_unique<ClassInfo>(*generic);

        // guardar nombre especializado con gestion de vida automatica
        auto name_buf = std::make_unique<char[]>(key.size() + 1);
        std::memcpy(name_buf.get(), key.c_str(), key.size() + 1);
        clone->name.data = reinterpret_cast<uint8_t *>(name_buf.get());
        clone->name.size = static_cast<uint32_t>(key.size());
        generic_store_names_.push_back(std::move(name_buf));

        // vincular plantilla original (para instanceof, reflexion y diagnosticos)
        clone->generic_parent = generic;

        // limpiar CLASS_FLAG_GENERIC: la especializacion es clase concreta
        clone->flags &= ~CLASS_FLAG_GENERIC;

        // --- clonar y rellenar GenericParam[].concrete ---
        // constraint se preserva intacto para validar bounds en tiempo de carga
        GenericParam *new_params = nullptr;
        size_t np = generic->type_param_count;
        if (np > 0 && generic->type_params != nullptr) {
            auto pbuf = std::make_unique<GenericParam[]>(np);
            std::memcpy(pbuf.get(), generic->type_params, np * sizeof(GenericParam));
            for (size_t i = 0; i < count && i < np; i++) {
                pbuf[i].concrete = type_args[i]; // asignar concreto; constraint intacto
            }
            new_params = pbuf.get();
            clone->type_params = new_params;
            generic_store_params_.push_back(std::move(pbuf));
        }

        // --- resolver FieldInfo[] de campos de instancia ---
        if (generic->fields != nullptr && generic->field_count > 0) {
            size_t fc = generic->field_count;
            auto fbuf = std::make_unique<FieldInfo[]>(fc);
            std::memcpy(fbuf.get(), generic->fields, fc * sizeof(FieldInfo));
            if (new_params != nullptr) {
                resolve_generic_fields(fbuf.get(), fc, new_params, np);
            }
            clone->fields = fbuf.get();
            generic_store_fields_.push_back(std::move(fbuf));
        }

        // --- resolver MethodInfo[]: argumentos y tipo de retorno ---
        if (generic->methods != nullptr && generic->method_count > 0) {
            size_t mc = generic->method_count;
            auto mbuf = std::make_unique<MethodInfo[]>(mc);
            std::memcpy(mbuf.get(), generic->methods, mc * sizeof(MethodInfo));

            for (size_t mi = 0; mi < mc; mi++) {
                // resolver argumentos del metodo
                if (new_params != nullptr &&
                    mbuf[mi].args != nullptr && mbuf[mi].arg_count > 0) {
                    size_t ac = mbuf[mi].arg_count;
                    auto abuf = std::make_unique<FieldInfo[]>(ac);
                    std::memcpy(abuf.get(), mbuf[mi].args, ac * sizeof(FieldInfo));
                    resolve_generic_fields(abuf.get(), ac, new_params, np);
                    mbuf[mi].args = abuf.get();
                    generic_store_fields_.push_back(std::move(abuf));
                }
                // resolver tipo de retorno si es un parametro de tipo
                if (new_params != nullptr &&
                    mbuf[mi].return_type != nullptr &&
                    mbuf[mi].return_type->is_type_param) {
                    auto rbuf = std::make_unique<FieldInfo[]>(1);
                    std::memcpy(rbuf.get(), mbuf[mi].return_type, sizeof(FieldInfo));
                    resolve_generic_fields(rbuf.get(), 1, new_params, np);
                    mbuf[mi].return_type = rbuf.get();
                    generic_store_fields_.push_back(std::move(rbuf));
                }
            }
            clone->methods = mbuf.get();
            generic_store_methods_.push_back(std::move(mbuf));
        }

        ClassInfo *raw = clone.get();
        generic_store_.push_back(std::move(clone)); // transferir propiedad
        generic_cache_.emplace(std::move(key), raw);
        return raw;
    }

}
