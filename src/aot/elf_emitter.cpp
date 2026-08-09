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
 * @file aot/elf_emitter.cpp
 * @brief Implementacion del emisor de archivos objeto ELF64.
 *
 * Genera un archivo .o ET_REL con secciones .text, .data, .rodata, .bss,
 * .symtab, .strtab, .shstrtab, .rela.text, .eh_frame y .comment.
 *
 * Tambien soporta emision directa de ejecutable ELF64 con program headers
 * (PT_LOAD, PT_GNU_STACK, PT_GNU_RELRO).
 */

#include "aot/elf_emitter.h"
#include <cstring>
#include <algorithm>

namespace aot {

    // =========================================================================
    //  Helpers de escritura little-endian
    // =========================================================================

    static void write_u8(std::vector<uint8_t> &buf, uint8_t v) {
        buf.push_back(v);
    }

    static void write_u16(std::vector<uint8_t> &buf, uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
    }

    static void write_u32(std::vector<uint8_t> &buf, uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v >> 16));
        buf.push_back(static_cast<uint8_t>(v >> 24));
    }

    static void write_u64(std::vector<uint8_t> &buf, uint64_t v) {
        for (int i = 0; i < 8; ++i)
            buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }

    static void write_i64(std::vector<uint8_t> &buf, int64_t v) {
        write_u64(buf, static_cast<uint64_t>(v));
    }

    static void write_at_u16(std::vector<uint8_t> &buf, size_t offset, uint16_t v) {
        if (offset + 2 > buf.size()) buf.resize(offset + 2);
        buf[offset]     = static_cast<uint8_t>(v);
        buf[offset + 1] = static_cast<uint8_t>(v >> 8);
    }

    static void write_at_u64(std::vector<uint8_t> &buf, size_t offset, uint64_t v) {
        if (offset + 8 > buf.size()) buf.resize(offset + 8);
        for (int i = 0; i < 8; ++i)
            buf[offset + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    }

    // =========================================================================
    //  Gestion de strings
    // =========================================================================

    uint32_t ElfEmitter::add_string(const std::string &str, std::vector<char> &table) {
        if (table.empty()) {
            table.push_back('\0');
        }
        uint32_t offset = static_cast<uint32_t>(table.size());
        for (char c : str) {
            table.push_back(c);
        }
        table.push_back('\0');
        return offset;
    }

    // =========================================================================
    //  API publica
    // =========================================================================

    void ElfEmitter::add_section(const SectionInfo &sec) {
        sections_.push_back(sec);
    }

    void ElfEmitter::add_symbol(const SymbolInfo &sym) {
        symbols_.push_back(sym);
    }

    void ElfEmitter::add_relocation(const RelocInfo &reloc) {
        relocations_.push_back(reloc);
    }

    void ElfEmitter::reset() {
        sections_.clear();
        symbols_.clear();
        relocations_.clear();
        strtab_.clear();
        shstrtab_.clear();
        source_file_.clear();
    }

    // =========================================================================
    //  Escritura de structs ELF
    // =========================================================================

    void ElfEmitter::write_ehdr(std::vector<uint8_t> &buf,
                                uint64_t shoff, uint16_t shnum, uint16_t shstrndx) {
        for (int i = 0; i < 16; ++i) buf.push_back(ELF_IDENT[i]);
        write_u16(buf, ET_REL);
        write_u16(buf, EM_X86_64);
        write_u32(buf, EV_CURRENT);
        write_u64(buf, 0);          // e_entry
        write_u64(buf, 0);          // e_phoff
        write_u64(buf, shoff);      // e_shoff
        write_u32(buf, 0);          // e_flags
        write_u16(buf, sizeof(Elf64_Ehdr));   // e_ehsize
        write_u16(buf, 0);          // e_phentsize
        write_u16(buf, 0);          // e_phnum
        write_u16(buf, sizeof(Elf64_Shdr));   // e_shentsize
        write_u16(buf, shnum);      // e_shnum
        write_u16(buf, shstrndx);   // e_shstrndx
    }

    void ElfEmitter::write_shdr(std::vector<uint8_t> &buf, const Elf64_Shdr &shdr) {
        write_u32(buf, shdr.sh_name);
        write_u32(buf, shdr.sh_type);
        write_u64(buf, shdr.sh_flags);
        write_u64(buf, shdr.sh_addr);
        write_u64(buf, shdr.sh_offset);
        write_u64(buf, shdr.sh_size);
        write_u32(buf, shdr.sh_link);
        write_u32(buf, shdr.sh_info);
        write_u64(buf, shdr.sh_addralign);
        write_u64(buf, shdr.sh_entsize);
    }

    void ElfEmitter::write_sym_entry(std::vector<uint8_t> &buf, const Elf64_Sym &sym) {
        write_u32(buf, sym.st_name);
        write_u8(buf, sym.st_info);
        write_u8(buf, sym.st_other);
        write_u16(buf, sym.st_shndx);
        write_u64(buf, sym.st_value);
        write_u64(buf, sym.st_size);
    }

    void ElfEmitter::write_rela_entry(std::vector<uint8_t> &buf, const Elf64_Rela &rela) {
        write_u64(buf, rela.r_offset);
        write_u64(buf, rela.r_info);
        write_i64(buf, rela.r_addend);
    }

    void ElfEmitter::write_phdr_entry(std::vector<uint8_t> &buf, const Elf64_Phdr &phdr) {
        write_u32(buf, phdr.p_type);
        write_u32(buf, phdr.p_flags);
        write_u64(buf, phdr.p_offset);
        write_u64(buf, phdr.p_vaddr);
        write_u64(buf, phdr.p_paddr);
        write_u64(buf, phdr.p_filesz);
        write_u64(buf, phdr.p_memsz);
        write_u64(buf, phdr.p_align);
    }

    // =========================================================================
    //  Emision de archivo objeto (ET_REL)
    // =========================================================================

    std::vector<uint8_t> ElfEmitter::emit() {
        std::vector<uint8_t> buf;
        buf.reserve(4096);

        // Construir .shstrtab
        shstrtab_.clear();
        shstrtab_.push_back('\0');

        struct SectionEntry {
            std::string name;
            Elf64_Shdr  shdr;
        };
        std::vector<SectionEntry> entries;

        // SHT_NULL
        entries.push_back({"", Elf64_Shdr{}});

        for (const auto &sec : sections_) {
            SectionEntry e;
            e.name = sec.name;
            std::memset(&e.shdr, 0, sizeof(e.shdr));
            e.shdr.sh_name      = add_string(sec.name, shstrtab_);
            e.shdr.sh_type      = sec.type;
            e.shdr.sh_flags     = sec.flags;
            e.shdr.sh_addr      = sec.addr;
            e.shdr.sh_size      = sec.data.size();
            e.shdr.sh_link      = sec.link;
            e.shdr.sh_info      = sec.info;
            e.shdr.sh_addralign = sec.align;
            e.shdr.sh_entsize   = sec.entsize;
            entries.push_back(e);
        }

        // Nombres de secciones sinteticas
        uint32_t symtab_name_off   = add_string(".symtab", shstrtab_);
        uint32_t strtab_name_off   = add_string(".strtab", shstrtab_);
        uint32_t shstrtab_name_off = add_string(".shstrtab", shstrtab_);
        uint32_t rela_name_off     = add_string(".rela.text", shstrtab_);
        uint32_t eh_frame_name_off = add_string(".eh_frame", shstrtab_);
        uint32_t comment_name_off  = add_string(".comment", shstrtab_);

        // Construir .strtab
        strtab_.clear();
        strtab_.push_back('\0');
        std::vector<uint32_t> sym_name_offs;
        sym_name_offs.push_back(0); // STN_UNDEF
        for (const auto &sym : symbols_) {
            sym_name_offs.push_back(add_string(sym.name, strtab_));
        }

        // Escribir datos de seccion (solo las secciones reales, no las
        // sinteticas .symtab/.strtab/etc. que se escriben despues).
        for (size_t i = 1; i <= sections_.size(); ++i) {
            entries[i].shdr.sh_offset = buf.size();
            if (entries[i].shdr.sh_type != SHT_NOBITS) {
                buf.insert(buf.end(), sections_[i - 1].data.begin(),
                           sections_[i - 1].data.end());
            }
        }

        // .symtab
        size_t symtab_offset = buf.size();
        {
            Elf64_Sym undef = {};
            undef.st_info  = (STB_LOCAL << 4) | STT_NOTYPE;
            undef.st_other = STV_DEFAULT;
            undef.st_shndx = SHN_UNDEF;
            write_sym_entry(buf, undef);
        }
        for (size_t i = 0; i < symbols_.size(); ++i) {
            Elf64_Sym sym;
            sym.st_name  = sym_name_offs[i + 1];
            sym.st_info  = symbols_[i].info;
            sym.st_other = symbols_[i].other;
            sym.st_shndx = symbols_[i].shndx;
            sym.st_value = symbols_[i].value;
            sym.st_size  = symbols_[i].size;
            write_sym_entry(buf, sym);
        }
        size_t symtab_size = buf.size() - symtab_offset;

        // .strtab
        size_t strtab_offset = buf.size();
        buf.insert(buf.end(), strtab_.begin(), strtab_.end());
        size_t strtab_size = buf.size() - strtab_offset;

        // .shstrtab
        size_t shstrtab_offset = buf.size();
        buf.insert(buf.end(), shstrtab_.begin(), shstrtab_.end());
        size_t shstrtab_size = buf.size() - shstrtab_offset;

        // .rela.text
        size_t rela_offset = 0, rela_size = 0;
        if (!relocations_.empty()) {
            rela_offset = buf.size();
            for (const auto &r : relocations_) {
                Elf64_Rela rela;
                rela.r_offset = r.offset;
                rela.r_info   = (static_cast<uint64_t>(r.sym_index) << 32) | r.type;
                rela.r_addend = r.addend;
                write_rela_entry(buf, rela);
            }
            rela_size = buf.size() - rela_offset;
        }

        // .eh_frame (vacio)
        size_t eh_frame_offset = buf.size();
        size_t eh_frame_size   = 0;

        // .comment
        size_t comment_offset = buf.size();
        const char *comment_str = "Vex Bare AOT v1.0 - VestaVM";
        size_t comment_len = std::strlen(comment_str) + 1;
        buf.insert(buf.end(), comment_str, comment_str + comment_len);
        size_t comment_size = buf.size() - comment_offset;

        // Padding para section headers
        while (buf.size() % 8 != 0) buf.push_back(0);

        // Calcular indices
        uint16_t sec_count = static_cast<uint16_t>(1 + sections_.size() + 1 + 1 + 1 + (relocations_.empty() ? 0 : 1) + 1 + 1);
        uint16_t shstrndx  = static_cast<uint16_t>(1 + sections_.size() + 1 + 1);

        // Section headers
        size_t shoff = buf.size();

        // SHT_NULL
        Elf64_Shdr null_shdr = {};
        write_shdr(buf, null_shdr);

        // Secciones de datos
        for (size_t i = 1; i < entries.size(); ++i) {
            write_shdr(buf, entries[i].shdr);
        }

        // .symtab
        {
            Elf64_Shdr shdr = {};
            shdr.sh_name      = symtab_name_off;
            shdr.sh_type      = SHT_SYMTAB;
            shdr.sh_flags     = 0;
            shdr.sh_addr      = 0;
            shdr.sh_offset    = symtab_offset;
            shdr.sh_size      = symtab_size;
            shdr.sh_link      = static_cast<uint32_t>(1 + sections_.size() + 1);
            shdr.sh_info      = 1;
            shdr.sh_addralign = 8;
            shdr.sh_entsize   = sizeof(Elf64_Sym);
            write_shdr(buf, shdr);
        }

        // .strtab
        {
            Elf64_Shdr shdr = {};
            shdr.sh_name      = strtab_name_off;
            shdr.sh_type      = SHT_STRTAB;
            shdr.sh_flags     = SHF_STRINGS;
            shdr.sh_addr      = 0;
            shdr.sh_offset    = strtab_offset;
            shdr.sh_size      = strtab_size;
            shdr.sh_link      = 0;
            shdr.sh_info      = 0;
            shdr.sh_addralign = 1;
            shdr.sh_entsize   = 0;
            write_shdr(buf, shdr);
        }

        // .shstrtab
        {
            Elf64_Shdr shdr = {};
            shdr.sh_name      = shstrtab_name_off;
            shdr.sh_type      = SHT_STRTAB;
            shdr.sh_flags     = SHF_STRINGS;
            shdr.sh_addr      = 0;
            shdr.sh_offset    = shstrtab_offset;
            shdr.sh_size      = shstrtab_size;
            shdr.sh_link      = 0;
            shdr.sh_info      = 0;
            shdr.sh_addralign = 1;
            shdr.sh_entsize   = 0;
            write_shdr(buf, shdr);
        }

        // .rela.text
        if (!relocations_.empty()) {
            Elf64_Shdr shdr = {};
            shdr.sh_name      = rela_name_off;
            shdr.sh_type      = SHT_RELA;
            shdr.sh_flags     = SHF_INFO_LINK;
            shdr.sh_addr      = 0;
            shdr.sh_offset    = rela_offset;
            shdr.sh_size      = rela_size;
            shdr.sh_link      = static_cast<uint32_t>(1 + sections_.size());
            shdr.sh_info      = 1;
            shdr.sh_addralign = 8;
            shdr.sh_entsize   = sizeof(Elf64_Rela);
            write_shdr(buf, shdr);
        }

        // .eh_frame
        {
            Elf64_Shdr shdr = {};
            shdr.sh_name      = eh_frame_name_off;
            shdr.sh_type      = SHT_PROGBITS;
            shdr.sh_flags     = SHF_ALLOC;
            shdr.sh_addr      = 0;
            shdr.sh_offset    = eh_frame_offset;
            shdr.sh_size      = eh_frame_size;
            shdr.sh_link      = 0;
            shdr.sh_info      = 0;
            shdr.sh_addralign = 8;
            shdr.sh_entsize   = 0;
            write_shdr(buf, shdr);
        }

        // .comment
        {
            Elf64_Shdr shdr = {};
            shdr.sh_name      = comment_name_off;
            shdr.sh_type      = SHT_PROGBITS;
            shdr.sh_flags     = SHF_STRINGS;
            shdr.sh_addr      = 0;
            shdr.sh_offset    = comment_offset;
            shdr.sh_size      = comment_size;
            shdr.sh_link      = 0;
            shdr.sh_info      = 0;
            shdr.sh_addralign = 1;
            shdr.sh_entsize   = 0;
            write_shdr(buf, shdr);
        }

        // Construir ELF header al inicio
        std::vector<uint8_t> header;
        /* e_shoff se calculo sobre el buffer SIN el header.  Al insertar el
         * header (sizeof(Elf64_Ehdr) bytes) al inicio, todos los offsets del
         * archivo se desplazan hacia delante: corregir e_shoff por +ehsize. */
        write_ehdr(header, shoff + sizeof(Elf64_Ehdr), sec_count, shstrndx);
        buf.insert(buf.begin(), header.begin(), header.end());

        return buf;
    }

    // =========================================================================
    //  Emision de ejecutable ELF64 (ET_EXEC)
    // =========================================================================

    std::vector<uint8_t> ElfEmitter::emit_executable(uint64_t entry) {
        std::vector<uint8_t> buf;
        buf.reserve(8192);

        std::vector<uint8_t> text_data, rodata_data, data_data;
        uint64_t bss_size = 0;

        for (const auto &sec : sections_) {
            if (sec.name == ".text")       text_data   = sec.data;
            else if (sec.name == ".rodata") rodata_data = sec.data;
            else if (sec.name == ".data")  data_data   = sec.data;
            else if (sec.name == ".bss")   bss_size    = sec.data.size();
        }

        uint64_t page_size = 0x1000;
        uint64_t ehdr_phdr_size = sizeof(Elf64_Ehdr) + 5 * sizeof(Elf64_Phdr);
        uint64_t text_off    = (ehdr_phdr_size + page_size - 1) & ~(page_size - 1);
        uint64_t text_vaddr  = 0x400000;
        uint64_t rodata_off  = (text_off + text_data.size() + page_size - 1) & ~(page_size - 1);
        uint64_t rodata_vaddr = text_vaddr + (rodata_off - text_off);
        uint64_t data_off    = (rodata_off + rodata_data.size() + page_size - 1) & ~(page_size - 1);
        uint64_t data_vaddr  = text_vaddr + (data_off - text_off);

        buf.resize(text_off, 0);
        buf.insert(buf.end(), text_data.begin(), text_data.end());
        while (buf.size() < rodata_off) buf.push_back(0);
        buf.insert(buf.end(), rodata_data.begin(), rodata_data.end());
        while (buf.size() < data_off) buf.push_back(0);
        buf.insert(buf.end(), data_data.begin(), data_data.end());

        // Program headers
        Elf64_Phdr phdrs[5];

        phdrs[0].p_type   = PT_LOAD;
        phdrs[0].p_flags  = PF_R | PF_X;
        phdrs[0].p_offset = text_off;
        phdrs[0].p_vaddr  = text_vaddr;
        phdrs[0].p_paddr  = text_vaddr;
        phdrs[0].p_filesz = rodata_off - text_off;
        phdrs[0].p_memsz  = rodata_off - text_off;
        phdrs[0].p_align  = page_size;

        phdrs[1].p_type   = PT_LOAD;
        phdrs[1].p_flags  = PF_R;
        phdrs[1].p_offset = rodata_off;
        phdrs[1].p_vaddr  = rodata_vaddr;
        phdrs[1].p_paddr  = rodata_vaddr;
        phdrs[1].p_filesz = data_off - rodata_off;
        phdrs[1].p_memsz  = data_off - rodata_off;
        phdrs[1].p_align  = page_size;

        phdrs[2].p_type   = PT_LOAD;
        phdrs[2].p_flags  = PF_R | PF_W;
        phdrs[2].p_offset = data_off;
        phdrs[2].p_vaddr  = data_vaddr;
        phdrs[2].p_paddr  = data_vaddr;
        phdrs[2].p_filesz = data_data.size();
        phdrs[2].p_memsz  = data_data.size() + bss_size;
        phdrs[2].p_align  = page_size;

        phdrs[3].p_type   = PT_GNU_STACK;
        phdrs[3].p_flags  = PF_R | PF_W | PF_X;
        phdrs[3].p_offset = 0;
        phdrs[3].p_vaddr  = 0;
        phdrs[3].p_paddr  = 0;
        phdrs[3].p_filesz = 0;
        phdrs[3].p_memsz  = 0;
        phdrs[3].p_align  = 16;

        phdrs[4].p_type   = PT_GNU_RELRO;
        phdrs[4].p_flags  = PF_R;
        phdrs[4].p_offset = data_off;
        phdrs[4].p_vaddr  = data_vaddr;
        phdrs[4].p_paddr  = data_vaddr;
        phdrs[4].p_filesz = data_data.size();
        phdrs[4].p_memsz  = data_data.size();
        phdrs[4].p_align  = page_size;

        // ELF header
        for (int i = 0; i < 16; ++i) buf[i] = ELF_IDENT[i];
        write_at_u16(buf, 16, 2);                // ET_EXEC
        write_at_u16(buf, 18, EM_X86_64);
        write_at_u64(buf, 24, entry);            // e_entry
        write_at_u64(buf, 32, sizeof(Elf64_Ehdr)); // e_phoff
        write_at_u16(buf, 54, sizeof(Elf64_Phdr)); // e_phentsize
        write_at_u16(buf, 56, 5);                  // e_phnum
        write_at_u16(buf, 58, sizeof(Elf64_Ehdr)); // e_ehsize

        // PHDRs
        std::vector<uint8_t> phdr_data;
        for (int i = 0; i < 5; ++i)
            write_phdr_entry(phdr_data, phdrs[i]);
        std::memcpy(&buf[sizeof(Elf64_Ehdr)], phdr_data.data(), phdr_data.size());

        return buf;
    }

} // namespace aot
