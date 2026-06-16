/*
 * VestaVM - Máquina Virtual Distribuida
 * 
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 * 
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 * 
 * Descargo: Autor no responsable por modificaciones.
 */
#include <iostream>
#include "assembly/assembly.h"

int main() {

    // --- Imprimir arquitecturas disponibles ---
    const ArchSupport& archs = get_available_architectures();
    std::cout << "Capstone supported architectures:\n";
    for (auto &a : archs.capstone) std::cout << "  " << a << "\n";
    std::cout << "Keystone supported architectures:\n";
    for (auto &a : archs.keystone) std::cout << "  " << a << "\n";

    // Ensamblar con Keystone
    ks_engine *ks;
    ks_err err;
    size_t count;
    unsigned char *encode;
    size_t size;

    err = ks_open(KS_ARCH_X86, KS_MODE_32, &ks);
    if (err != KS_ERR_OK) {
        std::cerr << "Failed to initialize Keystone\n";
        return -1;
    }

    const char* assembly = "mov eax, 0x1234\nadd eax, 0x1";
    err = static_cast<ks_err>(ks_asm(ks, assembly, 0, &encode, &size, &count));
    if (err != KS_ERR_OK) {
        std::cerr << "Keystone assembly error: " << ks_strerror(err) << "\n";
        return -1;
    }

    std::cout << "Assembled " << count << " instructions, " << size << " bytes:\n";
    for (size_t i = 0; i < size; i++) {
        printf("%02x ", encode[i]);
    }
    std::cout << "\n";

    ks_free(encode);
    ks_close(ks);

    // Desensamblar con Capstone
    csh handle;
    cs_insn *insn;
    size_t count_dis;

    if (cs_open(CS_ARCH_X86, CS_MODE_32, &handle) != CS_ERR_OK) {
        std::cerr << "Failed to initialize Capstone\n";
        return -1;
    }

    // Reensamblamos los bytes anteriores para Capstone
    uint8_t code[] = {0xb8, 0x34, 0x12, 0x00, 0x00, 0x83, 0xc0, 0x01}; // mov eax,0x1234; add eax,0x1
    count_dis = cs_disasm(handle, code, sizeof(code), 0x1000, 0, &insn);
    if (count_dis > 0) {
        for (size_t i = 0; i < count_dis; i++) {
            printf("0x%llx:\t%s\t\t%s\n",
                insn[i].address, insn[i].mnemonic, insn[i].op_str);
        }
        cs_free(insn, count_dis);
    } else {
        std::cerr << "Failed to disassemble\n";
    }

    cs_close(&handle);
    return 0;
}