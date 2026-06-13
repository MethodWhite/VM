/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file aot/elf_emitter.h
 * @brief Emisor de archivos objeto ELF64 (ET_REL) para el compilador AOT.
 *
 * Genera un archivo objeto ELF64 relocalizable (.o) que contiene:
 *   - ELF header (e_ident, e_type=ET_REL, e_machine=EM_X86_64)
 *   - Section headers (.text, .data, .rodata, .bss, .symtab, .strtab,
 *     .shstrtab, .rela.text, .eh_frame, .comment)
 *   - Symbol table con nombres de funciones
 *   - Relocation entries (.rela.text) para llamadas externas
 *
 * = Segment layout (executable final) =
 *
 *   PT_LOAD:     .text + .rodata + .data + .bss
 *   PT_GNU_STACK: flags RWE para hilos
 *   PT_GNU_RELRO: secciones de solo lectura post-reloc
 *
 * Referencia: System V ABI - AMD64 Supplement
 */

#ifndef AOT_ELF_EMITTER_H
#define AOT_ELF_EMITTER_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace aot {

    // =========================================================================
    //  Tipos ELF64
    // =========================================================================

    /** @brief ELF ident magic + class + data + version. */
    static const uint8_t ELF_IDENT[16] = {
        0x7F, 'E', 'L', 'F',          /* magic */
        2,                             /* ELFCLASS64 */
        1,                             /* ELFDATA2LSB */
        1,                             /* EV_CURRENT */
        0, 0, 0, 0, 0, 0, 0, 0        /* padding */
    };

    /** @brief e_type: ET_REL = 1 (archivo objeto relocalizable). */
    static constexpr uint16_t ET_REL = 1;

    /** @brief e_machine: EM_X86_64 = 62. */
    static constexpr uint16_t EM_X86_64 = 62;

    /** @brief e_version: EV_CURRENT = 1. */
    static constexpr uint32_t EV_CURRENT = 1;

    // Indices de secciones especiales
    static constexpr uint16_t SHN_UNDEF     = 0;
    static constexpr uint16_t SHN_ABS       = 0xFFF1;
    static constexpr uint16_t SHN_COMMON    = 0xFFF2;

    // Tipos de seccion
    static constexpr uint32_t SHT_NULL      = 0;
    static constexpr uint32_t SHT_PROGBITS  = 1;
    static constexpr uint32_t SHT_SYMTAB    = 2;
    static constexpr uint32_t SHT_STRTAB    = 3;
    static constexpr uint32_t SHT_RELA      = 4;
    static constexpr uint32_t SHT_NOBITS    = 8;

    // Flags de seccion
    static constexpr uint64_t SHF_WRITE     = 0x1;
    static constexpr uint64_t SHF_ALLOC     = 0x2;
    static constexpr uint64_t SHF_EXECINSTR = 0x4;
    static constexpr uint64_t SHF_STRINGS   = 0x20;
    static constexpr uint64_t SHF_INFO_LINK = 0x40;

    // Tipos de simbolo
    static constexpr uint8_t STT_NOTYPE  = 0;
    static constexpr uint8_t STT_OBJECT  = 1;
    static constexpr uint8_t STT_FUNC    = 2;
    static constexpr uint8_t STT_SECTION = 3;
    static constexpr uint8_t STT_FILE    = 4;

    // Bind de simbolo
    static constexpr uint8_t STB_LOCAL  = 0;
    static constexpr uint8_t STB_GLOBAL = 1;
    static constexpr uint8_t STB_WEAK   = 2;

    // Visibilidad
    static constexpr uint8_t STV_DEFAULT   = 0;
    static constexpr uint8_t STV_HIDDEN    = 2;

    // Tipos de relocacion x86-64
    static constexpr uint32_t R_X86_64_NONE     = 0;
    static constexpr uint32_t R_X86_64_64        = 1;
    static constexpr uint32_t R_X86_64_PC32      = 2;
    static constexpr uint32_t R_X86_64_PLT32     = 4;
    static constexpr uint32_t R_X86_64_32        = 10;
    static constexpr uint32_t R_X86_64_32S       = 11;

    // Program header types
    static constexpr uint32_t PT_NULL     = 0;
    static constexpr uint32_t PT_LOAD     = 1;
    static constexpr uint32_t PT_DYNAMIC  = 2;
    static constexpr uint32_t PT_INTERP   = 3;
    static constexpr uint32_t PT_NOTE     = 4;
    static constexpr uint32_t PT_GNU_STACK = 0x6474E551;
    static constexpr uint32_t PT_GNU_RELRO = 0x6474E552;

    // Program header flags
    static constexpr uint32_t PF_X = 1;
    static constexpr uint32_t PF_W = 2;
    static constexpr uint32_t PF_R = 4;

    // =========================================================================
    //  Estructuras ELF64 en memoria (layout directo para escritura)
    // =========================================================================

    /** @brief ELF64 header (64 bytes, SHT_NULL). */
    struct Elf64_Ehdr {
        uint8_t  e_ident[16];
        uint16_t e_type;
        uint16_t e_machine;
        uint32_t e_version;
        uint64_t e_entry;
        uint64_t e_phoff;
        uint64_t e_shoff;
        uint32_t e_flags;
        uint16_t e_ehsize;
        uint16_t e_phentsize;
        uint16_t e_phnum;
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
    };

    /** @brief ELF64 section header (64 bytes). */
    struct Elf64_Shdr {
        uint32_t sh_name;
        uint32_t sh_type;
        uint64_t sh_flags;
        uint64_t sh_addr;
        uint64_t sh_offset;
        uint64_t sh_size;
        uint32_t sh_link;
        uint32_t sh_info;
        uint64_t sh_addralign;
        uint64_t sh_entsize;
    };

    /** @brief ELF64 symbol table entry (24 bytes). */
    struct Elf64_Sym {
        uint32_t st_name;
        uint8_t  st_info;
        uint8_t  st_other;
        uint16_t st_shndx;
        uint64_t st_value;
        uint64_t st_size;
    };

    /** @brief ELF64 relocation entry with addend (24 bytes). */
    struct Elf64_Rela {
        uint64_t r_offset;
        uint64_t r_info;
        int64_t  r_addend;
    };

    /** @brief ELF64 program header (56 bytes). */
    struct Elf64_Phdr {
        uint32_t p_type;
        uint32_t p_flags;
        uint64_t p_offset;
        uint64_t p_vaddr;
        uint64_t p_paddr;
        uint64_t p_filesz;
        uint64_t p_memsz;
        uint64_t p_align;
    };

    /**
     * @brief Informacion de una seccion durante la emision.
     */
    struct SectionInfo {
        std::string name;
        uint32_t    type;
        uint64_t    flags;
        uint64_t    addr;
        uint64_t    align;
        uint64_t    entsize;
        uint32_t    link;
        uint32_t    info;
        std::vector<uint8_t> data;
    };

    /**
     * @brief Informacion de un simbolo para la tabla de simbolos.
     */
    struct SymbolInfo {
        std::string name;
        uint8_t     info;    // STT_* | (STB_* << 4)
        uint8_t     other;
        uint16_t    shndx;
        uint64_t    value;
        uint64_t    size;
    };

    /**
     * @brief Informacion de una relocacion.
     */
    struct RelocInfo {
        uint64_t  offset;
        uint32_t  type;
        uint32_t  sym_index;
        int64_t   addend;
    };

    /**
     * @class ElfEmitter
     * @brief Emisor de archivos objeto ELF64.
     *
     * Construye un archivo .o relocalizable con secciones, simbolos
     * y relocaciones.  Metodo principal: @c emit() -> vector de bytes.
     */
    class ElfEmitter {
    public:
        ElfEmitter() = default;

        /**
         * @brief Anyade una seccion al archivo objeto.
         * @param sec Informacion de la seccion.
         */
        void add_section(const SectionInfo &sec);

        /**
         * @brief Anyade un simbolo a la tabla de simbolos.
         * @param sym Informacion del simbolo.
         */
        void add_symbol(const SymbolInfo &sym);

        /**
         * @brief Anyade una relocacion a la seccion .rela.text.
         * @param reloc Informacion de la relocacion.
         */
        void add_relocation(const RelocInfo &reloc);

        /**
         * @brief Fija el nombre del archivo fuente (para simbolo STT_FILE).
         * @param name Nombre del archivo fuente.
         */
        void set_source_file(const std::string &name) { source_file_ = name; }

        /**
         * @brief Genera el archivo objeto ELF64 completo.
         * @return Vector de bytes con el archivo objeto.
         */
        std::vector<uint8_t> emit();

        /**
         * @brief Genera un ejecutable ELF64 directamente (linked estaticamente).
         * @param entry Punto de entrada (_start).
         * @return Vector de bytes con el ejecutable.
         */
        std::vector<uint8_t> emit_executable(uint64_t entry);

        /**
         * @brief Resetea el emisor para un nuevo archivo.
         */
        void reset();

    private:
        std::vector<SectionInfo> sections_;
        std::vector<SymbolInfo>  symbols_;
        std::vector<RelocInfo>   relocations_;
        std::string              source_file_;

        // Caches de strings para .strtab y .shstrtab
        std::vector<char> strtab_;
        std::vector<char> shstrtab_;

        /**
         * @brief Anyade una cadena a la tabla de strings.
         * @param str   Cadena a anyadir.
         * @param table Tabla destino (.strtab o .shstrtab).
         * @return Offset de la cadena en la tabla.
         */
        uint32_t add_string(const std::string &str, std::vector<char> &table);

        /**
         * @brief Escribe el ELF header.
         */
        void write_ehdr(std::vector<uint8_t> &buf, uint64_t shoff, uint16_t shnum, uint16_t shstrndx);

        /**
         * @brief Escribe un section header.
         */
        void write_shdr(std::vector<uint8_t> &buf, const Elf64_Shdr &shdr);

        /**
         * @brief Escribe un symbol table entry.
         */
        void write_sym_entry(std::vector<uint8_t> &buf, const Elf64_Sym &sym);

        /**
         * @brief Escribe un relocation entry.
         */
        void write_rela_entry(std::vector<uint8_t> &buf, const Elf64_Rela &rela);

        /**
         * @brief Escribe un program header.
         */
        void write_phdr_entry(std::vector<uint8_t> &buf, const Elf64_Phdr &phdr);
    };

} // namespace aot

#endif // AOT_ELF_EMITTER_H
