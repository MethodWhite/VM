/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_emitter.cpp
 * @brief Implementacion del emisor IR -> texto .vel (lowering con linear scan).
 *
 * Estructura del codigo emitido para cada funcion:
 *
 *   fn_label:
 *       enter <spill_count>      ; prologo: guarda rbp, reserva slots de pila
 *       ...instrucciones...
 *   fn_label_ret:
 *       leave
 *       ret
 *
 * Las etiquetas de bloque siguen el patron "fn_<nombre>_<bloque>".
 *
 * Manejo de operaciones de dos direcciones (.vel):
 *   adds r_dst, r_src  <=>  r_dst += r_src
 *   Si r_dst != reg_de(operando_izquierdo):
 *       mov r_dst, r_op0
 *       adds r_dst, r_op1
 *   Si r_dst == reg_de(operando_izquierdo):
 *       adds r_dst, r_op1     ; operacion en sitio, sin mov extra
 *
 * Spilling:
 *   Valores derramados se almacenan en slots de pila reservados por enter N.
 *   Cada slot ocupa 8 bytes; el acceso usa movc con base rbp y offset slot*8.
 *   load_src emite la carga antes de usar el valor; store_spilled emite el
 *   almacenamiento despues de calcularlo. Se usan r14 y r13 como scratches.
 */

#include "ir/ir_emitter.h"
#include "ir/ir_optimizer.h"
#include "ir/liveness.h"
#include "ir/regalloc.h"
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cstdint>

namespace ir {

// =========================================================================
//  Contexto interno de emision por funcion
// =========================================================================

struct EmitCtx {
    const IrFunction      &fn;       // funcion SSA a emitir
    const AllocResult     &alloc;    // asignacion de registros
    const LivenessResult  &liveness; // intervalos de vida (necesario para save/restore en CALL)
    std::ostringstream    &out;      // stream de salida .vel
    bool                   comments; // emitir comentarios de origen
    bool                   emit_debug; // emitir comentarios @line N por instruccion
    uint32_t               label_seq; // secuencia para etiquetas unicas de condicion
    // A.34.fix14: true si se emitio enter (spill_count > 0); false = metodo hoja sin frame.
    bool                   has_frame;

    // nombre base para etiquetas de esta funcion
    std::string fn_lbl;

    EmitCtx(const IrFunction &fn_, const AllocResult &alloc_,
            const LivenessResult &liveness_,
            std::ostringstream &out_, bool comments_, bool emit_debug_,
            bool has_frame_)
        : fn(fn_), alloc(alloc_), liveness(liveness_), out(out_), comments(comments_),
          emit_debug(emit_debug_), label_seq(0), has_frame(has_frame_),
          fn_lbl(sanitize(fn_.name)) {}

    // Convierte un nombre arbitrario a un identificador .vel valido
    static std::string sanitize(const std::string &s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (std::isalnum((unsigned char)c) || c == '_') r += c;
            else r += '_';
        }
        return r;
    }

    // Nombre de etiqueta para un bloque
    std::string block_label(IrBlockId bid) const {
        if (bid < fn.blocks.size())
            return fn_lbl + "_" + fn.blocks[bid].name;
        return fn_lbl + "_bb" + std::to_string(bid);
    }

    // Nombre de registro para un valor (o r14 como fallback — sin efecto de lado)
    std::string reg_of(IrValueId vid) const {
        if (vid == IR_NO_VALUE) return "r0";
        auto it = alloc.reg_map.find(vid);
        if (it != alloc.reg_map.end()) return reg_name(it->second);
        return reg_name(SCRATCH_REG);
    }

    // True si el valor vid tiene un registro asignado (no derramado)
    bool is_in_reg(IrValueId vid) const {
        return vid != IR_NO_VALUE && alloc.reg_map.count(vid) > 0;
    }

    // Numero de registro de un valor (SCRATCH_REG si derramado)
    int reg_num(IrValueId vid) const {
        auto it = alloc.reg_map.find(vid);
        if (it != alloc.reg_map.end()) return it->second;
        return SCRATCH_REG;
    }

    // true si el value @p vid es un host_ptr a un objeto
    // GC (marcado por el lowering en lower_new_expr / lower_class_field_load
    // / params CLASS de class methods).  Los values is_gc_object necesitan
    // tratamiento especial al spillarse: el SLOT contiene el GcHandle
    // (estable a evacuacion), mientras que el reg contiene el host_ptr
    // (lo que CALLVIRT, GETFIELD y demas opcodes esperan).
    bool is_gc_value(IrValueId vid) const {
        if (vid == IR_NO_VALUE) return false;
        if (static_cast<size_t>(vid) >= fn.values.size()) return false;
        return fn.values[vid].is_gc_object;
    }

    // Materializa un operando FUENTE: si esta derramado emite la carga del
    // slot al registro scratch (scratch_idx 0=r14, 1=r13) y devuelve su
    // nombre.  Para values is_gc_object spilled, el slot guarda el HANDLE
    // (no el host_ptr), por lo que tras el load emitimos gcderef + xchg
    // para devolver un host_ptr FRESCO (post-eventual GC move).  Asi el
    // resto del emisor recibe siempre host_ptr en regs is_gc_object,
    // independientemente de si vinieron de reg directo o de spill.
    std::string load_src(IrValueId vid, int scratch_idx = 0) {
        if (vid == IR_NO_VALUE) return "r0";
        {
            auto it = alloc.reg_map.find(vid);
            if (it != alloc.reg_map.end()) return reg_name(it->second);
        }
        {
            auto it = alloc.spill_map.find(vid);
            if (it != alloc.spill_map.end()) {
                int sr = (scratch_idx == 0) ? SCRATCH_REG : SCRATCH2_REG;
                // Calcular direccion: r13 = rbp + slot*8
                out << "    mov r13, rbp\n";
                out << "    addu r13, " << (it->second * 8) << "\n";
                out << "    mov " << reg_name(sr) << ", [r13]\n";
                if (is_gc_value(vid)) {
                    // El slot contiene el GcHandle.  Convertir a host_ptr
                    // fresco (gcderef indexa la HandleTable, que el GC
                    // mantiene actualizada tras moves -- transparent).
                    out << "    gcderef cur0, " << reg_name(sr) << "\n";
                    out << "    xchg cur0, " << reg_name(sr) << "\n";
                }
                return reg_name(sr);
            }
        }
        return reg_name(SCRATCH_REG);
    }

    // Devuelve el registro DESTINO de vid: si esta derramado retorna r14
    // sin emitir codigo (el llamante debe invocar store_spilled despues).
    std::string dst_of(IrValueId vid) {
        if (vid == IR_NO_VALUE) return "r0";
        auto it = alloc.reg_map.find(vid);
        if (it != alloc.reg_map.end()) return reg_name(it->second);
        return reg_name(SCRATCH_REG);
    }

    // Si vid esta derramado, persiste SCRATCH_REG en su slot de pila.
    // Para values is_gc_object, almacenamos el GcHandle en lugar del
    // host_ptr crudo: el handle es estable a una eventual evacuacion del
    // GC (HandleTable redirige internamente).  El siguiente load_src del
    // slot hace gcderef para recuperar el host_ptr fresco.
    void store_spilled(IrValueId vid) {
        if (vid == IR_NO_VALUE) return;
        auto it = alloc.spill_map.find(vid);
        if (it != alloc.spill_map.end()) {
            if (is_gc_value(vid)) {
                // gchandle in-place: r14 (host_ptr) -> r14 (handle).
                // El reg "actual" ya quedo clobbeado por la op que produjo
                // el value spilled, asi que pisarlo aqui es seguro.
                out << "    gchandle " << reg_name(SCRATCH_REG)
                    << ", "             << reg_name(SCRATCH_REG) << "\n";
            }
            // Calcular direccion: r13 = rbp + slot*8
            out << "    mov r13, rbp\n";
            out << "    addu r13, " << (it->second * 8) << "\n";
            out << "    mov [r13], " << reg_name(SCRATCH_REG) << "\n";
        }
    }

    // Emite un comentario si los comentarios estan activados
    void comment(const std::string &msg) {
        if (comments) out << "    // " << msg << "\n";
    }

    // Genera una etiqueta local unica
    std::string unique_lbl(const std::string &base) {
        return fn_lbl + "_" + base + "_" + std::to_string(label_seq++);
    }

    // Genera una referencia @Absolute con el prefijo de seccion "code."
    // Todas las etiquetas internas del .vel viven en la seccion "code".
    static std::string abs_lbl(const std::string &lbl) {
        return "code." + lbl;
    }
};

// =========================================================================
//  Utilidades internas del emisor
// =========================================================================

// Devuelve el sufijo de tamano de registro VM segun el ancho del IrType.
// Mapeo:  1 byte -> "b"  | 2 bytes -> "w"  | 4 bytes -> "d"  | 8 bytes -> ""
// Ejemplo: para r3 con tipo I32, devuelve "r3d" (32 bits low de r3).
static std::string reg_name_sized(int reg, IrType t) {
    std::string base = reg_name(reg);
    switch (t) {
        case IrType::I8:  case IrType::U8:  case IrType::BOOL: return base + "b";
        case IrType::I16: case IrType::U16:                    return base + "w";
        case IrType::I32: case IrType::U32: case IrType::F32:  return base + "d";
        default:                                               return base; // 64-bit
    }
}

// Devuelve el tamano en bytes del tipo IR (para strides de arrays y similares)
static uint64_t ir_type_size(IrType t) {
    switch (t) {
        case IrType::I8:  case IrType::U8:  case IrType::BOOL: return 1;
        case IrType::I16: case IrType::U16: return 2;
        case IrType::I32: case IrType::U32: case IrType::F32:  return 4;
        default:                                                 return 8;
    }
}

// =========================================================================
//  Emision de instrucciones individuales
// =========================================================================

// Emite "mov r_dst, r_src" si son distintos (evita mov rx, rx)
static void emit_mov_if_needed(EmitCtx &ctx, const std::string &dst,
                                const std::string &src) {
    if (dst != src) ctx.out << "    mov " << dst << ", " << src << "\n";
}

// =========================================================================
//  Helpers de save/restore alrededor de instrucciones CALL
// =========================================================================
//
// Modelo de calling convention de la VM (ver ir_emitter.h):
//   - r0          : valor de retorno
//   - r1..r12     : argumentos de llamada (caller-saved: el callee los toca)
//   - r13         : SCRATCH2 del emisor (caller-saved)
//   - r14         : SCRATCH del emisor (caller-saved)
//   - r15         : argc (caller-saved)
//
// Es decir, NO HAY registros callee-saved.  Todo valor que esta vivo a traves
// de un CALL y que ocupa un registro fisico (r0..r12) debe preservarse
// explicitamente con push antes del call y pop despues.
//
// El bug que motivo este codigo: el regalloc lineal asigna parametros a
// r1..r12 al inicio de la funcion.  Si esos parametros se siguen usando
// despues de una llamada recursiva, el move de argumento ("mov r1, r_arg")
// los pisaba.  Ahora los salvamos a la pila antes de los moves.

// Calcula la posicion lineal de la instruccion en el index del bloque.
static uint32_t lin_pos_of(const EmitCtx &ctx, IrBlockId bid, size_t idx) {
    if (bid < ctx.liveness.block_start.size()) {
        return ctx.liveness.block_start[bid] + (uint32_t)idx;
    }
    return 0;
}

// Devuelve los numeros de registro fisico (0..12) que tienen valores vivos
// a traves de la instruccion @p call_pos y que deben preservarse.
//
// Un valor v esta vivo a traves de call_pos si def(v) < call_pos < end(v).
// El propio @p dst del CALL se EXCLUYE: se define EN el call, no antes.
//
// Devuelve los registros ordenados ascendentemente y deduplicados.
static std::vector<int> live_regs_through_call(const EmitCtx &ctx,
                                                uint32_t call_pos,
                                                IrValueId dst) {
    std::vector<int> regs;
    regs.reserve(8);
    for (const auto &iv : ctx.liveness.intervals) {
        if (iv.id == dst) continue;            // dst nace en el call
        // Vivo a traves del call si fue definido ANTES o EN call_pos y
        // se usa DESPUES.  Antes era `def < call_pos`, lo que excluia
        // los parametros (que tienen def=0) cuando el primer CALL
        // estaba en posicion 0 -> bug grave en ctors que llaman a otra
        // funcion como primera operacion (e.g. `this.inner = new Inner(x)`):
        // los params (this, x) no se preservaban y el codigo post-call
        // los referenciaba con valores ya clobbeados por el parallel-move.
        // Excluimos dst arriba, asi que valores definidos exactamente
        // en call_pos (si existieran via PHI o similar) no entran al
        // false positive: si vive despues, ya estaba en algun reg antes.
        if (iv.def <= call_pos && call_pos < iv.end) {
            auto it = ctx.alloc.reg_map.find(iv.id);
            if (it == ctx.alloc.reg_map.end()) continue;  // valor spilled, no en registro
            const int r = it->second;
            if (r >= 0 && r <= 12) regs.push_back(r);
        }
    }
    std::sort(regs.begin(), regs.end());
    regs.erase(std::unique(regs.begin(), regs.end()), regs.end());
    return regs;
}

// true si el reg @p r contiene un IrValueId con is_gc_object.
// Iteramos `ctx.alloc.reg_map` (reg -> id) en sentido inverso para encontrar
// CUAL valor esta efectivamente en ese registro AL momento del CALL.  El
// regalloc puede haber asignado mas de un IrValueId al mismo reg en
// distintos puntos del programa, pero el live-range analysis garantiza que
// solo uno este vivo a traves del call.
static bool reg_holds_gc_object(const EmitCtx &ctx, uint32_t call_pos, int r) {
    for (const auto &iv : ctx.liveness.intervals) {
        if (!(iv.def <= call_pos && call_pos < iv.end)) continue;
        auto it = ctx.alloc.reg_map.find(iv.id);
        if (it == ctx.alloc.reg_map.end() || it->second != r) continue;
        if (static_cast<size_t>(iv.id) >= ctx.fn.values.size()) continue;
        if (ctx.fn.values[iv.id].is_gc_object) return true;
    }
    return false;
}

// emite el save de los regs vivos antes de un CALL.  Para los
// regs que contienen un host_ptr GC (is_gc_object), guardamos el GcHandle
// (estable a evacuacion del GC) en lugar del host_ptr.  IMPORTANTE: NO
// reescribimos el reg original con el handle: el parallel-move posterior
// puede leer ese reg como source para colocar el arg en r1..r_N.  Si lo
// machacasemos, el callvirt recibiria el handle en lugar del host_ptr y
// fallaria con segfault.  Usamos @c r14 como scratch transiente: tras el
// push, el handle vive en stack y r14 puede ser libremente clobbeado por
// el parallel-move (cycle-breaking).
static void emit_save_live_regs(EmitCtx &ctx, uint32_t call_pos,
                                 const std::vector<int> &regs_to_save)
{
    for (int r : regs_to_save) {
        if (reg_holds_gc_object(ctx, call_pos, r)) {
            // gchandle r14, reg : r14 = handle del ptr en reg.  reg queda
            // intacto con su host_ptr original (necesario para que el
            // parallel-move siguiente pueda usarlo como source).
            ctx.out << "    gchandle r14, " << reg_name(r) << "\n";
            ctx.out << "    push r14\n";
        } else {
            ctx.out << "    push " << reg_name(r) << "\n";
        }
    }
}

// emite el restore de los regs guardados.  Para los regs GC,
// tras el pop convertimos el GcHandle de vuelta a host_ptr via gcderef.
// Si la GC movio el objeto durante el callee, el host_ptr resultante es
// el NUEVO; el codigo posterior puede leer/escribir fields sin segfault.
//
// Patron emitido (por reg GC):
//   pop reg
//   gcderef cur0, reg     ; cur0 = host_ptr fresco (post-GC)
//   xchg cur0, reg        ; reg = host_ptr; cur0 = handle (descartado)
//
// El uso de cur0 sigue la convencion del loader (__new_<X>) donde gcderef
// escribe a cursor y luego se intercambia a un GP reg.  cur0 es scratch
// del runtime y nunca se preserva entre instrucciones VM.
static void emit_restore_live_regs(EmitCtx &ctx, uint32_t call_pos,
                                    const std::vector<int> &regs_to_save)
{
    for (auto it = regs_to_save.rbegin(); it != regs_to_save.rend(); ++it) {
        const int r = *it;
        ctx.out << "    pop " << reg_name(r) << "\n";
        if (reg_holds_gc_object(ctx, call_pos, r)) {
            ctx.out << "    gcderef cur0, " << reg_name(r) << "\n";
            ctx.out << "    xchg cur0, " << reg_name(r) << "\n";
        }
    }
}

// los values is_gc_object SPILLED no requieren
// save/restore extra alrededor de cada call: load_src/store_spilled ya
// convierten host_ptr <-> handle automaticamente al cruzar el slot stack.
// El handle es estable a evacuacion del GC (HandleTable redirige internamente),
// asi que un slot que contenga el handle del objeto sigue valido despues
// de cualquier numero de calls que disparen GC.  Esto reemplaza las
// funciones emit_save_spilled_gc / emit_restore_spilled_gc previas.
//
// Wrappers simples que mantienen la firma para no tocar 8 call sites.
static void emit_save_all_gc_aware(EmitCtx &ctx, uint32_t call_pos,
                                     const std::vector<int> &regs_to_save)
{
    emit_save_live_regs(ctx, call_pos, regs_to_save);
}

static void emit_restore_all_gc_aware(EmitCtx &ctx, uint32_t call_pos,
                                        const std::vector<int> &regs_to_save)
{
    emit_restore_live_regs(ctx, call_pos, regs_to_save);
}

// Resuelve los moves de argumentos para una llamada usando el algoritmo
// clasico de copia paralela.  Cada move es (target_reg, source_reg).
//
//  1) Mientras existan moves no resueltos:
//     a) Eliminar los triviales (target == source).
//     b) Buscar un move cuyo target NO sea source de ningun otro pendiente
//        y emitirlo.
//     c) Si no hay progreso (todo es ciclo): romper un ciclo usando un
//        scratch (r14) y reescribir.
//
// Esta funcion NO se preocupa por valores vivos a traves del call: esa
// preservacion debe haberse hecho con push antes de invocar este helper.
static void emit_parallel_arg_moves(EmitCtx &ctx,
                                     std::vector<std::pair<int, std::string>> moves) {
    auto reg_str_of = [](int r) { return std::string(reg_name(r)); };

    auto someone_uses_as_source = [&](const std::string &target_reg_str) {
        for (const auto &m : moves) {
            if (m.second == target_reg_str) return true;
        }
        return false;
    };

    // Bucle de resolucion.
    while (!moves.empty()) {
        // Paso a: eliminar triviales.
        bool removed_trivial = false;
        for (size_t i = 0; i < moves.size(); ) {
            if (moves[i].second == reg_str_of(moves[i].first)) {
                moves.erase(moves.begin() + (long)i);
                removed_trivial = true;
            } else {
                ++i;
            }
        }
        if (moves.empty()) break;

        // Paso b: buscar un move "seguro" (target no es source de nadie).
        bool emitted = false;
        for (size_t i = 0; i < moves.size(); ++i) {
            const std::string target_str = reg_str_of(moves[i].first);
            if (!someone_uses_as_source(target_str)) {
                ctx.out << "    mov " << target_str << ", " << moves[i].second << "\n";
                moves.erase(moves.begin() + (long)i);
                emitted = true;
                break;
            }
        }
        if (emitted) continue;
        if (removed_trivial) continue;

        // Paso c: ciclo.  Romperlo usando r14 (SCRATCH).  Tomamos el primer
        // move (s -> d), copiamos s a r14 y reescribimos cualquier otro
        // move que use s como source para que use r14, y emitimos al final
        // d <- r14 cuando llegue su turno.
        const int target_reg = moves.front().first;
        const std::string old_src = moves.front().second;
        const std::string scratch = reg_str_of(SCRATCH_REG);
        ctx.out << "    mov " << scratch << ", " << old_src << "\n";
        for (auto &m : moves) {
            if (m.second == old_src) m.second = scratch;
        }
        // Sustituir el move original (s -> d) por (r14 -> d).  No lo
        // emitimos aun: la siguiente iteracion lo emitira normalmente
        // porque ya no formara ciclo.
        moves.front().second = scratch;
        // (Nota: target_reg solo se usa como recordatorio del move roto;
        //  la siguiente iteracion lo emitira via el mismo loop.)
        (void)target_reg;
    }
}

// Emite una operacion binaria en formato dos-direcciones de .vel:
//   "op r_dst, r_src2"
// Carga operandos derramados desde pila (src1->r14, src2->r13) si es necesario.
// Almacena el resultado en pila si dst esta derramado.
static void emit_binop(EmitCtx &ctx, const std::string &mnemonic,
                        IrValueId dst, IrValueId src1, IrValueId src2) {
    std::string rs1 = ctx.load_src(src1, 0); // r14 si derramado
    std::string rs2 = ctx.load_src(src2, 1); // r13 si derramado
    std::string rd  = ctx.dst_of(dst);
    emit_mov_if_needed(ctx, rd, rs1);
    ctx.out << "    " << mnemonic << " " << rd << ", " << rs2 << "\n";
    ctx.store_spilled(dst);
}

// Emite una operacion unaria en dos-direcciones:
//   "op r_dst"
// Carga operando derramado (->r14) y almacena resultado si dst esta derramado.
static void emit_unop(EmitCtx &ctx, const std::string &mnemonic,
                       IrValueId dst, IrValueId src) {
    std::string rs = ctx.load_src(src, 0);
    std::string rd = ctx.dst_of(dst);
    emit_mov_if_needed(ctx, rd, rs);
    ctx.out << "    " << mnemonic << " " << rd << "\n";
    ctx.store_spilled(dst);
}

// =========================================================================
// Helpers float: bitcast GP <-> ZMM via stack memory roundtrip.
//
// El allocator solo conoce registros de proposito general (r0..r15), pero
// las instrucciones de aritmetica float (fadd/fsub/fmul/fdiv/fneg/...)
// requieren operandos en registros ZMM (f0..f15).  Como no hay opcode
// directo de "GP -> ZMM bits" (fcvt convierte VALOR, no bits), se hace
// un round-trip por la pila VM.  La encoding SIB no acepta @c rsp como
// registro base directo (encode_reg_general solo entiende r0..r15), asi
// que primero copiamos rsp a un GP scratch (r15: caller-saved, solo se
// usa para argc inmediatamente antes de CALL/CALLN -- entre IR ops es
// libre de clobber, igual que se asume para r13/r14 de spill scratch).
// Patron emitido (4 instrucciones por direccion):
//    subsp rsp, 8
//    mov   r15, rsp         ; r15 = direccion del slot (rsp ya bajado)
//    mov   [r15], <gp_reg>  ; deposita los 8 bytes IEEE 754
//    fload <zmm_reg>, r15   ; lee como float (mismo patron de bits)
//    addsp rsp, 8
// La direccion inversa es simetrica (fstore + mov gp_reg, [r15]).  Coste
// fijo de 5 instrucciones VM por bitcast hasta que se anada un asignador
// paralelo de ZMM (planificado para Phase D / MachineIR).
// =========================================================================
static void emit_gp_to_zmm_bits(EmitCtx &ctx,
                                 const std::string &gp_reg,
                                 const std::string &zmm_reg) {
    ctx.out << "    subsp rsp, 8\n";
    ctx.out << "    mov r15, rsp\n";
    ctx.out << "    mov [r15], " << gp_reg << "\n";
    ctx.out << "    fload " << zmm_reg << ", r15\n";
    ctx.out << "    addsp rsp, 8\n";
}

static void emit_zmm_to_gp_bits(EmitCtx &ctx,
                                 const std::string &zmm_reg,
                                 const std::string &gp_reg) {
    // El orden de operandos de fstore es r1=gp_addr, r2=zmm_src (al reves
    // que fload).  Documentado en src/emmit/emmit_decl.cpp:2034.
    ctx.out << "    subsp rsp, 8\n";
    ctx.out << "    mov r15, rsp\n";
    ctx.out << "    fstore r15, " << zmm_reg << "\n";
    ctx.out << "    mov " << gp_reg << ", [r15]\n";
    ctx.out << "    addsp rsp, 8\n";
}

// Emite una operacion float binaria con bitcast automatico.
// Carga ambos operandos GP en f0/f1, ejecuta "op f0, f1", y devuelve f0
// como bits IEEE 754 al GP destino.  El sufijo ".ps" se anade
// automaticamente cuando @c type es F32 para que el runtime use la ruta
// de aritmetica float-32 (read_f32 + write_f32 con zeroing del tope).
static void emit_float_binop(EmitCtx &ctx, const std::string &mnemonic,
                              IrType type,
                              IrValueId dst, IrValueId src1, IrValueId src2) {
    std::string rs1 = ctx.load_src(src1, 0);
    std::string rs2 = ctx.load_src(src2, 1);
    std::string rd  = ctx.dst_of(dst);
    const std::string suffix = (type == IrType::F32) ? ".ps" : "";
    emit_gp_to_zmm_bits(ctx, rs1, "f0");
    emit_gp_to_zmm_bits(ctx, rs2, "f1");
    ctx.out << "    " << mnemonic << suffix << " f0, f1\n";
    emit_zmm_to_gp_bits(ctx, "f0", rd);
    ctx.store_spilled(dst);
}

// Emite una operacion float unaria con bitcast automatico.
// Carga el operando GP en f0, ejecuta "op f0, f0" (los unarios reusan
// el mismo registro como destino y fuente segun emit_instr_freg), y
// devuelve f0 como bits IEEE 754 al GP destino.  El sufijo ".ps" se
// anade cuando @c type es F32.
static void emit_float_unop(EmitCtx &ctx, const std::string &mnemonic,
                             IrType type,
                             IrValueId dst, IrValueId src) {
    std::string rs = ctx.load_src(src, 0);
    std::string rd = ctx.dst_of(dst);
    const std::string suffix = (type == IrType::F32) ? ".ps" : "";
    emit_gp_to_zmm_bits(ctx, rs, "f0");
    ctx.out << "    " << mnemonic << suffix << " f0, f0\n";
    emit_zmm_to_gp_bits(ctx, "f0", rd);
    ctx.store_spilled(dst);
}

// Mnemonic de dos-direcciones para operaciones aritmeticas/logicas segun tipo.
static const char *arith_mnemonic(IrOp op, IrType type) {
    bool is_signed = (type == IrType::I8  || type == IrType::I16 ||
                      type == IrType::I32 || type == IrType::I64);
    switch (op) {
        case IrOp::ADD: return is_signed ? "adds" : "addu";
        case IrOp::SUB: return is_signed ? "subs" : "subu";
        case IrOp::MUL: return is_signed ? "muls" : "mulu";
        case IrOp::DIV: return is_signed ? "divs" : "divu";
        case IrOp::MOD: return is_signed ? "mods" : "modu";
        case IrOp::AND: return "and";
        case IrOp::OR:  return "or";
        case IrOp::XOR: return "xor";
        case IrOp::SHL: return "shl";
        case IrOp::SHR: return "shr";
        case IrOp::SAR: return "sar";
        // flotante
        case IrOp::FADD: return "fadd";
        case IrOp::FSUB: return "fsub";
        case IrOp::FMUL: return "fmul";
        case IrOp::FDIV: return "fdiv";
        case IrOp::FMIN: return "fmin";
        case IrOp::FMAX: return "fmax";
        default: return "add";
    }
}

// Mnemonic de comparacion segun tipo
static const char *cmp_mnemonic(IrOp op) {
    switch (op) {
        case IrOp::CMP_ULT: case IrOp::CMP_UGT:
        case IrOp::CMP_ULE: case IrOp::CMP_UGE: return "cmpu";
        case IrOp::FCMP_EQ: case IrOp::FCMP_NE:
        case IrOp::FCMP_LT: case IrOp::FCMP_GT:
        case IrOp::FCMP_LE: case IrOp::FCMP_GE: return "fcmp";
        default: return "cmps";
    }
}

// Inversion de condicion de salto: si cond es verdadera -> etiqueta false
// Emite "jmp.<cond_invertida> @Absolute(false_lbl)"
static void emit_cond_branch(EmitCtx &ctx, IrOp cmp_op,
                              const std::string &false_lbl) {
    const char *jmp = nullptr;
    switch (cmp_op) {
        case IrOp::CMP_EQ:  case IrOp::FCMP_EQ: jmp = "jmp.jne"; break;
        case IrOp::CMP_NE:  case IrOp::FCMP_NE: jmp = "jmp.je";  break;
        case IrOp::CMP_LT:  case IrOp::FCMP_LT: jmp = "jmp.jge"; break;
        case IrOp::CMP_GT:  case IrOp::FCMP_GT: jmp = "jmp.jle"; break;
        case IrOp::CMP_LE:  case IrOp::FCMP_LE: jmp = "jmp.jgt"; break;
        case IrOp::CMP_GE:  case IrOp::FCMP_GE: jmp = "jmp.jlt"; break;
        case IrOp::CMP_ULT: jmp = "jmp.jae"; break;
        case IrOp::CMP_UGT: jmp = "jmp.jls"; break;
        case IrOp::CMP_ULE: jmp = "jmp.jhi"; break;
        case IrOp::CMP_UGE: jmp = "jmp.jb";  break;
        default:             jmp = "jmp.je";  break; // cond==0 -> false
    }
    ctx.out << "    " << jmp << " @Absolute(\"" << EmitCtx::abs_lbl(false_lbl) << "\")\n";
}

// Emite el lowering de CMP standalone (no fusionada con BR_COND):
//   cmps r_a, r_b
//   jmp.<cond> __true
//   mov r_dst, 0
//   jmp __end
// __true:
//   mov r_dst, 1
// __end:
static void emit_cmp_standalone(EmitCtx &ctx, const IrInstr &ins) {
    if (ins.operands.size() < 2) return;
    std::string rd   = ctx.dst_of(ins.dst);
    std::string ra   = ctx.load_src(ins.operands[0], 0);
    std::string rb   = ctx.load_src(ins.operands[1], 1);
    std::string lbl_true = ctx.unique_lbl("ctrue");
    std::string lbl_end  = ctx.unique_lbl("cend");
    const char *cmp_mn = cmp_mnemonic(ins.op);

    const bool is_fcmp = (ins.op == IrOp::FCMP_EQ || ins.op == IrOp::FCMP_NE
                       || ins.op == IrOp::FCMP_LT || ins.op == IrOp::FCMP_GT
                       || ins.op == IrOp::FCMP_LE || ins.op == IrOp::FCMP_GE);
    if (is_fcmp) {
        // FCMP requiere registros ZMM; bitcast bits desde GP via stack.
        // El sufijo ".ps" se anade si los operandos son F32 (el tipo del
        // resultado de FCMP es BOOL, asi que miramos la fuente).
        const IrType ot = ctx.fn.values[ins.operands[0]].type;
        const std::string suffix = (ot == IrType::F32) ? ".ps" : "";
        emit_gp_to_zmm_bits(ctx, ra, "f0");
        emit_gp_to_zmm_bits(ctx, rb, "f1");
        ctx.out << "    fcmp" << suffix << " f0, f1\n";
    } else {
    ctx.out << "    " << cmp_mn << " " << ra << ", " << rb << "\n";
    }
    // saltar a true si condicion se cumple (condicion directa)
    const char *jmp_direct = nullptr;
    switch (ins.op) {
        case IrOp::CMP_EQ:  case IrOp::FCMP_EQ: jmp_direct = "jmp.je";  break;
        case IrOp::CMP_NE:  case IrOp::FCMP_NE: jmp_direct = "jmp.jne"; break;
        case IrOp::CMP_LT:  case IrOp::FCMP_LT: jmp_direct = "jmp.jlt"; break;
        case IrOp::CMP_GT:  case IrOp::FCMP_GT: jmp_direct = "jmp.jgt"; break;
        case IrOp::CMP_LE:  case IrOp::FCMP_LE: jmp_direct = "jmp.jle"; break;
        case IrOp::CMP_GE:  case IrOp::FCMP_GE: jmp_direct = "jmp.jge"; break;
        case IrOp::CMP_ULT: jmp_direct = "jmp.jb";  break;
        case IrOp::CMP_UGT: jmp_direct = "jmp.jhi"; break;
        case IrOp::CMP_ULE: jmp_direct = "jmp.jls"; break;
        case IrOp::CMP_UGE: jmp_direct = "jmp.jae"; break;
        default: jmp_direct = "jmp.je"; break;
    }
    ctx.out << "    " << jmp_direct << " @Absolute(\"" << EmitCtx::abs_lbl(lbl_true) << "\")\n";
    ctx.out << "    mov " << rd << ", 0\n";
    ctx.out << "    jmp @Absolute(\"" << EmitCtx::abs_lbl(lbl_end) << "\")\n";
    ctx.out << lbl_true << ":\n";
    ctx.out << "    mov " << rd << ", 1\n";
    ctx.out << lbl_end << ":\n";
    ctx.store_spilled(ins.dst);
}

// Emite las copias paralelas para destruccion de PHI en el bloque predecesor.
//
// Implementa el algoritmo de copia paralela con deteccion de ciclos:
//   1. Construir lista de pares (dst_reg, src_reg).
//   2. Eliminar triviales (dst == src).
//   3. Emitir copias acicilicas en orden topologico (un destino que no es
//      fuente de otra copia puede emitirse de forma segura).
//   4. Para los ciclos restantes: romper cada ciclo usando SCRATCH_REG (r14)
//      como temporal.
//
// Para valores derramados (no en reg_map) se usa una carga/almacenamiento
// secuencial a traves de r14; los ciclos con derrames se gestionan igual.
static void emit_phi_copies(EmitCtx &ctx, IrBlockId pred_id, IrBlockId succ_id) {
    if (succ_id >= static_cast<IrBlockId>(ctx.fn.blocks.size())) return;
    const IrBlock &succ = ctx.fn.blocks[succ_id];

    // Paso 1: recopilar pares (dst_vid, src_vid)
    struct PhiCopy { IrValueId dst; IrValueId src; };
    std::vector<PhiCopy> copies;
    for (const auto &ins : succ.instrs) {
        if (ins.op != IrOp::PHI) break;
        if (ins.dst == IR_NO_VALUE) continue;
        for (const auto &pa : ins.phi_args) {
            if (pa.block == pred_id && pa.value != IR_NO_VALUE) {
                copies.push_back({ins.dst, pa.value});
            }
        }
    }
    if (copies.empty()) return;

    // Paso 2: separar copias en-registro de copias con derrames.
    // Las copias con derrame se emiten de forma simple (carga/mov/almacena).
    // Las copias totalmente en registro se someten al algoritmo de paralela.
    std::vector<PhiCopy> reg_copies;
    for (const auto &c : copies) {
        bool dst_in_reg = ctx.alloc.reg_map.count(c.dst) > 0;
        bool src_in_reg = ctx.alloc.reg_map.count(c.src) > 0;
        if (dst_in_reg && src_in_reg) {
            reg_copies.push_back(c);
        } else {
            // Al menos un operando esta derramado: copia secuencial segura
            // (los derrames son slots distintos, no hay alias entre ellos y r14)
            std::string r_src = ctx.load_src(c.src, 0);  // carga en r14 si spill
            std::string r_dst;
            bool dst_spilled = (ctx.alloc.spill_map.count(c.dst) > 0);
            r_dst = dst_spilled ? reg_name(SCRATCH_REG) : reg_name(ctx.alloc.reg_map.at(c.dst));
            emit_mov_if_needed(ctx, r_dst, r_src);
            if (dst_spilled) ctx.store_spilled(c.dst);
        }
    }

    // Paso 3: copia paralela para valores en registro.
    // Mapa: dst_reg -> src_reg (solo registros numericos)
    std::unordered_map<int, int> pending;
    for (const auto &c : reg_copies) {
        int d = ctx.alloc.reg_map.at(c.dst);
        int s = ctx.alloc.reg_map.at(c.src);
        if (d != s) pending[d] = s;
    }

    // Emitir copias cuyo destino no es fuente de ninguna otra (no introduce RAW)
    bool changed = true;
    while (changed && !pending.empty()) {
        changed = false;
        for (auto it = pending.begin(); it != pending.end(); ) {
            int d = it->first;
            int s = it->second;
            // Seguro si d no es fuente de otra copia pendiente
            bool d_is_src = false;
            for (const auto &p : pending) {
                if (p.first != d && p.second == d) { d_is_src = true; break; }
            }
            if (!d_is_src) {
                ctx.out << "    mov " << reg_name(d) << ", " << reg_name(s) << "\n";
                it = pending.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    // Paso 4: romper ciclos con r14 como temporal
    while (!pending.empty()) {
        auto it   = pending.begin();
        int start = it->first;
        // Guardar el valor inicial del primer registro del ciclo en r14
        ctx.out << "    mov " << reg_name(SCRATCH_REG) << ", " << reg_name(start) << "\n";
        int cur = start;
        for (;;) {
            int nxt = pending.at(cur);
            pending.erase(cur);
            if (nxt == start) {
                // Fin del ciclo: restaurar desde r14
                ctx.out << "    mov " << reg_name(cur) << ", " << reg_name(SCRATCH_REG) << "\n";
                break;
            }
            ctx.out << "    mov " << reg_name(cur) << ", " << reg_name(nxt) << "\n";
            cur = nxt;
        }
    }
}

// =========================================================================
//  Emision de una instruccion completa
// =========================================================================

// Devuelve true si ins es una CMP cuyo unico uso es la siguiente instruccion
// BR_COND (para fusion cmp+branch).
static bool can_fuse_cmp_brcond(const IrBlock &bb, size_t cmp_idx,
                                  const IrInstr &br_cond_ins) {
    const IrInstr &cmp = bb.instrs[cmp_idx];
    if (cmp.dst == IR_NO_VALUE) return false;
    // La fusion es segura si el resultado cmp no se usa en ningun otro sitio
    // dentro del bloque, aparte de la siguiente instruccion BR_COND.
    // Comprobacion simplificada: solo verificamos que sea el uso inmediato.
    if (!br_cond_ins.operands.empty() && br_cond_ins.operands[0] == cmp.dst)
        return true;
    return false;
}

static void emit_instr(EmitCtx &ctx, const IrBlock &bb, size_t idx,
                        bool &skip_next) {
    skip_next = false;
    const IrInstr &ins = bb.instrs[idx];

    if (ctx.emit_debug && ins.source_line > 0) {
        ctx.out << "    // @line " << ins.source_line << "\n";
    }

    switch (ins.op) {

        // --- NOP ---
        case IrOp::NOP:
            ctx.out << "    nop1\n";
            break;

        // --- CONST ---
        case IrOp::CONST: {
            std::string rd = ctx.dst_of(ins.dst);
            ctx.out << "    mov " << rd << ", " << ins.imm << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        // --- STR_LIT_ADDR ---
        // Carga la direccion VM del literal estatico s_<imm> en el registro
        // destino.  Las etiquetas s_N viven dentro de la unica seccion
        // declarada del modulo ("code"), separadas del codigo ejecutable
        // por una directiva @c align (no por otra @Section, porque el
        // linker .velb actual no escribe datos de secciones secundarias
        // al binario final).  Por tanto se referencian via
        // @Absolute("code.s_N").
        case IrOp::STR_LIT_ADDR: {
            std::string rd = ctx.dst_of(ins.dst);
            ctx.out << "    mov " << rd
                    << ", @Absolute(\"code.s_" << ins.imm << "\")\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        // --- MOV ---
        case IrOp::MOV:
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, rs);
                ctx.store_spilled(ins.dst);
            }
            break;

        // --- Aritmetica entera binaria ---
        case IrOp::ADD: case IrOp::SUB: case IrOp::MUL:
        case IrOp::DIV: case IrOp::MOD:
        case IrOp::AND: case IrOp::OR:  case IrOp::XOR:
        case IrOp::SHL: case IrOp::SHR: case IrOp::SAR:
            if (ins.operands.size() >= 2)
                emit_binop(ctx, arith_mnemonic(ins.op, ins.type),
                           ins.dst, ins.operands[0], ins.operands[1]);
            break;
        // --- Aritmetica flotante binaria (requiere registros ZMM) ---
        case IrOp::FADD: case IrOp::FSUB: case IrOp::FMUL:
        case IrOp::FDIV: case IrOp::FMIN: case IrOp::FMAX:
            if (ins.operands.size() >= 2)
                emit_float_binop(ctx, arith_mnemonic(ins.op, ins.type),
                                  ins.type,
                                  ins.dst, ins.operands[0], ins.operands[1]);
            break;

        // --- Aritmetica entera unaria ---
        case IrOp::NEG: {
            // -x = 0 - x  => mov r_dst, 0; subs r_dst, r_src
            if (ins.operands.empty()) break;
            {
                std::string rd  = ctx.dst_of(ins.dst);
                std::string rs  = ctx.load_src(ins.operands[0], 0);
                ctx.out << "    mov " << rd << ", 0\n";
                ctx.out << "    subs " << rd << ", " << rs << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        // --- Operaciones unarias bitwise/float ---
        case IrOp::NOT:
            if (!ins.operands.empty())
                emit_unop(ctx, "not", ins.dst, ins.operands[0]);
            break;
        case IrOp::FNEG:
            if (!ins.operands.empty())
                emit_float_unop(ctx, "fneg", ins.type,
                                 ins.dst, ins.operands[0]);
            break;
        case IrOp::FABS:
            if (!ins.operands.empty())
                emit_float_unop(ctx, "fabs", ins.type,
                                 ins.dst, ins.operands[0]);
            break;
        case IrOp::FSQRT:
            if (!ins.operands.empty())
                emit_float_unop(ctx, "fsqrt", ins.type,
                                 ins.dst, ins.operands[0]);
            break;
        // --- Conversion de tipos ---
        case IrOp::CAST: case IrOp::ZEXT: case IrOp::SEXT:
        case IrOp::TRUNC: {
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, rs);
                ctx.store_spilled(ins.dst);
            }
            break;
        }
        case IrOp::ITOF:
        case IrOp::UITOF:
            // int VALOR (en GP) -> bits IEEE 754 (en GP).  Pasos:
            //   fcvt[.ps]  gp_src, f0  ; r1=GP -> direction=0 (GP->ZMM):
            //                            write_f64 si destino F64,
            //                            write_f32 si destino F32 (.ps)
            //   bitcast f0 -> gp_dst   ; mueve bits IEEE al GP destino
            // Convencion de fcvt (emit_instr_fcvt en src/emmit/emmit_decl.cpp:2086):
            //   r1=ZMM -> direction=1 (zmm->gp), r1=GP -> direction=0 (gp->zmm).
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                const std::string suffix = (ins.type == IrType::F32) ? ".ps" : "";
                ctx.out << "    fcvt" << suffix << " " << rs << ", f0\n";
                emit_zmm_to_gp_bits(ctx, "f0", rd);
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::FTOI:
        case IrOp::FTOUI:
            // bits IEEE 754 (en GP) -> int VALOR truncado (en GP).  Pasos:
            //   bitcast gp_src -> f0   ; bits a ZMM como float
            //   fcvt[.ps]  f0, gp_dst  ; r1=ZMM -> direction=1 (zmm->gp):
            //                            read_f64 si fuente F64,
            //                            read_f32 si fuente F32 (.ps)
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                const IrType ot = ctx.fn.values[ins.operands[0]].type;
                const std::string suffix = (ot == IrType::F32) ? ".ps" : "";
                emit_gp_to_zmm_bits(ctx, rs, "f0");
                ctx.out << "    fcvt" << suffix << " f0, " << rd << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::BITCAST: {
            // BITCAST entre tipos del MISMO ancho (e.g. f64<->i64): copia
            // de bits sin re-interpretacion.  emit_mov_if_needed basta.
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, rs);
                ctx.store_spilled(ins.dst);
            }
            break;
        }
        case IrOp::F32TOF64:
            // f32 (4 bytes IEEE en bajo de GP) -> f64 (8 bytes IEEE en GP).
            // Conversion REAL: el patron de bits cambia (re-bias del exp,
            // shift de mantissa).  Pasos:
            //   bitcast GP -> f0    ; f32 bits en bytes bajos de f0
            //   fextend  f1, f0     ; f1 = (double)(f32)f0
            //   bitcast f1 -> GP    ; 8 bytes IEEE 754 al GP destino
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                emit_gp_to_zmm_bits(ctx, rs, "f0");
                ctx.out << "    fextend f1, f0\n";
                emit_zmm_to_gp_bits(ctx, "f1", rd);
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::F64TOF32:
            // f64 (8 bytes IEEE en GP) -> f32 (4 bytes IEEE en bajo de GP).
            // Conversion REAL con perdida de precision; los 4 bytes altos
            // del GP destino quedan en cero (write_f32 zerifica el resto
            // del ZMM antes del bitcast inverso).
            //   bitcast GP -> f0    ; f64 bits en f0
            //   fnarrow  f1, f0     ; f1 = (float)(double)f0
            //   bitcast f1 -> GP    ; 4 bytes f32 + 4 bytes cero al GP
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                emit_gp_to_zmm_bits(ctx, rs, "f0");
                ctx.out << "    fnarrow f1, f0\n";
                emit_zmm_to_gp_bits(ctx, "f1", rd);
                ctx.store_spilled(ins.dst);
            }
            break;

        // --- Comparaciones (standalone, no fusionadas) ---
        case IrOp::CMP_EQ:  case IrOp::CMP_NE:
        case IrOp::CMP_LT:  case IrOp::CMP_GT:
        case IrOp::CMP_LE:  case IrOp::CMP_GE:
        case IrOp::CMP_ULT: case IrOp::CMP_UGT:
        case IrOp::CMP_ULE: case IrOp::CMP_UGE:
        case IrOp::FCMP_EQ: case IrOp::FCMP_NE:
        case IrOp::FCMP_LT: case IrOp::FCMP_GT:
        case IrOp::FCMP_LE: case IrOp::FCMP_GE: {
            // Intentar fusion con la siguiente instruccion BR_COND
            if (idx + 1 < bb.instrs.size()) {
                const IrInstr &next = bb.instrs[idx + 1];
                if (next.op == IrOp::BR_COND
                    && can_fuse_cmp_brcond(bb, idx, next)) {
                    // Fusion: emitir cmp + salto condicional ahora.
                    //
                    // BUG critico arreglado (2026-05-04): las copias PHI
                    // para AMBOS sucesores deben emitirse en sus puntos
                    // correspondientes.  Antes solo se emitian para
                    // @c target_block (true branch), dejando el
                    // @c false_block sin copias -> los PHIs de los
                    // bloques destino veian valores stale del predecesor
                    // cuando el salto condicional caia al false branch.
                    // Sintoma: loops con if dentro divergen (j no
                    // incrementa, i toma valor de j, etc.).
                    //
                    // Layout post-fix:
                    //   cmp ...
                    //   <copias phi del FALSE branch>
                    //   <salto condicional invertido al false_block>
                    //   <copias phi del TRUE branch>
                    //   jmp target_block
                    //
                    // El salto condicional usa la condicion INVERTIDA
                    // porque por convencion saltamos al false branch
                    // cuando cmp falla (y caemos al true branch).  Las
                    // copias del false branch deben emitirse antes del
                    // salto: si saltamos, queremos que se hayan ya
                    // ejecutado.  Las del true branch se emiten despues
                    // (no se pisan porque solo ejecutan si NO saltamos).
                    if (ins.operands.size() >= 2) {
                        const char *cmp_mn = cmp_mnemonic(ins.op);
                        std::string ra = ctx.load_src(ins.operands[0], 0);
                        std::string rb = ctx.load_src(ins.operands[1], 1);
                        const bool is_fcmp_fused =
                            (ins.op == IrOp::FCMP_EQ || ins.op == IrOp::FCMP_NE
                          || ins.op == IrOp::FCMP_LT || ins.op == IrOp::FCMP_GT
                          || ins.op == IrOp::FCMP_LE || ins.op == IrOp::FCMP_GE);
                        if (is_fcmp_fused) {
                            // FCMP fusionado con BR_COND: bitcast a ZMM antes
                            // de comparar.  Selecciona ".ps" si operandos F32.
                            const IrType ot = ctx.fn.values[ins.operands[0]].type;
                            const std::string suffix = (ot == IrType::F32) ? ".ps" : "";
                            emit_gp_to_zmm_bits(ctx, ra, "f0");
                            emit_gp_to_zmm_bits(ctx, rb, "f1");
                            ctx.out << "    fcmp" << suffix << " f0, f1\n";
                        } else {
                        ctx.out << "    " << cmp_mn << " " << ra << ", " << rb << "\n";
                        }
                        IrBlockId bid = static_cast<IrBlockId>(
                            &bb - ctx.fn.blocks.data());
                        // Copias phi para el false branch ANTES del salto
                        // condicional (que va a ese bloque).
                        emit_phi_copies(ctx, bid, next.false_block);
                        emit_cond_branch(ctx, ins.op,
                                         ctx.block_label(next.false_block));
                        // Copias phi para el true branch (cae aqui si la
                        // condicion no se cumplio).
                        emit_phi_copies(ctx, bid, next.target_block);
                        ctx.out << "    jmp @Absolute(\""
                                << EmitCtx::abs_lbl(ctx.block_label(next.target_block)) << "\")\n";
                    }
                    skip_next = true; // ya procesamos la siguiente instruccion
                    return;
                }
            }
            // Sin fusion: emitir comparacion como valor booleano
            emit_cmp_standalone(ctx, ins);
            break;
        }

        // --- Flujo de control ---
        case IrOp::BR: {
            IrBlockId bid = static_cast<IrBlockId>(&bb - ctx.fn.blocks.data());
            emit_phi_copies(ctx, bid, ins.target_block);
            ctx.out << "    jmp @Absolute(\""
                    << EmitCtx::abs_lbl(ctx.block_label(ins.target_block)) << "\")\n";
            break;
        }

        case IrOp::BR_COND: {
            // BR_COND no fusionada: el valor condicion es un bool (0 o 1)
            // Comparar r_cond con 0
            if (ins.operands.empty()) break;
            std::string rc  = ctx.load_src(ins.operands[0], 0);
            IrBlockId   bid = static_cast<IrBlockId>(&bb - ctx.fn.blocks.data());
            ctx.out << "    mov r14, 0\n";
            ctx.out << "    cmpu " << rc << ", r14\n";
            emit_phi_copies(ctx, bid, ins.false_block);
            ctx.out << "    jmp.je @Absolute(\""
                    << EmitCtx::abs_lbl(ctx.block_label(ins.false_block)) << "\")\n";
            emit_phi_copies(ctx, bid, ins.target_block);
            ctx.out << "    jmp @Absolute(\""
                    << EmitCtx::abs_lbl(ctx.block_label(ins.target_block)) << "\")\n";
            break;
        }

        case IrOp::RET: {
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                emit_mov_if_needed(ctx, "r0", rs);
            }
            ctx.out << "    jmp @Absolute(\"" << EmitCtx::abs_lbl(ctx.fn_lbl + "_ret") << "\")\n";
            break;
        }

        case IrOp::UNREACHABLE:
            ctx.out << "    hlt\n";
            break;

        // --- PHI: ya se manejo en emit_phi_copies; aqui es un no-op ---
        case IrOp::PHI:
            // Las copias se emitieron en los predecesores antes del salto
            break;

        // --- Llamadas ---
        //
        // Estructura comun para todas las variantes (CALL/CALLIND/CALLVIRT/CALLN/TAILCALL):
        //
        //   1. Calcular call_pos lineal (necesario para liveness).
        //   2. Identificar registros r0..r12 con valores vivos a traves del call
        //      (excluyendo el dst del propio call) y emitir un 'push' por cada uno.
        //   3. Pre-cargar fuentes de los argumentos (incluyendo spills) y emitir
        //      los moves a r1..r12 con resolucion de conflictos parallel-move.
        //   4. Emitir el callvm / tailcall / callvirt / calln segun corresponda.
        //   5. Mover r0 al destino si el call produce valor.
        //   6. Emitir 'pop' en orden inverso para restaurar los registros salvados.
        //
        // Los pasos 2 y 6 son la correccion del bug del regalloc en presencia
        // de valores vivos a traves de un CALL.  Antes, el move de un argumento
        // a r1 podia pisar un parametro que se necesitaba despues del call.

        case IrOp::CALL:
        case IrOp::TAILCALL: {
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            // 2. Save: push de cada registro caller-saved con valor vivo.
            // regs con is_gc_object usan gchandle antes del push.
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);

            // 3. Argument marshalling con parallel-move.
            const size_t nargs = std::min(ins.operands.size(), (size_t)12);
            std::vector<std::pair<int, std::string>> moves;
            moves.reserve(nargs);
            for (size_t ai = 0; ai < nargs; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai], 0);
                moves.emplace_back(static_cast<int>(ai + 1), r_arg);
            }
            emit_parallel_arg_moves(ctx, std::move(moves));

            // 4. argc + call.
            ctx.out << "    mov r15, " << nargs << "\n";
            if (ins.op == IrOp::TAILCALL) {
                // solo emitir leave si se emitio enter (has_frame).
                if (ctx.has_frame) {
                    ctx.out << "    leave\n";
                }
                // Cargar direccion en r0 y usar tailcall de registro (unica forma soportada).
                ctx.out << "    mov r0, @Absolute(\""
                        << EmitCtx::abs_lbl(EmitCtx::sanitize(ins.func_name)) << "\")\n";
                ctx.out << "    tailcall r0\n";
                // En tailcall no hay codigo posterior; los push previos los heredara
                // el callee, lo que rompe la pila.  Por seguridad, NO emitimos push
                // antes de un TAILCALL: en SSA clasico un tailcall implica que su
                // resultado es el ultimo uso de la funcion, asi que no hay valores
                // vivos despues.  Si live_regs_through_call devolvio algo, es bug
                // upstream; aun asi, en TAILCALL los pushes ya emitidos seran
                // popeados nunca, lo que es incorrecto.  Para A.1 los tailcalls
                // los emite explicitamente el optimizador IR y mantienen el invariante.
            } else {
                ctx.out << "    callvm @Absolute(\""
                        << EmitCtx::abs_lbl(EmitCtx::sanitize(ins.func_name)) << "\")\n";
                // 5. Mover r0 al destino si lo hay.  IMPORTANTE: hacerlo ANTES del
                // pop, porque despues de los pops r0 podria haber sido modificado
                // por el restore (no, push/pop no tocan r0, pero por orden).
                if (ins.dst != IR_NO_VALUE) {
                    std::string rd = ctx.dst_of(ins.dst);
                    emit_mov_if_needed(ctx, rd, "r0");
                    ctx.store_spilled(ins.dst);
                }
                // 6. Restore en orden inverso.  A.32.fix: refresh GC ptrs.
                emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            }
            break;
        }

        case IrOp::CALLIND: {
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            // El puntero de funcion debe materializarse a un registro ANTES de los
            // pushes (porque load_src puede usar SCRATCH y los pushes no lo afectan,
            // pero queremos un orden determinista).
            std::string rfn = ctx.load_src(ins.func_ptr, 0);

            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);

            const size_t nargs = std::min(ins.operands.size(), (size_t)12);
            std::vector<std::pair<int, std::string>> moves;
            moves.reserve(nargs);
            for (size_t ai = 0; ai < nargs; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai], 0);
                moves.emplace_back(static_cast<int>(ai + 1), r_arg);
            }
            emit_parallel_arg_moves(ctx, std::move(moves));

            ctx.out << "    mov r15, " << nargs << "\n";
            // CALLIND: el puntero de funcion vive en un registro -> usamos
            // @c callvmr (REG mode, 2 bytes) y NO @c callvm (INMED mode,
            // 10 bytes que espera una direccion absoluta literal).
            ctx.out << "    callvmr " << rfn << "\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            break;
        }

        case IrOp::CALLCLOSURE: {
            // closures: identico a CALLIND mas un `mov r14, env_ptr`
            // antes del callvmr.  La calling convention de los helpers
            // sintetizados (__lambda_<N>) es:
            //   r1..r12 = args declarados (max 12)
            //   r14     = env_ptr (puntero al bloque de captures, 0 si la
            //             lambda no captura nada)
            //   r15     = nargs (consistencia con CALL/CALLIND)
            //
            // La distribucion de operands en el IR es:
            //   ins.func_ptr   = SSA value con la direccion del helper
            //   ins.operands[0] = SSA value con env_ptr (0 si sin captures)
            //   ins.operands[1..] = args declarados
            //
            // Esto reusa todo el regalloc de CALLIND (parallel-move,
            // save/restore de live regs) y solo anade el move a r14.
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            // Materializar fn_addr y env_addr a registros antes de los pushes
            // y de los moves de argumentos para evitar conflictos.
            std::string rfn = ctx.load_src(ins.func_ptr, 0);
            std::string renv;
            if (!ins.operands.empty() && ins.operands[0] != IR_NO_VALUE) {
                renv = ctx.load_src(ins.operands[0], 1);
            }

            // CRITICO: si el regalloc puso fn_addr en r14, NUESTRO
            // `mov r14, env` (mas abajo) lo sobrescribira.  Evacuamos
            // fn_addr a r13 (registro no usado por la calling convention
            // ni por env) ANTES de tocar r14.
            //
            // Igualmente critico: si fn_addr quedo en r1..r12 (un slot
            // de argumento), el parallel-move de abajo lo destruira al
            // colocar el arg correspondiente.  Detectado en A.15 al
            // ejecutar `add5(10)` donde el `mov r1, [r2]` cargaba
            // fn_addr en r1 y luego `mov r1, r7` (parallel move del
            // arg=10) lo sobrescribia.  Mismo fix: evacuar a r13.
            // r13 nunca es target de la calling convention (r1..r12
            // args, r14 env, r15 nargs, r0 retorno) asi que es seguro.
            const size_t nargs_check = ins.operands.size() > 0
                                       ? std::min(ins.operands.size() - 1, (size_t)12)
                                       : 0;
            bool fn_in_arg_slot = false;
            if (rfn.size() >= 2 && rfn[0] == 'r') {
                const int rn = std::atoi(rfn.c_str() + 1);
                if (rn >= 1 && rn <= static_cast<int>(nargs_check)) {
                    fn_in_arg_slot = true;
                }
            }
            if (rfn == "r14" || fn_in_arg_slot) {
                ctx.out << "    mov r13, " << rfn << "\n";
                rfn = "r13";
            }

            // Save de live regs PRIMERO (igual que CALL/CALLIND).  El
            // orden importa para el balance del stack: tras el callvmr,
            // los pops se hacen en orden inverso al push.
            // fix: regs con is_gc_object usan gchandle antes del push.
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);

            // Si env esta en r1..r12 lo pusheamos AHORA (encima de los
            // live regs ya guardados).  El parallel-move puede clobbear
            // r1..r12 al colocar args, asi que necesitamos preservarlo.
            // Lo recuperaremos al r14 con pop INMEDIATAMENTE antes del
            // callvmr (despues del parallel-move) para que sea el TOP
            // del stack en ese momento.
            bool env_pushed = false;
            if (!renv.empty() && renv != "r13" && renv != "r14") {
                if (renv.size() >= 2 && renv[0] == 'r') {
                    int rn = std::atoi(renv.c_str() + 1);
                    if (rn >= 1 && rn <= 12) {
                        ctx.out << "    push " << renv << "\n";
                        env_pushed = true;
                    }
                }
            }

            // Args declarados: comienzan en operands[1] porque operands[0]
            // es el env.  Acotamos a 12 (limite del calling convention).
            const size_t total = ins.operands.size();
            const size_t nargs_decl = total > 0 ? std::min(total - 1, (size_t)12) : 0;
            std::vector<std::pair<int, std::string>> moves;
            moves.reserve(nargs_decl);
            for (size_t ai = 0; ai < nargs_decl; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai + 1], 0);
                moves.emplace_back(static_cast<int>(ai + 1), r_arg);
            }
            emit_parallel_arg_moves(ctx, std::move(moves));

            // Colocar el env_ptr en r14.  El pop saca el ultimo push
            // (env si env_pushed) que es el TOP correcto.
            if (env_pushed) {
                ctx.out << "    pop r14\n";
            } else if (!renv.empty()) {
                emit_mov_if_needed(ctx, "r14", renv);
            } else {
                ctx.out << "    mov r14, 0\n";
            }

            ctx.out << "    mov r15, " << nargs_decl << "\n";
            ctx.out << "    callvmr " << rfn << "\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            break;
        }

        case IrOp::CALLVIRT: {
            if (ins.operands.empty()) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            // fix.order - SAVE primero (libera scratches r14/r13).
            // load_src del receiver/args puede usar libremente esos
            // scratches sin que mi gchandle los pise: la conversion del
            // contenido del slot stack a host_ptr fresco la hace
            // load_src internamente (gcderef + xchg) cuando is_gc_object.
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);

            // Materializar el objeto receptor (load_src ya hace gcderef
            // si esta spilled e is_gc_object).
            std::string r_obj = ctx.load_src(ins.operands[0], 0);

            // Convencion del frontend Vex: el metodo recibe `this` como
            // primer parametro (r1) y los argumentos declarados a partir
            // de r2.  Por eso colocamos obj en r1 y los demas operandos
            // en r2, r3, ...  El parallel-move resuelve cualquier
            // reordenamiento (ej. arg que ya esta en r1 por live ranges).
            const size_t nargs = ins.operands.size() > 1
                                  ? std::min(ins.operands.size() - 1, (size_t)12) : 0;
            std::vector<std::pair<int, std::string>> moves;
            moves.reserve(nargs + 1);
            // r1 = this
            moves.emplace_back(1, r_obj);
            // r2..r_{N+1} = args declarados
            for (size_t ai = 0; ai < nargs; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai + 1], 0);
                moves.emplace_back(static_cast<int>(ai + 2), r_arg);
            }
            emit_parallel_arg_moves(ctx, std::move(moves));

            ctx.out << "    mov r15, " << (nargs + 1) << "\n";
            // El callvirt recibe el receptor en r1 (ya colocado por los
            // moves) y el indice del slot en la vtable.
            ctx.out << "    callvirt r1, " << ins.imm << "\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            break;
        }

        case IrOp::CALLM: {
            // Dispatch dinamico via MethodInfo* directo (A.5.2.b: interfaces,
            // y reflexion runtime).  Layout de operands en el IR:
            //   operands[0] = obj (host_ptr a ObjectHeader)
            //   operands[1] = method (MethodInfo* obtenido via findmethod)
            //   operands[2..] = args declarados (van a r2..r_{N+1})
            // El bytecode `callm r1, r_method` usa r1=obj y un reg con el
            // MethodInfo*.  Movemos obj a r1, args a r2..r_{N+1} y el method
            // a un scratch (r13) para no chocar con la calling convention.
            if (ins.operands.size() < 2) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            std::string r_obj = ctx.load_src(ins.operands[0], 0);
            std::string r_meth_src = ctx.load_src(ins.operands[1], 1);

            // fix: regs con is_gc_object usan gchandle antes del push.
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);

            // Mover MethodInfo* a un reg fijo (r13) que sobrevive el
            // marshalling de args.  r13 es SCRATCH2 del emisor.
            ctx.out << "    mov r13, " << r_meth_src << "\n";

            // Calling convention identica a CALLVIRT: r1 = this, args en r2..
            const size_t nargs = ins.operands.size() > 2
                                  ? std::min(ins.operands.size() - 2, (size_t)11) : 0;
            std::vector<std::pair<int, std::string>> moves;
            moves.reserve(nargs + 1);
            moves.emplace_back(1, r_obj);
            for (size_t ai = 0; ai < nargs; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai + 2], 0);
                moves.emplace_back(static_cast<int>(ai + 2), r_arg);
            }
            emit_parallel_arg_moves(ctx, std::move(moves));

            ctx.out << "    mov r15, " << (nargs + 1) << "\n";
            ctx.out << "    callm r1, r13\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            break;
        }

        case IrOp::CALLN: {
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            // FFI runtime indirect: si func_name empieza con
            // "__callni__", el primer operand es el puntero a funcion (ya
            // resuelto por el usuario via ffi_sym/dlsym) y los siguientes
            // son los args.  En vez de emitir `calln @Method("...")`
            // (resuelto en compile-time por el linker), emitimos
            // `callni reg_fn` (puntero leido en runtime).  Misma calling
            // convention: argc en R15, args en R01..R12, retorno en R00.
            const bool is_indirect =
                ins.func_name.size() >= 11
             && ins.func_name.compare(0, 11, "__callni__:") == 0;
            const size_t arg_offset = is_indirect ? 1 : 0;
            const size_t nargs = std::min(
                ins.operands.size() - arg_offset, (size_t)12);

            // fix.b - materializar el puntero de funcion ANTES de
            // emit_save_live_regs y del parallel-move.  Si el regalloc lo
            // puso en un reg que sera arg target (r1..r_N) o en r14
            // (scratch del cycle-breaking), evacuarlo a r13 que NO es
            // tocado por la calling convention.  Mismo fix que CALLCLOSURE.
            // Sin esto, el parallel-move colocaba un arg en el reg del
            // fn-ptr y `callni` saltaba a memoria invalida.
            std::string rfn;
            if (is_indirect) {
                rfn = ctx.load_src(ins.operands[0], 0);
                bool fn_in_arg_slot = false;
                if (rfn.size() >= 2 && rfn[0] == 'r') {
                    const int rn = std::atoi(rfn.c_str() + 1);
                    if (rn >= 1 && rn <= static_cast<int>(nargs)) {
                        fn_in_arg_slot = true;
                    }
                }
                if (rfn == "r14" || fn_in_arg_slot) {
                    ctx.out << "    mov r13, " << rfn << "\n";
                    rfn = "r13";
                }
            }

            // fix: regs con is_gc_object usan gchandle antes del push.
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);

            std::vector<std::pair<int, std::string>> moves;
            moves.reserve(nargs);
            for (size_t ai = 0; ai < nargs; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai + arg_offset], 0);
                moves.emplace_back(static_cast<int>(ai + 1), r_arg);
            }
            emit_parallel_arg_moves(ctx, std::move(moves));

            ctx.out << "    mov r15, " << nargs << "\n";
            if (is_indirect) {
                ctx.out << "    callni " << rfn << "\n";
            } else {
                ctx.out << "    calln @Method(\"" << ins.func_name << "\")\n";
            }
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            break;
        }

        // --- Memoria ---
        case IrOp::ALLOCA: {
            // Reservar espacio en pila.  Tamano = count * sizeof(type).
            // El frontend Vex pasa type=i8, imm=N para reservar N bytes
            // (variables struct); otros frontends pueden usar
            // type=i64, imm=N para arrays de N qwords.
            const uint64_t bytes = ins.imm * ir_type_size(ins.type);
            ctx.out << "    subsp rsp, " << bytes << "\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                // Capturar la direccion base de la zona reservada.  Tras
                // subsp, rsp apunta justo al inicio del nuevo bloque
                // (la pila crece "hacia abajo").  Hacemos `mov rd, rsp`
                // y los call sites pueden usar `[rd]` / `[rd + offset]`
                // para acceder a los slots.  No usamos readcur porque
                // los cursores son utiles cuando el offset es dinamico
                // (acceso a campos de objeto GC, p.ej.); para variables
                // de pila la direccion base directa es mas simple y
                // produce .vel mas pequeno.
                ctx.out << "    mov " << rd << ", rsp\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        case IrOp::LOAD: {
            if (ins.operands.empty()) break;
            std::string rp  = ctx.load_src(ins.operands[0], 0);
            // Tamano del LOAD segun ins.type.  Sin sufijo el parser
            // asume 64 bits y leeria mas alla del campo destino,
            // leyendo basura o causando segfault al tocar memoria no
            // mapeada.
            std::string rd_full = ctx.dst_of(ins.dst);
            std::string rd_sz   = ctx.is_in_reg(ins.dst)
                                    ? reg_name_sized(ctx.reg_num(ins.dst), ins.type)
                                    : rd_full;
            // La VM NO hace zero-extend en `mov rXd/w/b, [src]` (a
            // diferencia de x86-64 con la mitad inferior).  Los bits
            // altos del registro destino conservan su valor previo,
            // contaminando operaciones aritmeticas posteriores.  Si el
            // tipo cargado es < 64 bits, hacemos zero-extend manual
            // poniendo el registro entero a 0 antes del load.
            const size_t tsz = ir_type_size(ins.type);
            if (tsz < 8) {
                ctx.out << "    mov " << rd_full << ", 0\n";
            }
            // Si el puntero apunta a memoria HOST (resultado de raw_alloc o
            // derivado por aritmetica), usar `movh` (s=1) en lugar de `mov`
            // para que el ejecutor lea desde el espacio del proceso host
            // y no desde la memoria virtual de la VM.
            const bool host_ptr =
                ins.operands[0] != IR_NO_VALUE
             && ctx.fn.values[ins.operands[0]].is_host_ptr;
            const char *opcode = host_ptr ? "movh" : "mov";
            ctx.out << "    " << opcode << " " << rd_sz << ", [" << rp << "]\n";
            // Sign-extension manual para tipos signed < 64 bits.  Sin esto
            // los i8/i16/i32 con valores negativos se cargan con bits
            // altos a 0 (debido al zero-extend manual de arriba), y
            // operaciones signed posteriores como cmps o adds tratan al
            // valor como su representacion sin signo (e.g. -1 i32 se
            // convierte en 4294967295 i64).  La fix es shl + sar por
            // (64 - bits del tipo) que propaga el bit de signo.  Solo se
            // aplica a I8/I16/I32 (no a U*); I64 ya es full width.
            if (tsz < 8 && (ins.type == IrType::I8
                         || ins.type == IrType::I16
                         || ins.type == IrType::I32)) {
                const unsigned shift_bits = static_cast<unsigned>(64 - tsz * 8);
                // SHL/SAR de la VM solo aceptan reg-reg, no inmediatos.
                // Cargamos la cuenta en SCRATCH_REG (r14) que esta libre
                // entre ops del IR (las operaciones lo restauran sus
                // propios load_src).  Tras shl+sar el destino contiene
                // el valor con sign-extension propagado.
                const std::string scratch = reg_name(SCRATCH_REG);
                ctx.out << "    mov " << scratch << ", " << shift_bits << "\n";
                ctx.out << "    shl " << rd_full << ", " << scratch << "\n";
                ctx.out << "    sar " << rd_full << ", " << scratch << "\n";
            }
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STORE: {
            if (ins.operands.size() < 2) break;
            std::string rv = ctx.load_src(ins.operands[0], 0); // valor a escribir
            std::string rp = ctx.load_src(ins.operands[1], 1); // puntero destino
            // Sufijo de tamano para que mov escriba exactamente sizeof(type)
            // bytes (no 8 por defecto).  Igual que en LOAD.
            std::string rv_sized = rv;
            if (ctx.is_in_reg(ins.operands[0])) {
                rv_sized = reg_name_sized(ctx.reg_num(ins.operands[0]), ins.type);
            }
            const bool host_ptr =
                ins.operands[1] != IR_NO_VALUE
             && ctx.fn.values[ins.operands[1]].is_host_ptr;
            const char *opcode = host_ptr ? "movh" : "mov";
            ctx.out << "    " << opcode << " [" << rp << "], " << rv_sized << "\n";
            break;
        }

        case IrOp::RAW_ALLOC: {
            // Emite: alloc <r_size>;  el bytecode `alloc` (opcode 0xB0)
            // lee el tamano del registro indicado y deja el puntero host
            // en r0.  Movemos el resultado al destino SSA si lo hay.
            if (ins.operands.empty()) break;
            std::string r_size = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    alloc " << r_size << "\n";
            if (ins.dst != IR_NO_VALUE) {
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        case IrOp::RAW_FREE: {
            // Emite: free <r_ptr>;  el bytecode `free` (opcode 0xB1)
            // libera el bloque devuelto previamente por alloc.
            if (ins.operands.empty()) break;
            std::string r_ptr = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    free " << r_ptr << "\n";
            break;
        }

        case IrOp::MEMCPY: {
            if (ins.operands.size() < 3) break;
            std::string r_dst_ = ctx.reg_of(ins.operands[0]);
            std::string r_src_ = ctx.reg_of(ins.operands[1]);
            std::string r_len_ = ctx.reg_of(ins.operands[2]);
            ctx.out << "    vmcopy " << r_dst_ << ", " << r_src_ << ", " << r_len_ << "\n";
            break;
        }

        // --- OOP / GC ---
        case IrOp::NEWOBJ: {
            // fix5 - NEWOBJ internamente llama @c gc_heap.alloc() que
            // puede disparar minor/major GC (evacuacion de YOUNG -> OLD).
            // Sin save_live_regs alrededor, los regs vivos a traves del
            // newobj se pierden si la GC mueve el objeto que apuntan.
            //
            // Fix: tratar NEWOBJ como cualquier otro CALL.  Identifica los
            // regs vivos (excluyendo dst), guarda los que contienen GC
            // host_ptrs como handles (estables a evacuacion), restaura tras
            // el newobj.  El reg de dst recibe el GcHandle nuevo en r0.
            //
            // Antes de este fix, un patron como
            //   ResourceA owned = make_a();    // owned vivo en main
            //   while (i<N) { i64 obj = newInstance(cls); ... }
            // crasheaba porque el `mov r1, cls` para newobj clobeaba r1
            // que contenia owned, y al RET el cleanup de owned leia garbage.
            if (ins.operands.empty()) break;
            const uint32_t call_pos = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);
            // Excluir el reg que llevara r_cls (lo movemos manualmente
            // a r1 antes del newobj; preservarlo seria redundante).
            // El parallel-move es trivial: un solo arg.
            std::string r_cls = ctx.reg_of(ins.operands[0]);
            emit_save_live_regs(ctx, call_pos, regs_to_save);
            ctx.out << "    mov r1, " << r_cls << "\n";
            ctx.out << "    mov r15, 1\n";
            ctx.out << "    newobj r1\n";
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            emit_restore_live_regs(ctx, call_pos, regs_to_save);
            break;
        }

        case IrOp::GETFIELD: {
            // Restaurada la version que funciona con GcHandle (codigo .vel
            // manual de POO).  El frontend Vex calcula el puntero al
            // campo via ADD antes de invocar LOAD para usar la ruta de
            // memoria host (movh) sin cur0.  Por tanto este case sigue
            // funcionando para handles tipicos.
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_obj = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    gcderef cur0, " << r_obj << "\n";
            if (ins.imm) ctx.out << "    addcur cur0, " << ins.imm << "\n";
            ctx.out << "    readcur " << rd << ", cur0\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::SETFIELD: {
            // Version original (GcHandle): el frontend Vex calcula el
            // puntero via ADD y usa STORE con is_host_ptr=true en lugar
            // de SETFIELD para acceso directo via host pointer.
            if (ins.operands.size() < 2) break;
            std::string r_obj = ctx.load_src(ins.operands[0], 0);
            std::string r_val = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    gcderef cur0, " << r_obj << "\n";
            if (ins.imm) ctx.out << "    addcur cur0, " << ins.imm << "\n";
            ctx.out << "    writecur cur0, " << r_val << "\n";
            if (ins.type == IrType::HANDLE)
                ctx.out << "    gcwb " << r_obj << "\n";
            break;
        }

        case IrOp::INSTANCEOF: {
            if (ins.operands.size() < 2) break;
            std::string rd    = ctx.reg_of(ins.dst);
            std::string r_obj = ctx.reg_of(ins.operands[0]);
            std::string r_cls = ctx.reg_of(ins.operands[1]);
            ctx.out << "    instanceof " << rd << ", " << r_obj << ", " << r_cls << "\n";
            break;
        }

        case IrOp::CHECKCAST: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    checkcast " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        }

        case IrOp::ISNULL: {
            if (!ins.operands.empty())
                ctx.out << "    isnull " << ctx.reg_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        }

        case IrOp::UNWRAP: {
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                ctx.out << "    unwrap " << rd << ", " << rs << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        case IrOp::SPECIALIZE: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    specialize " << ctx.reg_of(ins.dst)
                    << ", " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        }

        // --- GEP / write barrier / arrays ---

        case IrOp::GEP: {
            // %ptr = gep.ptr %handle, byte_offset
            // Emite gcderef + addcur; el cursor cur0 queda apuntando al campo.
            // El %ptr resultante es un marcador; usar LOAD/STORE inmediatamente despues.
            if (ins.operands.empty()) break;
            std::string r_obj = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    gcderef cur0, " << r_obj << "\n";
            if (ins.imm) ctx.out << "    addcur cur0, " << ins.imm << "\n";
            break;
        }

        case IrOp::GCWB_IR: {
            // gcwb_ir %handle  ->  gcwb r_handle
            if (!ins.operands.empty())
                ctx.out << "    gcwb " << ctx.load_src(ins.operands[0], 0) << "\n";
            break;
        }

        case IrOp::GCDEREF_IR: {
            // gcderef_ir %handle  ->  gcderef cur0, r_handle
            // Nota: no hay instruccion VM para exportar cur0 a registro general.
            // Este opcode es util solo cuando seguido de readcur/writecur via RAW_ASM.
            if (!ins.operands.empty())
                ctx.out << "    gcderef cur0, " << ctx.load_src(ins.operands[0], 0) << "\n";
            break;
        }

        case IrOp::ARRAY_ALLOC: {
            // %h = array_alloc.T %len
            // Layout en memoria VM: [u64 length][data[len * sizeof(T)]]
            // Delega a helper nativo stdlib/native/array/vesta_array:va_alloc(proc, esize, count)
            std::string r_len = ins.operands.empty() ? "r0" : ctx.load_src(ins.operands[0], 0);
            uint64_t esize = ir_type_size(ins.type);
            ctx.out << "    getproc r1\n";
            ctx.out << "    mov r2, " << esize << "\n";
            emit_mov_if_needed(ctx, "r3", r_len);
            ctx.out << "    mov r15, 3\n";
            ctx.out << "    calln @Method(\"stdlib/native/array/vesta_array:va_alloc\")\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        case IrOp::ARRAY_LEN: {
            // longitud del array: offset 0 contiene el campo length (u64)
            if (ins.operands.empty()) break;
            std::string r_arr = ctx.load_src(ins.operands[0], 0);
            std::string rd    = ctx.dst_of(ins.dst);
            ctx.out << "    mov " << rd << ", [" << r_arr << "]\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::ARRAY_LOAD: {
            // direccion del elemento: r_arr + r_idx * stride + 8 (los primeros 8 bytes son el campo length)
            if (ins.operands.size() < 2) break;
            std::string r_arr = ctx.load_src(ins.operands[0], 0);
            std::string r_idx = ctx.load_src(ins.operands[1], 1);
            std::string rd    = ctx.dst_of(ins.dst);
            uint64_t stride   = ir_type_size(ins.type);
            ctx.out << "    mov r13, " << r_idx << "\n";
            if (stride > 1)
                ctx.out << "    mulu r13, " << stride << "\n";
            ctx.out << "    addu r13, 8\n";
            ctx.out << "    addu r13, " << r_arr << "\n";
            ctx.out << "    mov " << rd << ", [r13]\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::ARRAY_STORE: {
            // escritura de elemento: misma aritmetica de direccion que ARRAY_LOAD
            if (ins.operands.size() < 3) break;
            std::string r_arr = ctx.load_src(ins.operands[0], 0);
            std::string r_idx = ctx.load_src(ins.operands[1], 1);
            std::string r_val = ctx.load_src(ins.operands[2], 0);
            uint64_t stride   = ir_type_size(ins.type);
            ctx.out << "    mov r13, " << r_idx << "\n";
            if (stride > 1)
                ctx.out << "    mulu r13, " << stride << "\n";
            ctx.out << "    addu r13, 8\n";
            ctx.out << "    addu r13, " << r_arr << "\n";
            ctx.out << "    mov [r13], " << r_val << "\n";
            // write barrier si el tipo de elemento es HANDLE
            if (ins.type == IrType::HANDLE)
                ctx.out << "    gcwb " << r_arr << "\n";
            break;
        }

        // --- Operaciones de cadena ---

        case IrOp::STRMAKE: {
            // strmake.handle %buf_addr, %len [enc=imm]
            // fix.strgc - STRMAKE alloca un StringObject en el GC
            // heap, lo que puede triggerar GC y mover otros host_ptrs
            // vivos.  Tratamos el opcode como un CALL para fines de
            // spill: si hay regs is_gc_object vivos, hacemos el dance
            // gchandle/gcderef alrededor.  Sin esto, `this.field =
            // "lit"` en un ctor que tiene varios `new X()` previos veria
            // `this` stale tras el strmake.
            if (ins.operands.size() < 2) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            std::string r_buf = ctx.load_src(ins.operands[0], 0);
            std::string r_len = ctx.load_src(ins.operands[1], 1);
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);
            std::string rd    = ctx.dst_of(ins.dst);
            ctx.out << "    strmake " << rd << ", " << r_buf << ", " << r_len << "\n";
            ctx.store_spilled(ins.dst);
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            break;
        }

        case IrOp::STRLEN: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strlen " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRCAT: {
            if (ins.operands.size() < 2) break;
            std::string rd = ctx.dst_of(ins.dst);
            std::string ra = ctx.load_src(ins.operands[0], 0);
            std::string rb = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    strcat " << rd << ", " << ra << ", " << rb << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRCMP: {
            if (ins.operands.size() < 2) break;
            std::string rd = ctx.dst_of(ins.dst);
            std::string ra = ctx.load_src(ins.operands[0], 0);
            std::string rb = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    strcmp " << rd << ", " << ra << ", " << rb << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRSLICE: {
            if (ins.operands.size() < 2) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            std::string r_rng = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    strslice " << rd << ", " << r_str << ", " << r_rng << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRFLAT: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strflat " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRHASH: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strhash " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRINTERN: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strintern " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRRAW: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strraw " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRCONV: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            // el segundo operando puede ser un enc_handle o el imm codifica enc
            if (ins.operands.size() >= 2) {
                std::string r_enc = ctx.load_src(ins.operands[1], 1);
                ctx.out << "    strconv " << rd << ", " << r_str << ", " << r_enc << "\n";
            } else {
                ctx.out << "    strconv " << rd << ", " << r_str << ", " << ins.imm << "\n";
            }
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRRESERVE: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_cap = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strreserve " << rd << ", " << r_cap << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRFINALIZE: {
            if (ins.operands.size() < 2) break;
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            std::string r_len = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    strfinalize " << r_str << ", " << r_len << "\n";
            break;
        }

        // --- Excepciones ---
        case IrOp::THROW: {
            if (!ins.operands.empty())
                ctx.out << "    throw " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        }

        case IrOp::TRYENTER: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    tryenter " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        }

        case IrOp::TRYLEAVE:
            ctx.out << "    tryleave\n";
            break;

        case IrOp::LANDINGPAD:
            if (ins.dst != IR_NO_VALUE)
                ctx.out << "    mov " << ctx.reg_of(ins.dst) << ", r0\n";
            break;

        // --- Async / futures ---
        case IrOp::FUTURE: {
            ctx.out << "    future\n"; // resultado en r0
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        case IrOp::AWAIT: {
            if (!ins.operands.empty()) {
                emit_mov_if_needed(ctx, "r1", ctx.reg_of(ins.operands[0]));
                ctx.out << "    await r1\n"; // bloquea; resultado en r0
                if (ins.dst != IR_NO_VALUE)
                    emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            }
            break;
        }

        case IrOp::FULFILL: {
            if (ins.operands.size() < 2) break;
            std::string r_fut = ctx.reg_of(ins.operands[0]);
            std::string r_val = ctx.reg_of(ins.operands[1]);
            ctx.out << "    fulfill " << r_fut << ", " << r_val << "\n";
            break;
        }

        case IrOp::REJECT: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    reject " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        }

        // --- Distribucion ---
        case IrOp::MSGSEND: {
            if (ins.operands.size() < 3) break;
            ctx.out << "    msgsend " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1])
                    << ", " << ctx.reg_of(ins.operands[2]) << "\n";
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        case IrOp::MSGRECV: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    msgrecv " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        case IrOp::RSPAWN: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    rspawn " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        // --- Sincronizacion / monitores ---
        case IrOp::MONENTER:
            if (!ins.operands.empty())
                ctx.out << "    monenter " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::MONEXIT:
            if (!ins.operands.empty())
                ctx.out << "    monexit " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::MONWAIT:
            if (!ins.operands.empty())
                ctx.out << "    monwait " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::MONNOTI:
            if (!ins.operands.empty())
                ctx.out << "    monnoti " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::MONNOTA:
            if (!ins.operands.empty())
                ctx.out << "    monnota " << ctx.reg_of(ins.operands[0]) << "\n";
            break;

        // --- Intrinsics VM ---
        case IrOp::GETPROC:
            if (ins.dst != IR_NO_VALUE)
                ctx.out << "    getproc " << ctx.reg_of(ins.dst) << "\n";
            break;
        case IrOp::GETVM:
            if (ins.dst != IR_NO_VALUE)
                ctx.out << "    getvm " << ctx.reg_of(ins.dst) << "\n";
            break;
        case IrOp::GETMGR:
            if (ins.dst != IR_NO_VALUE)
                ctx.out << "    getmgr " << ctx.reg_of(ins.dst) << "\n";
            break;

        // --- Coroutines / scheduler ---
        case IrOp::SPAWN: {
            if (!ins.operands.empty()) {
                ctx.out << "    spawn " << ctx.reg_of(ins.operands[0]) << "\n";
                if (ins.dst != IR_NO_VALUE)
                    emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            }
            break;
        }
        case IrOp::RESUME:
            if (!ins.operands.empty())
                ctx.out << "    resume " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::YIELD:
            ctx.out << "    yield\n";
            break;
        case IrOp::SWAPCTX:
            if (ins.operands.size() >= 2)
                ctx.out << "    swapctx " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;

        case IrOp::RAW_ASM: {
            // Emitir cada linea del texto incrustado con indentacion estandar.
            // Substituimos los tokens:
            //   {dst}        -> nombre del registro asignado al destino SSA
            //   {src0}..{srcN-1} -> reg de cada operando (post-regalloc)
            // Permite que un bloque RAW_ASM produzca/consuma valores SSA sin
            // crear un IR op dedicado.  Los srcN se materializan via
            // load_src(scratch_idx=0) si estan spilled (puede usar SCRATCH).
            std::string dst_reg;
            if (ins.dst != IR_NO_VALUE) {
                dst_reg = ctx.dst_of(ins.dst);
            }
            // Resolver los registros de cada operando antes de emitir.  Si
            // un operando esta spilled, load_src genera el codigo necesario
            // para cargarlo a SCRATCH y devuelve ese reg como string.
            std::vector<std::string> src_regs;
            src_regs.reserve(ins.operands.size());
            for (size_t k = 0; k < ins.operands.size(); ++k) {
                if (ins.operands[k] == IR_NO_VALUE) {
                    src_regs.emplace_back();
                } else {
                    src_regs.push_back(ctx.load_src(ins.operands[k],
                                                    static_cast<int>(k % 2)));
                }
            }
            std::istringstream iss(ins.func_name);
            std::string ln;
            while (std::getline(iss, ln)) {
                if (ln.empty()) continue;
                if (!dst_reg.empty()) {
                    size_t pos = 0;
                    while ((pos = ln.find("{dst}", pos)) != std::string::npos) {
                        ln.replace(pos, 5, dst_reg);
                        pos += dst_reg.size();
                    }
                }
                for (size_t k = 0; k < src_regs.size(); ++k) {
                    if (src_regs[k].empty()) continue;
                    const std::string tok = "{src" + std::to_string(k) + "}";
                    size_t pos = 0;
                    while ((pos = ln.find(tok, pos)) != std::string::npos) {
                        ln.replace(pos, tok.size(), src_regs[k]);
                        pos += src_regs[k].size();
                    }
                }
                ctx.out << "    " << ln << "\n";
            }
            if (ins.dst != IR_NO_VALUE) {
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        default:
            ctx.comment("instruccion no soportada: " +
                        std::string(ir_op_name(ins.op)));
            ctx.out << "    nop1\n";
            break;
    }
}

// =========================================================================
//  Emision de una funcion completa
// =========================================================================

static std::string emit_function(const IrFunction &fn,
                                  const EmitOptions &opts,
                                  std::ostringstream &out,
                                  bool is_entry_point = false) {
    // Liveness + asignacion de registros
    LivenessResult liveness = compute_liveness(fn);
    AllocResult    alloc    = allocate_regs(fn, liveness);

    // fix14: solo emitir enter/leave si hay slots de spill O si la funcion
    // contiene ALLOCA (que genera subsp rsp, N sin un addsp correspondiente antes
    // del ret).  Sin enter/leave, el leave del epilogo no puede restaurar RSP al
    // valor que tenia cuando callvm empujo el ret_addr; el ret leeria una direccion
    // incorrecta y saltaria a basura.  Las funciones verdaderamente hoja (sin spill
    // y sin ALLOCA) son las unicas que pueden omitir enter/leave con seguridad:
    // solo tienen push/pop balanceados y callvm/ret que cancelan su propia RSP change.
    bool has_alloca = false;
    if (alloc.spill_count == 0) {
        for (const IrBlock &bb : fn.blocks) {
            for (const IrInstr &ins : bb.instrs) {
                if (ins.op == IrOp::ALLOCA) { has_alloca = true; break; }
            }
            if (has_alloca) break;
        }
    }
    const bool has_frame = (alloc.spill_count > 0) || has_alloca;

    // Construir el contexto (pasa has_frame para que TAILCALL tambien omita leave)
    EmitCtx ctx(fn, alloc, liveness, out, opts.emit_comments, opts.emit_debug, has_frame);

    // Etiqueta de funcion (exportada si corresponde)
    if (opts.export_all) {
        out << "@Export(" << ctx.fn_lbl << ")\n";
    }
    out << ctx.fn_lbl << ":\n";

    // Prologo (omitido solo cuando spill_count == 0 Y no hay ALLOCA en el cuerpo)
    if (has_frame) {
        out << "    enter " << alloc.spill_count << "\n";
    }

    // Spill de parametros extra (>12) que no caben en registros:
    // En la convencion actual se asumen ya en pila; solo emitimos comentario.
    if (opts.emit_comments && !fn.params.empty()) {
        out << "    // parametros: ";
        for (size_t i = 0; i < fn.params.size(); ++i) {
            IrValueId pid = fn.params[i];
            if (i > 0) out << ", ";
            if (alloc.reg_map.count(pid))
                out << fn.values[pid].name << "=" << reg_name(alloc.reg_map.at(pid));
            else
                out << fn.values[pid].name << "=[spill]";
        }
        out << "\n";
    }

    // Emision de bloques
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const IrBlock &bb = fn.blocks[b];

        // Etiqueta del bloque (el bloque 0 = entry no necesita etiqueta extra
        // porque la etiqueta de la funcion ya apunta ahi, pero la emitimos igualmente
        // para que los saltos desde otros bloques puedan apuntar al entry).
        out << ctx.block_label(static_cast<IrBlockId>(b)) << ":\n";

        bool skip_next = false;
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            if (skip_next) { skip_next = false; continue; }
            emit_instr(ctx, bb, i, skip_next);
        }

        // Si el bloque no termina en terminador (bloque vacio o sin ret/br),
        // no emitir nada extra; el proximo bloque continua por caida natural.
    }

    // Epilogo comun de retorno
    out << ctx.fn_lbl << "_ret:\n";
    // fix14: solo emitir leave si se emitio enter (spill_count > 0 o hay ALLOCA).
    if (has_frame) {
        out << "    leave\n";
    }
    // La funcion de entrada usa hlt para terminar la maquina explicitamente;
    // las demas funciones usan ret para retornar al llamador via callvm.
    out << (is_entry_point ? "    hlt\n\n" : "    ret\n\n");

    if (!alloc.spill_map.empty() && opts.emit_comments) {
        out << "    // INFO: " << alloc.spill_count
            << " valor(es) derramado(s) a pila; cargas/almacenamientos emitidos\n";
    }

    return {}; // sin error
}

// =========================================================================
//  Puntos de entrada publicos
// =========================================================================

EmitResult ir_emit_module(const IrModule &mod_in, const EmitOptions &opts) {
    EmitResult result;
    result.ok = true;

    // Trabajar sobre una copia para no modificar el modulo original
    IrModule mod = mod_in;

    // Aplicar optimizaciones IR
    ir_optimize(mod, opts.opt_level);

    std::ostringstream out;

    // Cabecera del modulo
    out << "// Emitido por ir_emitter - VestaVM\n";
    out << "// Nivel de optimizacion: O" << static_cast<int>(opts.opt_level) << "\n\n";

    // Cabecera .vel obligatoria: formato, espacio de direcciones y seccion
    // Si el modulo definio directivas @format/@space/@section las usamos;
    // en caso contrario emitimos valores por defecto razonables.
    std::string fmt = mod.format.empty() ? "velb" : mod.format;
    out << "@Format(\"" << fmt << "\")\n\n";

    if (mod.spaces.empty()) {
        // espacio de direcciones anonimo por defecto
        out << "@SpaceAddress {\n";
        out << "    @Name(\"anonymous\"),\n";
        out << "    @IniAddress(0x0000000000000000),\n";
        out << "    @EndAddress(0xFFFFFFFFFFFFFFFF)\n";
        out << "}\n\n";
    } else {
        for (const auto &sp : mod.spaces) {
            out << "@SpaceAddress {\n";
            out << "    @Name(\"" << sp.name << "\"),\n";
            out << "    @IniAddress(0x" << std::hex << std::setw(16) << std::setfill('0')
                << sp.ini_address << std::dec << "),\n";
            out << "    @EndAddress(0x"  << std::hex << std::setw(16) << std::setfill('0')
                << sp.end_address  << std::dec << ")\n";
            out << "}\n\n";
        }
    }

    if (mod.sections.empty()) {
        // seccion de codigo por defecto.  Las etiquetas que siguen
        // (main, main_entry, ...) pertenecen a esta seccion hasta que
        // se emita una nueva @Section.  La seccion "data" para los
        // literales se declara DESPUES de las funciones, justo antes
        // de los datos.
        out << "@Section {\n";
        out << "    @Name(\"code\"),\n";
        out << "    @SpaceAddress(\"anonymous\")\n";
        out << "    @Align(0x1000)\n";
        out << "}\n\n";
    } else {
        for (const auto &sec : mod.sections) {
            out << "@Section {\n";
            out << "    @Name(\"" << sec.name << "\"),\n";
            out << "    @SpaceAddress(\"" << sec.space_name << "\")\n";
            out << "    @Align(0x" << std::hex << sec.align << std::dec << ")\n";
            out << "}\n\n";
        }
    }

    // Declaracion de modulo (@Module es obligatorio antes de @Export)
    std::string mod_name = opts.module_name.empty() ? mod.name : opts.module_name;
    if (mod_name.empty() && opts.export_all) mod_name = "ir_output";
    if (!mod_name.empty()) {
        out << "@Module(" << EmitCtx::sanitize(mod_name) << ")\n\n";
    }

    // Declaraciones de librerias nativas
    for (const auto &lib : mod.native_libs) {
        out << "@Lib(\"" << lib << "\")\n";
    }
    if (!mod.native_libs.empty()) out << "\n";

    // Importaciones
    for (const auto &imp : mod.imports) {
        out << "@import " << imp << "\n";
    }
    if (!mod.imports.empty()) out << "\n";

    // Bloque @Import { @Method { @Lib(...) @Name(...) } } para CALLN.
    // El ensamblador .vel exige esta declaracion antes del primer uso de
    // la funcion nativa correspondiente (calln @Method("lib:name")).
    if (!mod.native_imports.empty()) {
        out << "@Import {\n";
        for (const auto &ni : mod.native_imports) {
            out << "    @Method { @Lib(\"" << ni.lib << "\")"
                << " @Name(\"" << ni.name << "\") }\n";
        }
        out << "}\n\n";
    }

    // Emision de cada funcion; la primera funcion no-nativa es el punto de entrada
    bool first_func = true;
    for (const auto &fn : mod.functions) {
        if (fn.is_native) {
            // Stub nativo: solo comentario de importacion
            out << "// funcion nativa: " << fn.name << " (no se emite codigo)\n\n";
            continue;
        }
        std::string err = emit_function(fn, opts, out, first_func);
        first_func = false;
        if (!err.empty()) {
            result.ok    = false;
            result.error = err;
            return result;
        }
    }

    // Datos estaticos: emitidos AL FINAL de la seccion "code", separados
    // del codigo ejecutable por una directiva 'align 16'.  No se usan
    // secciones independientes porque el linker .velb actual no
    // serializa los bytes de secciones distintas a la primera al
    // binario final, dejando los reloc apuntando a posiciones validas
    // pero sin contenido escrito.  Mismo patron que el ejemplo del
    // proyecto examples_codes_vm/test_vesta_io.vel.
    //
    // CRITICO: hay que emitir una etiqueta 'end_data:' DESPUES de los
    // bytes db.  El linker calcula el tamano del bloque ejecutable
    // como el VA del ultimo simbolo definido; si el ultimo simbolo es
    // 's_N:' (con bytes detras pero sin etiqueta posterior), el bloque
    // termina justo en VA(s_N) y los bytes db quedan fuera del rango
    // mapeado a memoria de la VM.  La etiqueta posterior end_data
    // empuja el rango hasta despues de los bytes.
    if (!mod.static_data.empty()) {
        out << "// --- datos estaticos del modulo (mismas seccion que el codigo) ---\n";
        out << "align 16\n";
        for (size_t i = 0; i < mod.static_data.size(); ++i) {
            const auto &bytes = mod.static_data[i];
            // El parser .vel espera el patron "etiqueta directiva valores"
            // EN LA MISMA LINEA (estilo NASM).  Si separamos la etiqueta
            // en su propia linea el assembler la trata como label vacio
            // y los bytes db nunca se incrustan en el binario.
            //
            // Se intenta emitir como string entrecomillado cuando el
            // contenido es ASCII imprimible puro (mismo formato que usa
            // examples_codes_vm/test_vesta_io.vel y que el assembler
            // procesa correctamente); si hay bytes de control o nuls
            // intermedios se cae a la lista hex byte a byte.
            bool printable = !bytes.empty();
            for (uint8_t b : bytes) {
                if (b < 0x20 || b > 0x7E || b == '"' || b == '\\') {
                    printable = false;
                    break;
                }
            }
            if (printable) {
                out << "    s_" << i << " db \"";
                for (uint8_t b : bytes) out << static_cast<char>(b);
                out << "\"\n";
            } else {
                out << "    s_" << i << " db ";
                for (size_t b = 0; b < bytes.size(); ++b) {
                    if (b > 0) out << ", ";
                    out << "0x" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<unsigned>(bytes[b])
                        << std::dec << std::setfill(' ');
                }
                if (!bytes.empty()) out << ", 0x00";
                else                 out << "0x00";
                out << "\n";
            }
        }
        // Etiqueta marker que extiende el rango ejecutable hasta despues
        // de los ultimos bytes; sin ella el linker calcula el tamano del
        // bloque como VA(s_N) y los bytes db quedan truncados.
        out << "    end_data db 0x00\n";
        out << "\n";
    }

    result.vel_text = out.str();
    return result;
}

EmitResult ir_emit_text(const std::string &ir_text, const EmitOptions &opts) {
    IrModule mod;
    std::string parse_err;
    if (!ir_parse(ir_text, mod, parse_err)) {
        EmitResult r;
        r.ok    = false;
        r.error = "parse: " + parse_err;
        return r;
    }
    return ir_emit_module(mod, opts);
}

} // namespace ir
