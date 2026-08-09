#include "vex/lowering.h"
#include "vex/collection_intrinsics.h"
#include "vex/comptime_introspect.h"
#include "ir/ir_optimizer.h"

#include <functional>
#include <set>
#include <sstream>
#include <utility>

namespace vex {

// Forward decls from other TUs
uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);
uint64_t intern_class_cache_slot(ir::IrModule &mod, const std::string &name);
ir::IrValueId emit_field_addr(ir::IrFunction *fn, ir::IrBlockId block,
                               ir::IrValueId base, uint64_t offset,
                               uint32_t source_line);

    void Lowering::lower_class_methods(ast::ClassDecl *cd, ir::IrModule &out) {
        // Para cada metodo / constructor de la clase, generamos una
        // IrFunction con nombre <Class>__<method> y un primer parametro
        // implicito 'this' de tipo PTR.  Reusamos la maquinaria del
        // lowering normal: preparamos param_bindings, scope, address-taken
        // pre-pase y bajamos el body con lower_block.
        //
        // Las interfaces se omiten: sus metodos son abstractos (sin body)
        // y solo aportan la metadata de la firma para validacion.
        if (cd->is_interface) return;
        // Templates genericos (con type_params) se omiten: sus
        // monomorphizaciones concretas (que SI aparecen en mod_.decls)
        // se procesan normalmente.
        if (!cd->type_params.empty()) return;
        for (auto &m_uptr: cd->methods) {
            auto *m = m_uptr.get();
            if (!m || !m->body) continue;

            ir::IrFunction fn;
            // Mangling: ClassName__methodName; constructor usa "ctor".
            std::string suffix = m->is_constructor ? std::string("ctor") : m->name;
            fn.name            = cd->name + "__" + suffix;

            // Tipo de retorno + detect SRET (Result/Optional).  Para class
            // methods retornando Result/Optional cross-module, el callee
            // necesita retbuf hidden como SEGUNDO param (this=r1, retbuf=r2,
            // args=r3..).  Sin esto el callee alocaba retbuf en su propio
            // stack y devolvia el ptr via R0 -> use-after-free post-leave.
            Type sem_ret_m = Type{PrimitiveKind::VOID};
            if (m->return_type) sem_ret_m = tc_.resolve_type_node(m->return_type.get());
            const bool method_sret = !m->is_constructor
                && (sem_ret_m.kind == PrimitiveKind::OPTIONAL
                 || sem_ret_m.kind == PrimitiveKind::RESULT);
            if (m->is_constructor) {
                fn.ret_type = ir::IrType::VOID;
            } else if (method_sret) {
                fn.ret_type = ir::IrType::VOID;
            } else if (m->return_type
                && m->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
                auto *pt    = static_cast<ast::PrimitiveTypeNode *>(m->return_type.get());
                fn.ret_type = ir_type_from_primitive(pt->prim);
            } else if (m->return_type) {
                fn.ret_type    = (sem_ret_m.kind != PrimitiveKind::COUNT
                                     && sem_ret_m.kind != PrimitiveKind::VOID)
                                     ? ir_type_from_primitive(sem_ret_m.kind)
                                     : ir::IrType::VOID;
            } else {
                fn.ret_type = ir::IrType::VOID;
            }

            // Param 0: 'this' como PTR.  Sin contar en m->params.
            // Bug fix 2026-05-23: metodos estaticos NO tienen 'this' implicito.
            std::vector<std::pair<std::string, ir::IrValueId> > bindings;
            ir::IrValueId this_vid = ir::IR_NO_VALUE;
            if (!m->is_static) {
                this_vid = fn.new_value(ir::IrType::PTR, "%this");
                fn.values[this_vid].is_param                                 = true;
                // fix - this es siempre un host_ptr a un objeto GC; debe
                // ser refrescado tras cualquier CALL que pueda disparar GC.
                fn.values[this_vid].is_host_ptr  = true;
                fn.values[this_vid].is_gc_object = true;
                fn.params.push_back(this_vid);
                bindings.emplace_back("this", this_vid);
            }

            // SRET retbuf: param hidden tras `this` para methods que
            // retornan Result/Optional.  CALLVIRT debe marshalear retbuf
            // a r2 y args a r3..r(N+2).
            ir::IrValueId v_method_retbuf = ir::IR_NO_VALUE;
            if (method_sret) {
                v_method_retbuf = fn.new_value(ir::IrType::PTR, "%__retbuf");
                fn.values[v_method_retbuf].is_param = true;
                // BugFix sret-cross-mem (2026-06-04): metodos de clase con
                // SRET tambien reciben retbuf como host_ptr (caller hace
                // ALLOCA en host memory).  Sin esta marca el callee escribe
                // al retbuf con `mov` (VM mem) en lugar de `movh` (host)
                // -> el Result llega siempre en ceros al caller.  Caso
                // observado: file_io.FileReader.read_all retornaba con
                // tag=0, error=0 aunque el archivo se hubiera leido OK.
                fn.values[v_method_retbuf].is_host_ptr = true;
                fn.params.push_back(v_method_retbuf);
            }

            // Resto de parametros declarados.
            for (auto &p: m->params) {
                ir::IrType pt                = ir::IrType::I64;
                bool       param_is_class    = false;
                bool       param_is_host_ptr = false;
                if (p->type
                    && p->type->kind == ast::NodeKind::PrimitiveTypeNode) {
                    auto *ptn = static_cast<ast::PrimitiveTypeNode *>(p->type.get());
                    pt        = ir_type_from_primitive(ptn->prim);
                } else if (p->type) {
                    const Type sem = tc_.resolve_type_node(p->type.get());
                    if (sem.kind != PrimitiveKind::COUNT
                        && sem.kind != PrimitiveKind::VOID) {
                        pt = ir_type_from_primitive(sem.kind);
                    }
                    if (sem.kind == PrimitiveKind::CLASS) param_is_class = true;
                    // PTR/ARRAY consultan is_virtual (mismo criterio que
                    // en lower_function): T* host -> host_ptr=true,
                    // VirtualPtr<T> -> host_ptr=false.
                    if ((sem.kind == PrimitiveKind::PTR
                            || sem.kind == PrimitiveKind::ARRAY)
                        && !sem.is_virtual) {
                        param_is_host_ptr = true;
                    }
                }
                const ir::IrValueId vid = fn.new_value(pt, "%" + p->name);
                fn.values[vid].is_param = true;
                if (param_is_class) {
                    // Param de tipo CLASS es host_ptr a un objeto GC.
                    // Marcamos @c is_gc_object para que el regalloc, al
                    // salvar este reg alrededor de un CALL que pueda
                    // disparar GC, lo haga via @c gchandle (handle estable)
                    // y restaure con @c gcderef (host_ptr fresco post-GC).
                    fn.values[vid].is_host_ptr  = true;
                    fn.values[vid].is_gc_object = true;
                } else if (param_is_host_ptr) {
                    fn.values[vid].is_host_ptr = true;
                }
                fn.params.push_back(vid);
                bindings.emplace_back(p->name, vid);
            }

            // Configurar contexto del lowering para esta funcion.
            const ir::IrBlockId entry = fn.new_block("entry");
            fn_                       = &fn;
            current_block_            = entry;
            block_terminated_         = false;
            scopes_.clear();
            push_scope();
            for (auto &kv: bindings) bind(kv.first, kv.second);

            // Pre-pase de address-taken para variables locales del cuerpo.
            address_taken_locals_.clear();
            host_bearing_locals_.clear();
            // fix.cleanup-leak - limpiar el stack de cleanups entre
            // metodos de clase.  Sin esto, si un metodo anterior (e.g. el
            // ctor o un metodo previo) dejo cleanups colgados, el siguiente
            // metodo los hereda y al hacer `return` los ejecuta sobre
            // valores SSA que no le pertenecen, generando CALLVIRT a la
            // dtor con `this` apuntando a un i32 arbitrario -> crash.
            cleanup_stack_.clear();
            escaping_locals_.clear();
            try_spill_slots_.clear();
            scan_address_taken(m->body.get());
            // fix5 - escape detection tambien para metodos de clase.
            // Sin esto, vars locales que escapan via `this.field = local`
            // (ej. `this.head = n` en LinkedList.prepend) NO se marcaban
            // como escaping y mi cleanup RAW_ASM las dropeaba al exit del
            // metodo, dejando `this.field` con un handle invalido.
            scan_escaping_locals(m->body.get());
            // fix9 - eliminado el pre-pase scan_loops del metodo.
            // Las flags solo se usaban para decidir si activar cleanup
            // RAW_ASM (eliminado tras fix8 stack scanning).  Reset
            // a false explicito por consistencia con lower_function.
            current_fn_has_loops_ = false;
            current_fn_has_try_   = false;

            // Marcar que estamos dentro del lowering de un metodo de clase
            // (el resto del lowering puede consultar current_class_lowering_
            // para saber a que ClassLayout pertenece 'this').
            const std::string saved_class = current_class_lowering_;
            current_class_lowering_       = cd->name;

            // Bug fix 2026-05-23 (Audit 14): metodos con return type STRING
            // necesitan auto-promotion de string literals a StringObject en
            // `return "lit"`.  Sin propagar `current_fn_returns_string_` aqui,
            // `lower_return` solo veia el flag para funciones top-level y
            // emitia `mov r0, @Absolute(s_N)` (ptr crudo) en metodos -> el
            // caller recibia un ptr raw como GcHandle -> str_equals/strraw
            // sobre garbage.  Reset al salir del metodo.
            const bool saved_returns_str = current_fn_returns_string_;
            {
                bool is_string_ret = false;
                if (m->return_type
                    && m->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
                    auto *ptn = static_cast<ast::PrimitiveTypeNode *>(m->return_type.get());
                    is_string_ret = (ptn->prim == PrimitiveKind::STRING);
                } else if (m->return_type) {
                    const Type sem = tc_.resolve_type_node(m->return_type.get());
                    is_string_ret = (sem.kind == PrimitiveKind::STRING);
                }
                current_fn_returns_string_ = is_string_ret;
            }

            // Instrumentacion: vex_trace:enter al inicio del metodo
            // (igual filtro que en lower_function -- saltamos solo helpers
            // sinteticos; los ctors/dtors/metodos normales se instrumentan).
            if (instrument_mode_ != "none" && instrument_mode_ != ""
                && fn.name != "__module_init"
                && fn.name.compare(0, 6, "__new_") != 0
                && fn.name.compare(0, 8, "__async_") != 0
                && fn.name.compare(0, 9, "__lambda_") != 0
                && fn.name.compare(0, 8, "__spawn_") != 0) {
                emit_instrument_enter(fn.name, m->loc.line);
            }

            // SRET context para metodos retornando Result/Optional.
            // `lower_return` consulta @c sret_active_ para copiar el slot al
            // retbuf en vez de devolver ptr via R0.
            const bool      saved_sret_active   = sret_active_;
            ir::IrValueId   saved_sret_retbuf   = sret_retbuf_;
            uint64_t        saved_sret_buf_size = sret_buf_size_;
            if (method_sret) {
                sret_active_   = true;
                sret_retbuf_   = v_method_retbuf;
                sret_buf_size_ = (sem_ret_m.kind == PrimitiveKind::OPTIONAL)
                                     ? 16ULL : 24ULL;
            }

            lower_block(m->body.get());

            if (method_sret) {
                sret_active_   = saved_sret_active;
                sret_retbuf_   = saved_sret_retbuf;
                sret_buf_size_ = saved_sret_buf_size;
            }

            current_class_lowering_     = saved_class;
            current_fn_returns_string_  = saved_returns_str;

            // augmentacion automatica del destructor: si este metodo
            // es @c is_destructor, antes del cierre invocamos los dtors de
            // todos los fields destructibles (CLASS con has_destructor o
            // has_destructible_field).  Esto implementa RAII recursivo: el
            // dtor del contenedor libera la cadena ownerships sin que el
            // usuario tenga que escribir el codigo manualmente.
            //
            // Orden: campos en orden de declaracion (no inverso) por
            // simplicidad.  Para clases con ciclos (LinkedList -> Node ->
            // Node ...), el primer @c null encontrado corta la cadena
            // gracias al if (field != null) check.
            //
            // El check de null se hace via cmp_eq + br_cond.  Sin esto,
            // CALLVIRT a un puntero null crashea con NPE.
            if (m->is_destructor && !block_terminated_) {
                auto it_lay = tc_.class_layouts().find(cd->name);
                if (it_lay != tc_.class_layouts().end()) {
                    const ClassLayout &lay = it_lay->second;
                    for (const auto &f: lay.fields) {
                        if (f.type.kind != PrimitiveKind::CLASS) continue;
                        auto it_inner = tc_.class_layouts().find(f.type.struct_name);
                        if (it_inner == tc_.class_layouts().end()) continue;
                        const ClassLayout &inner = it_inner->second;
                        if (!inner.has_destructor) continue;
                        // Localizar vtable_index del dtor del inner.
                        uint32_t inner_dtor_idx = UINT32_MAX;
                        for (const auto &im: inner.methods) {
                            if (im.is_destructor) {
                                inner_dtor_idx = im.vtable_index;
                                break;
                            }
                        }
                        if (inner_dtor_idx == UINT32_MAX) continue;

                        // 1) addr = this + offset
                        const ir::IrValueId addr = emit_field_addr(
                            fn_, current_block_, this_vid, f.offset, m->loc.line);
                        // 2) handle = LOAD i64 addr (handle al inner obj
                        //    almacenado por @c lower_class_field_store).
                        const ir::IrValueId v_handle = fn_->new_value(ir::IrType::I64);
                        ir::IrInstr         ld{};
                        ld.op          = ir::IrOp::LOAD;
                        ld.type        = ir::IrType::I64;
                        ld.dst         = v_handle;
                        ld.operands    = {addr};
                        ld.source_line = m->loc.line;
                        fn_->append(current_block_, std::move(ld));
                        // raw_asm-elim 2026-05-28: 2b) host_ptr fresco via IrOp::GC_DEREF_HOST.
                        const ir::IrValueId field_val       = fn_->new_value(ir::IrType::I64);
                        fn_->values[field_val].is_host_ptr  = true;
                        fn_->values[field_val].is_gc_object = true;
                        ir::IrInstr deref{};
                        deref.op          = ir::IrOp::GC_DEREF_HOST;
                        deref.type        = ir::IrType::PTR;
                        deref.dst         = field_val;
                        deref.operands    = {v_handle};
                        deref.source_line = m->loc.line;
                        fn_->append(current_block_, std::move(deref));
                        // 3) is_null = (field_val == 0)
                        const ir::IrValueId zero    = emit_const(ir::IrType::I64, 0, m->loc.line);
                        const ir::IrValueId is_null = fn_->new_value(ir::IrType::BOOL);
                        ir::IrInstr         cmp{};
                        cmp.op          = ir::IrOp::CMP_EQ;
                        cmp.type        = ir::IrType::BOOL;
                        cmp.dst         = is_null;
                        cmp.operands    = {field_val, zero};
                        cmp.source_line = m->loc.line;
                        fn_->append(current_block_, std::move(cmp));
                        // 4) br_cond is_null skip do_dtor
                        const ir::IrBlockId do_bb   = fn_->new_block("dtor_field");
                        const ir::IrBlockId skip_bb = fn_->new_block("dtor_skip");
                        ir::IrInstr         br{};
                        br.op           = ir::IrOp::BR_COND;
                        br.operands     = {is_null};
                        br.target_block = skip_bb; // true (null) -> skip
                        br.false_block  = do_bb;   // false -> do_dtor
                        br.source_line  = m->loc.line;
                        fn_->append(current_block_, std::move(br));
                        // 5) do_bb: callvirt field_val, inner_dtor_idx; br skip
                        current_block_ = do_bb;
                        ir::IrInstr cv{};
                        cv.op          = ir::IrOp::CALLVIRT;
                        cv.type        = ir::IrType::VOID;
                        cv.dst         = ir::IR_NO_VALUE;
                        cv.operands    = {field_val};
                        cv.imm         = static_cast<uint64_t>(inner_dtor_idx);
                        cv.source_line = m->loc.line;
                        fn_->append(current_block_, std::move(cv));
                        ir::IrInstr brj{};
                        brj.op           = ir::IrOp::BR;
                        brj.target_block = skip_bb;
                        brj.source_line  = m->loc.line;
                        fn_->append(current_block_, std::move(brj));
                        // 6) merge en skip_bb -> continuar con el siguiente field.
                        current_block_    = skip_bb;
                        block_terminated_ = false;
                    }
                }
            }

            // Cierre: anadir RET por defecto si el body no termino con uno.
            if (!block_terminated_) {
                // Instrumentacion: vex_trace:leave antes del RET implicito.
                if (instrument_mode_ != "none" && instrument_mode_ != ""
                    && fn.name != "__module_init"
                    && fn.name.compare(0, 6, "__new_") != 0
                    && fn.name.compare(0, 8, "__async_") != 0
                    && fn.name.compare(0, 9, "__lambda_") != 0
                    && fn.name.compare(0, 8, "__spawn_") != 0) {
                    emit_instrument_exit(fn.name, ir::IR_NO_VALUE, m->loc.line);
                }
                ir::IrInstr ret{};
                ret.op   = ir::IrOp::RET;
                ret.type = fn.ret_type;
                if (fn.ret_type != ir::IrType::VOID) {
                    const ir::IrValueId zero = emit_const(fn.ret_type, 0, m->loc.line);
                    ret.operands.push_back(zero);
                }
                ret.source_line = m->loc.line;
                fn.append(current_block_, std::move(ret));
                block_terminated_ = true;
            }

            pop_scope();
            propagate_is_gc_object_through_phis(fn);

            // B.3 contract: si la clase es una instanciacion generica
            // (e.g., `Box_i32` viene de `class Box<T>`), marcar la
            // IrFunction con el template + type args legibles.  Util
            // para C2 / AOT (dedup de specializations) y para tools
            // (mostrar "Box<i32>::get" en stack traces vs "Box_i32__get").
            if (const auto *mi = tc_.monomorph_info(cd->name)) {
                fn.generic_template_name = mi->template_name;
                fn.generic_type_args     = mi->type_args;
            }

            out.add_function(std::move(fn));
            fn_ = nullptr;
        }
    }

    // -----------------------------------------------------------------
    // Helpers de generacion de codigo .vel para POO dinamica.
    // -----------------------------------------------------------------

    /**
     * @brief Registra el nombre como bytes UTF-8 en static_data y devuelve
     *        el indice para construir @c @Absolute("code.s_<idx>").
    }


    /**
     * @brief Emite el ASM que construye una FindClassParams (16 bytes) en
     *        stack y deja en @c r12 el ClassInfo* localizado.
     */
    void Lowering::generate_new_helpers(ir::IrModule &out) {
        // Para cada clase declarada en el modulo, generamos una funcion
        // __new_<Class>(arg1, ..., argN) -> handle (GcHandle).  El cuerpo
        // es RAW_ASM que hace findclass + newobj + callvirt al ctor.
        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            // No se genera helper para interfaces: no son instanciables.
            if (cd->is_interface) continue;
            // Templates genericos (no monomorphizados): no instanciables.
            if (!cd->type_params.empty()) continue;
            auto it = tc_.class_layouts().find(cd->name);
            if (it == tc_.class_layouts().end()) continue;
            const ClassLayout &lay = it->second;

            // Localizar el constructor PROPIO (no heredado del super).
            // BugFix R1: si la clase deriva de un Base con ctor, los
            // methods incluyen Base.__ctor copiado al inicio (inherited).
            // Sin priorizar el ctor cuyo defining_class == cd->name, el
            // helper __new_<Derived> usaria los params del Base.__ctor
            // -> faltarian args en la llamada -> los campos propios del
            // Derived quedaban a 0.
            const ClassMethodInfo *ctor = nullptr;
            for (const auto &m: lay.methods) {
                if (m.is_constructor && m.defining_class == cd->name) {
                    ctor = &m;
                    break;
                }
            }
            // Fallback: si no hay ctor propio, usar el primero inherited.
            if (!ctor) {
                for (const auto &m: lay.methods) {
                    if (m.is_constructor) { ctor = &m; break; }
                }
            }
            // fix12 - si el ctor es zero-init trivial (solo asigna
            // campos a 0/null/false), saltarlo: el `gc_heap.alloc` ya
            // memset el payload a 0.  Solo aplica si el ctor existe Y
            // no tiene argumentos (ctor con args debe correr para que
            // los argumentos lleguen a campos).
            const bool ctor_is_trivial_zero_init =
                    ctor != nullptr
                    && ctor->is_zero_init_ctor
                    && ctor->param_types.empty();
            // Si el ctor es trivial zero-init, lo tratamos como si no existiera
            // (el helper omitira el callvirt y devolvera el objeto recien
            // alocado).  Esto ahorra ~9 instrucciones VM por `new` para POCs.
            const ClassMethodInfo *effective_ctor = ctor_is_trivial_zero_init
                                                        ? nullptr
                                                        : ctor;
            const size_t   nargs           = effective_ctor ? effective_ctor->param_types.size() : 0;
            const uint32_t ctor_vtable_idx = effective_ctor ? effective_ctor->vtable_index : 0;

            // Registrar el nombre de la clase como datos estaticos.
            const uint64_t name_idx = intern_class_name(out, cd->name);
            const uint32_t name_len = static_cast<uint32_t>(cd->name.size());
            // fix11 - reservar slot de cache para ClassInfo*.
            const uint64_t cache_idx = intern_class_cache_slot(out, cd->name);
            (void) name_idx;
            (void) name_len; // ya no se usa findclass aqui

            // Phase Z.6: emitir TANTO el helper local (__new_<Class>) como,
            // si la clase se uso con `shared` en algun var-decl, su variante
            // shared (__new_<Class>_shared).  El cuerpo es identico salvo
            // que la instruccion `newobj r1` se reemplaza por `newobjs r1`
            // (aloca en SharedHeap).  Generar ambos en el mismo bucle ahorra
            // duplicar toda la logica de calculo de ctor / cache slot.
            const bool need_shared_variant =
                (classes_used_shared_.count(cd->name) > 0);
            const int n_variants = need_shared_variant ? 2 : 1;

            for (int variant = 0; variant < n_variants; ++variant) {
            const bool is_shared_variant = (variant == 1);
            // Mnemonico para la instruccion alloc.  newobjs (Z.6) usa
            // exactamente el mismo encoding REG/FIXED_4 que newobj.
            const char *newobj_op = is_shared_variant ? "newobjs" : "newobj";

            // Construir IrFunction __new_<Class>[_shared].
            ir::IrFunction fn;
            fn.name     = is_shared_variant
                            ? ("__new_" + cd->name + "_shared")
                            : ("__new_" + cd->name);
            fn.ret_type = ir::IrType::PTR;

            // Params: replicar tipos del ctor (si existe).
            for (size_t i = 0; i < nargs; ++i) {
                const ir::IrType    pt  = ir_type_from_primitive(ctor->param_types[i].kind);
                const ir::IrValueId vid = fn.new_value(pt, "%a" + std::to_string(i));
                fn.values[vid].is_param = true;
                fn.params.push_back(vid);
            }

            const ir::IrBlockId entry = fn.new_block("entry");

            // raw_asm-elim Fase 1 (__new_X a IR): la variante LOCAL del helper
            // se construye con IR ops estructurados en vez de RAW_ASM textual,
            // para que el path vreg y el optimizer lo compilen sin el
            // mini-parser de raw_asm.  Patron equivalente al RAW_ASM del
            // bloque `else` de abajo:
            //   v_slot = STR_LIT_ADDR(cache_idx)   (@Absolute("code.s_N"))
            //   v_cls  = LOAD(v_slot)              (ClassInfo* cacheado, vm_mem)
            //   v_h    = NEWOBJ(v_cls)             (GcHandle, r0)
            //   v_this = GC_DEREF_HOST(v_h)        (host_ptr al ObjectHeader)
            //   --- si hay ctor efectivo: ---
            //   CALLVIRT(this=v_this, args=params, vtable_idx=ctor)
            //   v_ret  = GC_DEREF_HOST(v_h)        (RE-deref: el GC del ctor
            //                                       puede haber movido el obj)
            //   RET v_ret
            //   --- sin ctor: RET v_this (campos ya a 0 por gc_heap.alloc) ---
            // raw_asm-elim: la variante SHARED (__new_<X>_shared) tambien se
            // emite estructurada usando IrOp::NEWOBJS (newobjs -> SharedHeap)
            // en lugar de NEWOBJ.  Asi NINGUN __new_<X> queda en RAW_ASM.
            const bool emit_structured = true;
            if (emit_structured) {
                // v_slot = direccion del slot ClassInfo* cacheado (static_data).
                const ir::IrValueId v_slot = fn.new_value(ir::IrType::PTR);
                {
                    ir::IrInstr sa{};
                    sa.op          = ir::IrOp::STR_LIT_ADDR;
                    sa.type        = ir::IrType::PTR;
                    sa.dst         = v_slot;
                    sa.imm         = cache_idx;
                    sa.source_line = cd->loc.line;
                    fn.append(entry, std::move(sa));
                }
                // v_cls = LOAD(v_slot): ClassInfo* leido del slot (vm_mem).
                const ir::IrValueId v_cls = fn.new_value(ir::IrType::I64);
                {
                    ir::IrInstr ld{};
                    ld.op          = ir::IrOp::LOAD;
                    ld.type        = ir::IrType::I64;
                    ld.dst         = v_cls;
                    ld.operands    = {v_slot};
                    ld.source_line = cd->loc.line;
                    fn.append(entry, std::move(ld));
                }
                // v_h = NEWOBJ/NEWOBJS(v_cls): aloca el objeto -> GcHandle.  La
                // variante shared usa NEWOBJS (SharedHeap); el handle lleva el
                // bit 31 y GC_DEREF_HOST de abajo lo resuelve por el path shared.
                const ir::IrValueId v_h = fn.new_value(ir::IrType::I64);
                {
                    ir::IrInstr no{};
                    no.op          = is_shared_variant ? ir::IrOp::NEWOBJS
                                                       : ir::IrOp::NEWOBJ;
                    no.type        = ir::IrType::I64;
                    no.dst         = v_h;
                    no.operands    = {v_cls};
                    no.source_line = cd->loc.line;
                    fn.append(entry, std::move(no));
                }
                // v_this = GC_DEREF_HOST(v_h): host_ptr al ObjectHeader.
                const ir::IrValueId v_this = fn.new_value(ir::IrType::PTR);
                fn.values[v_this].is_host_ptr  = true;
                fn.values[v_this].is_gc_object = true;
                {
                    ir::IrInstr ra{};
                    ra.op          = ir::IrOp::GC_DEREF_HOST;
                    ra.type        = ir::IrType::PTR;
                    ra.dst         = v_this;
                    ra.operands    = {v_h};
                    ra.source_line = cd->loc.line;
                    fn.append(entry, std::move(ra));
                }

                if (effective_ctor) {
                    // CALLVIRT(this=v_this, args=params del helper).  El ctor
                    // es void; argc = nargs + 1 (this + args).
                    //
                    // CLAVE: v_this (host_ptr GC) vive a traves del call (se usa
                    // en el RET de abajo).  El mecanismo de preservacion de GC
                    // roots lo convierte a handle ANTES del call y lo refresca a
                    // host_ptr DESPUES (save_live_regs gchandle/push/pop/gcderef
                    // en el interp; spill + stackmap del commit 6 en el vreg) --
                    // exactamente lo que el RAW_ASM legacy hacia a mano.  Por
                    // eso NO re-derefamos ni marcamos el handle: marcar v_h
                    // (que YA es handle) como GC hacia que el interp le aplicara
                    // gchandle (host_ptr->handle) sobre un handle -> basura.
                    ir::IrInstr cv{};
                    cv.op          = ir::IrOp::CALLVIRT;
                    cv.type        = ir::IrType::VOID;
                    cv.dst         = ir::IR_NO_VALUE;
                    cv.operands.reserve(nargs + 1);
                    cv.operands.push_back(v_this);
                    for (size_t i = 0; i < nargs; ++i) {
                        cv.operands.push_back(fn.params[i]);
                    }
                    cv.imm         = static_cast<uint64_t>(ctor_vtable_idx);
                    cv.source_line = cd->loc.line;
                    fn.append(entry, std::move(cv));
                }

                // RET v_this (host_ptr al objeto; preservado/refrescado a
                // traves del callvirt si habia ctor).
                {
                    ir::IrInstr ret{};
                    ret.op          = ir::IrOp::RET;
                    ret.type        = ir::IrType::PTR;
                    ret.operands    = {v_this};
                    ret.source_line = cd->loc.line;
                    fn.append(entry, std::move(ret));
                }
            } else {
            // Construir RAW_ASM body.
            std::ostringstream asm_;

            // fix12 - optimizaciones bytecode-level del helper:
            //  (1) Cargar cache en r1 directamente (skip mov r1, r12).
            //  (2) push r0 directo (handle) en lugar de gchandle r12, r12.
            //  (3) xchg cur0, r1 post-newobj para obtener host_ptr en r1
            //      sin la secuencia xchg cur0, r12 + mov r1, r12.
            // Para nargs=0 estas tres optimizaciones combinan a 3 instr menos.
            // Para nargs>0 ahorran 1 instr (la shift derecha sigue necesaria).

            if (nargs == 0) {
                // Caso comun y ultra-optimizado: ctor sin args (o sin ctor,
                // o ctor zero-init trivial saltado por fix12).
                // Cargar ClassInfo* directo en r1 (sin pasar por r12).
                asm_ << "mov r1, @Absolute(\"code.s_" << cache_idx << "\")\n";
                asm_ << "mov r1, [r1]\n"; // r1 = ClassInfo* (cacheado)
                asm_ << "mov r15, 1\n";
                asm_ << newobj_op << " r1\n"; // r0 = GcHandle, r1 = ClassInfo*
                if (effective_ctor) {
                    // Preservar handle directamente con push r0 (sin gchandle).
                    // newobj acaba de devolver r0=handle; el GC del ctor body
                    // puede mover el objeto pero el handle es estable.
                    asm_ << "push r0\n";          // save handle pre-ctor
                    asm_ << "gcderef cur0, r0\n"; // cur0 = host_ptr
                    asm_ << "xchg cur0, r1\n";    // r1 = host_ptr (this); cur0 = ClassInfo*
                    asm_ << "mov r15, 1\n";
                    asm_ << "callvirt r1, " << ctor_vtable_idx << "\n";
                    asm_ << "pop r12\n";           // r12 = handle (restored)
                    asm_ << "gcderef cur0, r12\n"; // cur0 = host_ptr fresco
                    asm_ << "xchg cur0, r12\n";    // r12 = host_ptr
                    asm_ << "mov r0, r12\n";
                } else {
                    // Sin ctor (real o saltado por zero-init opt): convertir
                    // handle a host_ptr y devolverlo.  Los fields ya estan
                    // a 0 por el memset de gc_heap.alloc.
                    asm_ << "gcderef cur0, r0\n";
                    asm_ << "xchg cur0, r12\n"; // r12 = host_ptr
                    asm_ << "mov r0, r12\n";
                }
            } else {
                // Caso con args: necesitamos salvar/restaurar args alrededor
                // del newobj (que clobbera r1) y hacer shift derecha pre-callvirt.
                // Salvar args en stack: push r1..r_N (orden ascendente).
                for (size_t i = 0; i < nargs; ++i) {
                    asm_ << "push r" << (i + 1) << "\n";
                }
                // Cargar cache en r1 (los args ya estan salvados).
                asm_ << "mov r1, @Absolute(\"code.s_" << cache_idx << "\")\n";
                asm_ << "mov r1, [r1]\n"; // r1 = ClassInfo*
                asm_ << "mov r15, 1\n";
                asm_ << newobj_op << " r1\n"; // r0 = handle
                // Convertir handle a host_ptr en r12 (que esta libre).
                asm_ << "gcderef cur0, r0\n";
                asm_ << "xchg cur0, r12\n"; // r12 = host_ptr
                // Restaurar args en orden inverso (LIFO): r_N, ..., r1.
                for (size_t i = nargs; i > 0; --i) {
                    asm_ << "pop r" << i << "\n";
                }
                if (effective_ctor) {
                    // Shift derecha: r_{N+1}=r_N, ..., r2=r1.
                    for (size_t i = nargs + 1; i >= 2; --i) {
                        asm_ << "mov r" << i << ", r" << (i - 1) << "\n";
                    }
                    asm_ << "mov r1, r12\n"; // this = host_ptr
                    // fix - preservar handle a traves de callvirt
                    // porque el ctor body puede hacer GC moves.
                    asm_ << "gchandle r12, r12\n"; // r12 = handle
                    asm_ << "push r12\n";
                    asm_ << "mov r15, " << (nargs + 1) << "\n";
                    asm_ << "callvirt r1, " << ctor_vtable_idx << "\n";
                    asm_ << "pop r12\n";           // r12 = handle
                    asm_ << "gcderef cur0, r12\n"; // cur0 = host_ptr fresco
                    asm_ << "xchg cur0, r12\n";
                }
                asm_ << "mov r0, r12\n";
            }

            ir::IrInstr ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::PTR;
            ra.dst         = ir::IR_NO_VALUE;
            ra.func_name   = asm_.str();
            ra.source_line = cd->loc.line;
            fn.append(entry, std::move(ra));

            // Cerrar con RET PTR (r0 ya tiene el handle).
            ir::IrInstr ret{};
            ret.op   = ir::IrOp::RET;
            ret.type = ir::IrType::PTR;
            // No anyadimos operands; el emisor IR genera 'ret' simple y r0
            // ya esta cargado por el RAW_ASM previo.  Para que el emisor no
            // intente construir RET con un valor SSA, usamos VOID y luego
            // dejamos un fall-through.  Mejor: ret sin operandos como void.
            ret.type        = ir::IrType::VOID;
            ret.source_line = cd->loc.line;
            fn.append(entry, std::move(ret));
            } // fin else (path RAW_ASM legacy: ctor / nargs>0 / shared)

            propagate_is_gc_object_through_phis(fn);

            // B.3 contract: el helper @c __new_<Class> tambien lleva
            // metadata de monomorphizacion cuando la clase es una
            // instanciacion generica.  Asi C2/AOT pueden agrupar todas
            // las funciones (metodos + helpers) de una specialization
            // como una unidad.
            if (const auto *mi = tc_.monomorph_info(cd->name)) {
                fn.generic_template_name = mi->template_name;
                fn.generic_type_args     = mi->type_args;
            }

            // Bug fix 2026-05-23: registrar como helper PURO si el ctor
            // es trivial (sin callvirt al ctor user-defined).  Solo entonces
            // el DCE puede eliminar el __new_<X> cuando el handle no se usa.
            // Sin esto, ctors que pueden throw veian sus excepciones
            // swallow-eadas cuando el resultado del `new X()` no se usaba.
            if (!effective_ctor) {
                ir::register_pure_new_helper(fn.name);
            }
            out.add_function(std::move(fn));
            } // for variant in {local, shared}
        }
    }

    void Lowering::generate_module_init_function(ir::IrModule &out) {
        // Si no hay clases NI runtime globals que inicializar, no
        // generamos __module_init.
        bool any_class = false;
        for (auto &decl: mod_.decls) {
            if (decl && decl->kind == ast::NodeKind::ClassDecl) {
                any_class = true;
                break;
            }
        }
        const bool has_runtime_globals = !runtime_global_slots_.empty();
        if (!any_class && !has_runtime_globals) return;

        ir::IrFunction fn;
        fn.name                   = "__module_init";
        fn.ret_type               = ir::IrType::VOID;
        const ir::IrBlockId entry = fn.new_block("entry");

        // raw_asm-elim Fase 2c: __module_init se construye con IR ESTRUCTURADO
        // (en vez de un unico bloque RAW_ASM monolitico), para que el path vreg
        // del JIT lo compile sin el mini-parser de raw_asm y para que el futuro
        // AOT no vea bytecode opaco.  Patron equivalente al RAW_ASM legacy:
        //   - Un buffer de params FRESCO (ALLOCA 40 bytes = max de los structs
        //     24/32/40) por operacion.  El buffer NO se promueve a host (sus
        //     usos como operando de DEF*/FIND* son escape para
        //     promote_local_allocas) -> queda como vaddr valido que los ops de
        //     meta-OOP leen via params_vaddr.
        //   - STORE a vm_mem (el buffer es vaddr) arma cada struct; el DEF/FIND
        //     correspondiente lo consume inmediatamente.  Los ops meta-OOP son
        //     side-effecting + barreras de DSE/scheduler -> el optimizer no
        //     reordena ni elimina los STORE que arman el struct.
        //   - CADA clase y CADA advice se emiten en su PROPIO basic block
        //     (encadenados por BR).  Razon: el regalloc del .vel (interp) es
        //     fragil con un unico bloque enorme de cientos de temporales (una
        //     instruccion de mas voltea el spill a codigo incorrecto -> findclass
        //     lee params de una direccion basura).  Con un bloque por clase/
        //     advice los valores son LOCALES al bloque (buffers frescos, v_cls
        //     recargado del cache slot, valores AOP intra-advice) -> el liveness
        //     se resetea en cada frontera -> presion acotada -> regalloc correcto.
        //     Sin PHIs ni valores cross-block.
        const int ln = 0;  // ops sinteticas (sin linea fuente propia)
        ir::IrBlockId cur = entry;  // bloque actual (avanza con new_seg)

        // Crea un nuevo segmento (basic block) y encadena con BR desde el actual.
        auto new_seg = [&](const std::string &name) {
            const ir::IrBlockId nb = fn.new_block(name);
            ir::IrInstr br{};
            br.op = ir::IrOp::BR; br.type = ir::IrType::VOID;
            br.target_block = nb; br.source_line = ln;
            fn.append(cur, std::move(br));
            cur = nb;
        };

        // --- helpers locales para emitir IR sobre fn/cur ---
        auto emit_const64 = [&](uint64_t k) -> ir::IrValueId {
            const ir::IrValueId v = fn.new_value(ir::IrType::I64);
            ir::IrInstr c{};
            c.op = ir::IrOp::CONST; c.type = ir::IrType::I64;
            c.dst = v; c.imm = k; c.source_line = ln;
            fn.append(cur, std::move(c));
            return v;
        };
        auto emit_strlit = [&](uint64_t idx) -> ir::IrValueId {
            // Direccion vaddr del slot static_data idx (@Absolute("code.s_N")).
            const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
            ir::IrInstr s{};
            s.op = ir::IrOp::STR_LIT_ADDR; s.type = ir::IrType::PTR;
            s.dst = v; s.imm = idx; s.source_line = ln;
            fn.append(cur, std::move(s));
            return v;
        };
        auto emit_label_addr = [&](const std::string &label) -> ir::IrValueId {
            // Direccion vaddr de un label de codigo (@Absolute("code.LABEL")).
            const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
            ir::IrInstr s{};
            s.op = ir::IrOp::LABEL_ADDR; s.type = ir::IrType::PTR;
            s.dst = v; s.func_name = label; s.source_line = ln;
            fn.append(cur, std::move(s));
            return v;
        };
        // Buffer de params UNICO (40 bytes = max de los structs 24/32/40) en el
        // VM stack, alocado UNA sola vez y reusado para todos los defclass/
        // deffield/defmethod/findmethod/setmethdbg/findclass/addadvice.  Razones:
        //   - Un ALLOCA por OP inflaria el VM stack (subsp por op sin addsp hasta
        //     el `leave`); con muchas clases/metodos eso baja el VM-RSP cientos de
        //     bytes y cambia el layout VM-stack/heap -> destapa bugs latentes de
        //     scan/GC dependientes del layout (99/133/179 crasheaban en interp).
        //     Un solo ALLOCA mantiene el VM stack casi plano (como el RAW_ASM).
        //   - El multi-bloque (un block por clase/advice) + el SPILL cross-bloque
        //     del .vel regalloc evitan el clobber del registro del buffer (el bug
        //     del single-block era tenerlo en un registro vivo a traves de ~80
        //     ops; spillado a un slot y recargado por uso, no se clobbea).
        //   - La DSE-barrera de los ops meta-OOP preserva los STORE que arman el
        //     struct aunque se reuse buf+0 entre defs.
        ir::IrValueId shared_buf = ir::IR_NO_VALUE;
        auto fresh_buf = [&]() -> ir::IrValueId {
            if (shared_buf != ir::IR_NO_VALUE) return shared_buf;
            const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
            ir::IrInstr al{};
            al.op = ir::IrOp::ALLOCA; al.type = ir::IrType::I8;  // unidad 1 byte
            al.dst = v; al.imm = 40; al.source_line = ln;
            fn.append(cur, std::move(al));  // is_host_ptr=false -> VM stack
            shared_buf = v;
            return v;
        };
        // STORE v_val en buf + off (vm_mem; el buffer es vaddr).
        auto store_at = [&](ir::IrValueId buf, uint64_t off, ir::IrValueId v_val) {
            ir::IrValueId v_addr = buf;
            if (off != 0) {
                v_addr = fn.new_value(ir::IrType::PTR);
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD; ad.type = ir::IrType::I64;
                ad.dst = v_addr;
                ad.operands = {buf, emit_const64(off)};
                ad.source_line = ln;
                fn.append(cur, std::move(ad));  // is_host_ptr=false (vaddr)
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE; st.type = ir::IrType::I64;
            st.operands = {v_val, v_addr}; st.source_line = ln;
            fn.append(cur, std::move(st));
        };
        // FINDCLASS/DEFCLASS/FINDMETHOD: dst = op(buf).  ClassInfo*/MethodInfo*
        // son host_ptr nativos (no GC) -> is_host_ptr=true, is_gc_object=false.
        auto emit_find1 = [&](ir::IrOp op, ir::IrValueId buf) -> ir::IrValueId {
            const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
            fn.values[v].is_host_ptr = true;
            ir::IrInstr i{};
            i.op = op; i.type = ir::IrType::PTR; i.dst = v;
            i.operands = {buf}; i.source_line = ln;
            fn.append(cur, std::move(i));
            return v;
        };
        // DEFFIELD/DEFMETHOD: op(v_cls, buf) sin dst.
        auto emit_def2 = [&](ir::IrOp op, ir::IrValueId v_cls, ir::IrValueId buf) {
            ir::IrInstr i{};
            i.op = op; i.type = ir::IrType::VOID;
            i.operands = {v_cls, buf}; i.source_line = ln;
            fn.append(cur, std::move(i));
        };

        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            auto  it = tc_.class_layouts().find(cd->name);
            if (it == tc_.class_layouts().end()) continue;
            const ClassLayout &lay = it->second;

            // Bloque propio para esta clase (presion de registros acotada).
            new_seg("cls_" + cd->name);

            const uint64_t cname_idx = intern_class_name(out, cd->name);
            const uint32_t cname_len = static_cast<uint32_t>(cd->name.size());

            // 1) Si hay superclase: FindClassParams (16B) + findclass -> v_super.
            ir::IrValueId v_super = ir::IR_NO_VALUE;
            if (!cd->super_name.empty()) {
                const uint64_t sname_idx = intern_class_name(out, cd->super_name);
                const uint32_t sname_len = static_cast<uint32_t>(cd->super_name.size());
                const ir::IrValueId b = fresh_buf();
                store_at(b, 0, emit_strlit(sname_idx));
                store_at(b, 8, emit_const64(sname_len));
                v_super = emit_find1(ir::IrOp::FINDCLASS, b);
            }

            // 2) DefClassParams (32B) + defclass -> v_cls.
            const ir::IrValueId b_dc = fresh_buf();
            store_at(b_dc, 0, emit_strlit(cname_idx));
            // [+8] (flags<<32)|name_len con flags=CLASS_VIS_PUBLIC=1.
            store_at(b_dc, 8, emit_const64((uint64_t(1) << 32) | uint64_t(cname_len)));
            // [+16] super_class (ClassInfo* o 0).
            store_at(b_dc, 16, (v_super != ir::IR_NO_VALUE) ? v_super : emit_const64(0));
            // [+24] reserved = 0.
            store_at(b_dc, 24, emit_const64(0));
            const ir::IrValueId v_cls = emit_find1(ir::IrOp::DEFCLASS, b_dc);

            // 2.5) Cachear el ClassInfo* en su slot static_data (lo lee
            // __new_<Class> sin findclass).  STORE a vm_mem (slot = vaddr).
            const uint64_t cache_idx = intern_class_cache_slot(out, cd->name);
            {
                const ir::IrValueId v_cache = emit_strlit(cache_idx);
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE; st.type = ir::IrType::I64;
                st.operands = {v_cls, v_cache}; st.source_line = ln;
                fn.append(cur, std::move(st));
            }

            // Helper: recargar el ClassInfo* desde el cache slot (corto-vivo en
            // cada def para no estresar el regalloc con un v_cls vivo a traves
            // de muchos deffield/defmethod).
            auto reload_cls = [&]() -> ir::IrValueId {
                const ir::IrValueId v_a = emit_strlit(cache_idx);
                const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
                fn.values[v].is_host_ptr = true;
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD; ld.type = ir::IrType::I64;
                ld.dst = v; ld.operands = {v_a}; ld.source_line = ln;
                fn.append(cur, std::move(ld));
                return v;
            };

            // 3) Fields PROPIOS (los heredados ya los copio define_class).
            for (size_t fi = lay.inherited_field_count; fi < lay.fields.size(); ++fi) {
                const auto &   f         = lay.fields[fi];
                const uint64_t fname_idx = intern_class_name(out, f.name);
                const uint32_t fname_len = static_cast<uint32_t>(f.name.size());
                const ir::IrValueId b = fresh_buf();
                store_at(b, 0, emit_strlit(fname_idx));
                // [+8] name_len (kind/access/is_static = 0 para field de instancia).
                store_at(b, 8, emit_const64(uint64_t(fname_len)));
                store_at(b, 16, emit_const64(8));   // size_bytes = 8 (1 slot)
                store_at(b, 24, emit_const64(0));   // type_class = 0 (primitive)
                emit_def2(ir::IrOp::DEFFIELD, reload_cls(), b);
            }

            // 3.5) Static fields PROPIOS (is_static=1 en bit +48 del packed).
            for (size_t si = lay.inherited_static_field_count;
                 si < lay.static_fields.size(); ++si) {
                const auto &   f         = lay.static_fields[si];
                const uint64_t fname_idx = intern_class_name(out, f.name);
                const uint32_t fname_len = static_cast<uint32_t>(f.name.size());
                const ir::IrValueId b = fresh_buf();
                store_at(b, 0, emit_strlit(fname_idx));
                store_at(b, 8, emit_const64(uint64_t(fname_len) | (uint64_t(1) << 48)));
                store_at(b, 16, emit_const64(8));
                store_at(b, 24, emit_const64(0));
                emit_def2(ir::IrOp::DEFFIELD, reload_cls(), b);
            }

            // Las interfaces no emiten defmethod (metodos abstractos sin code).
            if (cd->is_interface) continue;

            // 4) Metodos PROPIOS o sobreescritos (defining_class == cd->name).
            for (const auto &m: lay.methods) {
                if (!m.defining_class.empty() && m.defining_class != cd->name)
                    continue;  // heredado puro
                const std::string suffix      = m.is_constructor ? std::string("ctor") : m.name;
                const std::string owner_class = m.defining_class.empty()
                                                    ? cd->name : m.defining_class;
                const std::string method_label = owner_class + "__" + suffix;
                const uint64_t mname_idx = intern_class_name(out, m.name);
                const uint32_t mname_len = static_cast<uint32_t>(m.name.size());
                const std::string desc_str = "()";
                const uint64_t    desc_idx = intern_class_name(out, desc_str);
                const uint32_t    desc_len = static_cast<uint32_t>(desc_str.size());

                uint64_t mflags = 0;
                if (m.is_constructor) mflags |= (1ULL << 9);   // METHOD_FLAG_CONSTRUCTOR
                else                  mflags |= (1ULL << 10);  // METHOD_FLAG_VIRTUAL

                // DefMethodParams (40B) + defmethod.
                const ir::IrValueId b = fresh_buf();
                store_at(b, 0, emit_strlit(mname_idx));
                store_at(b, 8, emit_const64(uint64_t(mname_len) | (uint64_t(desc_len) << 32)));
                store_at(b, 16, emit_strlit(desc_idx));
                store_at(b, 24, emit_label_addr(method_label));   // code_vaddr
                store_at(b, 32, emit_const64(mflags));
                emit_def2(ir::IrOp::DEFMETHOD, reload_cls(), b);

                // Debug info (file:line) si la hay: findmethod + setmethdbg.
                if (!m.source_file.empty() && m.source_line > 0) {
                    const uint64_t fname_idx = intern_class_name(out, m.source_file);
                    const uint32_t fname_len = static_cast<uint32_t>(m.source_file.size());

                    // FindMethodParams (24B) + findmethod -> v_method.
                    const ir::IrValueId bf = fresh_buf();
                    store_at(bf, 0, reload_cls());
                    store_at(bf, 8, emit_strlit(mname_idx));
                    store_at(bf, 16, emit_const64(uint64_t(mname_len)));
                    const ir::IrValueId v_method = emit_find1(ir::IrOp::FINDMETHOD, bf);

                    // SetMethDebugParams (24B) + setmethdbg.
                    const ir::IrValueId bs = fresh_buf();
                    store_at(bs, 0, v_method);
                    store_at(bs, 8, emit_strlit(fname_idx));
                    store_at(bs, 16, emit_const64(uint64_t(fname_len)
                                                  | (uint64_t(m.source_line) << 32)));
                    ir::IrInstr smd{};
                    smd.op = ir::IrOp::SETMETHDBG; smd.type = ir::IrType::VOID;
                    smd.operands = {v_method, bs}; smd.source_line = ln;
                    fn.append(cur, std::move(smd));
                }
            }
        }

        // -----------------------------------------------------------------
        // 2do pase: AOP.  Tras registrar todas las clases/metodos, recorremos
        // los aspectos y emitimos findclass + findmethod (x2) + addadvice por
        // cada @Before/@After/@Around (cada advice en su propio bloque).
        // Requiere que las clases target ya esten registradas (2do pase).
        // -----------------------------------------------------------------
        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            if (!cd->type_params.empty()) continue;  // template, no se procesa
            for (auto &m_uptr: cd->methods) {
                auto *m = m_uptr.get();
                if (!m || m->advice_kind == 0) continue;

                const std::string &target = m->advice_target;
                const size_t        dot   = target.find('.');
                if (dot == std::string::npos || dot == 0 || dot + 1 >= target.size()) {
                    error_at(m->loc,
                             "AOP: pointcut '" + target + "' no tiene formato 'Clase.metodo'");
                    continue;
                }
                const std::string tcls  = target.substr(0, dot);
                const std::string tmeth = target.substr(dot + 1);
                const uint8_t     rt_kind = static_cast<uint8_t>(m->advice_kind - 1);

                // Bloque propio para este advice (presion acotada).
                new_seg("aop_" + cd->name + "_" + m->name);

                // 1) findclass de la clase target -> v_tc.
                const uint64_t tcls_idx = intern_class_name(out, tcls);
                const uint32_t tcls_len = static_cast<uint32_t>(tcls.size());
                const ir::IrValueId b1 = fresh_buf();
                store_at(b1, 0, emit_strlit(tcls_idx));
                store_at(b1, 8, emit_const64(tcls_len));
                const ir::IrValueId v_tc = emit_find1(ir::IrOp::FINDCLASS, b1);

                // 2) findmethod del target -> v_tm.
                const uint64_t tmeth_idx = intern_class_name(out, tmeth);
                const uint32_t tmeth_len = static_cast<uint32_t>(tmeth.size());
                const ir::IrValueId b2 = fresh_buf();
                store_at(b2, 0, v_tc);
                store_at(b2, 8, emit_strlit(tmeth_idx));
                store_at(b2, 16, emit_const64(tmeth_len));
                const ir::IrValueId v_tm = emit_find1(ir::IrOp::FINDMETHOD, b2);

                // 3) findclass del aspecto -> v_ac.
                const uint64_t acls_idx = intern_class_name(out, cd->name);
                const uint32_t acls_len = static_cast<uint32_t>(cd->name.size());
                const ir::IrValueId b3 = fresh_buf();
                store_at(b3, 0, emit_strlit(acls_idx));
                store_at(b3, 8, emit_const64(acls_len));
                const ir::IrValueId v_ac = emit_find1(ir::IrOp::FINDCLASS, b3);

                // 4) findmethod del advice -> v_am.
                const uint64_t adm_idx = intern_class_name(out, m->name);
                const uint32_t adm_len = static_cast<uint32_t>(m->name.size());
                const ir::IrValueId b4 = fresh_buf();
                store_at(b4, 0, v_ac);
                store_at(b4, 8, emit_strlit(adm_idx));
                store_at(b4, 16, emit_const64(adm_len));
                const ir::IrValueId v_am = emit_find1(ir::IrOp::FINDMETHOD, b4);

                // 5) addadvice(target, advice, kind).
                ir::IrInstr aa{};
                aa.op = ir::IrOp::ADDADVICE; aa.type = ir::IrType::VOID;
                aa.operands = {v_tm, v_am}; aa.imm = rt_kind; aa.source_line = ln;
                fn.append(cur, std::move(aa));
            }
        }

        // L2.2: inicializar runtime globals con su literal de init.
        // Activamos el contexto del lowering (fn_=&fn, current_block_=cur)
        // temporalmente para reutilizar emit_const/STRMAKE/STORE.  Usamos
        // @c cur (el ultimo bloque encadenado), no @c entry, porque el
        // generador parte __module_init en multiples bloques.
        if (has_runtime_globals) {
            ir::IrFunction *saved_fn      = fn_;
            ir::IrBlockId   saved_block   = current_block_;
            bool            saved_term    = block_terminated_;
            fn_               = &fn;
            current_block_    = cur;
            block_terminated_ = false;
            for (auto &decl : mod_.decls) {
                if (!decl || decl->kind != ast::NodeKind::GlobalVarDecl) continue;
                auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
                if (gv->is_const || !gv->init) continue;
                auto rit = runtime_global_slots_.find(gv->name);
                if (rit == runtime_global_slots_.end()) continue;
                const uint64_t slot_idx = rit->second;
                const int ln = gv->loc.line;
                // Computar el valor inicial via lower_expr (cubre IntLit,
                // StringLit-no-interpolado promovido a StringObject por
                // lower_string_literal_to_string_object, etc.).
                ir::IrValueId v_init = ir::IR_NO_VALUE;
                if (gv->init->kind == ast::NodeKind::StringLitExpr) {
                    auto *slit = static_cast<ast::StringLitExpr *>(gv->init.get());
                    v_init = lower_string_literal_to_string_object(slit);
                } else {
                    v_init = lower_expr(gv->init.get());
                }
                if (v_init == ir::IR_NO_VALUE) continue;
                // STR_LIT_ADDR -> slot addr, STORE init.
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
                    ir::IrInstr is{};
                    is.op          = ir::IrOp::STR_LIT_ADDR;
                    is.type        = ir::IrType::PTR;
                    is.dst         = v_addr;
                    is.imm         = slot_idx;
                    is.source_line = ln;
                    fn_->append(current_block_, std::move(is));
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.operands    = {v_init, v_addr};
                st.source_line = ln;
                fn_->append(current_block_, std::move(st));
            }
            fn_               = saved_fn;
            current_block_    = saved_block;
            block_terminated_ = saved_term;
        }

        ir::IrInstr ret{};
        ret.op          = ir::IrOp::RET;
        ret.type        = ir::IrType::VOID;
        ret.source_line = 0;
        fn.append(cur, std::move(ret));  // ultimo bloque encadenado

        propagate_is_gc_object_through_phis(fn);
        out.add_function(std::move(fn));
    }

    std::string Lowering::build_module_init_asm(ir::IrModule & /*out_module*/) {
        // No se usa: la generacion de __module_init se hace via
        // generate_module_init_function (IrFunction completa, no cadena).
        return std::string();
    }

    ir::IrValueId Lowering::lower_new_expr(ast::NewExpr *e) {
        // bug4: array allocation `new T[N]`.  Emit RAW_ALLOC(N * sizeof(T))
        // y devolver host_ptr al primer elemento.  Cada slot zero-init
        // por RawAllocator.
        if (e->array_size) {
            // Resolver sizeof(T) en bytes.
            uint64_t elem_size = 8;  // default qword (suficiente para
                                      // class refs, strings handles, ptrs).
            auto pk_size = [](const std::string &n) -> uint64_t {
                if (n == "i8" || n == "u8" || n == "bool" || n == "char") return 1;
                if (n == "i16" || n == "u16") return 2;
                if (n == "i32" || n == "u32" || n == "f32" || n == "float") return 4;
                if (n == "i64" || n == "u64" || n == "f64" || n == "double" || n == "string")
                    return 8;
                return 0;  // no es primitivo
            };
            const uint64_t prim_sz = pk_size(e->class_name);
            if (prim_sz > 0) {
                elem_size = prim_sz;
            } else {
                // Clase user: host_ptr de 8 bytes por slot.
                auto it_cls = tc_.class_layouts().find(e->class_name);
                if (it_cls != tc_.class_layouts().end()) {
                    elem_size = 8;  // refs a objetos GC
                } else {
                    // Struct value-type o enum.
                    auto it_st = tc_.struct_layouts().find(e->class_name);
                    if (it_st != tc_.struct_layouts().end()) {
                        elem_size = static_cast<uint64_t>(it_st->second.size_bytes);
                    } else {
                        auto it_en = tc_.enum_layouts().find(e->class_name);
                        if (it_en != tc_.enum_layouts().end()) {
                            elem_size = static_cast<uint64_t>(it_en->second.size_bytes);
                        }
                    }
                }
            }
            // Lower count + multiplica por elem_size.
            const ir::IrValueId v_count = lower_expr(e->array_size.get());
            if (v_count == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            // Coerce count a i64 si no lo es.
            const ir::IrType ct = fn_->values[v_count].type;
            ir::IrValueId v_count_i64 = (ct == ir::IrType::I64 || ct == ir::IrType::U64)
                ? v_count
                : cast_if_needed(v_count, ct, ir::IrType::I64, e->loc.line,
                                  /*is_explicit=*/true);
            // total = count * elem_size.
            ir::IrValueId v_total;
            if (elem_size == 1) {
                v_total = v_count_i64;
            } else {
                const ir::IrValueId v_es = emit_const(ir::IrType::I64,
                    static_cast<int64_t>(elem_size), e->loc.line);
                v_total = fn_->new_value(ir::IrType::I64);
                ir::IrInstr mul{};
                mul.op          = ir::IrOp::MUL;
                mul.type        = ir::IrType::I64;
                mul.dst         = v_total;
                mul.operands    = {v_count_i64, v_es};
                mul.source_line = e->loc.line;
                fn_->append(current_block_, std::move(mul));
            }
            // RAW_ALLOC(total) -> host_ptr.
            const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ptr].is_host_ptr = true;
            ir::IrInstr ra{};
            ra.op          = ir::IrOp::RAW_ALLOC;
            ra.type        = ir::IrType::PTR;
            ra.dst         = v_ptr;
            ra.operands    = {v_total};
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            return v_ptr;
        }
        // BugFix R4: `new ExceptionClass(msg)` para predefined exceptions
        // (RuntimeException, ArithmeticException, etc.).  No hay __new_<X>
        // synthetic helper; emitimos newobj + store message inline.
        {
            auto it_cls_pre = tc_.class_layouts().find(e->class_name);
            if (it_cls_pre != tc_.class_layouts().end()
             && it_cls_pre->second.is_runtime_predefined
             && e->class_name != "FatalError") {
                if (e->args.size() != 1) {
                    error_at(e->loc,
                        "new " + e->class_name + "(msg): se espera 1 argumento");
                    return ir::IR_NO_VALUE;
                }
                // Auto-promocion literal -> StringObject (mismo patron que
                // lower_call para args string).
                ir::IrValueId v_msg;
                if (e->args[0]->kind == ast::NodeKind::StringLitExpr) {
                    auto *sl = static_cast<ast::StringLitExpr *>(e->args[0].get());
                    v_msg = lower_string_literal_to_string_object(sl);
                } else {
                    v_msg = lower_expr(e->args[0].get());
                }
                if (v_msg == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                // findclass <exc_name> -> v_cls (host_ptr a ClassInfo).
                const uint64_t cname_idx = intern_class_name(*out_mod_, e->class_name);
                const uint32_t cname_len = static_cast<uint32_t>(e->class_name.size());
                const ir::IrValueId v_cls = emit_findclass_by_name(
                    cname_idx, cname_len, e->loc.line);
                // Emit NEWOBJ IR (regalloc-aware) -> handle in v_handle.
                // Luego un minimal RAW_ASM convierte handle -> host_ptr
                // via `gcderef cur0, rX; xchg cur0, rX`.  La conversion
                // misma no tiene IR op dedicado, pero el resto de la
                // operacion (NEWOBJ + STORE) si esta en IR puro.
                const ir::IrValueId v_handle = fn_->new_value(ir::IrType::I64);
                {
                    ir::IrInstr no{};
                    no.op          = ir::IrOp::NEWOBJ;
                    no.type        = ir::IrType::PTR;  // tipo simbolico
                    no.dst         = v_handle;
                    no.operands    = {v_cls};
                    no.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(no));
                }
                // raw_asm-elim 2026-05-28: handle -> host_ptr via IrOp::GC_DEREF_HOST.
                // El IR op produce dst sin clobber del src; equivalente
                // semanticamente al RAW_ASM previo `gcderef + xchg + mov`.
                const ir::IrValueId v_obj = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_obj].is_host_ptr  = true;
                fn_->values[v_obj].is_gc_object = true;
                {
                    ir::IrInstr ra{};
                    ra.op          = ir::IrOp::GC_DEREF_HOST;
                    ra.type        = ir::IrType::PTR;
                    ra.dst         = v_obj;
                    ra.operands    = {v_handle};
                    ra.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(ra));
                }
                // store v_msg at v_obj + 24 (message field offset, after
                // ObjectHeader).  is_host_ptr=true for v_obj => emite movh.
                const ir::IrValueId v_off = emit_const(ir::IrType::I64, 24, e->loc.line);
                const ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_addr].is_host_ptr = true;
                {
                    ir::IrInstr ad{};
                    ad.op           = ir::IrOp::ADD;
                    ad.type         = ir::IrType::I64;
                    ad.dst          = v_addr;
                    ad.operands     = {v_obj, v_off};
                    ad.source_line  = e->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                {
                    ir::IrInstr st{};
                    st.op           = ir::IrOp::STORE;
                    st.type         = ir::IrType::I64;
                    st.dst          = ir::IR_NO_VALUE;
                    st.operands     = {v_msg, v_addr};
                    st.source_line  = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                }
                ssa_concrete_class_[v_obj] = e->class_name;
                return v_obj;
            }
        }

        // Bajar argumentos.  Bug fix 2026-05-23: si el ctor espera un
        // STRING param y el arg es un StringLitExpr, promover a StringObject.
        // Sin esto, `new P("alice")` pasaba el ptr raw del literal como
        // GcHandle invalido al ctor.
        const ClassMethodInfo *ctor_sig = nullptr;
        {
            auto it_cls_sig = tc_.class_layouts().find(e->class_name);
            if (it_cls_sig != tc_.class_layouts().end()) {
                for (const auto &m : it_cls_sig->second.methods) {
                    if (m.is_constructor && m.defining_class == e->class_name) {
                        ctor_sig = &m; break;
                    }
                }
                if (!ctor_sig) {
                    for (const auto &m : it_cls_sig->second.methods) {
                        if (m.is_constructor) { ctor_sig = &m; break; }
                    }
                }
            }
        }
        std::vector<ir::IrValueId> arg_vals;
        arg_vals.reserve(e->args.size());
        for (size_t ai = 0; ai < e->args.size(); ++ai) {
            auto &a = e->args[ai];
            const bool param_is_string =
                ctor_sig && ai < ctor_sig->param_types.size()
                && ctor_sig->param_types[ai].kind == PrimitiveKind::STRING;
            ir::IrValueId av;
            if (param_is_string && a->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = static_cast<ast::StringLitExpr *>(a.get());
                av = lower_string_literal_to_string_object(slit);
            } else {
                av = lower_expr(a.get());
            }
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
        }

        // Emit IrInstr::CALL a __new_<ClassName>(args).  La funcion auxiliar
        // se genera al final de run() via generate_new_helpers.
        //
        // Phase Z.6: si @c e->is_shared, despachamos al helper
        // @c __new_<ClassName>_shared (emite @c newobjs en lugar de @c newobj
        // -> aloca en el SharedHeap).  El frontend marca @c is_shared cuando
        // el var-decl padre tiene modificador @c shared.
        const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
        // fix - el resultado es un host_ptr a un objeto GESTIONADO por
        // GC.  Marcamos is_gc_object para que el regalloc, al spillarlo
        // alrededor de cualquier CALL posterior (que pueda disparar GC),
        // emita el dance gchandle/gcderef y refresque el host_ptr.
        fn_->values[dst].is_host_ptr  = true;
        fn_->values[dst].is_gc_object = true;
        // M.L7 ext: clase importada cross-module.  El helper @c __new_<X>
        // en el dep fue emitido con el nombre LOCAL del dep (e.g. "Buffer"),
        // no con el mangled del consumer ("buffer__Buffer").  Si el layout
        // tiene @c imported_helper_suffix , lo usamos como sufijo del label.
        std::string helper_class_name = e->class_name;
        {
            const auto &class_layouts = tc_.class_layouts();
            auto it_lay = class_layouts.find(e->class_name);
            if (it_lay != class_layouts.end()
             && !it_lay->second.imported_helper_suffix.empty()) {
                helper_class_name = it_lay->second.imported_helper_suffix;
            }
        }
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::CALL;
        ins.type        = ir::IrType::PTR;
        ins.dst         = dst;
        ins.func_name   = e->is_shared
                            ? ("__new_" + helper_class_name + "_shared")
                            : ("__new_" + helper_class_name);
        ins.operands    = std::move(arg_vals);
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        // Trackear tipo concreto del resultado para devirtualizacion
        // de calls via interface receiver en compile time.
        ssa_concrete_class_[dst] = e->class_name;
        return dst;
    }

    ir::IrValueId Lowering::lower_this_expr(ast::ThisExpr *e) {
        const ir::IrValueId v = lookup("this");
        if (v == ir::IR_NO_VALUE) {
            error_at(e->loc, "lowering: 'this' fuera de contexto de metodo");
        }
        return v;
    }

    /**
     * @brief Helper: construye un IrValueId que apunta a @c base+offset,
     *        marcado como host_ptr (para que LOAD/STORE emitan @c movh).
     *        Si offset es 0, devuelve el base directamente.
     */
    /* non-static (visible desde lowering_class.cpp en el futuro) */
    ir::IrValueId emit_field_addr(ir::IrFunction *fn,
                                         ir::IrBlockId   block,
                                         ir::IrValueId   base,
                                         uint64_t        offset,
                                         uint32_t        line) {
        if (offset == 0) {
            // El frontend marca el resultado como host_ptr para que LOAD/
            // STORE usen movh.  Si la base ya tiene is_host_ptr=true, la
            // propagacion es trivial; si no, lo forzamos aqui (siempre lo
            // sera para nuestros punteros de objeto Vex).
            fn->values[base].is_host_ptr = true;
            return base;
        }
        // Crear constante con el offset y sumar.
        ir::IrInstr         c{};
        const ir::IrValueId off_val   = fn->new_value(ir::IrType::I64);
        fn->values[off_val].is_const  = true;
        fn->values[off_val].const_val = offset;
        c.op                          = ir::IrOp::CONST;
        c.type                        = ir::IrType::I64;
        c.dst                         = off_val;
        c.imm                         = offset;
        c.source_line                 = line;
        fn->append(block, std::move(c));

        const ir::IrValueId addr = fn->new_value(ir::IrType::PTR);
        // Marcar host_ptr: las operaciones LOAD/STORE consultan este flag
        // para emitir mov (VM) o movh (host).  Las direcciones derivadas
        // de un host_ptr siguen siendo host_ptr.
        fn->values[addr].is_host_ptr = true;
        ir::IrInstr add{};
        add.op          = ir::IrOp::ADD;
        add.type        = ir::IrType::PTR;
        add.dst         = addr;
        add.operands    = {base, off_val};
        add.source_line = line;
        fn->append(block, std::move(add));
        return addr;
    }

    ir::IrValueId Lowering::lower_class_field_load(ast::FieldAccessExpr *e) {
        // Limitacion G (cerrada): @c property_kind == 3 marca acceso a
        // static field via @c ClassName.field.  El base es IdentExpr cuyo
        // nombre es la clase; lo resolvemos via findclass inline + getstatic
        // con offset compile-time.  No leemos el tipo del base con
        // @c check_expr (fallaria por "nombre no declarado") sino que
        // tomamos el ClassLayout directamente del nombre.
        if (e->property_kind == 3) {
            if (!e->base || e->base->kind != ast::NodeKind::IdentExpr) {
                error_at(e->loc, "lowering: static field con base no-ClassName");
                return ir::IR_NO_VALUE;
            }
            auto *base_id = static_cast<ast::IdentExpr *>(e->base.get());
            auto  it_cls  = tc_.class_layouts().find(base_id->name);
            if (it_cls == tc_.class_layouts().end()) {
                error_at(e->loc,
                         "lowering: clase desconocida '" + base_id->name + "'");
                return ir::IR_NO_VALUE;
            }
            const ClassLayout &lay_s = it_cls->second;
            uint32_t           s_off = 0;
            Type               s_typ = Type{PrimitiveKind::COUNT};
            bool               s_ok  = false;
            for (const auto &f: lay_s.static_fields) {
                if (f.name == e->field_name) {
                    s_off = f.offset;
                    s_typ = f.type;
                    s_ok  = true;
                    break;
                }
            }
            if (!s_ok) {
                error_at(e->loc,
                         "lowering: static field '" + e->field_name +
                         "' no encontrado en la clase '" + base_id->name + "'");
                return ir::IR_NO_VALUE;
            }
            // 1) Sprint 5: findclass via IR ops (ALLOCA + STORE + FINDCLASS).
            const uint64_t     cname_idx = intern_class_name(*out_mod_, base_id->name);
            const uint32_t     cname_len = static_cast<uint32_t>(base_id->name.size());
            const ir::IrValueId v_cls = emit_findclass_by_name(cname_idx, cname_len, e->loc.line);
            // 2) getstatic {dst}, {src0}, offset_imm  -> v_val.
            // El opcode lee SIEMPRE 8 bytes (i64).  Para tipos < i64 la
            // semantica de sign/zero-extension coincide porque setstatic
            // almacena los bits high del reg fuente que el productor
            // sign-extendio (LOAD/CONST genericos hacen shl+sar para signed).
            const ir::IrType    ir_t  = ir_type_from_primitive(s_typ.kind);
            // Emite GETSTATIC IR op; el bytecode lee i64 que truncamos por tipo
            // mas abajo si el SSA val se usa como ancho menor (semantica heredada).
            ir::IrValueId v_val = emit_getstatic(v_cls, static_cast<uint64_t>(s_off),
                                                  e->loc.line);
            // Cast al tipo logico del field si difiere de I64.
            if (ir_t != ir::IrType::I64) {
                v_val = cast_if_needed(v_val, ir::IrType::I64, ir_t,
                                       e->loc.line, /*is_explicit=*/true);
            }
            // Si el tipo del field es PTR host (no VirtualPtr), propagar
            // is_host_ptr al SSA value (mismo tratamiento que field de
            // instancia, ver final de esta funcion).
            // VirtualPtr (s_typ.is_virtual == true) NO recibe is_host_ptr.
            if (s_typ.kind == PrimitiveKind::PTR && !s_typ.is_virtual) {
                fn_->values[v_val].is_host_ptr = true;
            }
            return v_val;
        }

        const Type bt = e->base->result_type;
        if (bt.kind != PrimitiveKind::CLASS) {
            error_at(e->loc, "lowering: '.' sobre tipo no-clase en lower_class_field_load");
            return ir::IR_NO_VALUE;
        }
        auto it = tc_.class_layouts().find(bt.struct_name);
        if (it == tc_.class_layouts().end()) {
            error_at(e->loc,
                     "lowering: clase desconocida '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &lay = it->second;
        // si el type checker marco el acceso como propiedad, emitir
        // CALLVIRT al getter `get_<field_name>` en vez de getfield.
        if (e->property_kind == 1) {
            const std::string      getter_name = std::string("get_") + e->field_name;
            const ClassMethodInfo *mtd         = nullptr;
            for (const auto &m: lay.methods) {
                if (!m.is_constructor && m.name == getter_name) {
                    mtd = &m;
                    break;
                }
            }
            if (!mtd) {
                error_at(e->loc,
                         "lowering: getter de propiedad '" + e->field_name +
                         "' no encontrado en la clase '" + bt.struct_name + "'");
                return ir::IR_NO_VALUE;
            }
            const ir::IrValueId obj = lower_expr(e->base.get());
            if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            const ir::IrType    ret_ir = ir_type_from_primitive(mtd->return_type.kind);
            const ir::IrValueId dst    = (ret_ir == ir::IrType::VOID)
                                             ? ir::IR_NO_VALUE
                                             : fn_->new_value(ret_ir);
            // Sprint edge-bugs (2026-06-03): si el metodo retorna CLASS, el
            // dst es un host_ptr a un objeto GC.  Marcarlo asi para que el
            // regalloc lo trate como GC-managed (save_live_regs lo convierte
            // a GcHandle antes de cualquier CALL siguiente).  Sin esto un
            // patron `p2 = p1.factory(); p3 = p2.factory();` rompe en interp:
            // el host_ptr de p2 queda stale tras el GC dentro del segundo
            // factory.  Mismo bug con `*->is_host_ptr` no marcado para
            // tipos PTR (e.g. `int* get_buf()`).
            if (dst != ir::IR_NO_VALUE) {
                const PrimitiveKind rk = mtd->return_type.kind;
                if (rk == PrimitiveKind::CLASS) {
                    fn_->values[dst].is_host_ptr  = true;
                    fn_->values[dst].is_gc_object = true;
                } else if ((rk == PrimitiveKind::PTR
                         || rk == PrimitiveKind::ARRAY)
                        && !mtd->return_type.is_virtual) {
                    fn_->values[dst].is_host_ptr = true;
                }
            }
            ir::IrInstr ins{};
            ins.op   = ir::IrOp::CALLVIRT;
            ins.type = ret_ir;
            ins.dst  = dst;
            ins.operands.push_back(obj);
            ins.imm         = static_cast<uint64_t>(mtd->vtable_index);
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            return dst;
        }
        uint32_t off  = 0;
        bool     ok   = false;
        Type     ftyp = Type{PrimitiveKind::COUNT};
        for (const auto &f: lay.fields) {
            if (f.name == e->field_name) {
                off  = f.offset;
                ftyp = f.type;
                ok   = true;
                break;
            }
        }
        if (!ok) {
            error_at(e->loc,
                     "lowering: campo '" + e->field_name +
                     "' no encontrado en la clase '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ir::IrValueId obj = lower_expr(e->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        // Bajar a addr = obj + off (host_ptr) + LOAD estandar.  El emisor
        // IR consulta is_host_ptr y emite movh (memoria host).  Esto evita
        // el patron cur0/gcderef que colisionaba con el regalloc.
        const ir::IrValueId addr = emit_field_addr(fn_, current_block_, obj, off,
                                                   e->loc.line);
        const ir::IrType    ir_t = ir_type_from_primitive(ftyp.kind);
        const ir::IrValueId dst  = fn_->new_value(ir_t);
        ir::IrInstr         ld{};
        ld.op          = ir::IrOp::LOAD;
        ld.type        = ir_t;
        ld.dst         = dst;
        ld.operands    = {addr};
        ld.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ld));
        // fix: si el TIPO del campo es PTR (host pointer obtenido
        // via malloc o similar), propagar @c is_host_ptr=true al SSA value
        // resultante para que indexaciones / derefs posteriores emitan
        // @c movh y NO @c mov (que iria a VM memory y leeria garbage).
        // Sin esto, `box.p[i]` con `box.p: i32*` cargaba con `mov` en lugar
        // de `movh`, leyendo memoria virtual VM en vez del buffer host
        // de malloc -> garbage o segfault.
        // EXCEPCION: VirtualPtr<T> (ftyp.is_virtual == true) es una direccion
        // VM aunque el tipo base sea PTR.  El valor cargado es una VA del
        // espacio VM, NO un puntero host.  Marcar is_host_ptr=true sobre un
        // VirtualPtr causaria que `*field` emitiera movh en vez de mov,
        // interpretando la VA como direccion host -> segfault.
        if (ftyp.kind == PrimitiveKind::PTR && !ftyp.is_virtual) {
            fn_->values[dst].is_host_ptr = true;
        }
        // Dynamic arrays `T[]` (size==0) stored as fields also hold host_ptrs
        // (from `new T[N]` via RAW_ALLOC).  Sin esto, `box.data[i] = ...`
        // emitia `mov` (VM mem) en vez de `movh` (host mem) tras LOAD del
        // field -> escribia/leia en vm_mem en una direccion que es realmente
        // host -> valores corrompidos.  Bug bug4-extension.
        // Para arrays dinamicos (`T[]`, array_size==0) el field guarda
        // un host_ptr (de `new T[N]` que usa RAW_ALLOC).  El default de
        // @c Type::make_array es is_virtual=true pero ese flag aplica a
        // arrays SIZED en stack; los dinamicos son siempre host.
        if (ftyp.kind == PrimitiveKind::ARRAY && ftyp.array_size == 0) {
            fn_->values[dst].is_host_ptr = true;
        }
        // fix - field de tipo CLASS guarda un GcHandle (estable a
        // evacuacion del GC).  Tras LOADear el handle, hacemos @c gcderef
        // para obtener el host_ptr actual del objeto (refrescado tras
        // cualquier movimiento del GC).  Sin esta refresh, el ptr leido
        // del campo seria stale si el objeto migro a OldGen entre el store
        // y este load -> segfault al hacer @c callvirt o leer fields.
        if (ftyp.kind == PrimitiveKind::CLASS) {
            // raw_asm-elim 2026-05-28: gcderef + xchg -> IrOp::GC_DEREF_HOST.
            ir::IrValueId v_host             = fn_->new_value(ir::IrType::I64);
            fn_->values[v_host].is_host_ptr  = true;
            fn_->values[v_host].is_gc_object = true;
            ir::IrInstr deref{};
            deref.op          = ir::IrOp::GC_DEREF_HOST;
            deref.type        = ir::IrType::PTR;
            deref.dst         = v_host;
            deref.operands    = {dst};
            deref.source_line = e->loc.line;
            fn_->append(current_block_, std::move(deref));
            return v_host;
        }
        return dst;
    }

    ir::IrValueId Lowering::lower_class_field_store(ast::FieldAccessExpr *target,
                                                    ir::IrValueId         rhs,
                                                    const SourceLoc &     loc) {
        if (!target || rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        // Limitacion G (cerrada): @c property_kind == 3 marca asignacion a
        // static field via @c ClassName.field = v.  Mismo patron que
        // lower_class_field_load: findclass inline + setstatic con offset
        // compile-time.  El base es IdentExpr (nombre de clase), no
        // referenciable como SSA value; resolvemos directo del layout.
        if (target->property_kind == 3) {
            if (!target->base
                || target->base->kind != ast::NodeKind::IdentExpr) {
                error_at(loc, "lowering: static field store con base no-ClassName");
                return ir::IR_NO_VALUE;
            }
            auto *base_id = static_cast<ast::IdentExpr *>(target->base.get());
            auto  it_cls  = tc_.class_layouts().find(base_id->name);
            if (it_cls == tc_.class_layouts().end()) {
                error_at(loc,
                         "lowering: clase desconocida '" + base_id->name + "'");
                return ir::IR_NO_VALUE;
            }
            const ClassLayout &lay_s = it_cls->second;
            uint32_t           s_off = 0;
            Type               s_typ = Type{PrimitiveKind::COUNT};
            bool               s_ok  = false;
            for (const auto &f: lay_s.static_fields) {
                if (f.name == target->field_name) {
                    s_off = f.offset;
                    s_typ = f.type;
                    s_ok  = true;
                    break;
                }
            }
            if (!s_ok) {
                error_at(loc,
                         "lowering: static field '" + target->field_name +
                         "' no encontrado en la clase '" + base_id->name + "'");
                return ir::IR_NO_VALUE;
            }
            // Coerce rhs al tipo del field si difieren.
            const ir::IrType    field_ir = ir_type_from_primitive(s_typ.kind);
            const ir::IrValueId rhs_cast = cast_if_needed(rhs, fn_->values[rhs].type,
                                                          field_ir, loc.line);
            // 1) Sprint 5: findclass via IR ops.
            const uint64_t cname_idx = intern_class_name(*out_mod_, base_id->name);
            const uint32_t cname_len = static_cast<uint32_t>(base_id->name.size());
            const ir::IrValueId v_cls = emit_findclass_by_name(cname_idx, cname_len, loc.line);
            // 2) setstatic.  Coerce rhs_cast a I64 si fuera necesario.
            ir::IrValueId v_val_i64 = rhs_cast;
            if (fn_->values[rhs_cast].type != ir::IrType::I64) {
                v_val_i64 = cast_if_needed(rhs_cast, fn_->values[rhs_cast].type,
                                            ir::IrType::I64, loc.line,
                                            /*is_explicit=*/true);
            }
            emit_setstatic(v_cls, v_val_i64, static_cast<uint64_t>(s_off), loc.line);
            return rhs_cast;
        }

        const Type bt = target->base->result_type;
        if (bt.kind != PrimitiveKind::CLASS) {
            error_at(loc, "lowering: '.' sobre tipo no-clase en lower_class_field_store");
            return ir::IR_NO_VALUE;
        }
        auto it = tc_.class_layouts().find(bt.struct_name);
        if (it == tc_.class_layouts().end()) {
            error_at(loc, "lowering: clase desconocida '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &lay = it->second;
        // si el type checker marco el target como setter de
        // propiedad, emitir CALLVIRT al setter `set_<field_name>` en vez
        // de setfield.  El rhs se pasa como argumento del setter.
        if (target->property_kind == 2) {
            const std::string      setter_name = std::string("set_") + target->field_name;
            const ClassMethodInfo *mtd         = nullptr;
            for (const auto &m: lay.methods) {
                if (!m.is_constructor && m.name == setter_name) {
                    mtd = &m;
                    break;
                }
            }
            if (!mtd) {
                error_at(loc,
                         "lowering: setter de propiedad '" + target->field_name +
                         "' no encontrado en la clase '" + bt.struct_name + "'");
                return ir::IR_NO_VALUE;
            }
            const ir::IrValueId obj = lower_expr(target->base.get());
            if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            const ir::IrType param_ir = mtd->param_types.empty()
                                            ? ir::IrType::I64
                                            : ir_type_from_primitive(mtd->param_types.front().kind);
            const ir::IrValueId rhs_cast = cast_if_needed(rhs, fn_->values[rhs].type, param_ir, loc.line);
            ir::IrInstr         ins{};
            ins.op   = ir::IrOp::CALLVIRT;
            ins.type = ir::IrType::VOID;
            ins.dst  = ir::IR_NO_VALUE;
            ins.operands.push_back(obj);
            ins.operands.push_back(rhs_cast);
            ins.imm         = static_cast<uint64_t>(mtd->vtable_index);
            ins.source_line = loc.line;
            fn_->append(current_block_, std::move(ins));
            return rhs_cast;
        }
        uint32_t off  = 0;
        bool     ok   = false;
        Type     ftyp = Type{PrimitiveKind::COUNT};
        for (const auto &f: lay.fields) {
            if (f.name == target->field_name) {
                off  = f.offset;
                ftyp = f.type;
                ok   = true;
                break;
            }
        }
        if (!ok) {
            error_at(loc,
                     "lowering: campo '" + target->field_name +
                     "' no encontrado en la clase '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ir::IrValueId obj = lower_expr(target->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrValueId addr = emit_field_addr(fn_, current_block_, obj, off,
                                                   loc.line);
        const ir::IrType    ir_t     = ir_type_from_primitive(ftyp.kind);
        const ir::IrValueId rhs_cast = cast_if_needed(rhs, fn_->values[rhs].type, ir_t, loc.line);
        // Si el campo es CLASS, almacenamos el GcHandle (estable a evacuacion
        // del GC) en vez del host_ptr crudo.  Sin esto, una alocacion entre
        // el store y el siguiente load podria mover el objeto y dejar el ptr
        // guardado apuntando a memoria liberada/reusada -> segfault al
        // hacer @c callvirt sobre `this.field`.
        ir::IrValueId v_to_store = rhs_cast;
        if (ftyp.kind == PrimitiveKind::CLASS) {
            v_to_store = emit_gc_handle_for_ptr(rhs_cast, loc.line);
        }
        ir::IrInstr st{};
        st.op          = ir::IrOp::STORE;
        st.type        = ir_t;
        st.dst         = ir::IR_NO_VALUE;
        st.operands    = {v_to_store, addr};
        st.source_line = loc.line;
        fn_->append(current_block_, std::move(st));
        return rhs_cast;
    }

    ir::IrValueId Lowering::lower_class_method_call(ast::CallExpr *e) {
        // El callee es FieldAccessExpr cuyo base es de tipo CLASS.  Bajamos
        // el receptor, localizamos el vtable_index del metodo y emitimos
        // CALLVIRT.  El IR emitter coloca obj en r1 y args en r2..r_{N+1}
        // antes de la instruccion bytecode 'callvirt r1, vtable_idx'.
        if (!e->callee || e->callee->kind != ast::NodeKind::FieldAccessExpr) {
            error_at(e->loc, "lowering: callee no es field-access en class method call");
            return ir::IR_NO_VALUE;
        }
        auto *     fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());

        // Bug fix 2026-05-23: metodos estaticos.  property_kind=4 marca una
        // llamada estatica `ClassName.method(args)`.  Emitimos CALLVM directo
        // a `<Class>__<method>` sin pasar this como primer arg.
        if (fa->property_kind == 4) {
            std::string class_name;
            if (fa->base && fa->base->kind == ast::NodeKind::IdentExpr) {
                class_name = static_cast<ast::IdentExpr *>(fa->base.get())->name;
            }
            if (class_name.empty()) {
                error_at(e->loc, "lowering: nombre de clase vacio en llamada estatica");
                return ir::IR_NO_VALUE;
            }
            auto it_cls = tc_.class_layouts().find(class_name);
            if (it_cls == tc_.class_layouts().end()) {
                error_at(e->loc, "lowering: clase '" + class_name + "' no encontrada");
                return ir::IR_NO_VALUE;
            }
            const ClassMethodInfo *static_mtd = nullptr;
            for (const auto &m : it_cls->second.methods) {
                if (m.is_constructor) continue;
                if (m.is_static && m.name == fa->field_name) { static_mtd = &m; break; }
            }
            if (!static_mtd) {
                error_at(e->loc,
                    "lowering: metodo estatico '" + class_name + "." +
                    fa->field_name + "' no encontrado");
                return ir::IR_NO_VALUE;
            }
            // Bajar args.
            std::vector<ir::IrValueId> arg_vals;
            arg_vals.reserve(e->args.size());
            for (size_t ai = 0; ai < e->args.size(); ++ai) {
                auto &a = e->args[ai];
                if (!a) return ir::IR_NO_VALUE;
                // Auto-promotion para args string literales.
                const bool param_is_string =
                    ai < static_mtd->param_types.size()
                    && static_mtd->param_types[ai].kind == PrimitiveKind::STRING;
                if (param_is_string
                    && a->kind == ast::NodeKind::StringLitExpr) {
                    auto *slit = static_cast<ast::StringLitExpr *>(a.get());
                    arg_vals.push_back(lower_string_literal_to_string_object(slit));
                } else {
                    const ir::IrValueId av = lower_expr(a.get());
                    if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                    arg_vals.push_back(av);
                }
            }
            const ir::IrType ret_ir = ir_type_from_primitive(static_mtd->return_type.kind);
            ir::IrValueId dst = (ret_ir == ir::IrType::VOID)
                ? ir::IR_NO_VALUE
                : fn_->new_value(ret_ir);
            ir::IrInstr ins{};
            ins.op   = ir::IrOp::CALL;
            ins.type = ret_ir;
            ins.dst  = dst;
            ins.func_name = class_name + "__" + fa->field_name;
            ins.operands  = arg_vals;
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            return dst;
        }

        const Type bt = fa->base->result_type;
        if (bt.kind != PrimitiveKind::CLASS) {
            error_at(e->loc, "lowering: receptor no es CLASS en method call");
            return ir::IR_NO_VALUE;
        }
        auto it = tc_.class_layouts().find(bt.struct_name);
        if (it == tc_.class_layouts().end()) {
            error_at(e->loc, "lowering: clase desconocida '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &    lay = it->second;
        const ClassMethodInfo *mtd = nullptr;
        for (const auto &m: lay.methods) {
            if (!m.is_constructor && m.name == fa->field_name) {
                mtd = &m;
                break;
            }
        }
        if (!mtd) {
            error_at(e->loc,
                     "lowering: metodo '" + fa->field_name +
                     "' no encontrado en la clase '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        // Bajar receptor y argumentos.
        const ir::IrValueId obj = lower_expr(fa->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        std::vector<ir::IrValueId> arg_vals;
        arg_vals.reserve(e->args.size());
        for (size_t ai = 0; ai < e->args.size(); ++ai) {
            auto &a = e->args[ai];
            if (!a) return ir::IR_NO_VALUE;
            // Auto-promocion literal -> StringObject cuando el parametro
            // espera STRING y el arg es un StringLit no interpolado.
            // Mismo patron que @c lower_call usa para funciones libres:
            // sin esto, pasar @c helper("hola") a @c void helper(string s)
            // pushearia la direccion cruda del literal como i64 (PTR) y
            // el callee crashearia al hacer @c strraw s con ptr invalido.
            // Sin esto, str_cstr/str_bytes dentro del metodo trataban el
            // PTR del literal como GcHandle invalido y leian garbage.
            const bool param_is_string =
                ai < mtd->param_types.size()
                && mtd->param_types[ai].kind == PrimitiveKind::STRING;
            if (param_is_string
                && a->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = static_cast<ast::StringLitExpr *>(a.get());
                // Tanto literales puros como interpolados: el helper
                // construye el StringObject correcto.
                const ir::IrValueId av =
                    lower_string_literal_to_string_object(slit);
                if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                arg_vals.push_back(av);
                continue;
            }
            const ir::IrValueId av = lower_expr(a.get());
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
        }
        const ir::IrType ret_ir_decl = ir_type_from_primitive(mtd->return_type.kind);

        // SRET en class method: si el metodo declara devolver Optional/Result,
        // su firma IR real es void + retbuf hidden como segundo param (tras
        // this).  El caller alloca el buffer (16 / 24 bytes) y lo pasa como
        // primer "arg" del CALLVIRT (entre obj y los args declarados).  El
        // dst del CALLVIRT es VOID; el SSA value visible al lowering es el
        // retbuf (PTR), que se bindea a la var-decl o se pasa a otras fns.
        const bool method_call_sret = (mtd->return_type.kind == PrimitiveKind::OPTIONAL
                                    || mtd->return_type.kind == PrimitiveKind::RESULT);
        ir::IrValueId v_method_call_retbuf = ir::IR_NO_VALUE;
        if (method_call_sret) {
            uint64_t buf_bytes = (mtd->return_type.kind == PrimitiveKind::RESULT)
                                     ? 24ULL : 16ULL;
            v_method_call_retbuf = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.imm         = buf_bytes;
            al.dst         = v_method_call_retbuf;
            al.source_line = e->loc.line;
            // BugFix sret-cross-mem (2026-06-04): forzar host_alloca para
            // el retbuf de metodos Optional/Result.  Asi el callee escribe
            // con `movh` y el caller lee con `movh` consistentemente.
            al.set_host_alloca(true);
            fn_->append(current_block_, std::move(al));
            fn_->values[v_method_call_retbuf].is_host_ptr = true;
        }
        const ir::IrType ret_ir = method_call_sret ? ir::IrType::VOID : ret_ir_decl;

        // si el metodo esta marcado @Inline y el receptor NO es
        // interfaz (necesitamos la implementacion concreta), buscar el
        // AST ClassMethodDecl y expandir el cuerpo en el call site.
        // MVP: solo metodos cuyo body sea exactamente `{ return expr; }`.
        if (mtd->is_inline && !lay.is_interface) {
            const ast::ClassDecl *cd_orig = nullptr;
            for (auto &d: mod_.decls) {
                if (!d || d->kind != ast::NodeKind::ClassDecl) continue;
                auto *cdp = static_cast<const ast::ClassDecl *>(d.get());
                if (cdp->name == mtd->defining_class) {
                    cd_orig = cdp;
                    break;
                }
            }
            if (cd_orig) {
                const ast::ClassMethodDecl *mdecl = nullptr;
                for (const auto &um: cd_orig->methods) {
                    if (um && um->name == mtd->name && !um->is_constructor) {
                        mdecl = um.get();
                        break;
                    }
                }
                if (mdecl
                    && mdecl->body
                    && mdecl->body->body.size() == 1
                    && mdecl->body->body[0]
                    && mdecl->body->body[0]->kind == ast::NodeKind::ReturnStmt) {
                    auto *rs = static_cast<ast::ReturnStmt *>(mdecl->body->body[0].get());
                    if (rs->value) {
                        // Push scope con bindings: this -> obj, params -> args.
                        push_scope();
                        bind("this", obj);
                        const size_t np = std::min(mdecl->params.size(), arg_vals.size());
                        for (size_t i = 0; i < np; ++i) {
                            bind(mdecl->params[i]->name, arg_vals[i]);
                        }
                        const ir::IrValueId v = lower_expr(rs->value.get());
                        pop_scope();
                        return v;
                    }
                }
                // Si no se cumple la forma esperada, caer al CALLVIRT.
            }
        }

        const ir::IrValueId dst = (ret_ir == ir::IrType::VOID)
                                      ? ir::IR_NO_VALUE
                                      : fn_->new_value(ret_ir);
        // Sprint edge-bugs (2026-06-03): marcar dst con is_host_ptr/
        // is_gc_object cuando el metodo retorna CLASS/PTR.  Critico
        // para que el regalloc preserve el value a traves de calls
        // GC posteriores (save/restore con conversion a GcHandle).
        if (dst != ir::IR_NO_VALUE) {
            const PrimitiveKind rk = mtd->return_type.kind;
            if (rk == PrimitiveKind::CLASS) {
                fn_->values[dst].is_host_ptr  = true;
                fn_->values[dst].is_gc_object = true;
            } else if ((rk == PrimitiveKind::PTR
                     || rk == PrimitiveKind::ARRAY)
                    && !mtd->return_type.is_virtual) {
                fn_->values[dst].is_host_ptr = true;
            }
        }
        // El valor SSA "visible" al lowering tras el CALLVIRT.  Para SRET
        // es el retbuf (PTR); para calls normales es dst.
        const ir::IrValueId visible_dst = method_call_sret
                                              ? v_method_call_retbuf
                                              : dst;

        // -----------------------------------------------------------------
        // Devirtualizacion compile-time: si el tipo concreto del receptor
        // es estaticamente conocido (via @c ssa_concrete_class_), reescribir
        // el dispatch a CALLVIRT directo usando el vtable_idx del metodo en
        // la clase concreta.  Tanto port C como JIT consumen este CALLVIRT
        // como direct call, sin coste runtime de findmethod/callm.
        //
        // Aplica cuando el receptor es una variable interface tipada pero
        // su SSA value viene directamente de un @c new ConcreteClass()
        // (caso comun: @c IServicio s = new ImplA();).
        // -----------------------------------------------------------------
        if (lay.is_interface) {
            auto it_conc = ssa_concrete_class_.find(obj);
            if (it_conc != ssa_concrete_class_.end()) {
                const std::string &concrete_name = it_conc->second;
                auto it_lay = tc_.class_layouts().find(concrete_name);
                if (it_lay != tc_.class_layouts().end()) {
                    const auto &conc_lay = it_lay->second;
                    // Buscar metodo por nombre en la clase concreta.
                    for (const auto &cm: conc_lay.methods) {
                        if (cm.name == mtd->name && !cm.is_constructor) {
                            // CALLVIRT directo con el vtable_idx de la
                            // clase concreta -> backend devirtaliza.
                            ir::IrInstr cv{};
                            cv.op   = ir::IrOp::CALLVIRT;
                            cv.type = ret_ir;
                            cv.dst  = dst;
                            cv.imm  = static_cast<uint64_t>(cm.vtable_index);
                            cv.operands.push_back(obj);
                            // SRET: retbuf tras obj, antes de args.
                            if (method_call_sret)
                                cv.operands.push_back(v_method_call_retbuf);
                            for (auto av: arg_vals) cv.operands.push_back(av);
                            cv.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(cv));
                            // Propagar tipo concreto del retorno si es
                            // tambien tipo class conocido.
                            return visible_dst;
                        }
                    }
                }
            }
        }
        if (lay.is_interface) {
            // Marcar obj como host_ptr (instancia GC-derivada).
            fn_->values[obj].is_host_ptr = true;

            // Dispatch de interfaz via ITABLE (en vez de findmethod+callm):
            // construimos un @c ItfCallParams (32 bytes) en stack con el
            // nombre de la interfaz, el nombre del metodo, el indice del metodo
            // (posicion en la declaracion de la interfaz = vtable_index) y el
            // numero de metodos de la interfaz, y emitimos un solo CALLITF.
            //   +0  iface_name_addr (8)
            //   +8  iface_name_len (lo32) | method_index (hi32)
            //   +16 method_name_addr (8)
            //   +24 method_name_len (lo32) | count (hi32)
            // El interp despacha via la itable lazy de la clase concreta
            // (indice O(1) tras el warmup); el JIT inlinea el scan de itables.
            const std::string &iface_name = bt.struct_name; // == lay.name
            const std::string &method_name = mtd->name;
            const uint64_t iface_idx  = intern_class_name(*out_mod_, iface_name);
            const uint32_t iface_len  = static_cast<uint32_t>(iface_name.size());
            const uint64_t method_idx_str = intern_class_name(*out_mod_, method_name);
            const uint32_t method_len = static_cast<uint32_t>(method_name.size());
            const uint32_t method_index = mtd->vtable_index;
            const uint32_t mcount       = static_cast<uint32_t>(lay.methods.size());

            // Buffer de ItfCallParams (32 bytes) construido UNA VEZ en el entry
            // block (block 0) de la funcion -- su contenido es 100%
            // loop-invariante (nombres + indices constantes).  El @c callitf en
            // el loop solo LEE el buffer.  Critico para correctness:
            //   (1) un ALLOCA por dispatch creceria el VM stack en loops
            //       calientes (pic_real 3M iter desbordaba el stack);
            //   (2) construir el struct DENTRO del loop hacia que el LICM/
            //       regalloc hoisteara un CONST a un reg de arg (r1) que el
            //       marshalling del call clobbeaba -> v_buf corrupto en iter 2+.
            // Construir en el entry (dominador de todo) evita ambos.  Cada call
            // site tiene su propio buffer (params distintos); como el call site
            // se baja UNA vez, son pocos buffers (1 por dispatch textual).
            //
            // Insertamos las instrucciones de construccion en block 0 ANTES de
            // su terminador (o al final si aun no esta terminado).
            std::vector<ir::IrInstr> setup;
            const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA; al.type = ir::IrType::I8; al.imm = 32;
                al.dst = v_buf; al.source_line = e->loc.line;
                setup.push_back(std::move(al));
            }
            // Helper local: STORE i64 @c val a @c v_buf + @c off (instrs -> setup).
            auto setup_store_at = [&](uint64_t off, ir::IrValueId val) {
                ir::IrValueId base = v_buf;
                if (off != 0) {
                    const ir::IrValueId v_off = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr c{};
                    c.op = ir::IrOp::CONST; c.type = ir::IrType::I64; c.imm = off;
                    c.dst = v_off; c.source_line = e->loc.line;
                    setup.push_back(std::move(c));
                    base = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr add{};
                    add.op = ir::IrOp::ADD; add.type = ir::IrType::I64; add.dst = base;
                    add.operands = {v_buf, v_off}; add.source_line = e->loc.line;
                    setup.push_back(std::move(add));
                }
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE; st.type = ir::IrType::I64;
                st.dst = ir::IR_NO_VALUE; st.operands = {val, base};
                st.source_line = e->loc.line;
                setup.push_back(std::move(st));
            };
            auto setup_const = [&](uint64_t k) -> ir::IrValueId {
                const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
                ir::IrInstr c{};
                c.op = ir::IrOp::CONST; c.type = ir::IrType::I64; c.imm = k;
                c.dst = v; c.source_line = e->loc.line;
                setup.push_back(std::move(c));
                return v;
            };
            auto setup_str_lit = [&](uint64_t idx) -> ir::IrValueId {
                const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr ns{};
                ns.op = ir::IrOp::STR_LIT_ADDR; ns.type = ir::IrType::PTR;
                ns.dst = v; ns.imm = idx; ns.source_line = e->loc.line;
                setup.push_back(std::move(ns));
                return v;
            };
            // [+0] iface_name_addr ; [+8] iface_len|method_index<<32 ;
            // [+16] method_name_addr ; [+24] method_len|count<<32.
            setup_store_at(0, setup_str_lit(iface_idx));
            setup_store_at(8, setup_const(static_cast<uint64_t>(iface_len)
                               | (static_cast<uint64_t>(method_index) << 32)));
            setup_store_at(16, setup_str_lit(method_idx_str));
            setup_store_at(24, setup_const(static_cast<uint64_t>(method_len)
                               | (static_cast<uint64_t>(mcount) << 32)));
            // ----------------------------------------------------------------
            // (C2): preparar devirt especulativa estatica del CALLITF.
            // Si la interfaz tiene pocos (<=K) implementors concretos NO-aspecto
            // con el metodo inlineable, resolvemos el ClassInfo* de cada uno
            // (findclass en el entry, loop-invariante) y registramos los
            // candidatos; el pase ir_pass_spec_devirt reescribe el CALLITF en
            // un guard-chain por tipo + fallback al dispatch via itable.
            //  - Solo metodos que devuelven valor (dst != IR_NO_VALUE) y
            //    no-SRET (v1).
            //  - Conservador con AOP: si el modulo declara cualquier @Aspect,
            //    NO especulamos (un advice podria targetear el metodo y el CALL
            //    directo del fast path lo saltaria); el dispatch via itable
            //    recorre la advice_chain correctamente.
            // Resolucion v1 (2a): findclass directo en el entry, 1x/invocacion
            // (despreciable para dispatch-en-loop).  2b lo cambiara a slot-cache
            // eager en __module_init (1x total).
            std::vector<ir::DevirtCandidate> spec_cands;
            if (dst != ir::IR_NO_VALUE && !method_call_sret) {
                bool module_has_aspect = false;
                for (const auto &kv : tc_.class_layouts()) {
                    if (kv.second.is_aspect) { module_has_aspect = true; break; }
                }
                if (!module_has_aspect) {
                    constexpr size_t K_MAX = 4;
                    // (cls_name, callee_ir_name) por implementor concreto.
                    std::vector<std::pair<std::string, std::string>> impls;
                    bool too_many = false;
                    for (const auto &kv : tc_.class_layouts()) {
                        const auto &cl = kv.second;
                        if (cl.is_interface || cl.is_aspect) continue;
                        bool implements = false;
                        for (const auto &in : cl.interface_names)
                            if (in == iface_name) { implements = true; break; }
                        if (!implements) continue;
                        // Localizar el metodo de la interfaz en la clase.
                        const std::string *owner = nullptr;
                        for (const auto &mm : cl.methods) {
                            if (mm.name == method_name && !mm.is_constructor) {
                                owner = mm.defining_class.empty()
                                            ? &cl.name : &mm.defining_class;
                                break;
                            }
                        }
                        if (!owner) continue;  // no deberia pasar si implements
                        impls.emplace_back(cl.name, *owner + "__" + method_name);
                        if (impls.size() > K_MAX) { too_many = true; break; }
                    }
                    if (!too_many && !impls.empty() && impls.size() <= K_MAX) {
                        const int ln = e->loc.line;
                        // Resolver cada ClassInfo* via findclass construido en el
                        // vector `setup` (que se splice en block 0, el entry).
                        auto setup_findclass = [&](const std::string &cls_name)
                                                   -> ir::IrValueId {
                            const uint64_t nidx = intern_class_name(*out_mod_, cls_name);
                            const uint32_t nlen = static_cast<uint32_t>(cls_name.size());
                            const ir::IrValueId vp = fn_->new_value(ir::IrType::PTR);
                            { ir::IrInstr al{}; al.op = ir::IrOp::ALLOCA;
                              al.type = ir::IrType::I8; al.dst = vp; al.imm = 16;
                              al.source_line = ln; setup.push_back(std::move(al)); }
                            const ir::IrValueId vna = fn_->new_value(ir::IrType::PTR);
                            { ir::IrInstr la{}; la.op = ir::IrOp::LABEL_ADDR;
                              la.type = ir::IrType::PTR; la.dst = vna;
                              la.func_name = "s_" + std::to_string(nidx);
                              la.source_line = ln; setup.push_back(std::move(la)); }
                            { ir::IrInstr st{}; st.op = ir::IrOp::STORE;
                              st.type = ir::IrType::I64; st.operands = {vna, vp};
                              st.source_line = ln; setup.push_back(std::move(st)); }
                            const ir::IrValueId vlen = setup_const(static_cast<uint64_t>(nlen));
                            const ir::IrValueId voff = setup_const(8);
                            const ir::IrValueId vp8  = fn_->new_value(ir::IrType::PTR);
                            { ir::IrInstr add{}; add.op = ir::IrOp::ADD;
                              add.type = ir::IrType::I64; add.dst = vp8;
                              add.operands = {vp, voff}; add.source_line = ln;
                              setup.push_back(std::move(add)); }
                            { ir::IrInstr st{}; st.op = ir::IrOp::STORE;
                              st.type = ir::IrType::I64; st.operands = {vlen, vp8};
                              st.source_line = ln; setup.push_back(std::move(st)); }
                            const ir::IrValueId vc = fn_->new_value(ir::IrType::PTR);
                            fn_->values[vc].is_host_ptr = true;
                            { ir::IrInstr fc{}; fc.op = ir::IrOp::FINDCLASS;
                              fc.type = ir::IrType::PTR; fc.dst = vc; fc.operands = {vp};
                              fc.set_is_call_site(true); fc.source_line = ln;
                              setup.push_back(std::move(fc)); }
                            return vc;
                        };
                        for (const auto &pr : impls) {
                            const ir::IrValueId v_cls = setup_findclass(pr.first);
                            spec_cands.push_back(ir::DevirtCandidate{ v_cls, pr.second });
                        }
                    }
                }
            }

            // Splice de las instrucciones de construccion en block 0 antes de
            // su terminador (op de control de flujo final).  Si block 0 no esta
            // terminado (dispatch en el propio entry), se anexan al final.
            {
                auto &e0 = fn_->blocks[0].instrs;
                size_t pos = e0.size();
                if (pos > 0) {
                    const ir::IrOp last = e0.back().op;
                    if (last == ir::IrOp::BR || last == ir::IrOp::BR_COND
                        || last == ir::IrOp::RET || last == ir::IrOp::UNREACHABLE
                        || last == ir::IrOp::TAILCALL || last == ir::IrOp::RETHROW
                        || last == ir::IrOp::THROW) {
                        pos = e0.size() - 1;
                    }
                }
                e0.insert(e0.begin() + pos,
                          std::make_move_iterator(setup.begin()),
                          std::make_move_iterator(setup.end()));
            }

            // CALLITF: operands[0]=obj, [1]=params_ptr, [2..]=args (retbuf SRET
            // como [2] si aplica).  func_name = "iface\x1fmethod", imm packed.
            ir::IrInstr ci{};
            ci.op   = ir::IrOp::CALLITF;
            ci.type = ret_ir;
            ci.dst  = dst;
            ci.func_name = iface_name;
            ci.func_name.push_back('\x1f');
            ci.func_name += method_name;
            ci.imm  = (static_cast<uint64_t>(mcount) << 32)
                    | static_cast<uint64_t>(method_index);
            ci.operands.push_back(obj);
            ci.operands.push_back(v_buf);
            if (method_call_sret)
                ci.operands.push_back(v_method_call_retbuf);
            for (auto av: arg_vals) ci.operands.push_back(av);
            ci.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ci));
            // registrar el site especulativo (keyed por el dst del
            // CALLITF).  El pase ir_pass_spec_devirt lo consume @O2.
            if (!spec_cands.empty())
                fn_->spec_devirt_sites[dst] = std::move(spec_cands);
            return visible_dst;
        }

        // Path por defecto: dispatch via vtable_idx (clase concreta).
        ir::IrInstr ins{};
        ins.op   = ir::IrOp::CALLVIRT;
        ins.type = ret_ir;
        ins.dst  = dst;
        // operands[0] = obj, operands[1..] = args declarados
        ins.operands.push_back(obj);
        // SRET: retbuf tras obj, antes de args declarados.
        if (method_call_sret)
            ins.operands.push_back(v_method_call_retbuf);
        for (auto av: arg_vals) ins.operands.push_back(av);
        ins.imm         = static_cast<uint64_t>(mtd->vtable_index);
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        // Sprint edge-bugs (2026-06-03): marcar dst con flags GC.  Critico
        // para que el regalloc trate el value como host_ptr GC-managed
        // (save/restore convierte a GcHandle).  Esto es la correccion
        // arquitectural; queda un bug latente del INTERP en CALLVIRT
        // chained (p2 = factory(); p3 = p2.factory(); p3.field) donde el
        // host_ptr retornado puede ser stale -- el bug NO afecta JIT.
        // Documentado en limitaciones; los tests del Lombok @With usan
        // verificacion sin encadenar para evitar el bug latente.
        if (dst != ir::IR_NO_VALUE
            && mtd->return_type.kind == PrimitiveKind::CLASS) {
            fn_->values[dst].is_host_ptr  = true;
            fn_->values[dst].is_gc_object = true;
        }
        return visible_dst;
    }

    // ---------------------------------------------------------------------
    // Helpers de constantes y casts.
    // ---------------------------------------------------------------------

} // namespace vex
