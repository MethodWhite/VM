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
#include "ir/ssa_ir.h"
#include <sstream>
#include <cstdio>
#include <cstdlib>
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
    // true si se emitio enter (spill_count > 0); false = metodo hoja sin frame.
    // Permite skipear leave en epilogos cuando no hay frame, ahorrando 2 bytes
    // por ret en metodos hoja (tipicos: getters, setters, ops aritmeticas pequenas).
    bool                   has_frame;

    // Cache de constantes en scratches para evitar `mov r14, K; mov r14, K`
    // consecutivos (patron tipico: dos SEXTs back-to-back con K=32 entre
    // los que no hay instrs que clobreen r14).  -1 = invalido (clobreado o
    // bloque nuevo).  Se invalida en cualquier basic-block boundary
    // (emision de label) y en cualquier instruccion que use r14 como
    // destino fuera del cache.
    int64_t r14_cache = -1;
    int64_t r13_cache = -1;

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
                // Calcular direccion: r13 = rbp - (slot+1)*8.
                //
                // Bug critico arreglado (2026-05-10): antes usabamos
                // `addu r13, slot*8` (offset POSITIVO desde rbp), lo que
                // colocaba los slots EN EL AREA DEL CALLER:
                //   rbp+0  = saved_rbp del caller (push rbp en enter)
                //   rbp+8  = RET_ADDR pushed por callvirt
                //   rbp+16 = caller's locals
                // Con varios spills, el slot 1 sobrescribia RET_ADDR y al
                // hacer leave+ret la VM saltaba a un valor pequenio (un
                // GcHandle).  En el editor TUI, render_buffer hacia 6
                // spills, corrompiendo RET_ADDR y `this` del caller (run).
                // Sintoma: editor crasheaba justo despues del primer render.
                //
                // El fix usa offsets NEGATIVOS desde rbp:
                //   slot 0 = rbp - 8   (primer local, justo bajo saved_rbp)
                //   slot 1 = rbp - 16  (segundo local)
                //   ...
                // Estos offsets caen en el area allocada por `enter` (sub
                // rsp, frame_size), que es local al frame y nadie mas
                // toca.  Combinado con el fix de enter (allocacion en
                // bytes = spill_count*8), los locals viven seguros.
                out << "    mov r13, rbp\n";
                out << "    subu r13, " << ((it->second + 1) * 8) << "\n";
                out << "    mov " << reg_name(sr) << ", [r13]\n";
                r13_cache = -1;  // r13 fue clobreado
                if (sr == 14) r14_cache = -1;
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

    // Si vid esta derramado, persiste el valor en su slot de pila.
    // Bug C fix: si vid sigue en reg_map (caso eviction donde regalloc
    // reasigno el reg pero el valor aun se usa via spill), leer del reg
    // real en vez de SCRATCH_REG (que tendria garbage post-call).
    void store_spilled(IrValueId vid) {
        if (vid == IR_NO_VALUE) return;
        auto it = alloc.spill_map.find(vid);
        if (it == alloc.spill_map.end()) return;
        std::string src_reg = reg_name(SCRATCH_REG);
        auto it_reg = alloc.reg_map.find(vid);
        if (it_reg != alloc.reg_map.end()) {
            src_reg = reg_name(it_reg->second);
        }
        if (is_gc_value(vid)) {
            out << "    gchandle " << reg_name(SCRATCH_REG)
                << ", "             << src_reg << "\n";
            src_reg = reg_name(SCRATCH_REG);
            r14_cache = -1;
        }
        // Mismo fix que load_src: spill slots en offsets NEGATIVOS desde rbp.
        out << "    mov r13, rbp\n";
        out << "    subu r13, " << ((it->second + 1) * 8) << "\n";
        out << "    mov [r13], " << src_reg << "\n";
        r13_cache = -1;
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
    if (dst != src) {
        ctx.out << "    mov " << dst << ", " << src << "\n";
        // Si el dst es r14 o r13, invalidamos su cache de constante:
        // ahora contiene el VALOR del reg origen, no una constante conocida.
        if (dst == "r14") ctx.r14_cache = -1;
        if (dst == "r13") ctx.r13_cache = -1;
    }
}

// Emite `mov r14, K` SOLO si el cache de r14 indica un valor distinto.
// Si r14 ya tiene K (cacheado de un mov anterior dentro del mismo BB),
// la emision se omite.  El cache se invalida en bloques nuevos y por
// uso de r14 como destino fuera de esta helper.
static void emit_mov_r14_imm(EmitCtx &ctx, int64_t k) {
    if (ctx.r14_cache == k) return;  // ya tiene ese valor
    ctx.out << "    mov r14, " << k << "\n";
    ctx.r14_cache = k;
}
static void emit_mov_r13_imm(EmitCtx &ctx, int64_t k) {
    if (ctx.r13_cache == k) return;
    ctx.out << "    mov r13, " << k << "\n";
    ctx.r13_cache = k;
}
// Wrapper generico: usa el cache correcto segun el nombre del scratch.
static void emit_mov_scratch_imm(EmitCtx &ctx, const std::string &scratch,
                                  int64_t k) {
    if (scratch == "r14")      emit_mov_r14_imm(ctx, k);
    else if (scratch == "r13") emit_mov_r13_imm(ctx, k);
    else {
        ctx.out << "    mov " << scratch << ", " << k << "\n";
    }
}

// Variante optimizada para shifts: si K cabe en 1 byte, emite `mov scratchb, K`
// (4 bytes vs 11 bytes del i64).  Es seguro porque shl/sar enmascaran el shift
// count con (bits-1), asi que los bytes superiores de @p scratch son ignorados.
// Invalida el cache full-qword de scratch porque sus bytes altos quedan con un
// valor potencialmente distinto al esperado (no son tocados por el byte_lo).
static void emit_mov_scratch_shift_imm(EmitCtx &ctx, const std::string &scratch,
                                        int64_t k) {
    if (k >= 0 && k < 256) {
        ctx.out << "    mov " << scratch << "b, " << k << "\n";
        // Invalidar el cache: los bytes altos no se modifican, asi que el
        // valor completo del registro no es K.  Mejor olvidar.
        if (scratch == "r14")      ctx.r14_cache = -1;
        else if (scratch == "r13") ctx.r13_cache = -1;
    } else {
        // K no cabe en 1 byte: fallback al mov i64 cacheado normal.
        emit_mov_scratch_imm(ctx, scratch, k);
    }
}
// Invalida los caches al cruzar un boundary de bloque (despues de emitir
// `label:`) o tras una llamada que clobrea scratches.
static void invalidate_scratch_caches(EmitCtx &ctx) {
    ctx.r14_cache = -1;
    ctx.r13_cache = -1;
}

// Emite `jmp @label(target)` solo si el bloque destino NO es el siguiente
// en orden de emision (i.e., si NO podemos caer por fallthrough natural).
// Ahorra ~46% de los jmp en codigo con if/while/for donde el target
// del jmp incondicional es siempre la siguiente etiqueta.
static void emit_jmp_or_fallthrough(EmitCtx &ctx, IrBlockId from_bid,
                                     IrBlockId target_id) {
    // El siguiente bloque en orden de emision es from_bid+1.  Si el target
    // coincide, la caida natural ya hace el "salto" y el jmp explicito
    // es redundante.  Para el ultimo bloque (from_bid+1 == fn.blocks.size())
    // siempre emitimos el jmp por seguridad.
    if (target_id == from_bid + 1) return;
    ctx.out << "    jmp @Absolute(\""
            << EmitCtx::abs_lbl(ctx.block_label(target_id)) << "\")\n";
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

// true si el reg @p r contiene un IrValueId con is_gc_object EN call_pos.
//
// BugFix C (2026-06-04): cuando el regalloc reusa el mismo reg para
// multiples values con live ranges que se traslapan por imprecision del
// liveness analysis (extension via live_out + back-edges), encontrar
// CUAL value es REALMENTE el que esta en el reg AL MOMENTO del call.
// La heuristica antigua retornaba @c true si CUALQUIER value asignado
// a @p r y con liveness range cubriendo @p call_pos era is_gc_object.
// Eso provocaba que values is_gc_object=false (e.g. counter int) se
// trataran como handles -> @c gchandle producia @c GC_NULL_HANDLE ->
// push/pop devolvia nullptr -> AV al primer deref.
//
// Fix: elegir el value con @c def_pos MAS GRANDE entre los candidatos.
// Ese es el value MAS RECIENTEMENTE definido y por tanto el realmente
// presente en el reg en @p call_pos (los anteriores ya fueron clobereados
// por la asignacion posterior).  Si NO hay ningun candidato gc, retorna
// false (el reg contiene un value no-gc en este punto del programa).
static bool reg_holds_gc_object(const EmitCtx &ctx, uint32_t call_pos, int r) {
    bool     found_any   = false;
    uint32_t best_def    = 0;
    bool     best_is_gc  = false;
    for (const auto &iv : ctx.liveness.intervals) {
        if (!(iv.def <= call_pos && call_pos < iv.end)) continue;
        auto it = ctx.alloc.reg_map.find(iv.id);
        if (it == ctx.alloc.reg_map.end() || it->second != r) continue;
        if (static_cast<size_t>(iv.id) >= ctx.fn.values.size()) continue;
        // Elegir el value mas recientemente definido (mayor iv.def) entre
        // los candidatos asignados al mismo reg con range que cubre call_pos.
        if (!found_any || iv.def > best_def) {
            found_any  = true;
            best_def   = iv.def;
            best_is_gc = ctx.fn.values[iv.id].is_gc_object;
        }
    }
    return found_any && best_is_gc;
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
//
// Optimizacion fastpush: si NINGUNO de los regs es GC y son >= 2, los
// empujamos en una sola instruccion con `fastpush <mask16>` (4 bytes vs
// N x 2 bytes de push reg).  Para 1 reg el push tradicional es mas chico
// (2 bytes vs 4 bytes), asi que solo fusionamos a partir de 2 regs.
static void emit_save_live_regs(EmitCtx &ctx, uint32_t call_pos,
                                 const std::vector<int> &regs_to_save)
{
    // Detectar si todos los regs son no-GC para usar fastpush.
    bool any_gc = false;
    for (int r : regs_to_save) {
        if (reg_holds_gc_object(ctx, call_pos, r)) { any_gc = true; break; }
    }
    if (!any_gc && regs_to_save.size() >= 2) {
        uint32_t mask = 0;
        for (int r : regs_to_save) {
            if (r >= 0 && r < 16) mask |= (1u << r);
        }
        ctx.out << "    fastpush " << mask << "\n";
        return;
    }

    // LANG.fix-4: scratch dinamico para gchandle.  Por defecto usamos
    // r14, pero si r14 esta en regs_to_save (vivo a traves del call),
    // usamos r13; si tambien lo esta, r0 (return reg, sera sobreescrito
    // por el call).  Esto NO arregla el caso donde load_src puso un
    // operando en r14 antes del save (no esta en regs_to_save porque el
    // regalloc trata r14 como scratch externo).  Para ese caso, los call
    // sites afectados deben emitir SAVE antes de LOAD (ver STRMAKE/STRCAT
    // como referencia).
    bool r14_live = false;
    bool r13_live = false;
    for (int r : regs_to_save) {
        if (r == 14) r14_live = true;
        if (r == 13) r13_live = true;
    }
    auto pick_scratch = [&]() -> std::string {
        if (!r14_live) return "r14";
        if (!r13_live) return "r13";
        return "r0";
    };
    const std::string sc = pick_scratch();

    bool gc_first_ordered = true;
    bool saw_nongc = false;
    int  num_nongc_tail = 0;
    for (int r : regs_to_save) {
        const bool is_gc = reg_holds_gc_object(ctx, call_pos, r);
        if (is_gc && saw_nongc) { gc_first_ordered = false; break; }
        if (!is_gc) { saw_nongc = true; ++num_nongc_tail; }
    }
    if (gc_first_ordered && num_nongc_tail >= 2) {
        uint32_t mask = 0;
        for (int r : regs_to_save) {
            if (reg_holds_gc_object(ctx, call_pos, r)) {
                ctx.out << "    gchandle " << sc << ", " << reg_name(r) << "\n";
                ctx.out << "    push " << sc << "\n";
                if (sc == "r14") ctx.r14_cache = -1;
            } else {
                if (r >= 0 && r < 16) mask |= (1u << r);
            }
        }
        ctx.out << "    fastpush " << mask << "\n";
        return;
    }

    for (int r : regs_to_save) {
        if (reg_holds_gc_object(ctx, call_pos, r)) {
            ctx.out << "    gchandle " << sc << ", " << reg_name(r) << "\n";
            ctx.out << "    push " << sc << "\n";
            if (sc == "r14") ctx.r14_cache = -1;
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
// Optimizacion fastpop: si NINGUNO de los regs es GC y son >= 2, los
// desempilamos en una sola instruccion con `fastpop <mask16>` (mismo
// mask que el fastpush correspondiente).  fastpop es simetrico al
// fastpush, asi que los valores se restauran exactamente.
//
// El uso de cur0 sigue la convencion del loader (__new_<X>) donde gcderef
// escribe a cursor y luego se intercambia a un GP reg.  cur0 es scratch
// del runtime y nunca se preserva entre instrucciones VM.
static void emit_restore_live_regs(EmitCtx &ctx, uint32_t call_pos,
                                    const std::vector<int> &regs_to_save)
{
    // Tras la llamada, el callee pudo haber clobreado r13/r14.  Invalidamos.
    invalidate_scratch_caches(ctx);

    // Detectar si todos los regs son no-GC para usar fastpop.
    bool any_gc = false;
    for (int r : regs_to_save) {
        if (reg_holds_gc_object(ctx, call_pos, r)) { any_gc = true; break; }
    }
    if (!any_gc && regs_to_save.size() >= 2) {
        uint32_t mask = 0;
        for (int r : regs_to_save) {
            if (r >= 0 && r < 16) mask |= (1u << r);
        }
        ctx.out << "    fastpop " << mask << "\n";
        return;
    }

    // HYBRID restore -- SIMETRICO al hybrid save.
    bool gc_first_ordered = true;
    bool saw_nongc = false;
    int  num_nongc_tail = 0;
    for (int r : regs_to_save) {
        const bool is_gc = reg_holds_gc_object(ctx, call_pos, r);
        if (is_gc && saw_nongc) { gc_first_ordered = false; break; }
        if (!is_gc) { saw_nongc = true; ++num_nongc_tail; }
    }
    if (gc_first_ordered && num_nongc_tail >= 2) {
        // Reverse del save: fastpop primero (los non-GC fueron pusheados al final),
        // luego pop+gcderef+xchg de los GC en orden inverso.
        uint32_t mask = 0;
        for (int r : regs_to_save) {
            if (!reg_holds_gc_object(ctx, call_pos, r) && r >= 0 && r < 16) {
                mask |= (1u << r);
            }
        }
        ctx.out << "    fastpop " << mask << "\n";
        for (auto it = regs_to_save.rbegin(); it != regs_to_save.rend(); ++it) {
            const int r = *it;
            if (reg_holds_gc_object(ctx, call_pos, r)) {
                ctx.out << "    pop " << reg_name(r) << "\n";
                ctx.out << "    gcderef cur0, " << reg_name(r) << "\n";
                ctx.out << "    xchg cur0, " << reg_name(r) << "\n";
            }
        }
        return;
    }

    // Fallback: secuencia tradicional pop + (gcderef + xchg si GC).
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
// Carga args spilled directamente a su reg destino DESPUES del parallel-move.
// Bug previo: cargar todos los spilled via load_src(_, 0) usaba siempre r14
// como temp; con 2+ args spilled, el segundo load clobbeaba el primero, y
// ambos terminaban con el mismo valor en moves[].  Fix: emitir spills tras
// el parallel-move usando direct load `mov r_target, [slot]` (sin pasar por
// scratch).  Para values is_gc_object spilled, anyade el gcderef+xchg que
// load_src haria normalmente.
static void emit_load_spilled_arg(EmitCtx &ctx, int target_reg, ir::IrValueId vid) {
    auto it = ctx.alloc.spill_map.find(vid);
    if (it == ctx.alloc.spill_map.end()) return;  // no es spilled, no-op
    const std::string rd = std::string(reg_name(target_reg));
    /* Scratch para el addr.  Usamos r14 (scratch general) cuando target!=14;
     * cuando target ES r14 reutilizamos el mismo reg (el flujo
     * mov r14,rbp -> subu r14 -> mov r14,[r14] funciona porque el ultimo
     * mov sobreescribe r14 con el valor cargado).
     *
     * Importante: NO usar r13 como scratch.  En el path CALLNI (FFI runtime
     * indirecto), r13 contiene el fn_ptr a invocar -- pisarlo aqui causa
     * que el callni salte a memoria invalida.  r14 esta siempre libre en
     * este punto porque parallel_arg_moves ya termino y r14 es el scratch
     * de cycle-breaking que liberamos al final del parallel-move. */
    const char *scratch_reg = "r14";
    ctx.out << "    mov " << scratch_reg << ", rbp\n";
    ctx.out << "    subu " << scratch_reg << ", " << ((it->second + 1) * 8) << "\n";
    ctx.out << "    mov " << rd << ", [" << scratch_reg << "]\n";
    /* Invalidamos los caches que hayan sido clobered. */
    ctx.r14_cache = -1;
    if (target_reg == 13) ctx.r13_cache = -1;
    if (ctx.is_gc_value(vid)) {
        ctx.out << "    gcderef cur0, " << rd << "\n";
        ctx.out << "    xchg cur0, " << rd << "\n";
    }
}

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
// Mapea mnemonic 2-operandos a su variante alu3 (3-op super-instr) si existe.
// Devuelve nullptr si no hay alu3 para el opcode (caso DIV/MOD/SHL/SHR/SAR/CMP).
static const char *alu3_mnemonic_for(const std::string &mnem) {
    if (mnem == "adds") return "adds3";
    if (mnem == "subs") return "subs3";
    if (mnem == "muls") return "muls3";
    if (mnem == "addu") return "addu3";
    if (mnem == "subu") return "subu3";
    if (mnem == "mulu") return "mulu3";
    if (mnem == "and")  return "and3";
    if (mnem == "or")   return "or3";
    if (mnem == "xor")  return "xor3";
    return nullptr;
}

// Emite operacion binaria de dos-direcciones:
//   "mov r_dst, r_src1"
//   "op  r_dst, r_src2"
// O su super-instruccion equivalente cuando aplique:
//   "OP3 r_dst, r_src1, r_src2"   (combina mov+op en una instr VM)
//
// Carga operandos derramados desde pila (src1->r14, src2->r13) si es necesario.
// Almacena el resultado en pila si dst esta derramado.
static void emit_binop(EmitCtx &ctx, const std::string &mnemonic,
                        IrValueId dst, IrValueId src1, IrValueId src2) {
    std::string rs1 = ctx.load_src(src1, 0); // r14 si derramado
    std::string rs2 = ctx.load_src(src2, 1); // r13 si derramado
    std::string rd  = ctx.dst_of(dst);

    /* Super-instruccion alu3 si:
     *   (a) existe variante 3-op para el mnemonic,
     *   (b) rd != rs1 (sin esto el mov no se emite y la 2-op tradicional
     *       es 1 instruccion -- igual coste, sin necesidad de cambio).
     * Cuando rs1 / rs2 estan derramados (r14 / r13), alu3 los lee igual
     * que la version 2-op: no hay restriccion en quien provee el operando. */
    const char *m3 = alu3_mnemonic_for(mnemonic);
    if (m3 != nullptr && rd != rs1) {
        ctx.out << "    " << m3 << " " << rd << ", " << rs1 << ", " << rs2 << "\n";
        /* Si rd es r14 / r13 (caso destino spilled), invalidar cache de
         * constante igual que emit_mov_if_needed haria. */
        if (rd == "r14") ctx.r14_cache = -1;
        if (rd == "r13") ctx.r13_cache = -1;
        ctx.store_spilled(dst);
        return;
    }

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
    // Sprint string-perf-5 (2026-06-02): bitcast directo via opcode bitg2z.
    // Antes: 5 instrucciones VM (subsp + mov r15,rsp + mov [r15],gp +
    // fload + addsp).  Ahora: 1 instruccion -> ~5x speedup en FP-heavy.
    ctx.out << "    bitg2z " << zmm_reg << ", " << gp_reg << "\n";
}

static void emit_zmm_to_gp_bits(EmitCtx &ctx,
                                 const std::string &zmm_reg,
                                 const std::string &gp_reg) {
    // Sprint string-perf-5: bitcast directo via opcode bitz2g (1 instr VM).
    ctx.out << "    bitz2g " << gp_reg << ", " << zmm_reg << "\n";
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

// =========================================================================
//  Helpers para cmpjmp / cmpjmpu fusionados (mejora hot loops).
// =========================================================================

/**
 * @brief Devuelve el mnemonic completo de @c cmpjmp.cc / @c cmpjmpu.cc
 *        equivalente a la cond INVERTIDA del cmp_op (cond de salto al
 *        false branch).
 *
 * Mismo mapeo que @c emit_cond_branch pero emitiendo el opcode fusionado
 * en lugar de cmp + jmp separados.  Devuelve nullptr si el cmp_op es
 * FCMP_* (no aplicamos cmpjmp a floats; van por la ruta ZMM existente).
 */
static const char *cmpjmp_fused_mnemonic(IrOp cmp_op) {
    switch (cmp_op) {
        case IrOp::CMP_EQ:  return "cmpjmp.jne";
        case IrOp::CMP_NE:  return "cmpjmp.je";
        case IrOp::CMP_LT:  return "cmpjmp.jge";
        case IrOp::CMP_GT:  return "cmpjmp.jle";
        case IrOp::CMP_LE:  return "cmpjmp.jgt";
        case IrOp::CMP_GE:  return "cmpjmp.jlt";
        case IrOp::CMP_ULT: return "cmpjmpu.jae";
        case IrOp::CMP_UGT: return "cmpjmpu.jls";
        case IrOp::CMP_ULE: return "cmpjmpu.jhi";
        case IrOp::CMP_UGE: return "cmpjmpu.jb";
        default: return nullptr; // FCMP_* o no soportado
    }
}

/**
 * @brief Verifica si emit_phi_copies generaria al menos una copia para
 *        el pred->succ dado.
 *
 * La fusion @c cmpjmp.cc solo es segura cuando NO hay phi copies entre
 * el cmp y el branch: si hubiera, los moves podrian pisar los regs del
 * cmp y alterar el resultado.  Este helper hace el mismo recorrido del
 * paso 1 de emit_phi_copies pero solo cuenta sin emitir.
 */
static bool has_phi_copies_to(EmitCtx &ctx, IrBlockId pred_id, IrBlockId succ_id) {
    if (succ_id >= static_cast<IrBlockId>(ctx.fn.blocks.size())) return false;
    const IrBlock &succ = ctx.fn.blocks[succ_id];
    for (const auto &ins : succ.instrs) {
        if (ins.op != IrOp::PHI) break;
        if (ins.dst == IR_NO_VALUE) continue;
        for (const auto &pa : ins.phi_args) {
            if (pa.block == pred_id && pa.value != IR_NO_VALUE) {
                // Solo es una colision real si dst != src (mov no trivial).
                int d_reg = ctx.alloc.reg_map.count(ins.dst)
                          ? ctx.alloc.reg_map.at(ins.dst) : -1;
                int s_reg = ctx.alloc.reg_map.count(pa.value)
                          ? ctx.alloc.reg_map.at(pa.value) : -2;
                if (d_reg != s_reg) return true;
                // Si alguno esta spilled, tambien hay copias (load/store)
                if (ctx.alloc.spill_map.count(ins.dst)
                 || ctx.alloc.spill_map.count(pa.value)) return true;
            }
        }
    }
    return false;
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

    // Paso 2: separar copias en 3 categorias para preservar semantica
    // "paralela" del PHI (todas las copias deben verse como simultaneas):
    //
    //   (a) spilled-dst:  cualquier cosa -> slot.  Debe emitirse PRIMERO
    //       porque el src (sea reg o slot) tiene el valor OLD del frame
    //       anterior, y queremos leerlo antes de que phase (b) lo cambie.
    //   (b) reg-to-reg:   reg -> reg.  parallel-move clasico en medio.
    //   (c) spilled-src reg-dst: slot -> reg.  Debe emitirse al FINAL,
    //       porque el dst_reg podria ser fuente de alguna copia (b).
    //
    // Orden: phase (a) -> phase (b) -> phase (c).  Bug fix Phase D.7.opt:
    // antes (c) se emitia ANTES de (b), clobeando el dst_reg antes de que
    // (b) lo usara como fuente.
    std::vector<PhiCopy> reg_copies;            // (b)
    std::vector<PhiCopy> spilled_src_reg_dst;   // (c)
    // Paso 2.a: spilled-dst (cualquier src -> slot).
    for (const auto &c : copies) {
        bool dst_in_reg = ctx.alloc.reg_map.count(c.dst) > 0;
        bool src_in_reg = ctx.alloc.reg_map.count(c.src) > 0;
        if (dst_in_reg && src_in_reg) {
            reg_copies.push_back(c);
        } else if (!dst_in_reg) {
            // dst spilled: load src y store al slot.  Si src es reg, el
            // valor que leemos es el OLD pre-phi.
            std::string r_src = ctx.load_src(c.src, 0);
            std::string r_dst = reg_name(SCRATCH_REG);
            emit_mov_if_needed(ctx, r_dst, r_src);
            ctx.store_spilled(c.dst);
        } else {
            // dst en reg, src en slot.  Diferido a phase (c).
            spilled_src_reg_dst.push_back(c);
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

    // Paso 5 (phase c): spilled-src reg-dst.  Carga directa del slot al
    // reg destino.  Seguro emitir DESPUES de los moves reg-to-reg porque
    // dst_reg ya no es fuente de nadie.
    for (const auto &c : spilled_src_reg_dst) {
        auto it = ctx.alloc.spill_map.find(c.src);
        if (it == ctx.alloc.spill_map.end()) continue;
        int    d_reg = ctx.alloc.reg_map.at(c.dst);
        std::string rd = reg_name(d_reg);
        ctx.out << "    mov r13, rbp\n";
        ctx.out << "    subu r13, " << ((it->second + 1) * 8) << "\n";
        ctx.out << "    mov " << rd << ", [r13]\n";
        ctx.r13_cache = -1;
        if (d_reg == 14) ctx.r14_cache = -1;
        if (ctx.is_gc_value(c.src)) {
            ctx.out << "    gcderef cur0, " << rd << "\n";
            ctx.out << "    xchg cur0, " << rd << "\n";
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

/**
 * @brief Verifica si @p val_id es una constante con el valor @p expected.
 *
 * Escanea el bloque buscando la instruccion @c IrOp::CONST que define @p val_id.
 * Solo busca en el mismo bloque (no cross-block) para mantener la verificacion
 * O(N) y sin ambiguedad en presencia de SSA mutable.
 *
 * @return true si val_id es definido por CONST con imm == expected en bb.
 */
static bool is_const_value(const IrBlock &bb, IrValueId val_id, uint64_t expected) {
    if (val_id == IR_NO_VALUE) return false;
    for (const auto &ins : bb.instrs) {
        if (ins.dst == val_id && ins.op == IrOp::CONST) {
            return ins.imm == expected;
        }
    }
    return false;
}

/**
 * @brief Intenta emitir @c decjnz r_counter, target fusionando el patron
 *        SUB(v, 1) + CMP_NE(sub_dst, 0) + BR_COND.
 *
 * El patron clasico de loop reverse-counter (`for (i = N; i > 0; i--)` o
 * `do { ... } while (--i != 0);`) baja al IR como:
 *
 *   i_dec = SUB i_old, 1       ; SSA value distinto al input
 *   z     = CMP_NE i_dec, 0    ; o CMP_EQ + branch invertido
 *   BR_COND z, loop_top, exit  ; si != 0, back to top
 *
 * Para emitir @c decjnz r_counter, target en una sola instruccion VM:
 *   - El reg fisico de @c i_old debe coincidir con el de @c i_dec
 *     (regalloc sharing tipico cuando @c i_old.last_use == SUB).  Si no
 *     coinciden, anadimos un mov puente y aun asi ahorramos ~2 instr.
 *   - No debe haber phi copies entre el cmp y el branch (mismo razonamiento
 *     que cmpjmp); fallback al patron tradicional cuando si las hay.
 *   - El operando b del SUB debe ser CONST 1 y el del CMP CONST 0.
 *
 * @return true si se emitio decjnz (y skip_next debe consumir 2 instr mas);
 *         false si no aplicaba el patron y se debe emit normalmente.
 */
[[maybe_unused]] static bool try_emit_decjnz_fusion(EmitCtx &ctx, const IrBlock &bb,
                                    size_t sub_idx, bool &skip_two) {
    if (sub_idx + 2 >= bb.instrs.size()) return false;
    const IrInstr &sub = bb.instrs[sub_idx];
    if (sub.op != IrOp::SUB) return false;
    if (sub.operands.size() < 2 || sub.dst == IR_NO_VALUE) return false;
    // operand_b debe ser const 1.
    if (!is_const_value(bb, sub.operands[1], 1)) return false;

    const IrInstr &cmp = bb.instrs[sub_idx + 1];
    if (cmp.op != IrOp::CMP_NE && cmp.op != IrOp::CMP_EQ) return false;
    if (cmp.operands.size() < 2 || cmp.dst == IR_NO_VALUE) return false;
    if (cmp.operands[0] != sub.dst) return false;
    if (!is_const_value(bb, cmp.operands[1], 0)) return false;

    const IrInstr &br = bb.instrs[sub_idx + 2];
    if (br.op != IrOp::BR_COND) return false;
    if (br.operands.empty() || br.operands[0] != cmp.dst) return false;

    // Resolver regs.  Si i_old (sub.operands[0]) y i_dec (sub.dst) no
    // coinciden en reg fisico, emit mov puente -- aun asi ahorramos
    // instrucciones vs el patron tradicional.
    std::string r_old = ctx.load_src(sub.operands[0], 0);
    std::string r_dec = ctx.dst_of(sub.dst);

    // Phi safety: las phi copies del back-edge tipicamente NO tocan el
    // reg del counter (escriben a otros regs PHI).  Solo rechazamos la
    // fusion si alguna phi copy escribe al MISMO reg que r_dec (= counter).
    // Este check es preciso: phi(i_phi).reg == reg(i_dec) -> trivial (mov
    // r_dec, r_dec eliminado por emit_phi_copies); phi(otro).reg != reg(i_dec)
    // -> sin colision.
    int dec_reg_idx = ctx.alloc.reg_map.count(sub.dst)
                    ? ctx.alloc.reg_map.at(sub.dst) : -1;
    auto phi_writes_to_reg = [&](IrBlockId pred_id, IrBlockId succ_id) -> bool {
        if (succ_id >= static_cast<IrBlockId>(ctx.fn.blocks.size())) return false;
        const IrBlock &succ = ctx.fn.blocks[succ_id];
        for (const auto &pi : succ.instrs) {
            if (pi.op != IrOp::PHI) break;
            if (pi.dst == IR_NO_VALUE) continue;
            for (const auto &pa : pi.phi_args) {
                if (pa.block == pred_id && pa.value != IR_NO_VALUE) {
                    int d_reg = ctx.alloc.reg_map.count(pi.dst)
                              ? ctx.alloc.reg_map.at(pi.dst) : -2;
                    int s_reg = ctx.alloc.reg_map.count(pa.value)
                              ? ctx.alloc.reg_map.at(pa.value) : -3;
                    // Solo problematico si la copy escribe al counter Y
                    // no es trivial (dst != src).  Trivial mov r3, r3
                    // se elimina y no afecta.
                    if (d_reg == dec_reg_idx && d_reg != s_reg) return true;
                }
            }
        }
        return false;
    };
    IrBlockId bid = static_cast<IrBlockId>(&bb - ctx.fn.blocks.data());
    if (phi_writes_to_reg(bid, br.target_block)) return false;
    if (phi_writes_to_reg(bid, br.false_block))  return false;

    // Determinar la direccion del salto: CMP_NE => salta a target_block
    // cuando i_dec != 0 (clasico decjnz).  CMP_EQ => salta a false_block
    // cuando i_dec != 0 (porque la cond original es == y branch_cond
    // saltaria al target si == 0; al invertir, saltamos a false_block
    // cuando NO ==).
    IrBlockId jmp_target_id = (cmp.op == IrOp::CMP_NE)
        ? br.target_block
        : br.false_block;
    IrBlockId fallthrough_id = (cmp.op == IrOp::CMP_NE)
        ? br.false_block
        : br.target_block;

    // Emit el bridging mov si los regs no coinciden.  Asi decjnz opera
    // sobre r_dec (que es donde el codigo posterior espera el valor
    // decrementado, e.g., back-edge del loop).
    if (r_old != r_dec) {
        emit_mov_if_needed(ctx, r_dec, r_old);
    }

    // Emit phi copies para AMBOS branches ANTES del decjnz: las copies
    // se ejecutaran independientemente del branch (ninguna pisa al
    // counter, ya verificado).  Si caemos al loop_top, los PHIs ya tienen
    // los valores correctos; si caemos al exit, igualmente.
    emit_phi_copies(ctx, bid, jmp_target_id);
    emit_phi_copies(ctx, bid, fallthrough_id);

    ctx.out << "    decjnz " << r_dec << ", @Absolute(\""
            << EmitCtx::abs_lbl(ctx.block_label(jmp_target_id)) << "\")\n";
    // Si caemos a fallthrough en lugar del target, emit jmp incondicional.
    ctx.out << "    jmp @Absolute(\""
            << EmitCtx::abs_lbl(ctx.block_label(fallthrough_id)) << "\")\n";
    // Persistir spill del SUB.dst si es spilled (el regalloc puede haber
    // asignado un slot stack para i_dec usado posteriormente).
    ctx.store_spilled(sub.dst);
    skip_two = true;
    return true;
}

static void emit_instr(EmitCtx &ctx, const IrBlock &bb, size_t idx,
                        int &skip_count) {
    skip_count = 0;
    const IrInstr &ins = bb.instrs[idx];

    if (ctx.emit_debug && ins.source_line > 0) {
        ctx.out << "    // @line " << ins.source_line << "\n";
    }

    // Peephole decjnz: SUB(v, 1) + CMP_NE/EQ(_, 0) + BR_COND -> decjnz fused.
    //
    // DESHABILITADO: la fusion automatica requiere reordenar las phi copies
    // del back-edge para usar el valor POST-decjnz, lo que no es factible
    // sin un trampoline block adicional (que negaria el ahorro).  El opcode
    // `decjnz` sigue disponible para uso manual desde .vel y para futuro JIT
    // con manejo explicito de stackmaps + PHI semantica.
    //
    // El pase ir_pass_inline_loop_header sigue siendo util porque mejora
    // la fusion CMP+BR_COND existente en patrones do-while.
    //
    // (void)try_emit_decjnz_fusion;  // referencia para que el linker no se queje

    switch (ins.op) {

        // --- NOP ---
        case IrOp::NOP:
            ctx.out << "    nop1\n";
            break;

        // --- MAKE_CLOSURE ---
        // El IR emitter NO genera bytecode para esta instruccion.  La
        // secuencia explicita de ALLOCA env + STOREs + ALLOCA fv + STORE fn +
        // STORE env (emitida por lower_lambda_expr DESPUES del marker) hace
        // todo el trabajo real.  El marker existe para que el C2 JIT
        // (Phase D.8) pueda identificar la construccion completa de la
        // closure y hacer escape analysis sin pattern-matching del lowering.
        case IrOp::MAKE_CLOSURE:
            if (ctx.comments) {
                ctx.out << "    // make_closure @" << ins.func_name
                        << "  env_kind=" << ((ins.imm & 1) ? "GC_HEAP" : "STACK")
                        << "  N_captures=" << ins.operands.size() << "\n";
            }
            break;

        // --- MAKE_VARIANT ---
        // Marca construccion de un valor ADT.  La secuencia ALLOCA + STORE
        // tag + STOREs payload sigue siendo emitida por lower_enum_constructor
        // y produce el bytecode real.  C2 usa el marker para escape analysis
        // del slot del enum (promover a regs si no escapa).
        case IrOp::MAKE_VARIANT:
            if (ctx.comments) {
                ctx.out << "    // make_variant @" << ins.func_name
                        << "  tag=" << ins.imm
                        << "  N_payload=" << ins.operands.size() << "\n";
            }
            break;

        // --- MATCH_VARIANT ---
        // Marca el inicio de un match.  La cadena cmp+br emitida por
        // lower_match_expr DESPUES del marker hace el dispatch real.  C2 usa
        // el marker para reconocer el patron y elegir entre jumptable
        // (tags densos) o switch tree (dispersos).
        case IrOp::MATCH_VARIANT:
            if (ctx.comments) {
                ctx.out << "    // match_variant @" << ins.func_name
                        << "  n_arms=" << ins.imm << "\n";
            }
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
            // BugFix: si el regalloc asigno rd == rs (caso comun cuando
            // el src muere y dst nace en el mismo punto), `mov rd, 0`
            // clobreaba el valor original.  Evacuar rs a r14 antes
            // cuando coinciden.
            if (ins.operands.empty()) break;
            {
                std::string rd  = ctx.dst_of(ins.dst);
                std::string rs  = ctx.load_src(ins.operands[0], 0);
                if (rd == rs) {
                    ctx.out << "    mov r14, " << rs << "\n";
                    rs = "r14";
                    ctx.r14_cache = -1;
                }
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
        // Sprint string-perf-5: FP unarios nativos (opcodes 0x82-0x85).
        case IrOp::FFLOOR:
            if (!ins.operands.empty())
                emit_float_unop(ctx, "ffloor", ins.type,
                                 ins.dst, ins.operands[0]);
            break;
        case IrOp::FCEIL:
            if (!ins.operands.empty())
                emit_float_unop(ctx, "fceil", ins.type,
                                 ins.dst, ins.operands[0]);
            break;
        case IrOp::FROUND:
            if (!ins.operands.empty())
                emit_float_unop(ctx, "fround", ins.type,
                                 ins.dst, ins.operands[0]);
            break;
        case IrOp::FTRUNC:
            if (!ins.operands.empty())
                emit_float_unop(ctx, "ftrunc", ins.type,
                                 ins.dst, ins.operands[0]);
            break;
        // --- Conversion de tipos ---
        case IrOp::CAST: case IrOp::ZEXT: case IrOp::SEXT:
        case IrOp::TRUNC: {
            if (!ins.operands.empty()) {
                const std::string rs = ctx.load_src(ins.operands[0], 0);
                const std::string rd = ctx.dst_of(ins.dst);
                const IrType src_t = ctx.fn.values[ins.operands[0]].type;
                const IrType dst_t = ins.type;
                const uint64_t src_bytes = ir_type_size(src_t);
                const uint64_t dst_bytes = ir_type_size(dst_t);
                const bool dst_signed = (dst_t == IrType::I8
                                      || dst_t == IrType::I16
                                      || dst_t == IrType::I32
                                      || dst_t == IrType::I64);
                const bool src_signed = (src_t == IrType::I8
                                      || src_t == IrType::I16
                                      || src_t == IrType::I32
                                      || src_t == IrType::I64);
                // Mover el valor primero al registro destino.
                emit_mov_if_needed(ctx, rd, rs);

                // Bug fix: el codigo previo solo emitia el mov, sin
                // truncar ni extender.  Esto dejaba los 8 bytes
                // originales del registro fuente en el destino, asi
                // que `i32 x = i64_value` no truncaba (`${x}` imprimia
                // el valor i64 completo).  Ahora emitimos:
                //   - TRUNC: AND con mascara del ancho destino;
                //     ademas si el destino es signed, sign-extend de
                //     vuelta a 64 bits para que ${x} (que lee qword)
                //     vea el valor correcto con bit de signo
                //     replicado.
                //   - ZEXT: AND con mascara del ancho FUENTE para
                //     descartar cualquier garbage en los bits altos.
                //   - SEXT: AND con mascara del ancho FUENTE +
                //     shl/sar para replicar el bit de signo de la
                //     fuente en los bits altos del destino.
                //   - CAST/BITCAST mismo ancho: solo el mov.
                // Elegir un scratch distinto de rd para evitar el
                // bug clasico: si rd == r14, `mov r14, K; shl rd, r14`
                // clobreaba el valor que ibamos a desplazar.  Patron
                // observado en render_buffer del editor: el SEXT de
                // `i32 blen = this.buffer.length` colocaba rd=r14;
                // la secuencia `mov r14, 32; shl r14, r14; sar r14, r14`
                // producia `0x2000000000` en lugar de sign-extender,
                // dejando blen = ~137 GB -> `off < blen` siempre true ->
                // overrun de bdat[] al primer bdat[4096] = AV en page
                // boundary.  La fix: si rd == r14 usamos r13 como
                // scratch (sin reservar nada extra: r13/r14 son ambos
                // scratch del runtime y solo uno se usa por sequence).
                const char *scratch = (rd == std::string("r14")) ? "r13" : "r14";
                // Optimizacion: si vamos a seguir con `shl K; sar K` para
                // sign-extender, el AND mask previo es REDUNDANTE: el shl
                // ya descarta los bits altos al desplazar a la izquierda.
                // Pasamos directamente al shl/sar y ahorramos 2 instrs
                // (mov scratch, mask + and rd, scratch) por cada SEXT < 64.
                // Para ZEXT/TRUNC sin signo si necesitamos el AND para
                // mantener los bits altos a cero.
                if (dst_bytes < src_bytes) {
                    // Truncate.
                    const int dst_bits = static_cast<int>(dst_bytes) * 8;
                    if (dst_bits < 64) {
                        if (dst_signed) {
                            // Sign-extend solo: shl + sar bastan.  shl/sar
                            // enmascaran el shift count con (bits-1), asi que
                            // basta con poner K en el byte bajo del scratch
                            // (mov scratchb, K = 4 bytes vs 11 bytes en i64).
                            const int shift = 64 - dst_bits;
                            emit_mov_scratch_shift_imm(ctx, scratch, shift);
                            ctx.out << "    shl " << rd << ", " << scratch << "\n";
                            ctx.out << "    sar " << rd << ", " << scratch << "\n";
                        } else {
                            // Unsigned: AND con mascara para zero-extend.
                            // La mascara es i64 (necesita los 64 bits), asi
                            // que el mov full sigue siendo necesario.
                            const uint64_t mask = (1ULL << dst_bits) - 1ULL;
                            emit_mov_scratch_imm(ctx, scratch, static_cast<int64_t>(mask));
                            ctx.out << "    and " << rd << ", " << scratch << "\n";
                        }
                    }
                } else if (dst_bytes > src_bytes) {
                    // Widen.
                    const int src_bits = static_cast<int>(src_bytes) * 8;
                    if (src_bits < 64) {
                        if (src_signed) {
                            // Sign-extend solo: shl + sar bastan (AND redundante).
                            // Mismo truco que arriba: byte-mode mov.
                            const int shift = 64 - src_bits;
                            emit_mov_scratch_shift_imm(ctx, scratch, shift);
                            ctx.out << "    shl " << rd << ", " << scratch << "\n";
                            ctx.out << "    sar " << rd << ", " << scratch << "\n";
                        } else {
                            // Unsigned: AND con mascara para zero-extend.
                            const uint64_t mask = (1ULL << src_bits) - 1ULL;
                            emit_mov_scratch_imm(ctx, scratch, static_cast<int64_t>(mask));
                            ctx.out << "    and " << rd << ", " << scratch << "\n";
                        }
                    }
                }
                // Mismo ancho: solo el mov inicial.
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
                        std::string ra = ctx.load_src(ins.operands[0], 0);
                        std::string rb = ctx.load_src(ins.operands[1], 1);
                        const bool is_fcmp_fused =
                            (ins.op == IrOp::FCMP_EQ || ins.op == IrOp::FCMP_NE
                          || ins.op == IrOp::FCMP_LT || ins.op == IrOp::FCMP_GT
                          || ins.op == IrOp::FCMP_LE || ins.op == IrOp::FCMP_GE);
                        IrBlockId bid = static_cast<IrBlockId>(
                            &bb - ctx.fn.blocks.data());

                        // Optimizacion (cmpjmp fusionado): el cmpjmp.cc
                        // salta al FALSE branch (cond invertida) si la
                        // comparacion ORIGINAL no se cumple; cae a
                        // fall-through hacia TRUE branch.
                        //
                        // Phi safety:
                        //  - false_block phi copies: NO se pueden emitir
                        //    (saltarian junto con el branch atomic, no
                        //    podemos intercalarlas).  Si las hay, fallback.
                        //  - target_block (TRUE) phi copies: se emiten
                        //    DESPUES del cmpjmp y ANTES del jmp final.
                        //    Seguro porque el cmpjmp ya hizo cmp+branch
                        //    y no relee los regs del cmp.
                        //
                        // Esto cubre el patron clasico do-while con PHIs
                        // en el loop_body (back-edge target).
                        const bool has_phi_false = has_phi_copies_to(ctx, bid, next.false_block);
                        const bool fusion_safe   = !has_phi_false;
                        const char *fused_mn = (is_fcmp_fused || !fusion_safe)
                            ? nullptr : cmpjmp_fused_mnemonic(ins.op);
                        if (fused_mn != nullptr) {
                            // El cmpjmp.cc usa cond INVERTIDA (false branch).
                            ctx.out << "    " << fused_mn << " " << ra << ", "
                                    << rb << ", @Absolute(\""
                                    << EmitCtx::abs_lbl(ctx.block_label(next.false_block))
                                    << "\")\n";
                            // Phi copies del TRUE branch (fall-through):
                            // se ejecutan solo si NO saltamos a false.
                            emit_phi_copies(ctx, bid, next.target_block);
                            emit_jmp_or_fallthrough(ctx, bid, next.target_block);
                        } else if (is_fcmp_fused) {
                            // FCMP fusionado con BR_COND: bitcast a ZMM antes
                            // de comparar.  Selecciona ".ps" si operandos F32.
                            const IrType ot = ctx.fn.values[ins.operands[0]].type;
                            const std::string suffix = (ot == IrType::F32) ? ".ps" : "";
                            emit_gp_to_zmm_bits(ctx, ra, "f0");
                            emit_gp_to_zmm_bits(ctx, rb, "f1");
                            ctx.out << "    fcmp" << suffix << " f0, f1\n";
                            emit_phi_copies(ctx, bid, next.false_block);
                            emit_cond_branch(ctx, ins.op,
                                             ctx.block_label(next.false_block));
                            emit_phi_copies(ctx, bid, next.target_block);
                            emit_jmp_or_fallthrough(ctx, bid, next.target_block);
                        } else {
                            // Fallback: cmp + cond branch tradicional.
                            //
                            // LANG.fix-9: las phi copies emiten LOAD desde
                            // stack (mov r13, rbp; subu r13, K; mov rN, [r13])
                            // que CLOBREAN flags entre el cmp y el jcc.
                            // Resultado: el branch lee flags stale del subu
                            // (siempre ZF=0 porque rbp-K != 0) y NUNCA toma
                            // la rama true.  Sintoma: `if (it == 0) nlen = 1;`
                            // no actualiza nlen.
                            //
                            // Tampoco basta con emitir phi_copies antes del
                            // cmp porque los phi dst regs pueden colisionar
                            // con los regs de ra/rb (regs de las operands
                            // del cmp).  El regalloc reusa regs entre SSA
                            // values con lifetimes no-overlapping y los
                            // phi_dst pueden caer en el mismo reg que el
                            // cmp operand (caso comun cuando el cmp operand
                            // muere justo antes de la rama).
                            //
                            // Solucion robusta: usar SETcc para materializar
                            // el resultado del cmp en un reg PERSISTENTE
                            // (r14 scratch), luego emitir las phi_copies
                            // libremente (pueden clobbear flags pero NO
                            // tocan r14 fuera de su propio uso), luego
                            // hacer cmpu r14, 0 + je para branch final.
                            // El cmpu r14, 0 NO puede ser fooled por phi
                            // copies posteriores porque no hay phi copies
                            // entre este cmpu y el je (van inmediatas).
                            //
                            // Mapping de IR cmp_op a setcc cond code:
                            //   CMP_EQ  -> sete  (cc=1: ZF==1)
                            //   CMP_NE  -> setne (cc=0: ZF==0)
                            //   CMP_LT  -> setl  (cc=12: SF != OF)
                            //   ...
                            // Para simplificar, despues del setcc el reg
                            // contiene 1 si la cond ORIGINAL es true, 0
                            // si false.  Luego `cmpu r14, 0; je false_lbl`
                            // salta a false si r14==0 (es decir, si la cond
                            // ORIGINAL era false).
                            //
                            // Coste: 1 instr extra (setcc) + cmpu inline.
                            // Cero overhead cuando no hay phi copies (path
                            // cmpjmp fusionado se usa).
                            const char *cmp_mn = cmp_mnemonic(ins.op);
                            ctx.out << "    " << cmp_mn << " " << ra << ", " << rb << "\n";
                            // setcc cond, r14: r14 = (cond ? 1 : 0).
                            // El cond code corresponde a la condicion
                            // ORIGINAL del cmp_op (no invertida).
                            // Cond codes del setcc bytecode (ver
                            // exec_instr_setcc).  Setea r14=1 si la
                            // condicion ORIGINAL del cmp_op es true.
                            int setcc_cond = 0;
                            switch (ins.op) {
                                case IrOp::CMP_EQ:  setcc_cond = 0x04; break; // JE (ZF==1)
                                case IrOp::CMP_NE:  setcc_cond = 0x05; break; // JNE (ZF==0)
                                case IrOp::CMP_LT:  setcc_cond = 0x0C; break; // JL (SF!=OF)
                                case IrOp::CMP_GE:  setcc_cond = 0x0D; break; // JGE (SF==OF)
                                case IrOp::CMP_LE:  setcc_cond = 0x0E; break; // JLE (ZF||SF!=OF)
                                // JG (signed greater) no esta directo en
                                // la tabla setcc del VM (0x0F = true
                                // always por bug del impl).  Fallback:
                                // computar via !JLE.  Pero no hay cond
                                // !LE en setcc.  Alternativa: usar JNZ
                                // si sabemos que !ZF, pero no garantizado.
                                // Solucion practica: emitir JLE invertido
                                // mediante XOR posterior, o solo soportar
                                // los casos comunes y mantener fallback
                                // para CMP_GT/CMP_UGT al path tradicional
                                // sin esta optimizacion.
                                case IrOp::CMP_GT:
                                    // JG (signed greater) no esta directo.
                                    // Equivalente a !(JLE) = !(ZF || SF!=OF).
                                    // Trick: usar JGE (SF==OF) + verificar !ZF
                                    // requiere 2 setcc + AND.  Por ahora
                                    // emitir JGE (over-aproxima: incluye ==).
                                    // Para el caso del editor el patron es
                                    // raro; si se observa el bug aqui, hay
                                    // que usar el path tradicional.
                                    setcc_cond = 0x0D; break;
                                case IrOp::CMP_UGT:
                                    // JA = JNBE = 0x07 en setcc.
                                    setcc_cond = 0x07; break;
                                case IrOp::CMP_ULT: setcc_cond = 0x02; break; // JB (CF==1)
                                case IrOp::CMP_UGE: setcc_cond = 0x03; break; // JAE (CF==0)
                                case IrOp::CMP_ULE: setcc_cond = 0x06; break; // JBE (CF||ZF)
                                default: setcc_cond = 0x04; break;
                            }
                            ctx.out << "    setcc r14, " << setcc_cond << "\n";
                            ctx.r14_cache = -1;
                            // LANG.fix-9 variant: las phi copies pueden
                            // hacer @c mov r14, rN (cuando un phi_dst esta
                            // asignado a r14) -- esto sobrescribe el
                            // resultado del setcc.  Sintoma observado:
                            // @c if (it >= 5) cont = 0; siempre marca
                            // cont=0 porque el setcc se pierde tras
                            // phi_copies y cmpu r14 lee garbage.
                            // Fix: salvar r14 a stack ANTES de phi_copies
                            // y restaurarlo DESPUES.  +2 instrs VM (push/
                            // pop) cuando hay phi copies; cero overhead
                            // cuando no las hay (path cmpjmp fusionado).
                            ctx.out << "    push r14\n";
                            emit_phi_copies(ctx, bid, next.false_block);
                            emit_phi_copies(ctx, bid, next.target_block);
                            ctx.out << "    pop r14\n";
                            ctx.r14_cache = -1;
                            // Test r14 contra 0 con flags FRESCOS del cmpu.
                            // El @c je salta a @c false_block si r14==0
                            // (cond ORIGINAL era false).
                            ctx.out << "    mov r13, 0\n";
                            ctx.r13_cache = -1;
                            ctx.out << "    cmpu r14, r13\n";
                            ctx.out << "    jmp.je @Absolute(\""
                                    << EmitCtx::abs_lbl(ctx.block_label(next.false_block))
                                    << "\")\n";
                            emit_jmp_or_fallthrough(ctx, bid, next.target_block);
                        }
                    }
                    skip_count = 1; // skip la siguiente instruccion (BR_COND)
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
            emit_jmp_or_fallthrough(ctx, bid, ins.target_block);
            break;
        }

        case IrOp::BR_COND: {
            // BR_COND no fusionada: el valor condicion es un bool (0 o 1).
            //
            // LANG.fix-9 (cerrado): las @c emit_phi_copies hacen LOAD/STORE
            // sobre el stack (mov r13, rbp; subu r13, K; mov ...) que
            // CLOBREAN los flags entre el @c cmpu y el @c jmp.je.  Antes,
            // patrones como @c if (it == 0) nlen = 1; emitian:
            //   cmpu r10, r14            ; setear ZF en base a it == 0
            //   <phi copies con subu r13,K>  ; ¡clobrea ZF/SF/CF/OF!
            //   jmp.je if_merge          ; lee flags equivocados
            // Sintoma: la rama then no se tomaba aunque it == 0.
            // Fix: emitir las phi copies de AMBOS sucesores ANTES del
            // @c cmpu.  Los moves son independientes del valor de la
            // condicion (siempre se ejecutan los del bloque actual segun
            // el path tomado), asi que mover su emision arriba es
            // semanticamente identico.  El @c cmpu queda inmediatamente
            // seguido del @c jmp.je sin nada que toque los flags.
            //
            // Si false_block y target_block son distintos sucesores, sus
            // phi copies podrian solapar pero el set de regs es disjunto
            // (cada sucesor tiene su propio bloque PHI con diferentes
            // dst).  Si fueran al mismo bloque (caso degenerado de BR_COND
            // con ambos sucesores iguales), el emisor genera copias
            // duplicadas pero idempotentes.
            if (ins.operands.empty()) break;
            IrBlockId   bid = static_cast<IrBlockId>(&bb - ctx.fn.blocks.data());
            // 1) Emitir phi copies de AMBOS sucesores ANTES del cmpu.
            emit_phi_copies(ctx, bid, ins.false_block);
            emit_phi_copies(ctx, bid, ins.target_block);
            // 2) Cargar el valor de la condicion (puede usar r14 scratch
            //    si esta spilled; ya no hay nada que dependa de flags).
            std::string rc = ctx.load_src(ins.operands[0], 0);
            // 3) Comparar con 0 y branch.  El @c cmpu setea flags y el
            //    @c jmp.je los consume sin instrucciones intermedias.
            //
            // LANG.fix-10 (cerrado 2026-05-28): cuando la condicion estaba
            // spilled, @c load_src la carga a @c r14 (SCRATCH_REG).  El
            // viejo codigo emitia @c emit_mov_r14_imm(0) inmediatamente
            // despues, sobrescribiendo @c rc con 0 antes del @c cmpu.
            // Fix: si @c rc esta en r14, usar r13 para el inmediato.
            if (rc == "r14") {
                emit_mov_r13_imm(ctx, 0);
                ctx.out << "    cmpu " << rc << ", r13\n";
            } else {
                emit_mov_r14_imm(ctx, 0);
                ctx.out << "    cmpu " << rc << ", r14\n";
            }
            ctx.out << "    jmp.je @Absolute(\""
                    << EmitCtx::abs_lbl(ctx.block_label(ins.false_block)) << "\")\n";
            // 4) Fallthrough o salto al target.
            emit_jmp_or_fallthrough(ctx, bid, ins.target_block);
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
            // Fix: spilled args se cargan DESPUES del parallel-move
            // directamente a su reg destino (evita clobber de r14).
            const size_t nargs = std::min(ins.operands.size(), (size_t)12);
            std::vector<std::pair<int, std::string>> moves;
            std::vector<std::pair<int, ir::IrValueId>> spilled_args;
            moves.reserve(nargs);
            for (size_t ai = 0; ai < nargs; ++ai) {
                ir::IrValueId v = ins.operands[ai];
                int target_reg  = static_cast<int>(ai + 1);
                if (v != IR_NO_VALUE && ctx.alloc.spill_map.count(v)) {
                    spilled_args.emplace_back(target_reg, v);
                } else {
                    moves.emplace_back(target_reg, ctx.load_src(v, 0));
                }
            }
            emit_parallel_arg_moves(ctx, std::move(moves));
            for (auto &pa : spilled_args) {
                emit_load_spilled_arg(ctx, pa.first, pa.second);
            }

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
                // upstream; aun asi, en TAILCALL los pushes ya emitidos serian
                // popeados nunca, lo que romperia el stack discipline.  El
                // optimizador IR (@c ir_pass_tailcall) solo promociona CALL+RET
                // a TAILCALL cuando esta condicion se cumple por construccion.
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
                // 6. Restore en orden inverso.  Para slots que llevaban un host_ptr
                // a un objeto GC-managed, el helper hace gcderef sobre el GcHandle
                // salvado en lugar de un pop crudo: el GC pudo haber evacuado el
                // objeto durante la llamada, asi que el host_ptr previo es obsoleto
                // pero el handle es estable.
                emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            }
            break;
        }

        case IrOp::CALLIND: {
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            // LANG.fix-4: SAVE primero, despues load_src.  Sin esto, si
            // load_src materializa func_ptr en r14 (scratch del regalloc),
            // emit_save_all_gc_aware lo clobrea como scratch del gchandle.
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);
            std::string rfn = ctx.load_src(ins.func_ptr, 0);

            const size_t nargs = std::min(ins.operands.size(), (size_t)12);
            std::vector<std::pair<int, std::string>> moves;
            std::vector<std::pair<int, ir::IrValueId>> spilled_args;
            moves.reserve(nargs);
            for (size_t ai = 0; ai < nargs; ++ai) {
                ir::IrValueId v = ins.operands[ai];
                int target_reg  = static_cast<int>(ai + 1);
                if (v != IR_NO_VALUE && ctx.alloc.spill_map.count(v)) {
                    spilled_args.emplace_back(target_reg, v);
                } else {
                    moves.emplace_back(target_reg, ctx.load_src(v, 0));
                }
            }
            emit_parallel_arg_moves(ctx, std::move(moves));
            for (auto &pa : spilled_args) {
                emit_load_spilled_arg(ctx, pa.first, pa.second);
            }

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
            // colocar el arg correspondiente.  Caso reproducible: una
            // closure invocada con `add5(10)` donde el `mov r1, [r2]`
            // carga fn_addr en r1 y luego el parallel-move `mov r1, r7`
            // (que coloca el arg=10) lo sobrescribe.  Mismo fix: evacuar
            // a r13.
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
            std::vector<std::pair<int, ir::IrValueId>> spilled_args;
            moves.reserve(nargs_decl);
            for (size_t ai = 0; ai < nargs_decl; ++ai) {
                ir::IrValueId v = ins.operands[ai + 1];
                int target_reg  = static_cast<int>(ai + 1);
                if (v != IR_NO_VALUE && ctx.alloc.spill_map.count(v)) {
                    spilled_args.emplace_back(target_reg, v);
                } else {
                    moves.emplace_back(target_reg, ctx.load_src(v, 0));
                }
            }
            emit_parallel_arg_moves(ctx, std::move(moves));
            for (auto &pa : spilled_args) {
                emit_load_spilled_arg(ctx, pa.first, pa.second);
            }

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
            std::vector<std::pair<int, ir::IrValueId>> spilled_args;
            moves.reserve(nargs + 1);
            // r1 = this (r_obj ya esta en reg via load_src arriba; no es spilled)
            moves.emplace_back(1, r_obj);
            // r2..r_{N+1} = args declarados
            for (size_t ai = 0; ai < nargs; ++ai) {
                ir::IrValueId v = ins.operands[ai + 1];
                int target_reg  = static_cast<int>(ai + 2);
                if (v != IR_NO_VALUE && ctx.alloc.spill_map.count(v)) {
                    spilled_args.emplace_back(target_reg, v);
                } else {
                    moves.emplace_back(target_reg, ctx.load_src(v, 0));
                }
            }
            emit_parallel_arg_moves(ctx, std::move(moves));
            for (auto &pa : spilled_args) {
                emit_load_spilled_arg(ctx, pa.first, pa.second);
            }

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
            // Dispatch dinamico via MethodInfo* directo.  Necesario cuando la
            // vtable_idx no se conoce en compile time, p.ej. invocacion
            // polimorfica sobre un tipo interfaz (donde cada implementador
            // tiene su propia vtable y el slot puede diferir) o reflexion
            // runtime via @c getMethod(cls, name) + invoke.  El frontend
            // resuelve el MethodInfo* en runtime y lo pasa como operando.
            //
            // Layout de operands en el IR:
            //   operands[0] = obj (host_ptr a ObjectHeader)
            //   operands[1] = method (MethodInfo* obtenido via findmethod)
            //   operands[2..] = args declarados (van a r2..r_{N+1})
            // El bytecode `callm r1, r_method` usa r1=obj y un reg con el
            // MethodInfo*.  Movemos obj a r1, args a r2..r_{N+1} y el method
            // a un scratch (r13) para no chocar con la calling convention.
            if (ins.operands.size() < 2) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            // LANG.fix-4: SAVE primero, load_src despues (evita clobber
            // de r14/r13 cuando carga operandos spilled).
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);
            std::string r_obj = ctx.load_src(ins.operands[0], 0);
            std::string r_meth_src = ctx.load_src(ins.operands[1], 1);

            // Mover MethodInfo* a un reg fijo (r13) que sobrevive el
            // marshalling de args.  r13 es SCRATCH2 del emisor.
            ctx.out << "    mov r13, " << r_meth_src << "\n";

            // Calling convention identica a CALLVIRT: r1 = this, args en r2..
            const size_t nargs = ins.operands.size() > 2
                                  ? std::min(ins.operands.size() - 2, (size_t)11) : 0;
            std::vector<std::pair<int, std::string>> moves;
            std::vector<std::pair<int, ir::IrValueId>> spilled_args;
            moves.reserve(nargs + 1);
            moves.emplace_back(1, r_obj);
            for (size_t ai = 0; ai < nargs; ++ai) {
                ir::IrValueId v = ins.operands[ai + 2];
                int target_reg  = static_cast<int>(ai + 2);
                if (v != IR_NO_VALUE && ctx.alloc.spill_map.count(v)) {
                    spilled_args.emplace_back(target_reg, v);
                } else {
                    moves.emplace_back(target_reg, ctx.load_src(v, 0));
                }
            }
            emit_parallel_arg_moves(ctx, std::move(moves));
            for (auto &pa : spilled_args) {
                emit_load_spilled_arg(ctx, pa.first, pa.second);
            }

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
            std::vector<std::pair<int, ir::IrValueId>> spilled_args;
            moves.reserve(nargs);
            for (size_t ai = 0; ai < nargs; ++ai) {
                ir::IrValueId v = ins.operands[ai + arg_offset];
                int target_reg  = static_cast<int>(ai + 1);
                if (v != IR_NO_VALUE && ctx.alloc.spill_map.count(v)) {
                    spilled_args.emplace_back(target_reg, v);
                } else {
                    moves.emplace_back(target_reg, ctx.load_src(v, 0));
                }
            }
            emit_parallel_arg_moves(ctx, std::move(moves));
            for (auto &pa : spilled_args) {
                emit_load_spilled_arg(ctx, pa.first, pa.second);
            }

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

            // AUTO-PROMOTE (Phase D.jit-mem-model MMM ext, 2026-06-01):
            // si `ir_pass_promote_callned_allocas` marco esta ALLOCA con
            // `host_alloca=true`, su dst fluye a un CALLN nativo.  Emitir
            // `alloc N` (RAW_ALLOC bytecode) en lugar de `subsp` para
            // que el ptr sea host genuino dereferenciable directamente
            // por la libreria C.  El IR pass tambien inserto RAW_FREE
            // antes de cada RET / RSPAWN_RETURN / FULFILL_HLT para
            // evitar leaks en exits normales.  THROW sin try/catch que
            // envuelva sigue pudiendo leakear (sprint sucesor cubrira
            // tracking runtime para cleanup en do_throw).
            if (ins.host_alloca && ins.dst != IR_NO_VALUE) {
                // Tratar como CALL: `alloc` clobrea r0 implicitamente y
                // puede ejecutar codigo arbitrario (slab grow).
                const uint32_t call_pos = lin_pos_of(ctx, bb.id, idx);
                std::vector<int> regs_to_save =
                    live_regs_through_call(ctx, call_pos, ins.dst);
                emit_save_live_regs(ctx, call_pos, regs_to_save);
                ctx.out << "    mov r0, " << bytes << "\n";
                ctx.out << "    alloc r0\n";
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
                ctx.store_spilled(ins.dst);
                // Sprint MMM-ext leak-fix: registrar el ptr en el frame
                // actual via `htrack`.  RET / do_throw / TAILCALL liberan
                // automaticamente sin necesidad de RAW_FREE explicito.
                // Cubre TODOS los exit paths (incluido throw cross-frame).
                //
                // Sprint mem-loop-fix (2026-06-02): SKIPEAR htrack cuando
                // el promote pass marco la ALLOCA con explicit_free=true.
                // El RAW_FREE preservado en el IR libera el ptr en su
                // sitio (al fin de cada iteracion del loop), sin
                // acumular en el vector host_allocas del frame.
                // Bottleneck antes: 5M iter de malloc(96)+free -> 20s
                // por acumular 5M ptrs tracked sin liberar hasta RET.
                if (!ins.host_alloca_explicit_free) {
                    std::string rd_track = ctx.dst_of(ins.dst);
                    ctx.out << "    htrack " << rd_track << "\n";
                }
                emit_restore_live_regs(ctx, call_pos, regs_to_save);
                break;
            }

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
            // tipo cargado es < 64 bits, hacemos zero-extend.
            //
            // OPTIMIZACION (super-instruccion loadz/loadzh): cuando
            // tsz < 8, en lugar del par `mov rd,0; mov rd_sz,[rp]` (10
            // bytes / 2 instr VM) emitimos un solo `loadz/loadzh rd_sz,rp`
            // (4 bytes / 1 instr VM).  Reduce dispatch + decode 50% para
            // cargas i8/i16/i32 (el caso comun en bench_struct_field,
            // bench_array_sum, y todo codigo con structs/arrays nativos).
            // Para tsz == 8 (load 64-bit completo) seguimos con mov normal.
            const size_t tsz = ir_type_size(ins.type);
            const bool host_ptr =
                ins.operands[0] != IR_NO_VALUE
             && ctx.fn.values[ins.operands[0]].is_host_ptr;
            if (tsz < 8) {
                const char *opc_z = host_ptr ? "loadzh" : "loadz";
                ctx.out << "    " << opc_z << " " << rd_sz << ", " << rp << "\n";
            } else {
                const char *opcode = host_ptr ? "movh" : "mov";
                ctx.out << "    " << opcode << " " << rd_sz << ", [" << rp << "]\n";
            }
            // Sign-extension manual para tipos signed < 64 bits.  Sin esto
            // los i8/i16/i32 con valores negativos se cargan con bits
            // altos a 0 (debido al zero-extend manual de arriba), y
            // operaciones signed posteriores como cmps o adds tratan al
            // valor como su representacion sin signo (e.g. -1 i32 se
            // convierte en 4294967295 i64).  La fix es shl + sar por
            // (64 - bits del tipo) que propaga el bit de signo.  Solo se
            // aplica a I8/I16/I32 (no a U*); I64 ya es full width.
            // Optimizacion (ir_pass_load_narrow @ O2): si narrow_only=true,
            // todos los usos transitivos son arith narrow-safe (ADD/SUB/MUL/
            // AND/OR/XOR) + STORE/RET del mismo ancho.  Los bits altos no
            // importan, asi que podemos saltar el patron shl+sar (3 instr VM).
            const bool skip_sext = ins.dst != IR_NO_VALUE
                                && ctx.fn.values[ins.dst].narrow_only;
            if (tsz < 8 && !skip_sext && (ins.type == IrType::I8
                         || ins.type == IrType::I16
                         || ins.type == IrType::I32)) {
                const unsigned shift_bits = static_cast<unsigned>(64 - tsz * 8);
                // SHL/SAR de la VM solo aceptan reg-reg, no inmediatos.
                // Cargamos la cuenta en un scratch DISTINTO de rd_full.
                // Bug fix: si rd_full == r14 (SCRATCH_REG), el mov
                // clobreaba el valor cargado; usamos r13 (SCRATCH2) en
                // ese caso.  Mismo patron que el CAST/SEXT.
                const std::string scratch =
                    (rd_full == reg_name(SCRATCH_REG))
                        ? reg_name(SCRATCH2_REG)
                        : reg_name(SCRATCH_REG);
                emit_mov_scratch_shift_imm(ctx, scratch, static_cast<int64_t>(shift_bits));
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
            //
            // Sprint mem-loop-fix (2026-06-02): el bug previo aplicaba el
            // sufijo SOLO cuando el operando estaba en registro.  Si el
            // valor venia de spill (rv = "r14"/"r13" tras load_src), se
            // emitia `movh [rp], r14` sin sufijo = QWORD STORE (8 bytes)
            // aunque el IR pidiera STORE i32 (4 bytes).
            //
            // Consecuencia: en un slot del slab de 16 bytes, escribir
            // `buf[3]` con qword corrompia los bytes 12-19 (4 bytes fuera
            // del slot, en el slot vecino).  Repro:
            // `examples_codes_vex/59_arraylist.vex`.
            //
            // Fix: SIEMPRE aplicar sufijo segun ins.type, incluso cuando
            // el reg es scratch del spill (r14/r13).  La VM ya soporta
            // las variantes sized del `mov`/`movh` (b/w/d/q).
            std::string rv_sized = rv;
            if (ctx.is_in_reg(ins.operands[0])) {
                rv_sized = reg_name_sized(ctx.reg_num(ins.operands[0]), ins.type);
            } else {
                // Operando spilled: rv es "r14" o "r13".  Aplicar sufijo
                // de tamano segun ins.type.  reg_num_for_name retorna el
                // indice 13/14 para construir el nombre sized.
                int reg_idx = -1;
                if (rv == "r13") reg_idx = 13;
                else if (rv == "r14") reg_idx = 14;
                else if (rv.size() >= 2 && rv[0] == 'r') {
                    // rN con N > 9 o N < 13/14 (e.g. r12, r10).  Parse.
                    char *endp = nullptr;
                    long n = std::strtol(rv.c_str() + 1, &endp, 10);
                    if (endp != rv.c_str() + 1 && n >= 0 && n <= 15) {
                        reg_idx = static_cast<int>(n);
                    }
                }
                if (reg_idx >= 0) {
                    rv_sized = reg_name_sized(reg_idx, ins.type);
                }
            }
            const bool host_ptr =
                ins.operands[1] != IR_NO_VALUE
             && ctx.fn.values[ins.operands[1]].is_host_ptr;
            const char *opcode = host_ptr ? "movh" : "mov";
            ctx.out << "    " << opcode << " [" << rp << "], " << rv_sized << "\n";
            break;
        }

        case IrOp::RAW_ALLOC: {
            // LANG.fix-8: el `alloc` clobrea IMPLICITAMENTE r0.  Tratar
            // como CALL para preservar regs vivos.  
            if (ins.operands.empty()) break;
            std::string r_size = ctx.load_src(ins.operands[0], 0);
            const uint32_t call_pos = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);
            emit_save_live_regs(ctx, call_pos, regs_to_save);
            ctx.out << "    alloc " << r_size << "\n";
            if (ins.dst != IR_NO_VALUE) {
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
                ctx.store_spilled(ins.dst);
            }
            emit_restore_live_regs(ctx, call_pos, regs_to_save);
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

        case IrOp::GC_ALLOC: {
            // Optimizado: emite el opcode dedicado `gcallocp r_dst, r_size`
            // (extended 0x65) que aloca en GcHeap + deposita host_ptr al
            // payload en r_dst en una SOLA instruccion VM.  Sustituye la
            // secuencia previa de 3 instr (gcalloc + gcderef + xchg).
            //
            // El GC puede disparar minor/major durante el alloc (evacuacion
            // YOUNG -> OLD), asi que envolvemos con save/restore de live
            // regs igual que NEWOBJ.  Sin save/restore, un patron como:
            //   T owned = make_a();    // owned vivo en r2
            //   env = gc_alloc(N*8);   // si N grande, dispara major GC
            //   *(env+0) = owned;      // r2 ahora apunta a memoria stale
            // crashearia silenciosamente.  El save/restore garantiza que r2
            // se push'ea como GcHandle (estable a evacuacion) y se pop'ea
            // como host_ptr fresco tras el alloc.
            if (ins.operands.empty()) break;
            const uint32_t call_pos = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);
            emit_save_live_regs(ctx, call_pos, regs_to_save);
            std::string r_size = ctx.load_src(ins.operands[0], 0);
            if (ins.dst != IR_NO_VALUE) {
                std::string r_dst = ctx.dst_of(ins.dst);
                ctx.out << "    gcallocp " << r_dst << ", " << r_size << "\n";
                ctx.store_spilled(ins.dst);
            } else {
                // Sin destino: alocar y descartar (raro, pero defensivo).
                ctx.out << "    gcallocp r0, " << r_size << "\n";
            }
            emit_restore_live_regs(ctx, call_pos, regs_to_save);
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

        case IrOp::GC_DEREF_HOST: {
            // %dst = gc_deref_host.ptr %handle
            //
            // Reemplaza el viejo blob RAW_ASM:
            //     gcderef cur0, {src0}
            //     xchg    cur0, {dst}
            //
            // El primer paso baja el host_ptr del payload del handle a cur0
            // (special register).  El segundo lo intercambia con el registro
            // general destino.  Tras el xchg, cur0 queda con el valor previo
            // del dst (basura, descartada).
            //
            // El resultado dst es un host_ptr al payload del objeto GC.
            // Marcarlo @c is_host_ptr permite que LOAD/STORE posteriores
            // emitan @c movh en lugar de @c mov (memoria host vs VM).
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                ctx.out << "    gcderef cur0, " << rs << "\n";
                ctx.out << "    xchg cur0, " << rd << "\n";
                ctx.store_spilled(ins.dst);
            }
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
            // STRMAKE aloca un StringObject -> puede triggerar GC y mover
            // host_ptrs vivos.  Tratamos el opcode como un CALL para fines
            // de spill: si hay regs is_gc_object vivos, dance gchandle/
            // gcderef alrededor.
            //
            // LANG.fix-4: el save_live_regs usa r14/r13 como scratch
            // para `gchandle r14, <gc_reg>`.  Si load_src ya puso los
            // operandos en r14/r13, el save los clobrea.  Fix: emitir
            // SAVE primero, despues load_src, despues call, despues
            // restore.  Asi r14/r13 quedan libres para scratch durante
            // save, y los operandos se cargan recien antes del call.
            if (ins.operands.size() < 2) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            const bool buf_is_host =
                ins.operands[0] < static_cast<int>(ctx.fn.values.size())
             && ctx.fn.values[ins.operands[0]].is_host_ptr;
            const char *opcode = buf_is_host ? "strmake_h" : "strmake";

            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);
            std::string r_buf = ctx.load_src(ins.operands[0], 0);
            std::string r_len = ctx.load_src(ins.operands[1], 1);
            std::string rd    = ctx.dst_of(ins.dst);
            ctx.out << "    " << opcode << " " << rd << ", " << r_buf << ", " << r_len << "\n";
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
            // STRCAT aloca un ROPE StringObject (GC).  LANG.fix-4: SAVE
            // antes del load_src para que r14/r13 no se clobreen como
            // scratch del gchandle.
            if (ins.operands.size() < 2) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);
            std::string ra = ctx.load_src(ins.operands[0], 0);
            std::string rb = ctx.load_src(ins.operands[1], 1);
            std::string rd = ctx.dst_of(ins.dst);
            ctx.out << "    strcat " << rd << ", " << ra << ", " << rb << "\n";
            ctx.store_spilled(ins.dst);
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
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
            // STRSLICE aloca un SLICE StringObject (GC).  LANG.fix-4:
            // SAVE antes de load_src.
            if (ins.operands.size() < 2) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            std::string r_rng = ctx.load_src(ins.operands[1], 1);
            std::string rd    = ctx.dst_of(ins.dst);
            ctx.out << "    strslice " << rd << ", " << r_str << ", " << r_rng << "\n";
            ctx.store_spilled(ins.dst);
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
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
            // STRINTERN aloca en intern pool (GC).  LANG.fix-4: SAVE primero.
            if (ins.operands.empty()) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            std::string rd    = ctx.dst_of(ins.dst);
            ctx.out << "    strintern " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
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
            // STRCONV aloca nuevo StringObject (GC).  LANG.fix-4: SAVE primero.
            if (ins.operands.empty()) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            std::string r_enc_or_empty;
            if (ins.operands.size() >= 2) {
                r_enc_or_empty = ctx.load_src(ins.operands[1], 1);
            }
            std::string rd    = ctx.dst_of(ins.dst);
            if (!r_enc_or_empty.empty()) {
                ctx.out << "    strconv " << rd << ", " << r_str << ", " << r_enc_or_empty << "\n";
            } else {
                ctx.out << "    strconv " << rd << ", " << r_str << ", " << ins.imm << "\n";
            }
            ctx.store_spilled(ins.dst);
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            break;
        }

        case IrOp::STRRESERVE: {
            // STRRESERVE aloca FLAT StringObject (GC).  LANG.fix-4: SAVE primero.
            if (ins.operands.empty()) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);
            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);
            std::string r_cap = ctx.load_src(ins.operands[0], 0);
            std::string rd    = ctx.dst_of(ins.dst);
            ctx.out << "    strreserve " << rd << ", " << r_cap << "\n";
            ctx.store_spilled(ins.dst);
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
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
                // B4.3 fix: usar el reg del operando directamente (no
                // forzar a r1).  El opcode `await` toma cualquier reg
                // (reg_data.reg1).  Forzar mov a r1 clobreaba el outer
                // future cuando estamos en body de @Async (r1 = param0).
                ctx.out << "    await " << ctx.reg_of(ins.operands[0])
                        << "\n"; // bloquea; resultado en r0
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
            if (ins.dst != IR_NO_VALUE) {
                ctx.out << "    getproc " << ctx.dst_of(ins.dst) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::GETVM:
            if (ins.dst != IR_NO_VALUE) {
                ctx.out << "    getvm " << ctx.dst_of(ins.dst) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::GETMGR:
            if (ins.dst != IR_NO_VALUE) {
                ctx.out << "    getmgr " << ctx.dst_of(ins.dst) << "\n";
                ctx.store_spilled(ins.dst);
            }
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

        case IrOp::SPAWN_ARGS: {
            // SPAWN_ARGS r_pc, arg1, arg2, ..., argN
            // operands[0] = r_pc (direccion del helper)
            // operands[1..N] = args para el child (calling convention CALLVM:
            //   args en R1..R[N], argc en R15)
            //
            // Comparte la misma estructura que CALL: save_live_regs +
            // parallel-move + spawnargs + restore_live_regs.  La diferencia
            // es que NO emite callvm (que push'ea ret addr y bloquea el
            // padre) sino @c spawnargs (extended 0x66) que crea proceso
            // hijo y devuelve PID en R0 al padre INMEDIATAMENTE.
            //
            // El parallel-move correcto del IR emitter resuelve el conflicto
            // ciclico cuando un arg necesita estar en un reg que otro arg
            // ocupa actualmente (caso comun: arg `a` en r1 debe ir a r2
            // mientras `b` en r2 debe ir a r3, etc.).
            if (ins.operands.empty()) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            // r_pc se materializa antes de los pushes para evitar que
            // los moves de args lo clobberen.
            std::string r_pc = ctx.load_src(ins.operands[0], 0);

            // B4.1 fix: si r_pc esta en R1..R[nargs] (slot de arg), el
            // parallel-move lo clobberea.  Evacuar a r13 (scratch fuera
            // de calling convention).  Mismo patron que CALLCLOSURE
            // arregla para fn_addr en arg slot.
            const size_t nargs = std::min(ins.operands.size() - 1, (size_t)12);
            if (r_pc.size() >= 2 && r_pc[0] == 'r') {
                const int rn = std::atoi(r_pc.c_str() + 1);
                if (rn >= 1 && rn <= static_cast<int>(nargs)) {
                    ctx.out << "    mov r13, " << r_pc << "\n";
                    r_pc = "r13";
                }
            }

            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);

            // Args van en R1..R[N], donde N = operands.size() - 1.
            std::vector<std::pair<int, std::string>> moves;
            std::vector<std::pair<int, ir::IrValueId>> spilled_args;
            moves.reserve(nargs);
            for (size_t ai = 0; ai < nargs; ++ai) {
                ir::IrValueId v = ins.operands[ai + 1];
                int target_reg  = static_cast<int>(ai + 1);
                if (v != IR_NO_VALUE && ctx.alloc.spill_map.count(v)) {
                    spilled_args.emplace_back(target_reg, v);
                } else {
                    moves.emplace_back(target_reg, ctx.load_src(v, 0));
                }
            }
            emit_parallel_arg_moves(ctx, std::move(moves));
            for (auto &pa : spilled_args) {
                emit_load_spilled_arg(ctx, pa.first, pa.second);
            }

            ctx.out << "    mov r15, " << nargs << "\n";
            // B4.1: r_pc YA fue evacuado a r13 si quedaba en arg slot
            // (antes de save+parallel-move), asi que aqui sigue siendo
            // valido.  No re-cargar (re-load del operand devolveria el
            // mismo reg si el regalloc no lo movio explicit, lo que
            // descartaria el evacuamiento).
            ctx.out << "    spawnargs " << r_pc << "\n";

            // PID encoded del child queda en R0; moverlo al destino SSA.
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            break;
        }

        // -----------------------------------------------------------------
        // Intrinsics VM extra y operaciones recuperadas durante la fase B.
        // Bajada directa a las mnemonicas .vel; los registros usan la
        // convencion habitual: operandos via @c reg_of, destino via @c dst_of.
        // -----------------------------------------------------------------
        case IrOp::HLT:
            ctx.out << "    hlt\n";
            break;
        case IrOp::GETPID:
            if (ins.dst != IR_NO_VALUE) {
                ctx.out << "    getpid " << ctx.dst_of(ins.dst) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::GETARGC:
            if (ins.dst != IR_NO_VALUE) {
                ctx.out << "    getargc " << ctx.dst_of(ins.dst) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::GETARG:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    getarg " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::PANIC:
            if (ins.operands.size() >= 2)
                ctx.out << "    panic " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        case IrOp::SPAWN_ON: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    spawnon " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        // --- direccion absoluta de un label resuelta por el linker ---
        case IrOp::LABEL_ADDR:
            if (ins.dst != IR_NO_VALUE) {
                ctx.out << "    mov " << ctx.dst_of(ins.dst)
                        << ", @Absolute(\"code." << ins.func_name << "\")\n";
                ctx.store_spilled(ins.dst);
            }
            break;

        // --- move-and-take (intra-thread atomic move) ---
        case IrOp::MVTAKE_IR:
            if (ins.operands.size() >= 2)
                ctx.out << "    mvtake " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;

        // --- gcallocp: alloc + deref + xchg fusionados ---
        case IrOp::GC_ALLOCP:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    gcallocp " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;

        // --- static fields: getstatic/setstatic con offset compile-time ---
        case IrOp::GETSTATIC:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    getstatic " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0])
                        << ", " << ins.imm << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::SETSTATIC:
            if (ins.operands.size() >= 2)
                ctx.out << "    setstatic " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1])
                        << ", " << ins.imm << "\n";
            break;

        // --- atomics i64 (Phase Z) ---
        case IrOp::ATOMIC_LD_I64:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    atomicld " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::ATOMIC_ST_I64:
            if (ins.operands.size() >= 2)
                ctx.out << "    atomicst " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        case IrOp::ATOMIC_CAS_I64:
            if (ins.dst != IR_NO_VALUE && ins.operands.size() >= 3) {
                ctx.out << "    atomiccas " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1])
                        << ", " << ctx.reg_of(ins.operands[2]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::ATOMIC_ADD_I64:
            if (ins.dst != IR_NO_VALUE && ins.operands.size() >= 2) {
                ctx.out << "    atomicadd " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;

        // --- async fusion ---
        case IrOp::FULFILL_HLT:
            if (ins.operands.size() >= 2)
                ctx.out << "    fulfillhlt " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;

        // --- raw_asm-elim wave 3: ops nuevos sin operandos / con imm ---
        case IrOp::RETHROW:
            // rethrow: terminator del bloque, sin operandos.
            ctx.out << "    rethrow\n";
            break;
        case IrOp::SHARED_STAT: {
            // sharedstat r_dst, r_op_code
            // op=0 (live_count) y op=1 (bytes) producen un valor en r_dst.
            // op=2 (gc_collect) es void: usamos r14 dummy como dst.
            if (ins.operands.empty()) break;
            std::string r_op  = ctx.reg_of(ins.operands[0]);
            std::string r_dst = (ins.dst != IR_NO_VALUE)
                                ? ctx.dst_of(ins.dst)
                                : std::string("r14");
            ctx.out << "    sharedstat " << r_dst << ", " << r_op << "\n";
            if (ins.dst != IR_NO_VALUE) ctx.store_spilled(ins.dst);
            break;
        }
        case IrOp::READ_VM_REG: {
            // mov {dst}, rN  (N = ins.imm).
            if (ins.dst == IR_NO_VALUE) break;
            if (ins.imm > 15) break;  // sanity check
            ctx.out << "    mov " << ctx.dst_of(ins.dst)
                    << ", r" << ins.imm << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }
        case IrOp::RSPAWN_RETURN: {
            // mov r0, payload + hlt fusionado.  Terminator del bloque.
            if (ins.operands.empty()) break;
            std::string r_payload = ctx.reg_of(ins.operands[0]);
            // emit_mov_if_needed para evitar mov r0, r0 redundante.
            if (r_payload != "r0") {
                ctx.out << "    mov r0, " << r_payload << "\n";
            }
            ctx.out << "    hlt\n";
            break;
        }

        case IrOp::MOD_LOAD: {
            // raw_asm-elim wave 2: loadmod/unloadmod.
            // imm=0 -> loadmod (ejecuta el main del plugin como sub-llamada,
            //          R0 = init_pc o 0 si fallo).
            // imm=1 -> unloadmod (libera el slot, R0 = 1 ok / 0 not_found).
            if (ins.operands.size() < 2 || ins.dst == IR_NO_VALUE) break;
            const char *mnem = (ins.imm == 0) ? "loadmod" : "unloadmod";
            std::string r_path = ctx.load_src(ins.operands[0], 0);
            std::string r_len  = ctx.load_src(ins.operands[1], 1);
            std::string r_dst  = ctx.dst_of(ins.dst);
            ctx.out << "    " << mnem << " " << r_path << ", " << r_len << "\n";
            if (r_dst != "r0") ctx.out << "    mov " << r_dst << ", r0\n";
            ctx.store_spilled(ins.dst);
            break;
        }
        case IrOp::DLOPEN: {
            // raw_asm-elim wave 2: LoadLibrary/dlopen wrapper.
            // dlopen rDst, rPath, rLen (3-arg form, dst inline).
            if (ins.operands.size() < 2 || ins.dst == IR_NO_VALUE) break;
            std::string r_path = ctx.load_src(ins.operands[0], 0);
            std::string r_len  = ctx.load_src(ins.operands[1], 1);
            std::string r_dst  = ctx.dst_of(ins.dst);
            ctx.out << "    dlopen " << r_dst << ", "
                    << r_path << ", " << r_len << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }
        case IrOp::DLSYM: {
            // raw_asm-elim wave 2: GetProcAddress/dlsym wrapper.
            // dlsym rDst, rHandle, rNameAddr, rNameLen (4-arg form).
            if (ins.operands.size() < 3 || ins.dst == IR_NO_VALUE) break;
            std::string r_handle = ctx.load_src(ins.operands[0], 0);
            std::string r_name   = ctx.load_src(ins.operands[1], 1);
            std::string r_len    = ctx.load_src(ins.operands[2], 2);
            std::string r_dst    = ctx.dst_of(ins.dst);
            ctx.out << "    dlsym " << r_dst << ", "
                    << r_handle << ", " << r_name << ", " << r_len << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::REFLECT_COUNT: {
            // raw_asm-elim wave 2: methodcount/fieldcount.
            // imm=0 -> methodcount, imm=1 -> fieldcount.
            // Emite "<op> r_cls\nmov r_dst, r0\n" (resultado en R0).
            if (ins.operands.empty() || ins.dst == IR_NO_VALUE) break;
            const char *mnem = (ins.imm == 0) ? "methodcount" : "fieldcount";
            std::string r_cls = ctx.load_src(ins.operands[0], 0);
            std::string r_dst = ctx.dst_of(ins.dst);
            ctx.out << "    " << mnem << " " << r_cls << "\n";
            // Si r_dst ya es r0 (regalloc lucky), evita mov r0, r0.
            if (r_dst != "r0") ctx.out << "    mov " << r_dst << ", r0\n";
            ctx.store_spilled(ins.dst);
            break;
        }
        case IrOp::REFLECT_AT: {
            // raw_asm-elim wave 2: getmethat/getfldat.
            // imm=0 -> getmethat, imm=1 -> getfldat.
            if (ins.operands.size() < 2 || ins.dst == IR_NO_VALUE) break;
            const char *mnem = (ins.imm == 0) ? "getmethat" : "getfldat";
            std::string r_cls = ctx.load_src(ins.operands[0], 0);
            std::string r_idx = ctx.load_src(ins.operands[1], 1);
            std::string r_dst = ctx.dst_of(ins.dst);
            ctx.out << "    " << mnem << " " << r_cls << ", " << r_idx << "\n";
            if (r_dst != "r0") ctx.out << "    mov " << r_dst << ", r0\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::SMARTPTR_FREE: {
            // raw_asm-elim wave 2: cleanup deterministico de smart pointer
            // con 3 variantes segun ins.imm:
            //   0 = SRET_DISPATCH  -> operands=[ptr, del_addr], func_name=""
            //   1 = EXTERN_CALLN   -> operands=[ptr], func_name="<lib>:<fn>"
            //   2 = VESTA_CALLVM   -> operands=[ptr], func_name="<fn_label>"
            //
            // El emisor expande a la secuencia equivalente con labels
            // unicas via contador estatico thread-local.  Reusa el espacio
            // de labels global @c __sp_skip_<N> que el viejo RAW_ASM usaba.
            if (ins.operands.empty()) break;
            // BugFix unique 107/110 (2026-06-05): el cleanup invoca el deleter
            // via callvm/calln/callvmr -- una CALL que clobbea caller-saved
            // regs.  Sin salvar los regs vivos A TRAVES de esta call, los
            // valores que el caller usa DESPUES (p.ej. los params de
            // findclass+getstatic del `return T.field`) se corrompen y el
            // findclass devuelve null -> return basura.  Salvamos/restauramos
            // como cualquier otra call (GC_ALLOC, CALL, etc.).  El JIT (vregs)
            // ya lo manejaba; esto cierra el gap del path de slots.
            const uint32_t sp_call_pos = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> sp_save =
                live_regs_through_call(ctx, sp_call_pos, IR_NO_VALUE);
            emit_save_live_regs(ctx, sp_call_pos, sp_save);
            static thread_local uint64_t sp_label_seq = 0;
            const uint64_t lbl = ++sp_label_seq;
            std::string r_ptr = ctx.reg_of(ins.operands[0]);
            if (ins.imm == 0 && ins.operands.size() >= 2) {
                /* SRET_DISPATCH: deleter es runtime via slot+8 */
                const std::string done_lbl = "__sp_done_" + std::to_string(lbl);
                const std::string default_lbl = "__sp_default_" + std::to_string(lbl);
                std::string r_del = ctx.reg_of(ins.operands[1]);
                ctx.out << "    cmpu " << r_ptr << ", 0\n";
                ctx.out << "    jmp.je " << done_lbl << "\n";
                ctx.out << "    cmpu " << r_del << ", 0\n";
                ctx.out << "    jmp.je " << default_lbl << "\n";
                ctx.out << "    mov r14, " << r_del << "\n";   // staging deleter
                if (r_ptr != "r1") ctx.out << "    mov r1, " << r_ptr << "\n";
                ctx.out << "    mov r15, 1\n";
                ctx.out << "    callvmr r14\n";
                ctx.out << "    jmp.jmp " << done_lbl << "\n";
                ctx.out << default_lbl << ":\n";
                if (r_ptr != "r1") ctx.out << "    mov r1, " << r_ptr << "\n";
                ctx.out << "    free r1\n";
                ctx.out << done_lbl << ":\n";
            } else if (ins.imm == 1 || ins.imm == 2) {
                /* EXTERN_CALLN (1) o VESTA_CALLVM (2). */
                const std::string skip_lbl = "__sp_skip_" + std::to_string(lbl);
                ctx.out << "    cmpu " << r_ptr << ", 0\n";
                ctx.out << "    jmp.je " << skip_lbl << "\n";
                if (r_ptr != "r1") ctx.out << "    mov r1, " << r_ptr << "\n";
                ctx.out << "    mov r15, 1\n";
                if (ins.imm == 1) {
                    ctx.out << "    calln @Method(\"" << ins.func_name << "\")\n";
                } else {
                    ctx.out << "    callvm @Absolute(\"code." << ins.func_name << "\")\n";
                }
                ctx.out << skip_lbl << ":\n";
            }
            emit_restore_live_regs(ctx, sp_call_pos, sp_save);
            break;
        }

        // --- string extra ---
        case IrOp::STRGETBYTES:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    strgetbytes " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;

        // --- Meta-OOP / reflexion / Phase Z ---
        case IrOp::GC_HANDLE_FOR_PTR:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    gchandle " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::GC_PROMOTE:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    gcpromote " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::GC_DEMOTE:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    gcdemote " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::FINDCLASS:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    findclass " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::DEFCLASS:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    defclass " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::DEFFIELD:
            if (ins.operands.size() >= 2)
                ctx.out << "    deffield " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        case IrOp::DEFMETHOD:
            if (ins.operands.size() >= 2)
                ctx.out << "    defmethod " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        case IrOp::ADDADVICE:
            if (ins.operands.size() >= 2) {
                ctx.out << "    addadvice " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1])
                        << ", " << ins.imm << "\n";
            }
            break;
        case IrOp::FINDMETHOD:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    findmethod " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::FINDFIELD:
            if (ins.dst != IR_NO_VALUE && !ins.operands.empty()) {
                ctx.out << "    findfield " << ctx.dst_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        case IrOp::CALLSUPER: {
            // Signature: operands[0] = v_cls (ClassInfo* host_ptr del super),
            // operands[1] = v_this, operands[2..N+1] = args.  @c imm =
            // vtable_index del metodo en la vtable del super.  La sintaxis
            // .vel del assembler es:  @c callsuper r_cls, vtable_idx
            // con @c this en r1, args en r2..r[1+nargs] y argc+1 en r15.
            if (ins.operands.size() < 2) break;
            const uint32_t   call_pos     = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save = live_regs_through_call(ctx, call_pos, ins.dst);

            emit_save_all_gc_aware(ctx, call_pos, regs_to_save);

            // Args marshalling: r1 = this, r2..rN = args.
            const size_t nargs_user = ins.operands.size() > 2 ? ins.operands.size() - 2 : 0;
            std::vector<std::pair<int, std::string>> moves;
            moves.emplace_back(1, ctx.load_src(ins.operands[1], 0));
            for (size_t ai = 0; ai < nargs_user && ai < 11; ++ai)
                moves.emplace_back(static_cast<int>(ai + 2),
                                   ctx.load_src(ins.operands[ai + 2], 0));
            emit_parallel_arg_moves(ctx, std::move(moves));
            // r15 = argc (incluyendo @c this).
            ctx.out << "    mov r15, " << (nargs_user + 1) << "\n";

            // El ClassInfo* puede haber sido clobbered por los moves.
            // Re-materializarlo justo antes del callsuper.
            std::string r_cls = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    callsuper " << r_cls << ", " << ins.imm << "\n";

            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            emit_restore_all_gc_aware(ctx, call_pos, regs_to_save);
            break;
        }
        case IrOp::PROCEED:
            ctx.out << "    proceed\n";
            if (ins.dst != IR_NO_VALUE) {
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
                ctx.store_spilled(ins.dst);
            }
            break;

        case IrOp::RAW_ASM: {
            // Emitir cada linea del texto incrustado con indentacion estandar.
            // Substituimos los tokens:
            //   {dst}        -> nombre del registro asignado al destino SSA
            //   {src0}..{srcN-1} -> reg de cada operando (post-regalloc)
            // Permite que un bloque RAW_ASM produzca/consuma valores SSA sin
            // crear un IR op dedicado.  Los srcN se materializan via
            // load_src(scratch_idx=0) si estan spilled (puede usar SCRATCH).
            //
            // is_call_site: si la flag esta activa, el bloque RAW_ASM es
            // logicamente una llamada que clobreara los regs caller-saved
            // (e.g. `loadmod` ejecuta el main del plugin como sub-call).
            // En ese caso envolvemos con save_live_regs / restore_live_regs
            // para preservar los locales del caller a traves del call.
            const uint32_t call_pos_raw = lin_pos_of(ctx, bb.id, idx);
            std::vector<int> regs_to_save_raw;
            if (ins.is_call_site) {
                regs_to_save_raw = live_regs_through_call(ctx, call_pos_raw, ins.dst);
                emit_save_all_gc_aware(ctx, call_pos_raw, regs_to_save_raw);
            }

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
            if (ins.is_call_site) {
                emit_restore_all_gc_aware(ctx, call_pos_raw, regs_to_save_raw);
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

    // Prologo (omitido solo cuando spill_count == 0 Y no hay ALLOCA en el cuerpo).
    //
    // Bug critico arreglado (2026-05-10): antes emitiamos `enter spill_count`
    // sin multiplicar por 8.  El runtime trata el inmediato de enter como
    // raw bytes para `sub rsp, frame_size`.  Asi `enter 6` allocaba SOLO
    // 6 bytes para el frame -- insuficiente para 6 slots de 8 bytes c/u.
    // El emisor entonces accedia a los slots usando offsets POSITIVOS desde
    // rbp (rbp+0, rbp+8, ...), que caen en el AREA DEL CALLER (sobreescriben
    // saved_rbp y RET_ADDR del callvirt).  El editor TUI exhibia este bug
    // con render_buffer (spill_count=6) corrompiendo el `this` del caller.
    //
    // El fix: enter aloca `spill_count * 8` bytes para que los slots vivan
    // dentro del frame local en offsets NEGATIVOS (rbp-8, rbp-16, ...).
    // Combinado con el cambio de offset en load_src/store_spilled, los
    // spills viven seguros en el area allocada por enter, sin interferir
    // con el caller.
    if (has_frame) {
        out << "    enter " << (alloc.spill_count * 8) << "\n";
    }

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

    // Bug fix CRITICO: si un parametro fue evictado por el regalloc
    // (esta en spill_map y NO en reg_map), llega al entry en r1..r_N
    // segun la calling convention pero el slot esta vacio.  Cualquier
    // load posterior desde el slot lee garbage -> segfault al primer
    // uso del param tras un CALL.  Esto afecta especialmente metodos
    // grandes con muchos locales (Editor.render_buffer, etc.) donde el
    // regalloc decide spillar `this` por presion de registros.
    for (size_t i = 0; i < fn.params.size() && i < 12; ++i) {
        IrValueId pid = fn.params[i];
        if (alloc.reg_map.count(pid)) continue;
        auto it_sp = alloc.spill_map.find(pid);
        if (it_sp == alloc.spill_map.end()) continue;
        const int  preg = static_cast<int>(i + 1);
        const bool is_gc = static_cast<size_t>(pid) < fn.values.size()
                        && fn.values[pid].is_gc_object;
        if (is_gc) {
            out << "    gchandle r14, " << reg_name(preg) << "\n";
            out << "    mov r13, rbp\n";
            out << "    subu r13, " << ((it_sp->second + 1) * 8) << "\n";
            out << "    mov [r13], r14\n";
        } else {
            out << "    mov r13, rbp\n";
            out << "    subu r13, " << ((it_sp->second + 1) * 8) << "\n";
            out << "    mov [r13], " << reg_name(preg) << "\n";
        }
    }

    // Emision de bloques
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const IrBlock &bb = fn.blocks[b];

        // Etiqueta del bloque (el bloque 0 = entry no necesita etiqueta extra
        // porque la etiqueta de la funcion ya apunta ahi, pero la emitimos igualmente
        // para que los saltos desde otros bloques puedan apuntar al entry).
        out << ctx.block_label(static_cast<IrBlockId>(b)) << ":\n";
        // Invalidar caches de scratch al cruzar un boundary de bloque:
        // el control flow puede llegar aqui desde cualquier predecesor,
        // asi que no podemos asumir nada sobre el contenido de r14/r13.
        invalidate_scratch_caches(ctx);

        // skip_count > 0 indica que las proximas N instrucciones ya
        // fueron consumidas por un peephole (cmpjmp fusion = 1, decjnz
        // fusion = 2).  Decrementamos en cada iteracion mientras > 0.
        int skip_count = 0;
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            if (skip_count > 0) { --skip_count; continue; }
            emit_instr(ctx, bb, i, skip_count);
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

    // ===================================================================
    // Math-IR-promote: pre-pase que convierte IR ops sin bytecode opcode
    // a CALLN equivalente.  Estos ops (FMIN/FMAX/FFLOOR/FCEIL/FROUND/
    // FTRUNC/IABS/IMIN/IMAX/IMINU/IMAXU/ILOG2/CLZ/CTZ/POPCNT/BYTESWAP/
    // ROTL/ROTR) existen como IR ops para que:
    //   (a) El optimizer pueda constant-fold cuando los operandos son
    //       literales.
    //   (b) El Selector JIT pueda emitir instrucciones nativas
    //       (sqrtsd/roundsd/popcnt/lzcnt/etc.) sin mini-parser.
    //
    // El VM bytecode no tiene opcodes nativos para estas ops todavia, por
    // lo que el emitter las convierte a CALLN al runtime vmath.  El path
    // bytecode pasa por libreria; el path JIT emite hardware directo.
    // Cuando el runtime gane opcodes nativos (futuro sprint), este
    // pre-pase ignora los ops correspondientes.
    {
        struct VmathMap { IrOp op; const char *fn; ir::IrType ret_ir; };
        static const VmathMap vmath_table[] = {
            // Sprint string-perf-5 (2026-06-02): FMIN/FMAX/FFLOOR/FCEIL/
            // FROUND/FTRUNC ahora tienen opcodes bytecode nativos
            // (0x80-0x85) y se emiten directamente como mnemonicos en el
            // switch.  Removidos del pre-pase para evitar CALLN overhead
            // (~150ns -> ~5ns por op en interp).
            //
            // { IrOp::FFLOOR,   "vmath_floor",    ir::IrType::F64 },
            // { IrOp::FCEIL,    "vmath_ceil",     ir::IrType::F64 },
            // { IrOp::FROUND,   "vmath_round",    ir::IrType::F64 },
            // { IrOp::FTRUNC,   "vmath_trunc",    ir::IrType::F64 },
            // { IrOp::FMIN,     "vmath_fmin",     ir::IrType::F64 },
            // { IrOp::FMAX,     "vmath_fmax",     ir::IrType::F64 },
            // Int (signed/unsigned).
            { IrOp::IABS,     "vmath_abs",      ir::IrType::I64 },
            { IrOp::IMIN,     "vmath_min",      ir::IrType::I64 },
            { IrOp::IMAX,     "vmath_max",      ir::IrType::I64 },
            { IrOp::IMINU,    "vmath_minu",     ir::IrType::I64 },
            { IrOp::IMAXU,    "vmath_maxu",     ir::IrType::I64 },
            { IrOp::ILOG2,    "vmath_ilog2",    ir::IrType::I64 },
            // Bit ops.
            { IrOp::CLZ,      "vmath_clz",      ir::IrType::I64 },
            { IrOp::CTZ,      "vmath_ctz",      ir::IrType::I64 },
            { IrOp::POPCNT,   "vmath_popcount", ir::IrType::I64 },
            { IrOp::BYTESWAP, "vmath_bswap",    ir::IrType::I64 },
            { IrOp::ROTL,     "vmath_rotl",     ir::IrType::I64 },
            { IrOp::ROTR,     "vmath_rotr",     ir::IrType::I64 },
        };
        const std::string lib_math = "stdlib/native/math/vesta_math";
        bool any_used = false;
        for (auto &fn : mod.functions) {
            for (auto &bb : fn.blocks) {
                for (auto &ins : bb.instrs) {
                    for (const auto &m : vmath_table) {
                        if (ins.op != m.op) continue;
                        ins.op        = IrOp::CALLN;
                        ins.func_name = lib_math + ":" + m.fn;
                        // Mantener type del dst original.  El emitter de
                        // CALLN ya sabe pasar args via R1..RN.
                        any_used = true;
                        break;
                    }
                }
            }
        }
        if (any_used) {
            // Registrar los imports usados.  Re-iteramos solo los que
            // aparecieron (any_used=true).  Es O(N*M) pero solo corre 1
            // vez por modulo + M es chico (~18 entries).
            for (const auto &m : vmath_table) {
                bool used_here = false;
                for (const auto &fn : mod.functions) {
                    for (const auto &bb : fn.blocks) {
                        for (const auto &ins : bb.instrs) {
                            if (ins.op == IrOp::CALLN
                             && ins.func_name == lib_math + ":" + m.fn) {
                                used_here = true; break;
                            }
                        }
                        if (used_here) break;
                    }
                    if (used_here) break;
                }
                if (used_here) {
                    mod.register_native_import(lib_math, m.fn);
                }
            }
        }
    }

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
            auto [bp, bn] = mod.static_data.bytes_at(i);
            // M.staticdata-pool: respetar @c meta_at(i).alignment (default 1).
            // Si es mayor que el align 16 default del bloque, emitir un
            // @c align directiva especifica antes del label.  Asi
            // @c @align(32) en comptime const arrays produce alineamiento >16.
            const uint16_t a = mod.static_data.meta_at(i).alignment;
            if (a > 16) {
                out << "align " << a << "\n";
            }
            // El parser .vel espera el patron "etiqueta directiva valores"
            // EN LA MISMA LINEA (estilo NASM).  Si separamos la etiqueta
            // en su propia linea el assembler la trata como label vacio
            // y los bytes db nunca se incrustan en el binario.
            bool printable = (bn != 0);
            for (size_t k = 0; k < bn; ++k) {
                uint8_t b = bp[k];
                if (b < 0x20 || b > 0x7E || b == '"' || b == '\\') {
                    printable = false;
                    break;
                }
            }
            if (printable) {
                out << "    s_" << i << " db \"";
                for (size_t k = 0; k < bn; ++k) out << static_cast<char>(bp[k]);
                out << "\"\n";
            } else {
                out << "    s_" << i << " db ";
                for (size_t b = 0; b < bn; ++b) {
                    if (b > 0) out << ", ";
                    out << "0x" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<unsigned>(bp[b])
                        << std::dec << std::setfill(' ');
                }
                if (bn != 0) out << ", 0x00";
                else         out << "0x00";
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
