/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "ir/ir_optimizer.h"
#include "ir/ir_optimizer_internal.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <functional>
#include <sstream>
#include <algorithm>

namespace ir {

using ir::opt_internal::is_side_effecting;
using ir::opt_internal::is_terminator;
using ir::opt_internal::is_pure;
using ir::opt_internal::is_licm_hoistable_alloc;
using ir::opt_internal::strmake_reads_immutable;
using ir::opt_internal::is_pure_allocator_name;

namespace {

/** @brief Un sitio `call @__new_X(...)` candidato a scalar replacement. */
struct GcAllocSite {
    size_t      block_idx = 0;   ///< indice del bloque del CALL
    size_t      ins_idx   = 0;   ///< indice de la instr dentro del bloque
    IrValueId   dst       = IR_NO_VALUE; ///< SSA value del objeto (host_ptr)
    std::string class_name;      ///< "Foo" extraido de "__new_Foo"
    bool        escapes   = true;///< veredicto del analisis (conservador: true)
};

/**
 * @brief Devuelve true si @p name tiene la forma "__new_<ClassName>".
 *        Si lo es, escribe el ClassName en @p out_class.
 */
bool is_new_helper_name(const std::string &name, std::string *out_class) {
    if (name.size() <= 6) return false;
    if (name.compare(0, 6, "__new_") != 0) return false;
    /* Excluir variantes shared (`__new_X_shared`) que registran el objeto en
     * la SharedHandleTable -- eliminar ese alloc cambia shared_heap_live_count
     * (efecto observable), igual que en is_pure_allocator_name. */
    if (name.size() >= 7
     && name.compare(name.size() - 7, 7, "_shared") == 0) return false;
    if (out_class) *out_class = name.substr(6);
    return true;
}

/**
 * @brief Analisis de escape para los objetos `new X()` de una funcion.
 *
 * Para cada `call @__new_X` con dst valido, calcula si el host_ptr del objeto
 * escapa.  Algoritmo identico al de @c ir_pass_promote_local_allocas pero
 * seedeado en los dsts de los CALL `__new_*` en lugar de los dsts de ALLOCA:
 *
 *   1. Forward-flow del conjunto "derivado" desde cada candidato a traves de
 *      ADD/SUB/BITCAST/MOV/CAST/*EXT/TRUNC/PHI/GEP.  Un dst derivado de >1
 *      candidato distinto se marca @c ambiguous (escapan todos).
 *   2. Clasificacion de usos:
 *        - SEED (el propio `call __new_X`): no escapa su dst.  Sus OPERANDS
 *          (los args del ctor) SI pueden escapar otros candidatos (p.ej.
 *          `new Outer(inner)` -> inner escapa).
 *        - LOAD/GETFIELD addr=derivado: SAFE (lee campo; el valor leido NO
 *          es derivado).
 *        - STORE/SETFIELD addr=derivado, val=NO-derivado: SAFE (escribe campo).
 *          val=derivado -> ESCAPA (el ptr se guarda en memoria).
 *        - ADD/SUB/CAST/.../PHI sobre derivados: tracked, no escapa.
 *        - CMP/BR: read-only, no escapa.
 *        - Cualquier OTRA op con un operand derivado: ESCAPA (conservador).
 *
 * @return vector de @c GcAllocSite con el campo @c escapes resuelto.
 */
std::vector<GcAllocSite> analyze_gc_escape(const IrFunction &fn) {
    std::vector<GcAllocSite> sites;
    if (fn.is_native || fn.values.empty()) return sites;

    /* Step 1: recolectar candidatos `call __new_X`. */
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &blk = fn.blocks[bi];
        for (size_t ii = 0; ii < blk.instrs.size(); ++ii) {
            const auto &ins = blk.instrs[ii];
            if (ins.op != IrOp::CALL) continue;
            if (ins.dst == IR_NO_VALUE || ins.dst >= fn.values.size()) continue;
            std::string cls;
            if (!is_new_helper_name(ins.func_name, &cls)) continue;
            GcAllocSite s;
            s.block_idx  = bi;
            s.ins_idx    = ii;
            s.dst        = ins.dst;
            s.class_name = std::move(cls);
            s.escapes    = true;  /* default conservador */
            sites.push_back(std::move(s));
        }
    }
    if (sites.empty()) return sites;

    /* Cota dura de candidatos por funcion (indice cabe en int8 del map). */
    if (sites.size() > 127) sites.resize(127);

    /* Step 2: forward-flow del set derivado.  derived_from[v] = idx del
     * candidato del que v deriva (-1 = ninguno); ambiguous[v] si >1. */
    std::vector<int8_t> derived_from(fn.values.size(), -1);
    std::vector<bool>   ambiguous(fn.values.size(), false);

    auto set_derived = [&](IrValueId v, int8_t origin) -> bool {
        if (v == IR_NO_VALUE || v >= fn.values.size()) return false;
        if (derived_from[v] == -1) { derived_from[v] = origin; return true; }
        if (derived_from[v] != origin) ambiguous[v] = true;
        return false;
    };
    for (size_t i = 0; i < sites.size(); ++i) {
        set_derived(sites[i].dst, static_cast<int8_t>(i & 0x7F));
    }

    bool changed = true;
    int  it = 16;
    while (changed && it-- > 0) {
        changed = false;
        for (const auto &blk : fn.blocks) {
            for (const auto &ins : blk.instrs) {
                if (ins.dst == IR_NO_VALUE || ins.dst >= fn.values.size()) continue;
                if (derived_from[ins.dst] >= 0) continue;  /* ya marcado */
                auto from_op = [&](IrValueId v) -> int {
                    if (v == IR_NO_VALUE || v >= fn.values.size()) return -1;
                    return derived_from[v];
                };
                switch (ins.op) {
                    case IrOp::ADD: case IrOp::SUB:
                    case IrOp::BITCAST: case IrOp::MOV:
                    case IrOp::CAST: case IrOp::SEXT:
                    case IrOp::ZEXT: case IrOp::TRUNC:
                    case IrOp::GEP:
                        for (auto opv : ins.operands) {
                            int from = from_op(opv);
                            if (from >= 0) {
                                if (set_derived(ins.dst, static_cast<int8_t>(from)))
                                    changed = true;
                                break;
                            }
                        }
                        break;
                    case IrOp::PHI:
                        for (const auto &pa : ins.phi_args) {
                            int from = from_op(pa.value);
                            if (from >= 0) {
                                if (set_derived(ins.dst, static_cast<int8_t>(from)))
                                    changed = true;
                                break;
                            }
                        }
                        break;
                    default: break;
                }
            }
        }
    }

    /* Step 3: clasificar usos.  escapes[i]=1 si algun uso de un derivado del
     * candidato i es UNSAFE. */
    std::vector<uint8_t> escapes(sites.size(), 0u);

    auto mark_escape = [&](IrValueId v) {
        if (v == IR_NO_VALUE || v >= derived_from.size()) return;
        if (derived_from[v] < 0) return;
        if (ambiguous[v]) {
            for (size_t k = 0; k < escapes.size(); ++k) escapes[k] = 1u;
            return;
        }
        int idx = derived_from[v];
        if (idx >= 0 && static_cast<size_t>(idx) < escapes.size()) escapes[idx] = 1u;
    };
    auto is_derived = [&](IrValueId v) -> bool {
        return v != IR_NO_VALUE && v < derived_from.size() && derived_from[v] >= 0;
    };

    /* Whitelist de SAFE ops (identica a promote_local_allocas).  Cualquier op
     * fuera de esta lista (RAW_ASM, CALL*, GC_*, FINDCLASS, RET, THROW, ...)
     * con un operand derivado marca escape. */
    auto is_safe_op = [](IrOp op) -> bool {
        switch (op) {
            case IrOp::ADD: case IrOp::SUB: case IrOp::MUL:
            case IrOp::DIV: case IrOp::MOD: case IrOp::NEG:
            case IrOp::AND: case IrOp::OR:  case IrOp::XOR: case IrOp::NOT:
            case IrOp::SHL: case IrOp::SHR: case IrOp::SAR:
            case IrOp::BITCAST: case IrOp::MOV:
            case IrOp::CAST: case IrOp::SEXT:
            case IrOp::ZEXT: case IrOp::TRUNC:
            case IrOp::GEP:
            case IrOp::CMP_EQ: case IrOp::CMP_NE:
            case IrOp::CMP_LT: case IrOp::CMP_GT:
            case IrOp::CMP_LE: case IrOp::CMP_GE:
            case IrOp::CMP_ULT: case IrOp::CMP_UGT:
            case IrOp::CMP_ULE: case IrOp::CMP_UGE:
            case IrOp::BR: case IrOp::BR_COND:
            case IrOp::NOP: case IrOp::CONST:
                return true;
            default:
                return false;
        }
    };

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &blk = fn.blocks[bi];
        for (size_t ii = 0; ii < blk.instrs.size(); ++ii) {
            const auto &ins = blk.instrs[ii];

            /* El propio CALL seed NO escapa su dst (es el alloc); pero sus
             * operands (args del ctor) PUEDEN escapar OTROS candidatos. */
            if (ins.op == IrOp::CALL) {
                std::string cls;
                if (ins.dst != IR_NO_VALUE && is_new_helper_name(ins.func_name, &cls)) {
                    for (auto opv : ins.operands) mark_escape(opv);
                    continue;
                }
            }

            if (ins.op == IrOp::LOAD || ins.op == IrOp::GETFIELD) {
                /* addr=operands[0] derivado -> lee campo, SAFE.  El dst (valor
                 * leido) NO se considera derivado del objeto. */
                continue;
            }
            if (ins.op == IrOp::STORE) {
                /* STORE val=operands[0], addr=operands[1].
                 * val derivado -> el ptr se escribe en memoria -> ESCAPA. */
                if (ins.operands.size() >= 2 && is_derived(ins.operands[0])) {
                    mark_escape(ins.operands[0]);
                }
                continue;
            }
            if (ins.op == IrOp::SETFIELD) {
                /* SETFIELD obj=operands[0], val=operands[1].
                 * val derivado -> ESCAPA. obj derivado -> SAFE (escribe campo).*/
                if (ins.operands.size() >= 2 && is_derived(ins.operands[1])) {
                    mark_escape(ins.operands[1]);
                }
                continue;
            }
            if (ins.op == IrOp::PHI) {
                /* Si el dst NO es derivado pero algun phi_arg SI -> escapa. */
                if (ins.dst < derived_from.size() && derived_from[ins.dst] < 0) {
                    for (const auto &pa : ins.phi_args) mark_escape(pa.value);
                }
                continue;
            }
            if (is_safe_op(ins.op)) continue;

            /* UNSAFE op: cualquier operand derivado escapa. */
            for (auto opv : ins.operands) mark_escape(opv);
            for (const auto &pa : ins.phi_args) mark_escape(pa.value);
            if (ins.func_ptr != IR_NO_VALUE) mark_escape(ins.func_ptr);
        }
    }

    for (size_t i = 0; i < sites.size(); ++i) sites[i].escapes = (escapes[i] != 0u);
    return sites;
}

/** @brief true si la env var @p name esta activa (presente y != "0"/""). */
bool env_flag_on(const char *name) {
    const char *v = std::getenv(name);
    return v && v[0] != '\0' && v[0] != '0';
}

}  // namespace

/**
 * @brief Pase de DETECCION de objetos GC no-escapantes (log-only).
 *
 * Ejecuta @c analyze_gc_escape y, si VESTA_ESCAPE_DEBUG esta activo, loguea
 * cada sitio `new X()` con su veredicto (ESCAPA / NO-ESCAPA).  NO transforma
 * el IR: siempre devuelve false.  Sirve para validar el analisis con cero
 * riesgo antes de habilitar la transformacion (scalar replacement).
 */
bool ir_pass_escape_detect_gc(IrFunction &fn) {
    if (!env_flag_on("VESTA_ESCAPE_DEBUG")) return false;
    auto sites = analyze_gc_escape(fn);
    if (sites.empty()) return false;
    for (const auto &s : sites) {
        std::fprintf(stderr,
            "[escape] fn '%s': new %s() (dst %%%u) -> %s\n",
            fn.name.c_str(), s.class_name.c_str(),
            static_cast<unsigned>(s.dst),
            s.escapes ? "ESCAPA" : "NO-ESCAPA (candidato scalar-replace)");
    }
    return false;
}

//==============================================================================
//  Phase C2.13: Scalar Replacement (transformacion)
//
//  Para un `%obj = call @__new_X(args)` NO-ESCAPANTE cuyo constructor es un
//  "inicializador trivial de campos", elimina el alloc GC y reemplaza cada
//  `load.T (obj + off)` por el valor con el que el ctor inicializo ese campo
//  (un arg del `new` o una constante), convertido al tipo del campo.
//==============================================================================

namespace {

/** @brief Tamano en bytes de un IrType (para elegir trunc/widen). */
int sr_type_size(IrType t) {
    switch (t) {
        case IrType::I8:  case IrType::U8:  case IrType::BOOL: return 1;
        case IrType::I16: case IrType::U16: return 2;
        case IrType::I32: case IrType::U32: case IrType::F32: return 4;
        default: return 8;  /* I64/U64/F64/PTR/HANDLE */
    }
}
/** @brief true si @p t es un entero (no float, no ptr, no handle). */
bool sr_type_is_int(IrType t) {
    switch (t) {
        case IrType::I8:  case IrType::I16: case IrType::I32: case IrType::I64:
        case IrType::U8:  case IrType::U16: case IrType::U32: case IrType::U64:
        case IrType::BOOL:
            return true;
        default:
            return false;
    }
}
/** @brief true si @p t es un entero con signo. */
bool sr_type_is_signed(IrType t) {
    return t == IrType::I8 || t == IrType::I16
        || t == IrType::I32 || t == IrType::I64;
}

/** @brief Inicializacion de un campo por parte del ctor. */
struct SrFieldInit {
    enum Kind { PARAM, CONST } kind = PARAM;
    uint32_t  offset    = 0;          ///< offset del campo desde la base del objeto
    IrType    field_type = IrType::I64; ///< tipo con el que el ctor escribio el campo
    int       new_arg_index = -1;     ///< (PARAM) indice en los args de __new_X
    uint64_t  const_val = 0;          ///< (CONST) valor literal
};

/** @brief Modelo de un ctor "inicializador trivial de campos". */
struct SrCtorModel {
    bool                     valid = false;
    uint32_t                 num_new_args = 0; ///< params del ctor sin contar `this`
    std::vector<SrFieldInit> inits;            ///< una entrada por campo inicializado

    const SrFieldInit *find(uint32_t off) const {
        for (const auto &i : inits) if (i.offset == off) return &i;
        return nullptr;
    }
};

/** @brief Busca una IrFunction por nombre exacto. */
const IrFunction *sr_find_fn(const IrModule &mod, const std::string &name) {
    for (const auto &f : mod.functions) if (f.name == name) return &f;
    return nullptr;
}

/**
 * @brief Construye el modelo del ctor de la clase @p class_name.
 *
 * Solo tiene exito si:
 *   - La clase existe en @p mod.classes, NO tiene destructor ni campos
 *     destructibles, y NO es un aspecto (AOP).
 *   - Tiene EXACTAMENTE un metodo @c is_constructor (sin sobrecargas).
 *   - El cuerpo del ctor es un unico bloque cuyas unicas ops son:
 *     CONST, ADD(this, const_offset) para direcciones de campo, STORE de un
 *     param/const a una de esas direcciones, y RET.  @c this solo puede
 *     usarse como base de esas direcciones.  Cualquier otra op -> invalido.
 */
bool sr_build_ctor_model(const IrModule &mod, const std::string &class_name,
                         SrCtorModel &out, std::string *reason = nullptr) {
    out = SrCtorModel{};
    auto bail = [&](const char *r) -> bool { if (reason) *reason = r; return false; };

    /* 1) Localizar la clase + checks de seguridad. */
    const IrClass *cls = nullptr;
    for (const auto &c : mod.classes) {
        if (c.name == class_name) { cls = &c; break; }
    }
    if (!cls) return bail("clase no encontrada en mod.classes");
    if (cls->has_destructor)         return bail("clase con destructor");
    if (cls->has_destructible_field) return bail("clase con campo destructible");
    if (cls->is_aspect)              return bail("clase es @Aspect");

    /* El modulo entero usa AOP -> los CALLVIRT (incluido el del ctor) pueden
     * disparar advice chains; eliminar el ctor las saltaria. */
    for (const auto &c : mod.classes) {
        if (c.is_aspect) return bail("modulo usa AOP");
    }

    /* 2) Un unico constructor DEFINIDO en esta clase (los heredados tienen
     * defining_class distinto -> no cuentan; las sobrecargas reales si). */
    const IrMethod *ctor_m = nullptr;
    int ctor_count = 0;
    for (const auto &m : cls->methods) {
        if (!m.is_constructor) continue;
        /* Filtrar constructores heredados: solo el de esta clase.  Si
         * defining_class esta vacio (metadata incompleta), usar el match por
         * nombre ir_fn_name == "<clase>__ctor". */
        const bool own = m.defining_class.empty()
            ? (m.ir_fn_name == class_name + "__ctor")
            : (m.defining_class == class_name);
        if (!own) continue;
        ctor_m = &m; ++ctor_count;
    }
    if (ctor_count != 1)               return bail("0 o >1 constructores propios");
    if (!ctor_m || ctor_m->ir_fn_name.empty()) return bail("ctor sin ir_fn_name");

    const IrFunction *ctor = sr_find_fn(mod, ctor_m->ir_fn_name);
    if (!ctor || ctor->is_native) return bail("ctor IrFunction no hallada/native");
    if (ctor->blocks.size() != 1) return bail("ctor con control de flujo (>1 bloque)");
    if (ctor->params.empty())     return bail("ctor sin param this");

    const IrValueId this_vid = ctor->params[0];
    out.num_new_args = static_cast<uint32_t>(ctor->params.size() - 1);

    /* indice de param (en ctor->params) -> ; -1 si no es param. */
    auto param_index_of = [&](IrValueId v) -> int {
        for (size_t i = 0; i < ctor->params.size(); ++i) {
            if (ctor->params[i] == v) return static_cast<int>(i);
        }
        return -1;
    };

    /* Mapa fieldaddr_vid -> offset (direcciones `this + const`). */
    std::unordered_map<IrValueId, uint32_t> field_addr;
    /* CONST values definidos en el ctor (para resolver offsets y store-vals). */
    std::unordered_map<IrValueId, uint64_t> const_vals;

    const auto &blk = ctor->blocks[0];

    /* Pasada 1: recolectar consts. */
    for (const auto &ins : blk.instrs) {
        if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE) {
            const_vals[ins.dst] = ins.imm;
        }
    }

    /* Pasada 2: validar cada instr + recolectar field addrs + stores. */
    for (const auto &ins : blk.instrs) {
        switch (ins.op) {
            case IrOp::CONST:
            case IrOp::NOP:
                break;  /* inocuos */

            case IrOp::RET:
                /* ret.void: no debe retornar `this` ni un derivado. */
                for (auto v : ins.operands) {
                    if (v == this_vid || field_addr.count(v))
                        return bail("ctor retorna this/field-addr");
                }
                break;

            case IrOp::ADD: {
                /* Solo permitido como `add this, const` -> direccion de campo.
                 * Cualquier otro ADD que toque `this` invalida el modelo. */
                if (ins.operands.size() != 2) {
                    /* ADD que no toca this es inocuo; si toca this, invalido. */
                    for (auto v : ins.operands)
                        if (v == this_vid) return bail("ADD raro sobre this");
                    break;
                }
                const IrValueId a = ins.operands[0];
                const IrValueId b = ins.operands[1];
                IrValueId base = IR_NO_VALUE, offv = IR_NO_VALUE;
                if (a == this_vid)      { base = a; offv = b; }
                else if (b == this_vid) { base = b; offv = a; }
                if (base == this_vid) {
                    auto it = const_vals.find(offv);
                    if (it == const_vals.end()) return bail("offset de campo no const");
                    if (ins.dst == IR_NO_VALUE) return bail("field-addr sin dst");
                    field_addr[ins.dst] = static_cast<uint32_t>(it->second);
                } else {
                    /* ADD sin this; pero si algun operando es una field-addr
                     * derivada, no lo soportamos. */
                    if (field_addr.count(a) || field_addr.count(b))
                        return bail("aritmetica sobre field-addr");
                }
                break;
            }

            case IrOp::STORE: {
                /* store val=operands[0], addr=operands[1]. */
                if (ins.operands.size() < 2) return bail("STORE mal formado");
                const IrValueId val  = ins.operands[0];
                const IrValueId addr = ins.operands[1];
                uint32_t off;
                if (addr == this_vid) {
                    off = 0;
                } else {
                    auto it = field_addr.find(addr);
                    if (it == field_addr.end()) return bail("STORE a addr no-campo");
                    off = it->second;
                }
                /* val debe ser un param (>=1) o un const. */
                SrFieldInit fi;
                fi.offset = off;
                int pidx = param_index_of(val);
                if (pidx == 0) {
                    return bail("ctor guarda this en un campo (self-ref)");
                } else if (pidx >= 1) {
                    fi.kind          = SrFieldInit::PARAM;
                    fi.new_arg_index = pidx - 1;
                    if (val >= ctor->values.size()) return bail("param fuera de rango");
                    fi.field_type    = ctor->values[val].type;
                } else {
                    auto cit = const_vals.find(val);
                    if (cit == const_vals.end())
                        return bail("store-val no es param ni const (cast/expr)");
                    fi.kind       = SrFieldInit::CONST;
                    fi.const_val  = cit->second;
                    fi.field_type = (val < ctor->values.size())
                                  ? ctor->values[val].type : IrType::I64;
                }
                /* No permitir dos stores al mismo offset (ambiguo). */
                if (out.find(off)) return bail("dos stores al mismo campo");
                out.inits.push_back(fi);
                break;
            }

            default:
                /* Cualquier otra op (CALL, LOAD, NEWOBJ, GC*, RAW_ASM, MOV,
                 * casts, super-ctor, ...) invalida el modelo trivial. */
                return bail("ctor con op no-trivial (CALL/LOAD/cast/super/...)");
        }
    }

    out.valid = true;
    return true;
}

/**
 * @brief Reescribe (o solo valida) la instr @p ld (un LOAD) para producir el
 *        valor del campo a partir del arg/const con el que el ctor lo inicializo.
 *
 * Con @p apply == false NO muta nada (ni @p ld ni @c fn.values): solo
 * comprueba si la reescritura es posible.  Con @p apply == true aplica la
 * reescritura (asume que la validacion previa devolvio true).  Esto permite
 * un transform transaccional: validar TODOS los loads antes de tocar nada.
 *
 * @return true si la reescritura es posible / se aplico; false -> abortar.
 */
bool sr_rewrite_load(IrInstr &ld, const SrFieldInit &fi,
                     const std::vector<IrValueId> &args, IrFunction &fn,
                     bool apply) {
    const IrType T = ld.type;  /* tipo leido del campo */
    /* Solo enteros por ahora (float/ptr/handle -> abortar, conservador). */
    if (!sr_type_is_int(T)) return false;
    /* El ctor escribio el campo con field_type; debe coincidir con el read. */
    if (fi.field_type != T) return false;

    if (fi.kind == SrFieldInit::CONST) {
        uint64_t v = fi.const_val;
        int sz = sr_type_size(T);
        if (sz < 8) v &= ((uint64_t{1} << (sz * 8)) - 1);
        if (apply) {
            /* Reescribir el LOAD como CONST T value (truncado al ancho de T). */
            ld.op = IrOp::CONST;
            ld.imm = v;
            ld.operands.clear();
            ld.func_name.clear();
            if (ld.dst != IR_NO_VALUE && ld.dst < fn.values.size()) {
                fn.values[ld.dst].is_const    = true;
                fn.values[ld.dst].const_val   = v;
                fn.values[ld.dst].is_host_ptr = false;
            }
        }
        return true;
    }

    /* PARAM: el valor del campo = convert(args[new_arg_index], T). */
    if (fi.new_arg_index < 0
     || static_cast<size_t>(fi.new_arg_index) >= args.size()) return false;
    const IrValueId arg = args[fi.new_arg_index];
    if (arg == IR_NO_VALUE || arg >= fn.values.size()) return false;
    const IrType Ta = fn.values[arg].type;
    if (!sr_type_is_int(Ta)) return false;  /* arg no entero -> abortar */

    const int szA = sr_type_size(Ta);
    const int szT = sr_type_size(T);
    /* arg mas estrecho que el campo: widening (raro al pasar literales).
     * v1 no lo modela con seguridad -> abortar. */
    if (szA < szT) return false;

    if (apply) {
        ld.operands.clear();
        ld.operands.push_back(arg);
        ld.func_name.clear();
        if (ld.dst != IR_NO_VALUE && ld.dst < fn.values.size()) {
            fn.values[ld.dst].is_const    = false;
            fn.values[ld.dst].is_host_ptr = false;
        }
        /* szA > szT -> truncar; szA == szT -> copia directa. */
        ld.op = (szA > szT) ? IrOp::TRUNC : IrOp::MOV;
    }
    return true;
}

//==============================================================================
//  Dominancia: idom + dominance frontier + dom-tree (para SROA/mem2reg).
//==============================================================================

struct SrDom {
    size_t                              N = 0;
    IrBlockId                           UNDEF = 0;
    std::vector<std::vector<IrBlockId>> preds, succs;
    std::vector<IrBlockId>              idom;          ///< inmediato dominador
    std::vector<std::vector<IrBlockId>> df;            ///< dominance frontier
    std::vector<std::vector<IrBlockId>> dom_children;  ///< hijos en el dom-tree
    std::vector<uint8_t>                reachable;     ///< alcanzable desde entry

    bool dominates(IrBlockId T, IrBlockId B) const {
        if (T == B) return true;
        if (T >= N || B >= N || idom[B] == UNDEF) return false;
        IrBlockId cur = B;
        while (idom[cur] != cur) { cur = idom[cur]; if (cur == T) return true; }
        return false;
    }
};

/**
 * @brief Computa CFG + dominadores (Cooper-Harvey-Kennedy) + dominance frontier
 *        (Cytron) + dom-tree para @p fn.  Convencion: bloque 0 = entry.
 */
SrDom sr_compute_dom(const IrFunction &fn) {
    SrDom d;
    const size_t N = fn.blocks.size();
    d.N = N;
    d.UNDEF = static_cast<IrBlockId>(N);
    d.preds.assign(N, {});
    d.succs.assign(N, {});
    d.idom.assign(N, d.UNDEF);
    d.df.assign(N, {});
    d.dom_children.assign(N, {});
    d.reachable.assign(N, 0);
    if (N == 0) return d;

    /* CFG desde los terminadores. */
    for (size_t b = 0; b < N; ++b) {
        const auto &bb = fn.blocks[b];
        if (bb.instrs.empty()) continue;
        const auto &last = bb.instrs.back();
        IrBlockId t1 = IR_NO_BLOCK, t2 = IR_NO_BLOCK;
        if (last.op == IrOp::BR) { t1 = last.target_block; }
        else if (last.op == IrOp::BR_COND) { t1 = last.target_block; t2 = last.false_block; }
        if (t1 != IR_NO_BLOCK && t1 < N) { d.preds[t1].push_back((IrBlockId)b); d.succs[b].push_back(t1); }
        if (t2 != IR_NO_BLOCK && t2 < N) { d.preds[t2].push_back((IrBlockId)b); d.succs[b].push_back(t2); }
    }

    const IrBlockId entry = 0;
    /* Reverse postorder via DFS iterativo (evita stack overflow en CFGs grandes). */
    std::vector<IrBlockId> rpo;
    {
        std::vector<uint8_t> vis(N, 0);
        std::vector<std::pair<IrBlockId,size_t>> st;  /* (bloque, idx_succ) */
        st.push_back({entry, 0});
        vis[entry] = 1; d.reachable[entry] = 1;
        std::vector<IrBlockId> post;
        while (!st.empty()) {
            auto &top = st.back();
            if (top.second < d.succs[top.first].size()) {
                IrBlockId s = d.succs[top.first][top.second++];
                if (s < N && !vis[s]) { vis[s] = 1; d.reachable[s] = 1; st.push_back({s, 0}); }
            } else {
                post.push_back(top.first);
                st.pop_back();
            }
        }
        rpo.assign(post.rbegin(), post.rend());
    }
    std::vector<uint32_t> rpo_pos(N, UINT32_MAX);
    for (size_t i = 0; i < rpo.size(); ++i) rpo_pos[rpo[i]] = (uint32_t)i;

    d.idom[entry] = entry;
    auto intersect = [&](IrBlockId b1, IrBlockId b2) -> IrBlockId {
        while (b1 != b2) {
            while (b1 != d.UNDEF && rpo_pos[b1] > rpo_pos[b2]) b1 = d.idom[b1];
            while (b2 != d.UNDEF && rpo_pos[b2] > rpo_pos[b1]) b2 = d.idom[b2];
            if (b1 == d.UNDEF || b2 == d.UNDEF) return d.UNDEF;
        }
        return b1;
    };
    bool ch = true;
    while (ch) {
        ch = false;
        for (IrBlockId b : rpo) {
            if (b == entry) continue;
            IrBlockId nd = d.UNDEF;
            for (IrBlockId p : d.preds[b]) {
                if (d.idom[p] != d.UNDEF) {
                    nd = (nd == d.UNDEF) ? p : intersect(nd, p);
                    if (nd == d.UNDEF) break;
                }
            }
            if (nd != d.UNDEF && nd != d.idom[b]) { d.idom[b] = nd; ch = true; }
        }
    }

    /* Dom-tree children. */
    for (IrBlockId b = 0; b < N; ++b) {
        if (b != entry && d.idom[b] != d.UNDEF) d.dom_children[d.idom[b]].push_back(b);
    }

    /* Dominance frontier (Cytron): por cada bloque b con >=2 preds, por cada
     * pred p, sube en el dom-tree desde p hasta idom[b] anyadiendo b al DF. */
    for (IrBlockId b = 0; b < N; ++b) {
        if (d.preds[b].size() < 2) continue;
        for (IrBlockId p : d.preds[b]) {
            IrBlockId runner = p;
            while (runner != d.UNDEF && runner != d.idom[b]) {
                d.df[runner].push_back(b);
                if (d.idom[runner] == runner) break;  /* entry */
                runner = d.idom[runner];
            }
        }
    }
    return d;
}

//==============================================================================
//  SROA/mem2reg de los campos de un objeto GC no-escapante.
//
//  Promueve cada campo (offset) del objeto a forma SSA a traves del control de
//  flujo (incluyendo loops): inserta PHIs en el dominance frontier de las
//  definiciones (ctor-init + stores) y renombra (Cytron) reemplazando cada load
//  por la definicion que lo alcanza.  Tras esto el objeto no toca memoria -> el
//  alloc + los stores se borran.
//
//  Precondiciones (el caller las garantiza salvo lo que se revalida aqui):
//    - El objeto NO escapa y TODOS sus usos son field-access (load/store de
//      `obj` o de `add obj, Kconst`), nunca en phi_args/func_ptr/CALL/RET.
//    - El ctor es un inicializador trivial (modelo @p model).
//    - CFG reducible (el frontend Vex lo garantiza).
//
//  Conservador: si cualquier campo accedido no esta en el modelo, no es entero,
//  o los tipos no son consistentes -> bail (no muta nada).
//==============================================================================

bool sr_mem2reg_object(IrFunction &fn,
                       const SrCtorModel &model,
                       size_t call_bi, size_t call_ii, IrValueId obj,
                       const std::vector<IrValueId> &args,
                       const std::unordered_map<IrValueId, uint32_t> &fieldaddr_off,
                       std::string &reason) {
    const size_t N = fn.blocks.size();
    if (N == 0) { reason = "fn vacia"; return false; }

    /* Helper: la instr es un load/store de un campo del objeto?  Devuelve
     * offset + si es store + el valor almacenado. */
    auto classify = [&](const IrInstr &in, uint32_t &off, bool &is_ld,
                        bool &is_st, IrValueId &sval) -> bool {
        is_ld = is_st = false;
        if (in.op == IrOp::LOAD && !in.operands.empty()) {
            IrValueId a = in.operands[0];
            if (a == obj) { off = 0; is_ld = true; return true; }
            auto it = fieldaddr_off.find(a);
            if (it != fieldaddr_off.end()) { off = it->second; is_ld = true; return true; }
        } else if (in.op == IrOp::STORE && in.operands.size() >= 2) {
            IrValueId a = in.operands[1];
            if (a == obj)        { off = 0; is_st = true; sval = in.operands[0]; return true; }
            auto it = fieldaddr_off.find(a);
            if (it != fieldaddr_off.end()) { off = it->second; is_st = true; sval = in.operands[0]; return true; }
        }
        return false;
    };

    /* 1) Recolectar offsets accedidos + tipo por offset + bloques con store. */
    std::unordered_map<uint32_t, IrType> field_type;   /* offset -> tipo */
    std::unordered_map<uint32_t, std::vector<IrBlockId>> store_blocks;
    std::vector<uint32_t> offsets;
    for (size_t bi = 0; bi < N; ++bi) {
        for (const auto &in : fn.blocks[bi].instrs) {
            uint32_t off; bool ld, st; IrValueId sv;
            if (!classify(in, off, ld, st, sv)) continue;
            /* Tipo del campo: para load = in.type; para store = tipo del valor. */
            IrType t = ld ? in.type
                          : (sv < fn.values.size() ? fn.values[sv].type : IrType::I64);
            if (!sr_type_is_int(t)) { reason = "campo no-entero en mem2reg"; return false; }
            auto fit = field_type.find(off);
            if (fit == field_type.end()) { field_type[off] = t; offsets.push_back(off); }
            else if (fit->second != t)   { reason = "tipo inconsistente del campo"; return false; }
            if (st) store_blocks[off].push_back((IrBlockId)bi);
        }
    }
    if (offsets.empty()) { reason = "sin accesos a campos"; return false; }

    /* Todo offset accedido debe estar en el modelo (ctor-init disponible) y su
     * tipo coincidir con el del ctor (default-0 no soportado en mem2reg v1). */
    for (uint32_t off : offsets) {
        const SrFieldInit *fi = model.find(off);
        if (!fi) { reason = "campo no inicializado por el ctor (default-0)"; return false; }
        if (fi->field_type != field_type[off]) { reason = "tipo ctor/acceso difiere"; return false; }
    }

    SrDom dom = sr_compute_dom(fn);
    /* El bloque del alloc debe ser alcanzable (lo es: contiene el call). */
    if (call_bi >= N || !dom.reachable[call_bi]) { reason = "call_bi inalcanzable"; return false; }
    /* Todos los bloques con acceso a campos deben ser alcanzables + dominados
     * por el alloc (garantizado por SSA, pero revalidamos defensivamente). */

    /* 2) Materializar el valor de construccion (init) de cada campo como un
     * SSA value disponible en el sitio del alloc.  Si requiere conversion
     * (trunc) o es const, se insertara una instruccion JUSTO antes del call. */
    std::unordered_map<uint32_t, IrValueId> init_val;     /* offset -> SSA value */
    std::vector<IrInstr> init_instrs;                      /* a insertar antes del call */
    for (uint32_t off : offsets) {
        const SrFieldInit *fi = model.find(off);
        const IrType T = field_type[off];
        if (fi->kind == SrFieldInit::CONST) {
            uint64_t v = fi->const_val;
            int sz = sr_type_size(T);
            if (sz < 8) v &= ((uint64_t{1} << (sz * 8)) - 1);
            IrInstr ci; ci.op = IrOp::CONST; ci.type = T; ci.imm = v;
            IrValue nv; nv.id = (IrValueId)fn.values.size(); nv.type = T;
            nv.is_const = true; nv.const_val = v;
            nv.name = "%m2ri" + std::to_string(nv.id);
            ci.dst = nv.id;
            fn.values.push_back(nv);
            init_val[off] = nv.id;     /* antes del move de ci */
            init_instrs.push_back(std::move(ci));
        } else {
            /* PARAM. */
            if (fi->new_arg_index < 0 || (size_t)fi->new_arg_index >= args.size()) {
                reason = "arg index fuera de rango"; return false;
            }
            IrValueId arg = args[fi->new_arg_index];
            if (arg == IR_NO_VALUE || arg >= fn.values.size()) { reason = "arg invalido"; return false; }
            IrType Ta = fn.values[arg].type;
            if (!sr_type_is_int(Ta)) { reason = "arg no entero"; return false; }
            int szA = sr_type_size(Ta), szT = sr_type_size(T);
            if (szA == szT) {
                init_val[off] = arg;           /* sin conversion */
            } else if (szA > szT) {
                IrInstr ti; ti.op = IrOp::TRUNC; ti.type = T; ti.operands.push_back(arg);
                IrValue nv; nv.id = (IrValueId)fn.values.size(); nv.type = T;
                nv.name = "%m2ri" + std::to_string(nv.id);
                ti.dst = nv.id; fn.values.push_back(nv);
                init_val[off] = nv.id;     /* antes del move de ti */
                init_instrs.push_back(std::move(ti));
            } else {
                reason = "widening arg->campo no soportado"; return false;  /* szA < szT */
            }
        }
    }

    /* 2.5) Deteccion de loops (headers + bloques in-loop) desde back-edges.
     * Necesario para el COST-MODEL: un PHI in-loop que NO esta en un loop
     * header (= escritura condicional de campo dentro de un loop) anyade copies
     * en el path no-tomado por iteracion.  Medido: regresiona el interp (16
     * registros VM -> presion + copies).  Los PHIs de loop-header (acumuladores
     * incondicionales) y los if-merge FUERA de loops (coste unico) SI son win. */
    std::vector<uint8_t> is_loop_header(N, 0), in_loop(N, 0);
    for (IrBlockId b = 0; b < N; ++b) {
        for (IrBlockId h : dom.succs[b]) {
            if (!dom.dominates(h, b)) continue;   /* back-edge b->h */
            is_loop_header[h] = 1; in_loop[h] = 1;
            std::vector<IrBlockId> stk;
            if (!in_loop[b]) { in_loop[b] = 1; stk.push_back(b); }
            while (!stk.empty()) {
                IrBlockId x = stk.back(); stk.pop_back();
                if (x == h) continue;
                for (IrBlockId p : dom.preds[x]) {
                    if (!in_loop[p]) { in_loop[p] = 1; if (p != h) stk.push_back(p); }
                }
            }
        }
    }

    /* 3) Insercion de PHIs: por cada offset, iterated dominance frontier de los
     * def-blocks (= {call_bi} U store_blocks).  Crea SSA values para los phis. */
    /* phi_value[offset][block] = SSA value del phi (IR_NO_VALUE = no hay). */
    std::unordered_map<uint64_t, IrValueId> phi_value;  /* key = (off<<32)|block */
    std::unordered_map<IrValueId, uint32_t> phi_dst_off; /* phi dst -> offset */
    auto pkey = [](uint32_t off, IrBlockId b) -> uint64_t {
        return ((uint64_t)off << 32) | (uint64_t)b;
    };
    /* COST-MODEL (default-on): permitir VESTA_ESCAPE_MEM2REG_FORCE para saltarlo
     * y promover siempre (util para medir / casos JIT-only). */
    const bool force = env_flag_on("VESTA_ESCAPE_MEM2REG_FORCE");
    for (uint32_t off : offsets) {
        std::vector<IrBlockId> worklist;
        std::unordered_set<IrBlockId> on_work, has_phi;
        worklist.push_back((IrBlockId)call_bi); on_work.insert((IrBlockId)call_bi);
        for (IrBlockId b : store_blocks[off]) {
            if (!on_work.count(b)) { worklist.push_back(b); on_work.insert(b); }
        }
        size_t wp = 0;
        while (wp < worklist.size()) {
            IrBlockId b = worklist[wp++];
            if (b >= N) continue;
            for (IrBlockId f : dom.df[b]) {
                if (has_phi.count(f)) continue;
                if (!dom.reachable[f]) continue;
                /* Solo PHIs en bloques DOMINADOS por el alloc: ahi todos los
                 * preds estan dominados por el call -> el objeto existe en cada
                 * pred -> el operando del phi siempre tiene def alcanzante.  Un
                 * merge no-dominado no puede leer el campo (violaria SSA), asi
                 * que su phi seria muerto; lo omitimos. */
                if (!dom.dominates((IrBlockId)call_bi, f)) continue;
                /* COST-MODEL: if-merge DENTRO de un loop = escritura condicional
                 * en el loop -> pessimiza el interp.  Bail (a menos que FORCE). */
                if (!force && in_loop[f] && !is_loop_header[f]) {
                    reason = "escritura condicional de campo en loop (cost-model)";
                    return false;
                }
                has_phi.insert(f);
                /* Crear el SSA value del phi. */
                IrValue nv; nv.id = (IrValueId)fn.values.size(); nv.type = field_type[off];
                nv.name = "%m2rphi" + std::to_string(nv.id);
                fn.values.push_back(nv);
                phi_value[pkey(off, f)] = nv.id;
                phi_dst_off[nv.id] = off;
                if (!on_work.count(f)) { worklist.push_back(f); on_work.insert(f); }
            }
        }
    }

    /* 4) Renaming (Cytron) DFS sobre el dom-tree.  current[off] = def alcanzante.
     * Se construyen: load_repl (load dst -> valor), store_remove (posiciones),
     * y los phi_args de cada phi insertado.  NO se muta el IR todavia. */
    /* Plan de mutacion: */
    std::unordered_map<IrValueId, IrValueId> load_repl;  /* load.dst -> valor reemplazo */
    /* phi_args[phi_dst] = lista de (pred_block, value). */
    std::unordered_map<IrValueId, std::vector<IrPhiArg>> phi_args_plan;

    std::unordered_map<uint32_t, std::vector<IrValueId>> stack;  /* off -> pila de defs */
    auto cur = [&](uint32_t off) -> IrValueId {
        auto it = stack.find(off);
        return (it != stack.end() && !it->second.empty()) ? it->second.back() : IR_NO_VALUE;
    };

    bool rename_ok = true;
    const char *rfail = nullptr;
    std::function<void(IrBlockId, int)> rename = [&](IrBlockId b, int depth) {
        if (!rename_ok) return;
        if (depth > 4096) { rename_ok = false; rfail = "dom-tree demasiado profundo"; return; }
        std::vector<uint32_t> pushed;  /* offsets con un push en este bloque (para pop) */

        /* (a) PHIs planeados para este bloque (aun NO insertados como
         * instrucciones): definen current[off].  Se consultan via phi_value, no
         * escaneando instrucciones (que no existen todavia en esta fase). */
        for (uint32_t off : offsets) {
            auto it = phi_value.find(pkey(off, b));
            if (it == phi_value.end()) continue;
            stack[off].push_back(it->second); pushed.push_back(off);
        }

        /* (b) instrucciones en orden. */
        for (size_t ii = 0; ii < fn.blocks[b].instrs.size(); ++ii) {
            const IrInstr &in = fn.blocks[b].instrs[ii];
            /* El alloc: define todos los campos = init_val. */
            if (b == call_bi && ii == call_ii) {
                for (uint32_t off : offsets) {
                    stack[off].push_back(init_val[off]); pushed.push_back(off);
                }
                continue;
            }
            uint32_t off; bool ld, st; IrValueId sv;
            if (!classify(in, off, ld, st, sv)) continue;
            if (ld) {
                IrValueId rv = cur(off);
                if (rv == IR_NO_VALUE) { rename_ok = false; rfail = "load sin def alcanzante"; return; }
                if (in.dst != IR_NO_VALUE) load_repl[in.dst] = rv;
            } else if (st) {
                stack[off].push_back(sv); pushed.push_back(off);
            }
        }

        /* (c) rellenar operandos de los phis planeados de los sucesores. */
        for (IrBlockId s : dom.succs[b]) {
            for (uint32_t off : offsets) {
                auto it = phi_value.find(pkey(off, s));
                if (it == phi_value.end()) continue;
                IrValueId rv = cur(off);
                if (rv == IR_NO_VALUE) { rename_ok = false; rfail = "phi operand sin def"; return; }
                phi_args_plan[it->second].push_back(IrPhiArg{rv, b});
            }
        }

        /* (d) recursion en hijos del dom-tree. */
        for (IrBlockId c : dom.dom_children[b]) rename(c, depth + 1);

        /* (e) pop. */
        for (auto it = pushed.rbegin(); it != pushed.rend(); ++it) stack[*it].pop_back();
    };
    rename(0, 0);
    if (!rename_ok) { reason = rfail ? rfail : "rename fallo"; return false; }

    /* ====================================================================
     * 5) APLICAR el plan (todas las precondiciones validadas).
     * ==================================================================== */
    /* (a) Reescribir loads -> MOV del valor reemplazo (copy_prop lo limpia). */
    for (auto &bb : fn.blocks) {
        for (auto &in : bb.instrs) {
            if ((in.op == IrOp::LOAD) && in.dst != IR_NO_VALUE) {
                auto it = load_repl.find(in.dst);
                if (it == load_repl.end()) continue;
                /* Confirmar que es un load de un campo del objeto. */
                uint32_t off; bool ld, st; IrValueId sv;
                if (!classify(in, off, ld, st, sv) || !ld) continue;
                in.op = IrOp::MOV; in.operands.clear();
                in.operands.push_back(it->second);
                in.func_name.clear();
                if (in.dst < fn.values.size()) {
                    fn.values[in.dst].is_const = false;
                    fn.values[in.dst].is_host_ptr = false;
                }
            }
        }
    }

    /* (b) Insertar los PHIs al frente de sus bloques con sus operandos. */
    for (const auto &kv : phi_dst_off) {
        IrValueId phidst = kv.first;
        uint32_t  off    = kv.second;
        /* Localizar el bloque (clave inversa: buscar en phi_value). */
        IrBlockId blk = dom.UNDEF;
        for (IrBlockId b = 0; b < N; ++b) {
            auto it = phi_value.find(pkey(off, b));
            if (it != phi_value.end() && it->second == phidst) { blk = b; break; }
        }
        if (blk >= N) continue;
        IrInstr phi; phi.op = IrOp::PHI; phi.type = field_type[off]; phi.dst = phidst;
        auto pit = phi_args_plan.find(phidst);
        if (pit != phi_args_plan.end()) phi.phi_args = pit->second;
        fn.blocks[blk].instrs.insert(fn.blocks[blk].instrs.begin(), std::move(phi));
    }

    /* (c) Eliminar TODOS los stores a campos del objeto (NOP).  Tras mem2reg
     * ningun store es necesario (el valor fluye por SSA).  Se re-localizan por
     * contenido (no por indice) porque (b) inserto phis al frente. */
    for (auto &bb : fn.blocks) {
        for (auto &in : bb.instrs) {
            uint32_t off; bool ld, st; IrValueId sv;
            if (in.op == IrOp::STORE && classify(in, off, ld, st, sv) && st) {
                in.op = IrOp::NOP; in.operands.clear();
                in.dst = IR_NO_VALUE; in.func_name.clear();
            }
        }
    }

    /* (d) Insertar las init_instrs antes del call + NOPear el call + field-addrs.
     * El call sigue identificable por dst==obj. */
    for (auto &bb : fn.blocks) {
        for (size_t ii = 0; ii < bb.instrs.size(); ++ii) {
            IrInstr &in = bb.instrs[ii];
            if (in.op == IrOp::CALL && in.dst == obj
             && is_new_helper_name(in.func_name, nullptr)) {
                /* Insertar init_instrs justo antes. */
                if (!init_instrs.empty()) {
                    bb.instrs.insert(bb.instrs.begin() + ii,
                                     init_instrs.begin(), init_instrs.end());
                    ii += init_instrs.size();
                }
                IrInstr &call = bb.instrs[ii];
                call.op = IrOp::NOP; call.operands.clear();
                call.dst = IR_NO_VALUE; call.func_name.clear();
                goto done_call;
            }
        }
    }
    done_call:;

    /* (e) NOPear las field-addr (add obj, K) -- ahora muertas. */
    for (auto &bb : fn.blocks) {
        for (auto &in : bb.instrs) {
            if (in.op == IrOp::ADD && in.dst != IR_NO_VALUE
             && fieldaddr_off.count(in.dst)) {
                in.op = IrOp::NOP; in.operands.clear();
                in.dst = IR_NO_VALUE; in.func_name.clear();
            }
        }
    }

    /* (f) compactar NOPs sin dst/operandos. */
    for (auto &bb : fn.blocks) {
        auto &is = bb.instrs;
        is.erase(std::remove_if(is.begin(), is.end(), [](const IrInstr &i) {
            return i.op == IrOp::NOP && i.operands.empty() && i.dst == IR_NO_VALUE;
        }), is.end());
    }
    return true;
}

}  // namespace

/**
 * @brief Phase C2.13: Scalar Replacement de objetos GC no-escapantes.
 *
 * Elimina los `new X()` que no escapan y cuyo ctor es un inicializador
 * trivial de campos, reemplazando los field-reads por los valores de
 * construccion.  Resultado: el alloc GC desaparece por completo (tanto en
 * interp como en JIT, porque ambos consumen el mismo IR optimizado).
 *
 * Conservador por diseno: cada sitio se transforma SOLO si todas las
 * precondiciones de seguridad se cumplen; en caso de duda, no se toca.
 *
 * @return true si se transformo algun sitio.
 */
bool ir_pass_scalar_replace_gc(IrFunction &fn, const IrModule &mod) {
    if (fn.is_native || fn.values.empty()) return false;

    auto sites = analyze_gc_escape(fn);
    if (sites.empty()) return false;

    const bool dbg = env_flag_on("VESTA_ESCAPE_DEBUG");
    auto diag = [&](const GcAllocSite &s, const std::string &why) {
        if (dbg) std::fprintf(stderr,
            "[escape] fn '%s': new %s() (dst %%%u) NO transformado: %s\n",
            fn.name.c_str(), s.class_name.c_str(),
            static_cast<unsigned>(s.dst), why.c_str());
    };

    /* Cache de modelos de ctor por clase (validos e invalidos) + razon. */
    struct CachedModel { SrCtorModel m; std::string reason; };
    std::unordered_map<std::string, CachedModel> model_cache;
    auto get_model = [&](const std::string &cls, std::string &out_reason)
            -> const SrCtorModel * {
        auto it = model_cache.find(cls);
        if (it == model_cache.end()) {
            CachedModel cm;
            sr_build_ctor_model(mod, cls, cm.m, &cm.reason);
            it = model_cache.emplace(cls, std::move(cm)).first;
        }
        out_reason = it->second.reason;
        return it->second.m.valid ? &it->second.m : nullptr;
    };

    bool changed = false;

    for (const auto &site : sites) {
        if (site.escapes) continue;
        const IrValueId obj = site.dst;

        std::string mreason;
        const SrCtorModel *model = get_model(site.class_name, mreason);
        if (!model) { diag(site, "ctor: " + mreason); continue; }

        /* La instr del CALL seed (para leer sus args + NOPearla luego). */
        if (site.block_idx >= fn.blocks.size()) continue;
        auto &seed_blk = fn.blocks[site.block_idx];
        if (site.ins_idx >= seed_blk.instrs.size()) continue;
        IrInstr &call_ins = seed_blk.instrs[site.ins_idx];
        if (call_ins.op != IrOp::CALL || call_ins.dst != obj) continue;
        const std::vector<IrValueId> args = call_ins.operands;  /* copia */
        if (args.size() != model->num_new_args) continue;       /* arity mismatch */

        /* --- Recolectar TODOS los usos de obj.  Deben ser solo
         * `add.ptr obj, Kconst` (direccion de campo) o `load obj` (offset 0).
         * Cada field-addr solo puede usarse en LOADs.  Si algo no encaja ->
         * abortar este sitio (no transformar). --- */
        struct LoadRef  { size_t bi; size_t ii; uint32_t off; };
        struct StoreRef { size_t ii; uint32_t off; };
        std::vector<LoadRef>  loads;       /* loads a reescribir */
        std::vector<std::pair<size_t,size_t>> dead_addr;  /* add.ptr a NOPear */
        std::unordered_map<IrValueId, uint32_t> fieldaddr_off;  /* addr_vid -> off */
        bool ok = true;
        bool single_block = true;          /* todos los usos en el bloque del call */
        bool has_writes   = false;         /* algun STORE a un campo del objeto */
        const char *use_reason = "uso no soportado de obj";

        /* Helper: lee el offset const de un `add.ptr obj, K`. */
        auto const_value_of = [&](IrValueId v, uint64_t &out_k) -> bool {
            if (v == IR_NO_VALUE || v >= fn.values.size()) return false;
            if (fn.values[v].is_const) { out_k = fn.values[v].const_val; return true; }
            /* Buscar la CONST que produjo v. */
            for (const auto &b : fn.blocks)
                for (const auto &in : b.instrs)
                    if (in.dst == v && in.op == IrOp::CONST) { out_k = in.imm; return true; }
            return false;
        };

        /* Pasada A: localizar field-addrs derivadas de obj + loads/stores
         * directos (offset 0).  Trackea single_block + has_writes. */
        for (size_t bi = 0; bi < fn.blocks.size() && ok; ++bi) {
            const auto &b = fn.blocks[bi];
            for (size_t ii = 0; ii < b.instrs.size() && ok; ++ii) {
                const auto &in = b.instrs[ii];
                /* obj usado en phi_args o func_ptr -> uso no modelable como
                 * field-access (p.ej. `x = phi(a, b)`).  El escape analysis lo
                 * considera no-escapante pero el transform no sabe materializar
                 * el campo a traves de un merge -> abortar (necesitaria SROA con
                 * PHI de los valores de campo). */
                bool bad_use = false;
                for (const auto &pa : in.phi_args) if (pa.value == obj) { bad_use = true; break; }
                if (in.func_ptr == obj) bad_use = true;
                if (bad_use) {
                    ok = false; use_reason = "obj usado en PHI/func_ptr (necesita SROA con PHI)";
                    break;
                }
                bool uses_obj = false;
                for (auto v : in.operands) if (v == obj) { uses_obj = true; break; }
                if (!uses_obj) continue;

                if (in.op == IrOp::ADD && in.operands.size() == 2
                 && in.dst != IR_NO_VALUE) {
                    /* `add obj, Kconst` o `add Kconst, obj`. */
                    if (in.operands[0] == obj && in.operands[1] == obj) {
                        ok = false; use_reason = "obj en ambos operandos de add"; break;
                    }
                    IrValueId other = (in.operands[0] == obj) ? in.operands[1]
                                                              : in.operands[0];
                    uint64_t k;
                    if (!const_value_of(other, k)) {
                        ok = false; use_reason = "field-addr con offset no-const"; break;
                    }
                    fieldaddr_off[in.dst] = static_cast<uint32_t>(k);
                    dead_addr.push_back({bi, ii});
                    if (bi != site.block_idx) single_block = false;
                } else if (in.op == IrOp::LOAD && !in.operands.empty()
                        && in.operands[0] == obj) {
                    loads.push_back({bi, ii, 0});  /* load directo -> offset 0 */
                    if (bi != site.block_idx) single_block = false;
                } else if (in.op == IrOp::STORE && in.operands.size() >= 2
                        && in.operands[1] == obj && in.operands[0] != obj) {
                    has_writes = true;             /* store directo -> offset 0 */
                    if (bi != site.block_idx) single_block = false;
                } else {
                    ok = false;
                    use_reason = "obj usado fuera de field-access (CMP/callvirt/store-val/GEP/...)";
                    break;
                }
            }
        }
        if (!ok) { diag(site, use_reason); continue; }

        /* Pasada B: cada field-addr solo puede usarse en LOAD o STORE-addr.
         * Recolecta loads + marca has_writes; trackea single_block. */
        for (size_t bi = 0; bi < fn.blocks.size() && ok; ++bi) {
            const auto &b = fn.blocks[bi];
            for (size_t ii = 0; ii < b.instrs.size() && ok; ++ii) {
                const auto &in = b.instrs[ii];
                /* field-addr usada en phi_args/func_ptr -> no soportado. */
                for (const auto &pa : in.phi_args)
                    if (fieldaddr_off.count(pa.value)) { ok = false; break; }
                if (in.func_ptr != IR_NO_VALUE && fieldaddr_off.count(in.func_ptr)) ok = false;
                if (!ok) { use_reason = "field-addr usada en PHI/func_ptr"; break; }
                /* Es field-addr operando de esta instr? */
                bool touches_fa = false; IrValueId fav = IR_NO_VALUE; uint32_t foff = 0;
                for (auto v : in.operands) {
                    auto it = fieldaddr_off.find(v);
                    if (it != fieldaddr_off.end()) { touches_fa = true; fav = v; foff = it->second; break; }
                }
                if (!touches_fa) continue;

                if (in.op == IrOp::LOAD && !in.operands.empty()
                 && in.operands[0] == fav) {
                    loads.push_back({bi, ii, foff});
                    if (bi != site.block_idx) single_block = false;
                } else if (in.op == IrOp::STORE && in.operands.size() >= 2
                        && in.operands[1] == fav && in.operands[0] != fav) {
                    /* field-write: la field-addr es la DIRECCION (operand 1),
                     * no el valor.  Si la field-addr fuera el VALOR -> escape. */
                    has_writes = true;
                    if (bi != site.block_idx) single_block = false;
                } else {
                    ok = false;
                    use_reason = "field-addr usado por op no-LOAD/STORE (o como valor)";
                }
            }
        }
        if (!ok) { diag(site, use_reason); continue; }

        /* ====================================================================
         * Caso A: SIN escrituras -> path read-only (cross-block OK).
         * Reemplaza cada load por el valor de construccion del campo. */
        if (!has_writes) {
            struct PendingRewrite { size_t bi; size_t ii; const SrFieldInit *fi; };
            std::vector<PendingRewrite> pending;
            for (const auto &lr : loads) {
                const SrFieldInit *fi = model->find(lr.off);
                if (!fi) {
                    ok = false;
                    use_reason = "load de campo no inicializado por el ctor (default-0)";
                    break;
                }
                IrInstr &probe = fn.blocks[lr.bi].instrs[lr.ii];
                if (!sr_rewrite_load(probe, *fi, args, fn, /*apply=*/false)) {
                    ok = false;
                    use_reason = "tipo de campo no soportado (float/ptr/handle/widening)";
                    break;
                }
                pending.push_back({lr.bi, lr.ii, fi});
            }
            if (!ok) { diag(site, use_reason); continue; }

            for (const auto &pr : pending) {
                IrInstr &ld = fn.blocks[pr.bi].instrs[pr.ii];
                sr_rewrite_load(ld, *pr.fi, args, fn, /*apply=*/true);
            }
            for (const auto &da : dead_addr) {
                IrInstr &ai = fn.blocks[da.first].instrs[da.second];
                ai.op = IrOp::NOP; ai.operands.clear();
                ai.dst = IR_NO_VALUE; ai.func_name.clear();
            }
            call_ins.op = IrOp::NOP; call_ins.operands.clear();
            call_ins.dst = IR_NO_VALUE; call_ins.func_name.clear();
            changed = true;
            continue;
        }

        /* ====================================================================
         * Caso B: CON escrituras.
         *
         * B.1 single-block -> field versioning LINEAL: un walk lineal hace
         *     store-to-load forwarding, elimina los stores y borra el alloc.
         *
         * B.2 cross-block / loop -> SROA/mem2reg: promueve los campos a SSA con
         *     insercion de PHI (Cytron) + renaming.
         *
         *     Default-on con COST-MODEL: el propio @c sr_mem2reg_object baila si
         *     promover anyadiria un if-merge PHI DENTRO de un loop (= escritura
         *     condicional de campo en el loop), que pessimiza el interp (16
         *     registros VM -> copies + presion).  Los casos que SI promueve son
         *     win (acumuladores incondicionales en loop: +14..41% interp; cross-
         *     block fuera de loops: coste unico + elimina el alloc).
         *     VESTA_NO_ESCAPE_MEM2REG=1 lo desactiva entero;
         *     VESTA_ESCAPE_MEM2REG_FORCE=1 ignora el cost-model. */
        if (!single_block) {
            static const bool mem2reg_off = env_flag_on("VESTA_NO_ESCAPE_MEM2REG");
            if (!mem2reg_off) {
                std::string mr;
                if (sr_mem2reg_object(fn, *model, site.block_idx, site.ins_idx,
                                      obj, args, fieldaddr_off, mr)) {
                    changed = true;
                    continue;
                }
                diag(site, "mem2reg: " + mr);
            } else {
                diag(site, "field-write cross-block (mem2reg off)");
            }
            continue;
        }

        {
            auto &blkv = fn.blocks[site.block_idx].instrs;
            /* DRY-RUN: simular current[offset] en orden de bloque + validar. */
            struct LdPlan { size_t ii; bool from_store; IrValueId stored; const SrFieldInit *fi; };
            std::vector<LdPlan> ld_plan;
            std::vector<size_t> store_iis;
            std::unordered_map<uint32_t, IrValueId> sim;  /* offset -> ultimo valor escrito */
            const char *vreason = "versioning";
            bool vok = true;

            for (size_t ii = 0; ii < blkv.size() && vok; ++ii) {
                const auto &in = blkv[ii];
                uint32_t off = 0; bool is_ld = false, is_st = false; IrValueId sval = IR_NO_VALUE;
                if (in.op == IrOp::LOAD && !in.operands.empty()) {
                    IrValueId addr = in.operands[0];
                    if (addr == obj) { off = 0; is_ld = true; }
                    else { auto it = fieldaddr_off.find(addr);
                           if (it != fieldaddr_off.end()) { off = it->second; is_ld = true; } }
                } else if (in.op == IrOp::STORE && in.operands.size() >= 2) {
                    IrValueId addr = in.operands[1];
                    if (addr == obj) { off = 0; is_st = true; sval = in.operands[0]; }
                    else { auto it = fieldaddr_off.find(addr);
                           if (it != fieldaddr_off.end()) { off = it->second; is_st = true; sval = in.operands[0]; } }
                }
                if (is_ld) {
                    /* Solo enteros: el forwarding via MOV de un valor float/
                     * ptr/handle podria mezclar bancos GP/ZMM en codegen.
                     * Consistente con el path read-only (int-only). */
                    if (!sr_type_is_int(in.type)) {
                        vok = false; vreason = "campo no-entero con field-write (float/ptr/handle)"; break;
                    }
                    auto sit = sim.find(off);
                    if (sit != sim.end()) {
                        /* forward del ultimo store: el tipo del valor debe
                         * coincidir con el tipo leido. */
                        IrValueId v = sit->second;
                        if (v == IR_NO_VALUE || v >= fn.values.size()
                         || fn.values[v].type != in.type) {
                            vok = false; vreason = "tipo store/load no coincide"; break;
                        }
                        ld_plan.push_back({ii, true, v, nullptr});
                    } else {
                        /* primer load del campo: valor de construccion. */
                        const SrFieldInit *fi = model->find(off);
                        if (!fi) { vok = false; vreason = "load de campo no inicializado (default-0)"; break; }
                        IrInstr probe = blkv[ii];   /* copia: apply=false no muta */
                        if (!sr_rewrite_load(probe, *fi, args, fn, /*apply=*/false)) {
                            vok = false; vreason = "tipo de campo no soportado (float/ptr/handle/widening)"; break;
                        }
                        ld_plan.push_back({ii, false, IR_NO_VALUE, fi});
                    }
                } else if (is_st) {
                    sim[off] = sval;
                    store_iis.push_back(ii);
                }
            }
            if (!vok) { diag(site, vreason); continue; }

            /* APLICAR: reescribir loads, NOPear stores + field-addrs + call. */
            for (const auto &lp : ld_plan) {
                IrInstr &ld = blkv[lp.ii];
                if (lp.from_store) {
                    ld.op = IrOp::MOV; ld.operands.clear();
                    ld.operands.push_back(lp.stored);
                    ld.func_name.clear();
                    if (ld.dst != IR_NO_VALUE && ld.dst < fn.values.size()) {
                        fn.values[ld.dst].is_const = false;
                        fn.values[ld.dst].is_host_ptr = false;
                    }
                } else {
                    sr_rewrite_load(ld, *lp.fi, args, fn, /*apply=*/true);
                }
            }
            for (size_t sii : store_iis) {
                IrInstr &st = blkv[sii];
                st.op = IrOp::NOP; st.operands.clear();
                st.dst = IR_NO_VALUE; st.func_name.clear();
            }
            for (const auto &da : dead_addr) {
                IrInstr &ai = fn.blocks[da.first].instrs[da.second];
                ai.op = IrOp::NOP; ai.operands.clear();
                ai.dst = IR_NO_VALUE; ai.func_name.clear();
            }
            call_ins.op = IrOp::NOP; call_ins.operands.clear();
            call_ins.dst = IR_NO_VALUE; call_ins.func_name.clear();
            changed = true;
        }
    }

    /* Compactar NOPs introducidos (mismo patron que promote_local_raw_alloc). */
    if (changed) {
        for (auto &blk : fn.blocks) {
            auto &is = blk.instrs;
            is.erase(std::remove_if(is.begin(), is.end(), [](const IrInstr &i) {
                return i.op == IrOp::NOP && i.operands.empty()
                    && i.dst == IR_NO_VALUE;
            }), is.end());
        }
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_simplify
// =========================================================================
//
// Aplica identidades algebraicas, plegado de constantes a traves de
// casts, y simplificacion de phis triviales.  Beneficia IGUAL al JIT
// (menos instrucciones emitidas) y a los port targets (codigo C mas
// limpio).
//
// Patrones reconocidos:
//   (A) Algebraic identities:
//       add x, 0     -> x
//       sub x, 0     -> x
//       mul x, 1     -> x
//       mul x, 0     -> 0
//       and x, 0     -> 0
//       and x, -1    -> x
//       or  x, 0     -> x
//       or  x, -1    -> -1
//       xor x, 0     -> x
//       sub x, x     -> 0
//       xor x, x     -> 0
//       shl x, 0, shr x, 0, sar x, 0 -> x
//
//   (B) Cast de constantes:
//       sext.T (const.U K)   -> const.T sign_extend(K)
//       zext.T (const.U K)   -> const.T zero_extend(K)
//       trunc.T (const.U K)  -> const.T K & mask(T)
//       bitcast.T (const.U K) -> const.T K  (mismo ancho ya)
//       cast.T (const.U K)   -> const.T K   (best-effort)
//
//   (C) Phi simplification:
//       %a = phi(b, b, b)   -> %a = mov b   (todos iguales)
//       %a = phi(a, x, y)   -> ignorar self-ref; si solo queda 1 unique -> %a = mov ese
//
// Helpers para Trabajar con CONST + sus valores.

namespace {

bool is_const_with_value(const IrFunction &fn, IrValueId vid, int64_t &out) {
    if (vid == IR_NO_VALUE) return false;
    /* Solo CONST inicializa SSA con un imm conocido.  Buscar la instr
     * que produjo @p vid en cualquier bloque. */
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.dst == vid && ins.op == IrOp::CONST) {
                out = static_cast<int64_t>(ins.imm);
                return true;
            }
        }
    }
    return false;
}

} // namespace


} // namespace ir
