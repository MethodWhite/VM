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



namespace {

    inline uint64_t ir_type_size(::ir::IrType t) noexcept {

        switch (t) {

            case ::ir::IrType::I8:

            case ::ir::IrType::U8:

            case ::ir::IrType::BOOL: return 1;

            case ::ir::IrType::I16:

            case ::ir::IrType::U16: return 2;

            case ::ir::IrType::I32:

            case ::ir::IrType::U32:

            case ::ir::IrType::F32:

            case ::ir::IrType::HANDLE: return 4;

            case ::ir::IrType::I64:

            case ::ir::IrType::U64:

            case ::ir::IrType::F64:

            case ::ir::IrType::PTR: return 8;

            default: return 8;

        }

    }

}

    void Lowering::lower_synchronized(ast::SynchronizedStmt *s) {
        if (!s || !s->target) {
            error_at(s ? s->loc : SourceLoc{}, "lowering: synchronized sin target");
            return;
        }
        if (!s->body) {
            error_at(s->loc, "lowering: synchronized sin body");
            return;
        }

        // 1. Bajar el target a un valor SSA (host pointer al ObjectHeader).
        const ir::IrValueId v_obj = lower_expr(s->target.get());
        if (v_obj == ir::IR_NO_VALUE) return;

        // Crear bloques: body + handler (excepcion) + merge (continuacion).
        const ir::IrBlockId body_bb    = fn_->new_block("sync_body");
        const ir::IrBlockId handler_bb = fn_->new_block("sync_handler");
        const ir::IrBlockId merge_bb   = fn_->new_block("sync_merge");

        // 2. ptr -> handle + monenter en un RAW_ASM con {dst}={src0} (SSA-aware).
        // El regalloc asigna v_handle a un registro libre y monenter usa
        // ese mismo registro.  Crucial: no hardcodear r1/r2 aqui porque
        // colisionan con valores SSA vivos (el regalloc no inspecciona el
        // texto del RAW_ASM y no sabe que clobreamos).
        // Sprint 6.C: ptr -> handle via GC_HANDLE_FOR_PTR IR op + MONENTER IR op.
        const ir::IrValueId v_handle = emit_gc_handle_for_ptr(v_obj, s->loc.line);
        {
            ir::IrInstr me{};
            me.op          = ir::IrOp::MONENTER;
            me.type        = ir::IrType::VOID;
            me.dst         = ir::IR_NO_VALUE;
            me.operands    = {v_handle};
            me.source_line = s->loc.line;
            fn_->append(current_block_, std::move(me));
        }

        // tryenter catch-all: setup handler_pc y type=NULL via SSA values.
        const std::string   handler_label = fn_->name + "_" + fn_->blocks[handler_bb].name;
        const ir::IrValueId v_handler_pc  = emit_label_addr(handler_label, s->loc.line);
        const ir::IrValueId v_type_null   = emit_const(ir::IrType::I64, 0, s->loc.line); {
            ir::IrInstr ra{};
            ra.op          = ir::IrOp::TRYENTER;
            ra.type        = ir::IrType::VOID;
            ra.dst         = ir::IR_NO_VALUE;
            ra.operands    = {v_handler_pc, v_type_null};
            ra.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ra));
        }

        // br body_bb.  Edges fantasmas a handler_bb (alcanzable via excepcion).
        {
            ir::IrInstr br{};
            br.op           = ir::IrOp::BR;
            br.target_block = body_bb;
            br.source_line  = s->loc.line;
            fn_->append(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(body_bb);
            fn_->blocks[current_block_].succs.push_back(handler_bb);
            fn_->blocks[body_bb].preds.push_back(current_block_);
            fn_->blocks[handler_bb].preds.push_back(current_block_);
        }

        // 3. Push cleanup: tryleave + monexit en early-return.
        // Sprint 6.C: Kind::SYNC_EXIT emite TRYLEAVE + MONEXIT como IR ops.
        {
            CleanupAction act;
            act.kind        = CleanupAction::Kind::SYNC_EXIT;
            act.operands    = {v_handle};
            act.source_line = s->loc.line;
            cleanup_stack_.push_back(std::move(act));
        }

        // 4. Bajar el body.
        current_block_    = body_bb;
        block_terminated_ = false;
        lower_block(s->body.get());

        // 5. Pop cleanup (el body ya no necesita protegerse via emit_cleanups_all).
        cleanup_stack_.pop_back();

        // 6. Salida normal del body: TRYLEAVE + MONEXIT IR ops + br merge.
        if (!block_terminated_) {
            // Sprint 6.C: TRYLEAVE y MONEXIT son IR ops puros.
            {
                ir::IrInstr tl{};
                tl.op          = ir::IrOp::TRYLEAVE;
                tl.type        = ir::IrType::VOID;
                tl.dst         = ir::IR_NO_VALUE;
                tl.source_line = s->loc.line;
                fn_->append(current_block_, std::move(tl));
            }
            {
                ir::IrInstr me{};
                me.op          = ir::IrOp::MONEXIT;
                me.type        = ir::IrType::VOID;
                me.dst         = ir::IR_NO_VALUE;
                me.operands    = {v_handle};
                me.source_line = s->loc.line;
                fn_->append(current_block_, std::move(me));
            }

            ir::IrInstr brm{};
            brm.op           = ir::IrOp::BR;
            brm.target_block = merge_bb;
            brm.source_line  = s->loc.line;
            fn_->append(current_block_, std::move(brm));
            fn_->blocks[current_block_].succs.push_back(merge_bb);
            fn_->blocks[merge_bb].preds.push_back(current_block_);
            block_terminated_ = true;
        }

        // 7. Handler: alcanzable solo via excepcion del body.  do_throw nos
        // dejo el objeto excepcion en r0.  Hacemos monexit + rethrow para
        // que el caller decida que hacer con la excepcion.  Notese que NO
        // necesitamos tryleave aqui: do_throw ya consumio el frame al saltar.
        current_block_    = handler_bb;
        block_terminated_ = false; {
            // Sprint 6.C: handler = MONEXIT IR op + rethrow (RAW_ASM minimal).
            {
                ir::IrInstr me{};
                me.op          = ir::IrOp::MONEXIT;
                me.type        = ir::IrType::VOID;
                me.dst         = ir::IR_NO_VALUE;
                me.operands    = {v_handle};
                me.source_line = s->loc.line;
                fn_->append(current_block_, std::move(me));
            }
            // raw_asm-elim wave 3: rethrow IR op dedicado.  Terminator del
            // bloque (re-lanza current_exception; nada sigue a esta instr).
            ir::IrInstr rt{};
            rt.op          = ir::IrOp::RETHROW;
            rt.type        = ir::IrType::VOID;
            rt.dst         = ir::IR_NO_VALUE;
            rt.source_line = s->loc.line;
            fn_->append(current_block_, std::move(rt));
        }
        block_terminated_ = true; // rethrow es terminador del bloque

        // Continuar en merge (alcanzable solo via salida normal).
        current_block_    = merge_bb;
        block_terminated_ = false;
    }

    // ---------------------------------------------------------------------
    // spawn { body } -- arranca proceso hijo en scheduler actual.
    //
    // Estrategia:
    //   1. generate_spawn_helper compila el body como funcion sintetica
    //      __spawn_<N> con return type VOID + body original + hlt al final
    //      (NO ret: un proceso hijo no retorna a un caller que no existe).
    //   2. lower_spawn_expr emite RAW_ASM con:
    //        mov {dst_pc_holder}, @Absolute("code.__spawn_<N>")
    //        spawn {dst_pc_holder}
    //        mov {dst}, r0     ; r0 contiene el PID encoded del hijo
    //   3. SSA value devuelto = PID encoded como i64.
    // ---------------------------------------------------------------------

    // BugFix R3: scan recursivo del body para detectar IdentExprs que
    // referencian nombres NO definidos dentro del propio body.  Esos son
    // candidatos a captures que el spawn helper recibira como params.
    // Devuelve la lista de nombres unicos (orden de aparicion) que estan
    // en el scope encerrante actual via @c lookup.
    static void collect_spawn_captures_in_expr(
        const ast::Expr *e,
        std::unordered_set<std::string> &locals_defined,
        std::vector<std::string> &out_captures,
        std::unordered_set<std::string> &seen,
        Lowering &lw);
    static void collect_spawn_captures_in_stmt(
        const ast::Stmt *s,
        std::unordered_set<std::string> &locals_defined,
        std::vector<std::string> &out_captures,
        std::unordered_set<std::string> &seen,
        Lowering &lw);

    static void collect_spawn_captures_in_expr(
        const ast::Expr *e,
        std::unordered_set<std::string> &locals_defined,
        std::vector<std::string> &out_captures,
        std::unordered_set<std::string> &seen,
        Lowering &lw)
    {
        if (!e) return;
        switch (e->kind) {
            case ast::NodeKind::IdentExpr: {
                const auto *id = static_cast<const ast::IdentExpr *>(e);
                if (locals_defined.count(id->name)) return;
                if (seen.count(id->name)) return;
                // Buscar el nombre en TODOS los scopes activos del lowering.
                // Accedemos a scopes_ via friend-style desde el contexto
                // estatico (Lowering nos da acceso indirecto via lookup
                // publico mediante un wrapper inline).
                if (lw.spawn_capture_resolve_public(id->name) != ir::IR_NO_VALUE) {
                    seen.insert(id->name);
                    out_captures.push_back(id->name);
                }
                return;
            }
            case ast::NodeKind::FieldAccessExpr: {
                const auto *fa = static_cast<const ast::FieldAccessExpr *>(e);
                collect_spawn_captures_in_expr(fa->base.get(), locals_defined,
                                                out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::BinaryExpr: {
                const auto *b = static_cast<const ast::BinaryExpr *>(e);
                collect_spawn_captures_in_expr(b->lhs.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(b->rhs.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::UnaryExpr: {
                const auto *u = static_cast<const ast::UnaryExpr *>(e);
                collect_spawn_captures_in_expr(u->operand.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::AssignExpr: {
                const auto *a = static_cast<const ast::AssignExpr *>(e);
                collect_spawn_captures_in_expr(a->target.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(a->value.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::CallExpr: {
                const auto *c = static_cast<const ast::CallExpr *>(e);
                collect_spawn_captures_in_expr(c->callee.get(), locals_defined, out_captures, seen, lw);
                for (const auto &a : c->args)
                    collect_spawn_captures_in_expr(a.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::IndexExpr: {
                const auto *ix = static_cast<const ast::IndexExpr *>(e);
                collect_spawn_captures_in_expr(ix->base.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(ix->index.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::NewExpr: {
                const auto *n = static_cast<const ast::NewExpr *>(e);
                for (const auto &a : n->args)
                    collect_spawn_captures_in_expr(a.get(), locals_defined, out_captures, seen, lw);
                if (n->array_size)
                    collect_spawn_captures_in_expr(n->array_size.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::CastExpr: {
                const auto *c = static_cast<const ast::CastExpr *>(e);
                collect_spawn_captures_in_expr(c->operand.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::TernaryExpr: {
                const auto *t = static_cast<const ast::TernaryExpr *>(e);
                collect_spawn_captures_in_expr(t->cond.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(t->then_expr.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(t->else_expr.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            default: return;
        }
    }

    static void collect_spawn_captures_in_stmt(
        const ast::Stmt *s,
        std::unordered_set<std::string> &locals_defined,
        std::vector<std::string> &out_captures,
        std::unordered_set<std::string> &seen,
        Lowering &lw)
    {
        if (!s) return;
        switch (s->kind) {
            case ast::NodeKind::BlockStmt: {
                const auto *b = static_cast<const ast::BlockStmt *>(s);
                for (const auto &st : b->body)
                    collect_spawn_captures_in_stmt(st.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::VarDeclStmt: {
                const auto *v = static_cast<const ast::VarDeclStmt *>(s);
                if (v->init)
                    collect_spawn_captures_in_expr(v->init.get(), locals_defined, out_captures, seen, lw);
                locals_defined.insert(v->name);
                return;
            }
            case ast::NodeKind::ExprStmt: {
                const auto *e = static_cast<const ast::ExprStmt *>(s);
                collect_spawn_captures_in_expr(e->expr.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::IfStmt: {
                const auto *i = static_cast<const ast::IfStmt *>(s);
                collect_spawn_captures_in_expr(i->cond.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(i->then_branch.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(i->else_branch.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::WhileStmt: {
                const auto *w = static_cast<const ast::WhileStmt *>(s);
                collect_spawn_captures_in_expr(w->cond.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(w->body.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::ForStmt: {
                const auto *f = static_cast<const ast::ForStmt *>(s);
                collect_spawn_captures_in_stmt(f->init.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(f->cond.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(f->step.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(f->body.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::SynchronizedStmt: {
                const auto *sy = static_cast<const ast::SynchronizedStmt *>(s);
                collect_spawn_captures_in_expr(sy->target.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(sy->body.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::ReturnStmt: {
                const auto *r = static_cast<const ast::ReturnStmt *>(s);
                if (r->value)
                    collect_spawn_captures_in_expr(r->value.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::ThrowStmt: {
                const auto *t = static_cast<const ast::ThrowStmt *>(s);
                if (t->value)
                    collect_spawn_captures_in_expr(t->value.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::TryStmt: {
                // Bug fix audit Z.2: scan recursivo del try body + catches + finally.
                // Sin esto, capturas usadas dentro de try{} se reportan como
                // "nombre no resuelto" en el spawn body.
                const auto *t = static_cast<const ast::TryStmt *>(s);
                if (t->body) collect_spawn_captures_in_stmt(
                    static_cast<const ast::Stmt *>(t->body.get()),
                    locals_defined, out_captures, seen, lw);
                for (const auto &cc : t->catches) {
                    if (cc.body) collect_spawn_captures_in_stmt(
                        static_cast<const ast::Stmt *>(cc.body.get()),
                        locals_defined, out_captures, seen, lw);
                }
                if (t->finally_body) collect_spawn_captures_in_stmt(
                    static_cast<const ast::Stmt *>(t->finally_body.get()),
                    locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::DoWhileStmt: {
                const auto *dw = static_cast<const ast::DoWhileStmt *>(s);
                collect_spawn_captures_in_stmt(dw->body.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(dw->cond.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::ForEachStmt: {
                const auto *fe = static_cast<const ast::ForEachStmt *>(s);
                collect_spawn_captures_in_expr(fe->iter_expr.get(), locals_defined, out_captures, seen, lw);
                locals_defined.insert(fe->iter_name);
                collect_spawn_captures_in_stmt(fe->body.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            default: return;
        }
    }

    std::string Lowering::generate_spawn_helper(ast::BlockStmt *body, const SourceLoc &loc) {
        const size_t      spawn_idx = spawn_func_counter_++;
        const std::string fn_name   = "__spawn_" + std::to_string(spawn_idx);

        // BugFix R3: pre-scan del body para detectar capturas (idents
        // libres que estan en el scope encerrante).  Cada captura se
        // pasara como param al helper via la convencion @c spawnargs.
        std::vector<std::string> captures;
        {
            std::unordered_set<std::string> locals_defined;
            std::unordered_set<std::string> seen;
            collect_spawn_captures_in_stmt(body, locals_defined, captures, seen, *this);
            // Limitamos a 11 captures (R1..R12 menos el implicit return reg).
            if (captures.size() > 11) {
                error_at(loc,
                    "spawn { body }: maximo 11 capturas soportadas (recibidos "
                    + std::to_string(captures.size()) + ")");
                captures.resize(11);
            }
        }
        // Capturar los SSA values de las capturas en el caller ANTES de
        // cambiar de contexto.  Esos seran los args de spawnargs.
        spawn_captured_ssa_values_.clear();
        spawn_captured_names_ = captures;
        for (const auto &nm : captures) {
            ir::IrValueId v = lookup(nm);
            spawn_captured_ssa_values_.push_back(v);
        }

        // Phase Z.9: escape analysis cross-process.  Para cada captura que
        // es un objeto GC (is_gc_object=true en su SSA value) Y NO fue
        // declarada con @c shared, emitir warning con sugerencia clara.
        // Esto detecta el bug clasico (t13): pasar un objeto local al
        // spawn body, que no puede resolverlo cross-process.
        for (size_t i = 0; i < captures.size(); ++i) {
            const ir::IrValueId v = spawn_captured_ssa_values_[i];
            if (v == ir::IR_NO_VALUE) continue;
            if (v >= fn_->values.size()) continue;
            const auto &val = fn_->values[v];
            // Solo warn para GC objects (host_ptrs a objetos GC).
            if (!val.is_gc_object) continue;
            // Si el local fue declarado con @c shared, todo OK.
            if (shared_locals_.count(captures[i])) continue;
            // Sugerencia con loc del spawn (no del var-decl original; el
            // user vera el spawn body que es donde el problema importa).
            diags_.warning(loc,
                "spawn captura '" + captures[i] +
                "' (objeto GC local-only); el child no podra resolver el "
                "host_ptr cross-process.\n"
                "  sugerencia: declara con 'shared': `shared T " +
                captures[i] + " = new T();`  o llama `" +
                captures[i] + " = share(" + captures[i] + ")` antes del spawn.");
        }

        // Guardar el contexto del lowering (estamos dentro de la funcion
        // padre que invoca spawn) y crear uno nuevo para la funcion hijo.
        ir::IrFunction *                                             saved_fn         = fn_;
        ir::IrBlockId                                                saved_block      = current_block_;
        bool                                                         saved_terminated = block_terminated_;
        std::vector<std::unordered_map<std::string, ir::IrValueId> > saved_scopes
                = std::move(scopes_);
        std::unordered_set<std::string> saved_addr_taken
                = std::move(address_taken_locals_);
        std::vector<CleanupAction> saved_cleanups
                = std::move(cleanup_stack_);

        // Construir la nueva IrFunction.
        ir::IrFunction child_fn;
        child_fn.name             = fn_name;
        child_fn.ret_type         = ir::IrType::VOID;
        const ir::IrBlockId entry = child_fn.new_block("entry");

        fn_               = &child_fn;
        current_block_    = entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();

        // BugFix R3: registrar las capturas como params del helper.  Cada
        // captura llega via la calling convention spawnargs (R1, R2, ...).
        for (size_t i = 0; i < captures.size(); ++i) {
            ir::IrValueId v = child_fn.new_value(ir::IrType::I64,
                                                  "%" + captures[i]);
            child_fn.values[v].is_param = true;
            // Si el SSA value original era host_ptr CLASS, propagarlo.
            if (spawn_captured_ssa_values_[i] != ir::IR_NO_VALUE
             && spawn_captured_ssa_values_[i] < saved_fn->values.size()) {
                const auto &src_val = saved_fn->values[spawn_captured_ssa_values_[i]];
                if (src_val.is_host_ptr)  child_fn.values[v].is_host_ptr = true;
                if (src_val.is_gc_object) child_fn.values[v].is_gc_object = true;
            }
            child_fn.params.push_back(v);
            // Bindear el nombre en el topmost scope para que IdentExpr
            // resuelva via @c lookup.
            scopes_.back()[captures[i]] = v;
        }

        // Setup de pila: ahora lo hace exec_instr_spawn directamente al
        // crear el proceso hijo (rsp/rbp = base unica por local_pid).
        // El frontend solo necesita asegurarse de que las instrucciones
        // de `enter`/`leave` y las locales caben en la region (1 MiB
        // por defecto, mas que suficiente para spawn bodies tipicos).

        if (body) lower_block(body);

        // Sprint 6.D: terminador HLT via IR op puro (no RAW_ASM).
        if (!block_terminated_) {
            ir::IrInstr h{};
            h.op          = ir::IrOp::HLT;
            h.type        = ir::IrType::VOID;
            h.dst         = ir::IR_NO_VALUE;
            h.source_line = loc.line;
            fn_->append(current_block_, std::move(h));
            block_terminated_ = true;
        }

        pop_scope();
        // Guardar el helper en la cola pendiente; se anyadira a out_mod_
        // al final de run() para que main se mantenga como primera funcion
        // (el emisor IR la trata como entry point y termina con hlt).
        pending_spawn_helpers_.push_back(std::move(child_fn));

        // Restaurar el contexto del padre.
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        return fn_name;
    }

    // ---------------------------------------------------------------------
    // rspawn helper generator.  Identico a generate_spawn_helper
    // pero con `is_rspawn_body_ = true` activado mientras se baja el body
    // para que cualquier `return X` se intercepte en `lower_return` y se
    // transforme en `mov r0, X + hlt`.  El runtime distribuido captura R0
    // al detectar HALT en un proceso con `rspawn_future_id != 0` y envia
    // VDP_FUTURE_FULFILL al nodo origen con ese valor.
    // ---------------------------------------------------------------------
    std::string Lowering::generate_rspawn_helper(ast::BlockStmt *body, const SourceLoc &loc) {
        const size_t      spawn_idx = spawn_func_counter_++;
        const std::string fn_name   = "__rspawn_" + std::to_string(spawn_idx);

        // Guardar contexto del lowering del padre.
        ir::IrFunction *                                             saved_fn         = fn_;
        ir::IrBlockId                                                saved_block      = current_block_;
        bool                                                         saved_terminated = block_terminated_;
        std::vector<std::unordered_map<std::string, ir::IrValueId> > saved_scopes
                = std::move(scopes_);
        std::unordered_set<std::string> saved_addr_taken
                = std::move(address_taken_locals_);
        std::vector<CleanupAction> saved_cleanups
                = std::move(cleanup_stack_);
        bool saved_rspawn = is_rspawn_body_;

        ir::IrFunction child_fn;
        child_fn.name             = fn_name;
        child_fn.ret_type         = ir::IrType::VOID;
        const ir::IrBlockId entry = child_fn.new_block("entry");

        fn_               = &child_fn;
        current_block_    = entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();
        is_rspawn_body_ = true; // activar interception de return en lower_return

        if (body) lower_block(body);

        // Sprint 6.D: terminador HLT con R0=0 via IR op puro.
        if (!block_terminated_) {
            const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, loc.line);
            ir::IrInstr h{};
            h.op          = ir::IrOp::HLT;
            h.type        = ir::IrType::VOID;
            h.dst         = ir::IR_NO_VALUE;
            h.operands    = {v_zero};
            h.source_line = loc.line;
            fn_->append(current_block_, std::move(h));
            block_terminated_ = true;
        }

        pop_scope();
        pending_spawn_helpers_.push_back(std::move(child_fn));

        // Restaurar contexto.
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        is_rspawn_body_       = saved_rspawn;
        return fn_name;
    }

    ir::IrValueId Lowering::lower_rspawn_expr(ast::RSpawnExpr *e) {
        if (!e || !e->body || !e->node_idx) {
            error_at(e ? e->loc : SourceLoc{}, "lowering: rspawn sin body o sin node_idx");
            return ir::IR_NO_VALUE;
        }
        // 1. Generar la funcion remota y obtener su nombre (label .vel).
        const std::string fn_name = generate_rspawn_helper(e->body.get(), e->loc);

        // 2. Cargar la direccion absoluta del helper en un SSA value.
        const ir::IrValueId v_pc = emit_label_addr(fn_name, e->loc.line);

        // 3. Bajar la expresion del node_idx y promover a I64.
        ir::IrValueId v_node = lower_expr(e->node_idx.get());
        if (v_node == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrType src_t = fn_->values[v_node].type;
        v_node                 = cast_if_needed(v_node, src_t, ir::IrType::I64, e->loc.line);

        // 4. RSPAWN IR op (0xD2): node_idx + fn_addr -> Future handle en R0.
        const ir::IrValueId v_fut = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr rs{};
            rs.op           = ir::IrOp::RSPAWN;
            rs.type         = ir::IrType::I64;
            rs.dst          = v_fut;
            // Convencion del IR emitter de RSPAWN: operands[0]=node_idx,
            // operands[1]=fn_addr.  Ver case IrOp::RSPAWN en ir_emitter.cpp.
            rs.operands     = {v_node, v_pc};
            rs.set_is_call_site(true);
            rs.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(rs));
        }
        return v_fut;
    }

    // ---------------------------------------------------------------------
    // closures: generate_lambda_helper + lower_lambda_expr.
    //
    // Diseno:
    //   - Cada @c LambdaExpr produce una @c IrFunction sintetica
    //     @c __lambda_<N>(p0, p1, ...) cuyos params son los declarados
    //     por la lambda.  Los captures se pasan via R14 (env_ptr) y se
    //     leen en el prologue del helper desde @c [r14 + 8*i] a SSA
    //     values con el nombre de la captura.  Asi el body trata las
    //     capturas como si fueran locales, sin distinguirlas de los
    //     params (el type checker ya las acumulo en e->captures).
    //
    //   - El call site emite:
    //       1. ALLOCA @c 8*N bytes para el env block (si N > 0).
    //       2. STORE de cada captura en @c [env + 8*i].
    //       3. ALLOCA 16 bytes para el "function value":
    //            `[+0 fn_addr][+8 env_addr]`.
    //       4. RAW_ASM @c mov ..., @c \@Absolute("code.__lambda_<N>")
    //          + STORE en @c [fv+0].
    //       5. STORE @c env_addr en @c [fv+8] (o 0 si sin captures).
    //
    //   - Al llamar a la closure (vease @c lower_call cuando callee es
    //     IdentExpr de tipo FUNCTION): se cargan @c fn_addr y @c env_addr
    //     del slot, y se emite IrOp::CALLCLOSURE con func_ptr=fn_addr y
    //     operands=[env_addr, args...].  El emisor IR coloca env en R14,
    //     args en R1..R12 via parallel-move y emite @c callvmr.
    // ---------------------------------------------------------------------

    std::string Lowering::generate_lambda_helper(ast::LambdaExpr *e) {
        const size_t      lam_idx = lambda_counter_++;
        const std::string fn_name = "__lambda_" + std::to_string(lam_idx);

        // Salvar contexto del padre para poder restaurarlo despues.
        ir::IrFunction *                                             saved_fn         = fn_;
        ir::IrBlockId                                                saved_block      = current_block_;
        bool                                                         saved_terminated = block_terminated_;
        std::vector<std::unordered_map<std::string, ir::IrValueId> > saved_scopes
                = std::move(scopes_);
        std::unordered_set<std::string> saved_addr_taken
                = std::move(address_taken_locals_);
        std::vector<CleanupAction> saved_cleanups
                = std::move(cleanup_stack_);
        // el helper sintetico es una FUNCION SEPARADA con su propia
        // firma.  No debe heredar el SRET del padre (que se referia al
        // retbuf del padre).  El helper retorna un valor escalar i32/i64
        // mediante R0, sin SRET.  Save y reset.
        const bool          saved_sret_active   = sret_active_;
        const ir::IrValueId saved_sret_retbuf   = sret_retbuf_;
        const uint64_t      saved_sret_buf_size = sret_buf_size_;
        const bool          saved_returns_fn    = current_fn_returns_function_;
        sret_active_                            = false;
        sret_retbuf_                            = ir::IR_NO_VALUE;
        sret_buf_size_                          = 0;
        // El helper en si mismo no retorna FUNCTION (los tests actuales
        // no anidan factories de closures dentro de lambdas).  Si en el
        // futuro lo necesitamos, detectarlo via e->result_type.pointee.
        current_fn_returns_function_ = false;

        // Construir la nueva IrFunction.
        ir::IrFunction child_fn;
        child_fn.name = fn_name;
        // Determinar return type del helper a partir del tipo deducido en
        // la lambda (e->result_type es Type{FUNCTION, ...}; pointee es el
        // return type).  Por defecto VOID si no hay informacion.
        ir::IrType ret_ir = ir::IrType::VOID;
        if (e->result_type.kind == PrimitiveKind::FUNCTION
            && e->result_type.pointee
            && e->result_type.pointee->kind != PrimitiveKind::VOID) {
            ret_ir = ir_type_from_primitive(e->result_type.pointee->kind);
        }
        child_fn.ret_type = ret_ir;

        // Declarar parametros en la signature del helper IR.  Usamos los
        // mismos nombres y tipos que el AST de la lambda; la convencion
        // del emisor coloca cada param en r1, r2, ...
        // Modelo del IR: @c IrFunction::params es @c vector<IrValueId>;
        // los nombres se mantienen aparte para hacer el bind() en el
        // scope local del lowering tras crear el bloque entry.
        std::vector<std::pair<std::string, ir::IrValueId> > param_bindings;
        param_bindings.reserve(e->params.size());
        for (size_t i = 0; i < e->params.size(); ++i) {
            ir::IrType pt  = ir::IrType::I64;
            const Type sem = tc_.resolve_type_node(e->params[i]->type.get());
            if (sem.kind != PrimitiveKind::COUNT
                && sem.kind != PrimitiveKind::VOID) {
                pt = ir_type_from_primitive(sem.kind);
            }
            ir::IrValueId pv             = child_fn.new_value(pt, "%" + e->params[i]->name);
            child_fn.values[pv].is_param = true;
            child_fn.params.push_back(pv);
            param_bindings.emplace_back(e->params[i]->name, pv);
        }

        const ir::IrBlockId entry = child_fn.new_block("entry");

        fn_               = &child_fn;
        current_block_    = entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();

        // Bind de los params en el scope local.
        for (auto &kv: param_bindings) bind(kv.first, kv.second);

        // Prologue de captures: leer cada @c [r14 + 8*i] a un SSA value
        // que se bindea con el nombre del capture.  Asi el resto del body
        // referencia las capturas como si fueran variables ordinarias.
        //
        // El env_ptr esta en R14 por convencion (callvmr -> R14 ya cargado
        // por el emisor al ejecutar CALLCLOSURE).  Para acceder a R14
        // desde el IR usamos un RAW_ASM `mov {dst}, r14` que captura el
        // valor a un SSA value.
        ir::IrValueId env_ptr = ir::IR_NO_VALUE;
        if (!e->captures.empty()) {
            env_ptr = child_fn.new_value(ir::IrType::PTR);
            // (gap O): si la lambda fue marcada como env_in_heap,
            // el env_ptr en R14 apunta a HEAP RAW (host_ptr), no a
            // stack VM.  Marcamos is_host_ptr para que LOAD/STORE
            // contra el env block emitan @c movh en lugar de @c mov.
            if (e->env_in_heap) {
                child_fn.values[env_ptr].is_host_ptr = true;
            }
            // raw_asm-elim wave 3: prologue del closure helper lee R14 (env_ptr)
            // via IrOp::READ_VM_REG.  Reemplaza el viejo RAW_ASM `mov {dst}, r14`.
            ir::IrInstr rr{};
            rr.op          = ir::IrOp::READ_VM_REG;
            rr.type        = ir::IrType::PTR;
            rr.dst         = env_ptr;
            rr.imm         = 14;   // R14 = env_ptr en la calling convention de callclosure
            rr.source_line = e->loc.line;
            child_fn.append(entry, std::move(rr));

            for (size_t i = 0; i < e->captures.size(); ++i) {
                // Tipo del capture: usar el tipo guardado por el type
                // checker.  Si por algun motivo es COUNT/VOID, defaulteamos
                // a i64 (el ancho de los slots del env).
                ir::IrType cap_ir = ir::IrType::I64;
                if (i < e->capture_types.size()
                    && e->capture_types[i].kind != PrimitiveKind::COUNT
                    && e->capture_types[i].kind != PrimitiveKind::VOID) {
                    cap_ir = ir_type_from_primitive(e->capture_types[i].kind);
                }

                // ¿Es esta captura mutable?  Si lo es, el env contiene un
                // PUNTERO a la celda en el outer scope (capture-by-ref);
                // si no, contiene el VALOR (capture-by-value, copiado).
                bool is_mutable = false;
                for (const auto &nm: e->mutable_captures) {
                    if (nm == e->captures[i]) {
                        is_mutable = true;
                        break;
                    }
                }

                // addr_i = env_ptr + 8*i (0 -> reusamos env_ptr directamente)
                ir::IrValueId addr_i = env_ptr;
                if (i > 0) {
                    addr_i = child_fn.new_value(ir::IrType::PTR);
                    // Propagar is_host_ptr para que el LOAD siguiente sepa
                    // emitir @c movh contra heap raw cuando el env vive en
                    // heap (caso de closures retornadas por una funcion,
                    // donde el env sobrevive al stack del creador via
                    // alocacion en heap GC).
                    if (child_fn.values[env_ptr].is_host_ptr) {
                        child_fn.values[addr_i].is_host_ptr = true;
                    }
                    ir::IrValueId off = emit_const(ir::IrType::I64,
                                                   static_cast<uint64_t>(i * 8),
                                                   e->loc.line);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = addr_i;
                    ad.operands    = {env_ptr, off};
                    ad.source_line = e->loc.line;
                    child_fn.append(entry, std::move(ad));
                }

                // Leer 8 bytes (i64) del slot.  Para mutable_capture esto
                // es el PTR a la celda; para by-value es el valor.
                ir::IrValueId raw_v = child_fn.new_value(
                    is_mutable ? ir::IrType::PTR : ir::IrType::I64);
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = raw_v;
                ld.operands    = {addr_i};
                ld.source_line = e->loc.line;
                child_fn.append(entry, std::move(ld));

                if (is_mutable) {
                    // Capture-by-reference: el SSA value `raw_v` es el PTR
                    // a la celda en el outer scope.  Lo bindeamos como
                    // address_taken_local del helper para que read_local /
                    // write_local emitan LOAD/STORE indirectos.
                    address_taken_locals_.insert(e->captures[i]);
                    bind(e->captures[i], raw_v);
                } else {
                    // Capture-by-value: cast/trunc si el tipo es mas
                    // estrecho que i64.
                    ir::IrValueId final_v = raw_v;
                    if (cap_ir != ir::IrType::I64 && cap_ir != ir::IrType::PTR) {
                        final_v = child_fn.new_value(cap_ir);
                        ir::IrInstr cv{};
                        cv.op          = ir::IrOp::TRUNC;
                        cv.type        = cap_ir;
                        cv.dst         = final_v;
                        cv.operands    = {raw_v};
                        cv.source_line = e->loc.line;
                        child_fn.append(entry, std::move(cv));
                    }
                    bind(e->captures[i], final_v);
                }
            }
        }

        // Lower del body.  Si no hay return explicito, anadimos un RET
        // void al final para garantizar terminador.  Este patron lo usan
        // tambien generate_spawn_helper / generate_rspawn_helper.
        if (e->body) lower_block(e->body.get());
        if (!block_terminated_) {
            ir::IrInstr rt{};
            rt.op          = ir::IrOp::RET;
            rt.type        = ir::IrType::VOID;
            rt.source_line = e->loc.line;
            fn_->append(current_block_, std::move(rt));
            block_terminated_ = true;
        }

        pop_scope();
        // Encolar el helper para volcado al modulo al final de run().
        pending_spawn_helpers_.push_back(std::move(child_fn));

        // Restaurar contexto del padre.
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        // restaurar contexto SRET y returns_function del padre.
        sret_active_                 = saved_sret_active;
        sret_retbuf_                 = saved_sret_retbuf;
        sret_buf_size_               = saved_sret_buf_size;
        current_fn_returns_function_ = saved_returns_fn;
        return fn_name;
    }

    ir::IrValueId Lowering::lower_lambda_expr(ast::LambdaExpr *e) {
        if (!e || !e->body) {
            error_at(e ? e->loc : SourceLoc{}, "lowering: lambda sin body");
            return ir::IR_NO_VALUE;
        }
        // Capturar valores ANTES de generar el helper.  El helper modifica
        // pending_spawn_helpers_ y temporalmente intercambia el contexto
        // (fn_, scopes_), asi que los SSA values de los captures debemos
        // leerlos en el frame del CALLER, antes de cambiar de contexto.
        // El type checker ya pre-computo @c e->captures y @c e->capture_types
        // recorriendo el body y registrando IdentExpr externos.
        const size_t               N = e->captures.size();
        std::vector<ir::IrValueId> capture_vals;
        capture_vals.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            // ¿Es esta captura mutable?  Si lo es, guardamos el PTR a la
            // celda outer (capture-by-reference).  Si no, el VALOR
            // (capture-by-value, snapshot).
            bool is_mutable = false;
            for (const auto &nm: e->mutable_captures) {
                if (nm == e->captures[i]) {
                    is_mutable = true;
                    break;
                }
            }

            ir::IrType cap_ir = ir::IrType::I64;
            if (i < e->capture_types.size()
                && e->capture_types[i].kind != PrimitiveKind::COUNT
                && e->capture_types[i].kind != PrimitiveKind::VOID) {
                cap_ir = ir_type_from_primitive(e->capture_types[i].kind);
            }
            ir::IrValueId v = ir::IR_NO_VALUE;
            if (is_mutable) {
                // Para captures mutables, scan_address_taken ya marco
                // la variable outer como address-taken.  lookup devuelve
                // la direccion del ALLOCA estable; eso es lo que
                // queremos guardar en el env.
                v = lookup(e->captures[i]);
            } else {
                // Capture-by-value: leer el valor actual.  Si es
                // address-taken (por algun otro motivo, e.g. &var),
                // read_local emite el LOAD apropiado.
                v = read_local(e->captures[i], cap_ir, e->loc.line);
            }
            if (v == ir::IR_NO_VALUE) {
                v = emit_const(cap_ir, 0, e->loc.line);
            }
            capture_vals.push_back(v);
        }

        // (gap O): MARCAR el flag env_in_heap ANTES de generar
        // el helper, ya que el helper inspecciona @c e->env_in_heap para
        // marcar @c env_ptr.is_host_ptr=true en su prologue.  La
        // condicion: hay capturas (N>0) Y la funcion contenedora
        // retorna FUNCTION.
        if (N > 0 && current_fn_returns_function_) {
            e->env_in_heap = true;
        }

        // Generar el helper sintetico con su prologue de captures.
        const std::string fn_name = generate_lambda_helper(e);
        e->synthetic_name         = fn_name;

        // marker: emitir MAKE_CLOSURE ANTES de la secuencia explicita
        // de ALLOCA env + STOREs + ALLOCA fv + STORE fn + STORE env.
        // Identifica la construccion completa para que el C2 JIT 
        // pueda hacer escape analysis y eventualmente promover env a stack /
        // eliminar la alocacion si la closure no escapa.  El IR emitter
        // actual lo trata como no-op; las instrucciones siguientes hacen el
        // trabajo real.  Capacidad de capturas marcadas como mutables: 16
        // (caben en bits 1..16 del imm; >= 16 deja el sobrante sin marcar y
        // el C2 cae al path conservativo).
        {
            uint64_t mutable_mask = 0;
            for (size_t i = 0; i < N && i < 16; ++i) {
                for (const auto &nm: e->mutable_captures) {
                    if (nm == e->captures[i]) {
                        mutable_mask |= (1ULL << i);
                        break;
                    }
                }
            }
            ir::IrInstr mc{};
            mc.op          = ir::IrOp::MAKE_CLOSURE;
            mc.type        = ir::IrType::VOID;
            mc.dst         = ir::IR_NO_VALUE;
            mc.operands    = capture_vals;          // N captures como SSA values
            mc.func_name   = fn_name;               // nombre del helper sintetico
            mc.imm         = (e->env_in_heap ? 1ULL : 0ULL)  // bit 0: env_kind
                           | (mutable_mask << 1);            // bits 1..16: mutable mask
            mc.source_line = e->loc.line;
            fn_->append(current_block_, std::move(mc));
        }

        // -------------------------------------------------------------
        // 1. Alocar env block (8*N bytes; un qword por capture sin huecos).
        //
        // Por defecto va en STACK del caller (ALLOCA) - cero overhead GC,
        // suficiente para closures que no escapan al scope donde nacen.
        //
        // (gap O): si la funcion contenedora retorna FUNCTION (es
        // decir: probablemente esta cerrando esta lambda como su valor
        // de retorno), alocamos el env block en HEAP RAW via RAW_ALLOC.
        // El env sobrevive al RET y la closure es invocable por el caller
        // sin use-after-free.  Coste: un leak controlado por env (no hay
        // free automatico todavia; se libera cuando el proceso muere).
        // Se marca @c is_host_ptr en el SSA value para que LOAD/STORE
        // contra el env block emitan @c movh (host mem) en lugar de
        // @c mov (vm mem).
        // -------------------------------------------------------------
        ir::IrValueId env_addr;
        if (N == 0) {
            env_addr = emit_const(ir::IrType::I64, 0, e->loc.line);
        } else if (e->env_in_heap) {
            // Heap GC-tracked via GC_ALLOC.  El bloque entra en HandleTable
            // y el GC ve el payload (mark_reachable lo escanea como qword
            // array): si algun capture es un GcHandle vivo, se mantiene
            // marcado transitivamente; cuando ningun root referencia el
            // env (function value muere), se libera en el proximo major_gc.
            //
            // Marcamos is_host_ptr=true para que los STOREs siguientes (de
            // los captures al env) emitan movh (memoria HOST), ya que el
            // GcHeap usa ArenaManager con VirtualAlloc/mmap (host).
            //
            // Sustituye al RAW_ALLOC anterior (cerrado: gap O / leak en
            // closures que escapan).  Coste vs RAW_ALLOC: 1 slot extra en
            // HandleTable + zero-init del payload (que ya hacia rawalloc).
            env_addr                          = fn_->new_value(ir::IrType::PTR);
            fn_->values[env_addr].is_host_ptr = true;
            const ir::IrValueId v_size        = emit_const(ir::IrType::I64,
                                                           static_cast<uint64_t>(N * 8),
                                                           e->loc.line);
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::GC_ALLOC;
            ins.type        = ir::IrType::PTR;
            ins.dst         = env_addr;
            ins.operands    = {v_size};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
        } else {
            // Stack via ALLOCA (cero overhead, valido si la closure no
            // escapa al scope donde nace).
            env_addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8; // unidad: 1 byte
            al.dst         = env_addr;
            al.imm         = static_cast<uint64_t>(N * 8);
            al.source_line = e->loc.line;
            fn_->append(current_block_, std::move(al));
        }

        // Escribir cada capture en su slot del env (ambos casos: stack y
        // heap).  STORE i64 para todos (uniformidad).  En el caso heap el
        // is_host_ptr propagado hace que el emisor IR use movh.
        if (N > 0) {
            for (size_t i = 0; i < N; ++i) {
                ir::IrValueId addr_i = env_addr;
                if (i > 0) {
                    addr_i = fn_->new_value(ir::IrType::PTR);
                    if (fn_->values[env_addr].is_host_ptr) {
                        fn_->values[addr_i].is_host_ptr = true;
                    }
                    ir::IrValueId off = emit_const(ir::IrType::I64,
                                                   static_cast<uint64_t>(i * 8),
                                                   e->loc.line);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = addr_i;
                    ad.operands    = {env_addr, off};
                    ad.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                // Si el capture es de un tipo mas estrecho que i64,
                // primero promocionamos a i64 para que el slot sea
                // siempre 8 bytes.  Para PTR / I64 / valor de funcion
                // (16 bytes en sentido lexico, pero aqui el SSA value
                // es el puntero al slot, asi que i64 es correcto) la
                // promocion es identidad.
                ir::IrValueId v  = capture_vals[i];
                ir::IrType    vt = fn_->values[v].type;
                if (vt != ir::IrType::I64 && vt != ir::IrType::PTR) {
                    v = cast_if_needed(v, vt, ir::IrType::I64, e->loc.line);
                }

                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.operands    = {v, addr_i};
                st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
            }
        }

        // -------------------------------------------------------------
        // 2. Alocar slot 16 bytes para el function value.
        // -------------------------------------------------------------
        ir::IrValueId fv_addr = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = fv_addr;
            al.imm         = 16;
            al.source_line = e->loc.line;
            fn_->append(current_block_, std::move(al));
        }

        // -------------------------------------------------------------
        // 3. Cargar fn_addr (= @Absolute("code.__lambda_<N>")) via LABEL_ADDR.
        // -------------------------------------------------------------
        ir::IrValueId fn_addr = emit_label_addr(fn_name, e->loc.line);

        // -------------------------------------------------------------
        // 4. STORE fn_addr en [fv_addr+0] y env_addr en [fv_addr+8].
        // -------------------------------------------------------------
        {
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {fn_addr, fv_addr};
            st.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st));
        } {
            ir::IrValueId fv_plus_8 = fn_->new_value(ir::IrType::PTR);
            ir::IrValueId off8      = emit_const(ir::IrType::I64, 8, e->loc.line);
            ir::IrInstr   ad{};
            ad.op          = ir::IrOp::ADD;
            ad.type        = ir::IrType::I64;
            ad.dst         = fv_plus_8;
            ad.operands    = {fv_addr, off8};
            ad.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ad));

            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {env_addr, fv_plus_8};
            st.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st));
        }

        // El SSA value de la lambda es la direccion del slot de 16 bytes.
        // Cuando se asigna a una variable @c fn(...), bind() la registra
        // tal cual; cuando se llama, lower_call carga fn_addr y env_addr
        // del slot y emite CALLCLOSURE.
        return fv_addr;
    }

    // ---------------------------------------------------------------------
    // ADTs: lower_enum_constructor + lower_match_expr.
    //
    // Layout del slot del enum (mismo para todas las variantes):
    //   [+0  i64 tag]
    //   [+8  i64 payload[0]]
    //   [+16 i64 payload[1]] ...
    // Tamano total: 8 + 8 * max_payload_fields.  Cada payload se
    // promociona a i64 para tener acceso uniforme por offset.  Cero
    // alocaciones de heap; cero overhead GC; mismo modelo que
    // Optional / Result.
    // ---------------------------------------------------------------------

    /// Kind del backing de un valued enum (i32/u64/...); default I64.
    static PrimitiveKind backing_kind_of(const EnumLayout &elay) {
        const std::string &b = elay.backing_type_name;
        if (b == "i8" || b == "int8_t")      return PrimitiveKind::I8;
        if (b == "i16" || b == "int16_t")    return PrimitiveKind::I16;
        if (b == "i32" || b == "int32_t")    return PrimitiveKind::I32;
        if (b == "i64" || b == "int64_t")    return PrimitiveKind::I64;
        if (b == "u8" || b == "uint8_t")     return PrimitiveKind::U8;
        if (b == "u16" || b == "uint16_t")   return PrimitiveKind::U16;
        if (b == "u32" || b == "uint32_t")   return PrimitiveKind::U32;
        if (b == "u64" || b == "uint64_t")   return PrimitiveKind::U64;
        if (b == "string")                   return PrimitiveKind::STRING;
        if (b == "float")                    return PrimitiveKind::F32;
        if (b == "double")                   return PrimitiveKind::F64;
        return PrimitiveKind::I64;
    }

    /// Parsea el literal de un valor de variante a @c val (signed int64).
    static bool parse_enum_value(const std::string &text, PrimitiveKind bk,
                                 int64_t &val, const SourceLoc &loc) {
        (void)loc;
        std::string t = text;
        bool neg = false;
        if (!t.empty() && t[0] == '-') { neg = true; t = t.substr(1); }
        try {
            size_t pos = 0;
            int64_t v = std::stoll(t, &pos, 0);
            if (pos != t.size()) return false;
            val = neg ? -v : v;
            return true;
        } catch (...) {
            return false;
        }
    }

    ir::IrValueId Lowering::lower_enum_constructor(
        const std::string &                             enum_name,
        const std::string &                             variant_name,
        const std::vector<std::unique_ptr<ast::Expr> > &args,
        const SourceLoc &                               loc) {
        // Localizar el layout del enum y la variante.
        const auto &elays = tc_.enum_layouts();
        auto        it    = elays.find(enum_name);
        if (it == elays.end()) {
            error_at(loc, "lowering: enum desconocido '" + enum_name + "'");
            return ir::IR_NO_VALUE;
        }
        const EnumLayout &     elay = it->second;
        const EnumVariantInfo *var  = nullptr;
        for (const auto &v: elay.variants) {
            if (v.name == variant_name) {
                var = &v;
                break;
            }
        }
        if (!var) {
            error_at(loc, "lowering: variante desconocida '" + variant_name +
                     "' en enum '" + enum_name + "'");
            return ir::IR_NO_VALUE;
        }

        // Valued enum C-style: la variante tiene un valor explicito
        // (`A = 42`).  Devolvemos el valor como constante en lugar de
        // alocar un slot ADT con tag.
        if (var->has_value) {
            const PrimitiveKind bk = backing_kind_of(elay);
            const ir::IrType ir_t  = ir_type_from_primitive(bk);
            int64_t val = 0;
            if (!parse_enum_value(var->value_text, bk, val, loc)) {
                error_at(loc, "valor de variante '" + variant_name +
                         "' no parseable: '" + var->value_text + "'");
                return ir::IR_NO_VALUE;
            }
            return emit_const(ir_t, static_cast<uint64_t>(val), loc.line);
        }

        // marker: MAKE_VARIANT identifica la construccion completa de
        // un valor ADT.  Emitido ANTES de la secuencia ALLOCA + STOREs para
        // que el C2 JIT (Phase D.8) pueda reconocer el patron y aplicar
        // escape analysis (promocion del slot a regs si no escapa) +
        // case-splitting eficiente del match downstream.  No produce SSA
        // value; el emitter actual lo trata como no-op.
        //
        // Lower de los args ANTES del marker para que sus SSA values
        // esten disponibles como operandos.  Cada payload se promueve a
        // un slot de 8 bytes (i64).  Para floats (F32/F64) usamos BITCAST
        // (preserva los bits IEEE) en lugar de FTOI/F2I (que truncaria
        // el valor).  El bug se manifiesta como `Circle(5.0)` con payload
        // 0 porque FTOI(5.0) -> 5, pero luego al destructurar como f64
        // se interpreta 5 como bits IEEE de un denormal cerca de cero.
        std::vector<ir::IrValueId> payload_vals;
        payload_vals.reserve(args.size());
        for (size_t i = 0; i < args.size() && i < var->field_types.size(); ++i) {
            // Auto-promotion literal -> StringObject cuando el payload
            // tipo es STRING.  Sin esto, `Token.Word("hello")` almacena
            // el raw ptr del literal en lugar del GcHandle, y la
            // extraccion `case Word(s) => s` da un ptr invalido al
            // intentar usarlo como string.
            ir::IrValueId v;
            const ast::Expr *ae = args[i].get();
            if (var->field_types[i].kind == PrimitiveKind::STRING
             && ae && ae->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = const_cast<ast::StringLitExpr *>(
                    static_cast<const ast::StringLitExpr *>(ae));
                v = lower_string_literal_to_string_object(slit);
            } else {
                v = lower_expr(args[i].get());
            }
            if (v == ir::IR_NO_VALUE) {
                v = emit_const(ir::IrType::I64, 0, loc.line);
            }
            ir::IrType vt = fn_->values[v].type;
            if (vt == ir::IrType::F64) {
                // bitcast f64 -> i64 (mismo ancho, preserva bits IEEE).
                ir::IrValueId v2 = fn_->new_value(ir::IrType::I64);
                ir::IrInstr   bc{};
                bc.op          = ir::IrOp::BITCAST;
                bc.type        = ir::IrType::I64;
                bc.dst         = v2;
                bc.operands    = {v};
                bc.source_line = loc.line;
                fn_->append(current_block_, std::move(bc));
                v = v2;
            } else if (vt == ir::IrType::F32) {
                // f32: primero ampliar a f64 (preserva el valor), luego
                // bitcast a i64 (preserva los bits IEEE).
                ir::IrValueId vw = fn_->new_value(ir::IrType::F64); {
                    ir::IrInstr ext{};
                    ext.op          = ir::IrOp::F32TOF64;
                    ext.type        = ir::IrType::F64;
                    ext.dst         = vw;
                    ext.operands    = {v};
                    ext.source_line = loc.line;
                    fn_->append(current_block_, std::move(ext));
                }
                ir::IrValueId v2 = fn_->new_value(ir::IrType::I64); {
                    ir::IrInstr bc{};
                    bc.op          = ir::IrOp::BITCAST;
                    bc.type        = ir::IrType::I64;
                    bc.dst         = v2;
                    bc.operands    = {vw};
                    bc.source_line = loc.line;
                    fn_->append(current_block_, std::move(bc));
                }
                v = v2;
            } else if (vt != ir::IrType::I64 && vt != ir::IrType::PTR) {
                // Tipos enteros mas estrechos: promocion normal a i64
                // (sign/zero-extend segun signedness).
                v = cast_if_needed(v, vt, ir::IrType::I64, loc.line);
            }
            payload_vals.push_back(v);
        }
        {
            ir::IrInstr mv{};
            mv.op          = ir::IrOp::MAKE_VARIANT;
            mv.type        = ir::IrType::VOID;
            mv.dst         = ir::IR_NO_VALUE;
            mv.operands    = payload_vals;
            mv.func_name   = enum_name + "." + variant_name;
            mv.imm         = static_cast<uint64_t>(var->tag);
            mv.source_line = loc.line;
            fn_->append(current_block_, std::move(mv));
        }

        // 1. ALLOCA slot del enum (size_bytes = 8 + 8*max_payload_fields).
        const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = addr;
            al.imm         = static_cast<uint64_t>(elay.size_bytes);
            al.source_line = loc.line;
            fn_->append(current_block_, std::move(al));
        }

        // 2. STORE i64 tag en offset 0 (= addr).
        {
            ir::IrValueId tag_v = emit_const(ir::IrType::I64,
                                             static_cast<uint64_t>(var->tag),
                                             loc.line);
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {tag_v, addr};
            st.source_line = loc.line;
            fn_->append(current_block_, std::move(st));
        }

        // 3. STORE de cada payload arg en offset 8 + 8*i (promovido a i64).
        // Reusa los payload_vals ya lowereados arriba (para el marker
        // MAKE_VARIANT): evita doble-lowering de los args.
        for (size_t i = 0; i < payload_vals.size(); ++i) {
            ir::IrValueId v = payload_vals[i];
            if (v == ir::IR_NO_VALUE) continue;

            // Calcular addr_i = addr + (8 + 8*i).
            const uint64_t off    = 8ULL + 8ULL * static_cast<uint64_t>(i);
            ir::IrValueId  addr_i = fn_->new_value(ir::IrType::PTR);
            ir::IrValueId  off_v  = emit_const(ir::IrType::I64, off, loc.line);
            ir::IrInstr    ad{};
            ad.op          = ir::IrOp::ADD;
            ad.type        = ir::IrType::I64;
            ad.dst         = addr_i;
            ad.operands    = {addr, off_v};
            ad.source_line = loc.line;
            fn_->append(current_block_, std::move(ad));

            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {v, addr_i};
            st.source_line = loc.line;
            fn_->append(current_block_, std::move(st));
        }

        // El SSA value de la expresion es la direccion del slot.
        return addr;
    }

    ir::IrValueId Lowering::lower_match_expr(ast::MatchExpr *e) {
        if (!e || !e->scrutinee) {
            error_at(e ? e->loc : SourceLoc{}, "lowering: match sin scrutinee");
            return ir::IR_NO_VALUE;
        }
        // Tipo del scrutinee debe ser STRUCT con struct_name en
        // enum_layouts_.  Si no, el type checker ya reporto error y
        // devolvemos NO_VALUE silenciosamente para no inundar.
        const Type st = e->scrutinee->result_type;
        if (st.kind != PrimitiveKind::STRUCT) return ir::IR_NO_VALUE;
        const auto &elays = tc_.enum_layouts();
        auto        it    = elays.find(st.struct_name);
        if (it == elays.end()) return ir::IR_NO_VALUE;
        const EnumLayout &elay = it->second;

        // 1. Lower del scrutinee -> SSA PTR al slot.
        ir::IrValueId scrut_addr = lower_expr(e->scrutinee.get());
        if (scrut_addr == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        // marker: MATCH_VARIANT identifica el inicio del match.  Emitido
        // ANTES del LOAD del tag + cadena cmp+br para que el C2 JIT
        // reconozca el patron y emita dispatch eficiente
        // (jumptable si tags densos, switch tree balanceado si dispersos).
        // No produce SSA value; el emitter actual lo trata como no-op.
        {
            size_t n_concrete = 0;
            for (const auto &arm: e->arms) {
                if (arm.variant_name != "_") n_concrete++;
            }
            ir::IrInstr mt{};
            mt.op          = ir::IrOp::MATCH_VARIANT;
            mt.type        = ir::IrType::VOID;
            mt.dst         = ir::IR_NO_VALUE;
            mt.operands    = {scrut_addr};
            mt.func_name   = st.struct_name;  // nombre del enum
            mt.imm         = static_cast<uint64_t>(n_concrete);
            mt.source_line = e->loc.line;
            fn_->append(current_block_, std::move(mt));
        }

        // 2. LOAD i64 del tag en offset 0.
        ir::IrValueId tag_v = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr ld{};
            ld.op          = ir::IrOp::LOAD;
            ld.type        = ir::IrType::I64;
            ld.dst         = tag_v;
            ld.operands    = {scrut_addr};
            ld.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ld));
        }

        // 3. Construir bloques: uno por arm + uno default + uno merge.
        // Estrategia simple O(N): para cada arm con variant concreto,
        // emitimos un cmp_eq + br_cond a su body block.  Si ninguno
        // matchea, caemos en default_block (= arm "_") o merge_block
        // (continuacion del programa) si no hay default.
        const ir::IrBlockId merge_bb = fn_->new_block("match_end");
        // Localizar arm default (si existe).
        ssize_t default_arm_idx = -1;
        for (size_t i = 0; i < e->arms.size(); ++i) {
            if (e->arms[i].variant_name == "_") {
                default_arm_idx = static_cast<ssize_t>(i);
                break;
            }
        }
        ir::IrBlockId default_bb = (default_arm_idx >= 0)
                                       ? fn_->new_block("match_default")
                                       : merge_bb;

        // Pre-crear un bloque por cada arm concreto (no-default).
        std::vector<ir::IrBlockId> arm_blocks(e->arms.size(), ir::IR_NO_BLOCK);
        // Bug fix 2026-05-23: arm_fall_bbs[i] = fall_bb del cmp del arm i.
        // Si el arm tiene guard y falla en runtime, saltamos a este bloque
        // (que reanuda con el cmp del arm siguiente).
        std::vector<ir::IrBlockId> arm_fall_bbs(e->arms.size(), ir::IR_NO_BLOCK);
        for (size_t i = 0; i < e->arms.size(); ++i) {
            if (static_cast<ssize_t>(i) == default_arm_idx) continue;
            arm_blocks[i] = fn_->new_block(
                std::string("match_arm_") + e->arms[i].variant_name);
        }

        // Encadenar cmps en el bloque actual: si tag == tag_i, br a
        // arm_blocks[i]; si no, fall-through al siguiente cmp.  El
        // ultimo cmp falla -> br a default_bb (o merge_bb si no hay
        // default).
        for (size_t i = 0; i < e->arms.size(); ++i) {
            if (static_cast<ssize_t>(i) == default_arm_idx) continue;
            // Buscar el tag de la variante.
            const EnumVariantInfo *var = nullptr;
            for (const auto &v: elay.variants) {
                if (v.name == e->arms[i].variant_name) {
                    var = &v;
                    break;
                }
            }
            if (!var) continue;

            // cmp_eq tag_v == var->tag
            ir::IrValueId cmp_v     = fn_->new_value(ir::IrType::BOOL);
            ir::IrValueId tag_const = emit_const(ir::IrType::I64,
                                                 static_cast<uint64_t>(var->tag),
                                                 e->arms[i].loc.line); {
                ir::IrInstr cm{};
                cm.op          = ir::IrOp::CMP_EQ;
                cm.type        = ir::IrType::BOOL;
                cm.dst         = cmp_v;
                cm.operands    = {tag_v, tag_const};
                cm.source_line = e->arms[i].loc.line;
                fn_->append(current_block_, std::move(cm));
            }

            // br_cond cmp -> arm_blocks[i], else fall_block
            const ir::IrBlockId fall_bb = fn_->new_block("match_next");
            arm_fall_bbs[i] = fall_bb;  // para uso si la arm tiene guard
            ir::IrInstr         br{};
            br.op = ir::IrOp::BR_COND;
            br.operands.push_back(cmp_v);
            br.target_block = arm_blocks[i];
            br.false_block  = fall_bb;
            br.source_line  = e->arms[i].loc.line;
            fn_->append(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(arm_blocks[i]);
            fn_->blocks[current_block_].succs.push_back(fall_bb);
            fn_->blocks[arm_blocks[i]].preds.push_back(current_block_);
            fn_->blocks[fall_bb].preds.push_back(current_block_);

            current_block_    = fall_bb;
            block_terminated_ = false;
        }
        // Tras la cadena de cmps, el bloque actual es la rama "ninguna
        // variante matcheada".  Saltamos al default si existe, o al
        // merge directamente.
        {
            ir::IrInstr br{};
            br.op           = ir::IrOp::BR;
            br.target_block = default_bb;
            br.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(default_bb);
            fn_->blocks[default_bb].preds.push_back(current_block_);
        }

        // Snapshot bindings ANTES de las arms para PHI en merge.
        // Mismo patron que lower_try: cada arm asigna en el scope enclosing,
        // y al merge llegan multiples binding distintos.  Sin PHI el
        // binding final es el de la ultima arm lowered (no determinista).
        //
        // BUG FIX 2026-05-26: guardar TODOS los scopes enclosing, no solo
        // el inmediato.  Antes solo se restauraba scopes_.back() entre
        // arms; pero un arm puede modificar vars en cualquier nivel deeper
        // (e.g. `n` declarada en la funcion, dentro de un match dentro
        // de un while).  Esas mutaciones leakean al siguiente arm si
        // no restauramos toda la stack de scopes.
        const auto entry_all_scopes_match = scopes_;
        // Mantener compat con codigo que referencia entry_bindings_match
        // (solo el outer del match) para el PHI de merge.
        const auto entry_bindings_match = scopes_.back();

        struct ArmSnapshot {
            // Cada arm captura TODOS los niveles de scope post-body para
            // poder hacer PHI merge a multiples niveles.
            std::vector<std::unordered_map<std::string, ir::IrValueId>>
                                                           all_scopes;
            std::unordered_map<std::string, ir::IrValueId> bindings;
            ir::IrBlockId                                  pred;
            bool                                           reaches_merge;
        };
        std::vector<ArmSnapshot> arm_snaps;
        arm_snaps.reserve(e->arms.size());

        // 4. Emitir el body de cada arm (incluido el default).
        for (size_t i = 0; i < e->arms.size(); ++i) {
            const auto &        arm    = e->arms[i];
            const ir::IrBlockId target = (static_cast<ssize_t>(i) == default_arm_idx)
                                             ? default_bb
                                             : arm_blocks[i];
            current_block_    = target;
            block_terminated_ = false;
            // Restaurar TODOS los scopes enclosing al estado de entry
            // antes de cada arm.  Sin esto, mutaciones de outer-outer
            // scopes en arm_A se ven en arm_B (-> SSA values cruzados).
            scopes_ = entry_all_scopes_match;
            push_scope();

            // Bind de los payload bindings (si la variante tiene
            // payload).  Para cada binding i, emit LOAD i64 de
            // [scrut_addr + 8 + 8*i] y bind con el nombre del binding.
            //
            // Para tipos float, el slot guarda BITS IEEE como i64
            // (escrito por el constructor via BITCAST).  Aqui bitcasteamos
            // de vuelta a F64 antes del bind para que las operaciones
            // posteriores (multiplicacion, comparacion) usen FMUL/FCMP en
            // vez de IMUL/CMP int.  Para F32 originalmente guardado:
            // BITCAST i64 -> f64 + F64TOF32 narrow (recupera el valor).
            if (arm.variant_name != "_") {
                // Localizar la EnumVariantInfo para conocer field_types.
                const EnumVariantInfo *arm_var = nullptr;
                for (const auto &vinfo: elay.variants) {
                    if (vinfo.name == arm.variant_name) {
                        arm_var = &vinfo;
                        break;
                    }
                }
                for (size_t bi = 0; bi < arm.bindings.size(); ++bi) {
                    const uint64_t off    = 8ULL + 8ULL * static_cast<uint64_t>(bi);
                    ir::IrValueId  addr_i = fn_->new_value(ir::IrType::PTR);
                    ir::IrValueId  off_v  = emit_const(ir::IrType::I64, off,
                                                       arm.loc.line); {
                        ir::IrInstr ad{};
                        ad.op          = ir::IrOp::ADD;
                        ad.type        = ir::IrType::I64;
                        ad.dst         = addr_i;
                        ad.operands    = {scrut_addr, off_v};
                        ad.source_line = arm.loc.line;
                        fn_->append(current_block_, std::move(ad));
                    }
                    ir::IrValueId v = fn_->new_value(ir::IrType::I64); {
                        ir::IrInstr ld{};
                        ld.op          = ir::IrOp::LOAD;
                        ld.type        = ir::IrType::I64;
                        ld.dst         = v;
                        ld.operands    = {addr_i};
                        ld.source_line = arm.loc.line;
                        fn_->append(current_block_, std::move(ld));
                    }
                    // Si el field declarado es float, bitcastear i64 -> f64
                    // (recupera los bits IEEE escritos por el constructor).
                    if (arm_var && bi < arm_var->field_types.size()) {
                        const Type &ft = arm_var->field_types[bi];
                        if (ft.kind == PrimitiveKind::F64) {
                            ir::IrValueId v2 = fn_->new_value(ir::IrType::F64);
                            ir::IrInstr   bc{};
                            bc.op          = ir::IrOp::BITCAST;
                            bc.type        = ir::IrType::F64;
                            bc.dst         = v2;
                            bc.operands    = {v};
                            bc.source_line = arm.loc.line;
                            fn_->append(current_block_, std::move(bc));
                            v = v2;
                        } else if (ft.kind == PrimitiveKind::F32) {
                            // Recuperar f32: el slot guardo BITCAST(F32TOF64(x))
                            // como i64.  Invertimos: BITCAST i64->f64 +
                            // F64TOF32 narrow para volver al f32 original.
                            ir::IrValueId vd = fn_->new_value(ir::IrType::F64); {
                                ir::IrInstr bc{};
                                bc.op          = ir::IrOp::BITCAST;
                                bc.type        = ir::IrType::F64;
                                bc.dst         = vd;
                                bc.operands    = {v};
                                bc.source_line = arm.loc.line;
                                fn_->append(current_block_, std::move(bc));
                            }
                            ir::IrValueId v2 = fn_->new_value(ir::IrType::F32); {
                                ir::IrInstr nr{};
                                nr.op          = ir::IrOp::F64TOF32;
                                nr.type        = ir::IrType::F32;
                                nr.dst         = v2;
                                nr.operands    = {vd};
                                nr.source_line = arm.loc.line;
                                fn_->append(current_block_, std::move(nr));
                            }
                            v = v2;
                        }
                        // Para tipos enteros mas estrechos, dejar v como i64;
                        // el codigo de uso aplicara cast_if_needed cuando
                        // sea necesario (sign-extend / truncate).
                    }
                    bind(arm.bindings[bi], v);
                }
            }

            // Bug fix 2026-05-23: si la arm tiene guard, evaluarlo ANTES
            // del body.  Si falso, saltar al fall_bb (siguiente cmp).
            if (arm.guard && arm.variant_name != "_") {
                const ir::IrValueId guard_v = lower_expr(arm.guard.get());
                if (guard_v != ir::IR_NO_VALUE && arm_fall_bbs[i] != ir::IR_NO_BLOCK) {
                    const ir::IrBlockId body_bb = fn_->new_block("match_arm_body");
                    ir::IrInstr br{};
                    br.op = ir::IrOp::BR_COND;
                    br.operands.push_back(guard_v);
                    br.target_block = body_bb;
                    br.false_block  = arm_fall_bbs[i];
                    br.source_line  = arm.loc.line;
                    fn_->append(current_block_, std::move(br));
                    fn_->blocks[current_block_].succs.push_back(body_bb);
                    fn_->blocks[current_block_].succs.push_back(arm_fall_bbs[i]);
                    fn_->blocks[body_bb].preds.push_back(current_block_);
                    fn_->blocks[arm_fall_bbs[i]].preds.push_back(current_block_);
                    current_block_    = body_bb;
                    block_terminated_ = false;
                }
            }

            if (arm.body) lower_stmt(arm.body.get());
            // Capturar snapshot ANTES de pop_scope.  Guardamos TANTO
            // el scope outer inmediato (size-2) PARA compatibilidad con
            // el PHI merge existente, COMO la stack completa (size-2 y
            // todos los inferiores) para PHI multi-nivel.
            ArmSnapshot snap;
            snap.reaches_merge = !block_terminated_;
            if (snap.reaches_merge) {
                if (scopes_.size() >= 2) {
                    snap.bindings = scopes_[scopes_.size() - 2];
                    // Capturar todos los niveles enclosing (excluyendo
                    // el scope local del arm, size-1).  Necesario para
                    // que el PHI insert al merge atrape mutaciones de
                    // vars declaradas en funcion/loop body/etc.
                    snap.all_scopes.assign(scopes_.begin(),
                                            scopes_.begin() + (scopes_.size() - 1));
                }
                snap.pred = current_block_;
            } else {
                snap.pred = ir::IR_NO_BLOCK;
            }
            arm_snaps.push_back(std::move(snap));

            if (!block_terminated_) {
                // br merge_bb
                ir::IrInstr br{};
                br.op           = ir::IrOp::BR;
                br.target_block = merge_bb;
                br.source_line  = arm.loc.line;
                fn_->append(current_block_, std::move(br));
                fn_->blocks[current_block_].succs.push_back(merge_bb);
                fn_->blocks[merge_bb].preds.push_back(current_block_);
                block_terminated_ = true;
            }
            pop_scope();
        }

        // Si NO hubo arm default y el merge_bb es el destino del
        // fall-through "ninguna variante matcheo", aseguramos que
        // continuamos en merge_bb.  Si hubo default, el fall_block ya
        // saltaba a default_bb que a su vez hace br a merge_bb.
        current_block_    = merge_bb;
        block_terminated_ = false;

        // PHI insertion en merge_bb para variables modificadas
        // en multiples arms.  Mismo algoritmo que lower_try.
        // Reset TODOS los scopes al estado de entry; el PHI insert
        // multi-nivel sobreescribe las vars que difieren.
        scopes_ = entry_all_scopes_match;
        struct MergeContrib2 {
            ir::IrBlockId pred;
            const std::vector<std::unordered_map<std::string, ir::IrValueId>>
                          *all_scopes;
        };
        std::vector<MergeContrib2> contribs;
        contribs.reserve(arm_snaps.size());
        for (auto &as: arm_snaps) {
            if (as.reaches_merge) {
                contribs.push_back({as.pred, &as.all_scopes});
            }
        }
        // PHI insertion multi-nivel: por cada scope-level enclosing del
        // match, para cada var, si los arms producen valores distintos,
        // insertar PHI en merge_bb y actualizar el scope correspondiente.
        // Esto cierra el bug del match-en-loop donde mutaciones a vars
        // declaradas en niveles outer-outer (e.g. funcion) no eran
        // PHI-merged y la back-edge tomaba el valor del ULTIMO arm.
        if (contribs.size() >= 2) {
            const size_t n_levels = entry_all_scopes_match.size();
            for (size_t lvl = 0; lvl < n_levels; ++lvl) {
                for (const auto &kv: entry_all_scopes_match[lvl]) {
                    const std::string & name      = kv.first;
                    const ir::IrValueId entry_val = kv.second;
                    bool                any_diff  = false;
                    for (const auto &c: contribs) {
                        if (lvl >= c.all_scopes->size()) continue;
                        auto it_b = (*c.all_scopes)[lvl].find(name);
                        if (it_b == (*c.all_scopes)[lvl].end()) continue;
                        if (it_b->second != entry_val) {
                            any_diff = true;
                            break;
                        }
                    }
                    if (!any_diff) continue;

                    ir::IrType ity = ir::IrType::I64;
                    if (entry_val != ir::IR_NO_VALUE
                        && entry_val < fn_->values.size()) {
                        ity = fn_->values[entry_val].type;
                    }
                    ir::IrValueId v_phi = fn_->new_value(ity);
                    ir::IrInstr   phi{};
                    phi.op          = ir::IrOp::PHI;
                    phi.type        = ity;
                    phi.dst         = v_phi;
                    phi.source_line = e->loc.line;
                    for (const auto &c: contribs) {
                        ir::IrValueId in_val = entry_val;
                        if (lvl < c.all_scopes->size()) {
                            auto it_b = (*c.all_scopes)[lvl].find(name);
                            if (it_b != (*c.all_scopes)[lvl].end()) {
                                in_val = it_b->second;
                            }
                        }
                        ir::IrPhiArg arg{};
                        arg.value = in_val;
                        arg.block = c.pred;
                        phi.phi_args.push_back(arg);
                    }
                    fn_->blocks[merge_bb].instrs.insert(
                        fn_->blocks[merge_bb].instrs.begin(),
                        std::move(phi));
                    scopes_[lvl][name] = v_phi;
                }
            }
        } else if (contribs.size() == 1) {
            // Solo un arm reach merge: copiar sus all_scopes directos.
            const auto &as = *contribs[0].all_scopes;
            const size_t n_min = std::min(as.size(), scopes_.size());
            for (size_t lvl = 0; lvl < n_min; ++lvl) {
                for (const auto &kv: as[lvl]) {
                    scopes_[lvl][kv.first] = kv.second;
                }
            }
        }
        return ir::IR_NO_VALUE; // statement-like
    }

    ir::IrValueId Lowering::lower_spawn_expr(ast::SpawnExpr *e) {
        if (!e || !e->body) {
            error_at(e ? e->loc : SourceLoc{}, "lowering: spawn sin body");
            return ir::IR_NO_VALUE;
        }
        // 1. Generar la funcion hijo y obtener su nombre (label .vel).
        const std::string fn_name = generate_spawn_helper(e->body.get(), e->loc);

        // 2. Emit RAW_ASM en el bloque actual del padre:
        //    a) cargar la direccion absoluta de fn_name en {dst_pc}.
        //    b) ejecutar `spawn {dst_pc}` (Auto) o `spawnon {dst_pc}, {src1}`
        //       (Here / Pinned) segun la policy del SpawnExpr.
        //    c) capturar R0 al SSA value de la expresion via {dst}.
        const ir::IrValueId v_pc = emit_label_addr(fn_name, e->loc.line);

        // BugFix R3: si hay capturas, usar `spawnargs` en lugar de `spawn`.
        // Convencion: R1..R[N]=capturas, R15=N, spawnargs r_pc copia los
        // regs al child antes de make_ready.  Aplica para Auto policy.
        const auto &caps = spawn_captured_ssa_values_;
        if (e->policy == ast::SpawnExpr::Policy::Auto && !caps.empty()) {
            // raw_asm-elim wave 2: usar IrOp::SPAWN_ARGS nativo en lugar de
            // raw_asm.  El IR emitter ya genera el parallel-move correcto
            // del regalloc + spawnargs + restore.  operands[0]=r_pc,
            // [1..N]=args; dst=PID encoded en R0.
            const ir::IrValueId v_pid = fn_->new_value(ir::IrType::I64);
            std::vector<ir::IrValueId> ops;
            ops.push_back(v_pc);
            for (auto v : caps) ops.push_back(v);
            ir::IrInstr sa{};
            sa.op           = ir::IrOp::SPAWN_ARGS;
            sa.type         = ir::IrType::I64;
            sa.dst          = v_pid;
            sa.operands     = std::move(ops);
            sa.source_line  = e->loc.line;
            sa.set_is_call_site(true);
            fn_->append(current_block_, std::move(sa));
            return v_pid;
        }

        // si la policy es Auto, mantener el opcode SPAWN
        // (sin overhead).  Para Here y Pinned usar SPAWN_ON con el hint en
        // el segundo registro:
        //   - Here:   hint = -1 (signed) -> mismo scheduler que el padre.
        //   - Pinned: hint = expr        -> scheduler hint % num_schedulers.
        if (e->policy == ast::SpawnExpr::Policy::Auto) {
            // SPAWN IR op (0xF3): crea proceso hijo, PID encoded en R0/dst.
            const ir::IrValueId v_pid = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         sp{};
            sp.op           = ir::IrOp::SPAWN;
            sp.type         = ir::IrType::I64;
            sp.dst          = v_pid;
            sp.operands     = {v_pc};
            sp.set_is_call_site(true);
            sp.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(sp));
            return v_pid;
        }

        // Construir el SSA value del hint segun la policy.
        ir::IrValueId v_hint = ir::IR_NO_VALUE;
        if (e->policy == ast::SpawnExpr::Policy::Here) {
            // hint = -1 como i64 inmediato.
            v_hint = emit_const(ir::IrType::I64, static_cast<uint64_t>(-1LL),
                                e->loc.line);
        } else {
            // Pinned
            if (!e->sched_idx) {
                error_at(e->loc, "lowering: spawn on(...) sin expresion");
                return ir::IR_NO_VALUE;
            }
            v_hint = lower_expr(e->sched_idx.get());
            if (v_hint == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            // Promover a I64 si es necesario para que el runtime lea int64.
            const ir::IrType src_t = fn_->values[v_hint].type;
            v_hint                 = cast_if_needed(v_hint, src_t, ir::IrType::I64, e->loc.line);
        }

        // spawn_on IR op: combines fn_addr + hint -> PID encoded en R0.
        const ir::IrValueId v_pid = fn_->new_value(ir::IrType::I64);
        ir::IrInstr         sp{};
        sp.op           = ir::IrOp::SPAWN_ON;
        sp.type         = ir::IrType::I64;
        sp.dst          = v_pid;
        sp.operands     = {v_pc, v_hint};
        sp.set_is_call_site(true);
        sp.source_line  = e->loc.line;
        fn_->append(current_block_, std::move(sp));
        return v_pid;
    }

    // ---------------------------------------------------------------------
    // @Async sugar.  Transforma una FunctionDecl con flag
    // is_async en DOS funciones IR:
    //
    //   1. Wrapper publico `<name>` con firma `i64 <name>()` que el caller
    //      invoca.  Internamente:
    //        a. future_alloc -> fut handle
    //        b. spawn helper sintetica __async_<name>
    //        c. msgsend(child_pid, fut handle)
    //        d. return fut handle
    //
    //   2. Spawn helper `__async_<name>(void)` que ejecuta el body original
    //      como hijo cooperativo:
    //        a. msgrecv -> my_fut handle (set como async_fut_id_)
    //        b. lower body original (cada return X intercepta a fulfill+hlt)
    //        c. fallback al final: fulfill(my_fut, 0) + hlt
    //
    // El usuario escribe:
    //   @Async i64 compute() { return 42; }
    //   i32 main() { i64 r = await compute(); return r; }
    // ---------------------------------------------------------------------
    void Lowering::lower_async_function(ast::FunctionDecl *fd, ir::IrModule &out) {
        if (!fd || !fd->body) {
            error_at(fd ? fd->loc : SourceLoc{}, "@Async: funcion sin body");
            return;
        }
        const std::string helper_name = std::string("__async_") + fd->name;

        // Mejora II optimizada: numero de parametros del usuario.  Pasamos
        // los args al helper via @c spawnargs (R1..R[argc]) en lugar de
        // serializarlos a un buffer y enviarlos via msgsend.  El handle
        // del Future tambien viaja por R1 (slot 0 en la nueva calling
        // convention del helper: arg[0] = future handle, arg[1..N] = args).
        // Asi:
        //   - Wrapper: emit args en R2..R[N+1] + R1=fut + R15=N+1 + spawnargs.
        //     ~3 instr en lugar de ~12 (alloca + N+1 stores + msgsend).
        //   - Helper:  los params estan ya en R1..R[N+1], NO necesita
        //     ALLOCA + msgrecv + N+1 LOADs + casts.
        // Total: ~26 instr -> ~3 instr por @Async call (~9x mas rapido).
        const size_t n_params = fd->params.size();
        if (n_params + 1 > 12) {
            // Calling convention de spawnargs: R1..R[argc] con argc <= 12.
            // Reservamos R1 para el handle del Future, asi quedan 11 slots
            // para args del usuario.
            error_at(fd->loc,
                "@Async: numero de parametros excede el maximo (11)");
            return;
        }

        // ---------------------------------------------------------------
        // 1. Construir el SPAWN HELPER (lo encolamos en pending_spawn_helpers_
        //    para que se vuelque al final, despues de main).
        // ---------------------------------------------------------------
        ir::IrFunction *                                             saved_fn         = fn_;
        ir::IrBlockId                                                saved_block      = current_block_;
        bool                                                         saved_terminated = block_terminated_;
        std::vector<std::unordered_map<std::string, ir::IrValueId> > saved_scopes
                = std::move(scopes_);
        std::unordered_set<std::string> saved_addr_taken
                = std::move(address_taken_locals_);
        std::vector<CleanupAction> saved_cleanups
                = std::move(cleanup_stack_);
        ir::IrValueId saved_async_fut = async_fut_id_;

        ir::IrFunction helper_fn;
        helper_fn.name            = helper_name;
        helper_fn.ret_type        = ir::IrType::VOID;
        const ir::IrBlockId entry = helper_fn.new_block("entry");

        fn_               = &helper_fn;
        current_block_    = entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();

        // Mejora II optimizada: el helper recibe args directos en R1..R[N+1]
        // gracias a @c spawnargs (sin msgrecv + buffer).  Calling convention:
        //   R1         = handle del Future
        //   R2..R[N+1] = parametros del usuario en el orden declarado
        // Declarar formalmente los params como un IrFunction normal: el
        // primer param es el handle (siempre I64), el resto son los del
        // usuario con su tipo declarado.
        ir::IrValueId v_my_fut;
        {
            // Primer parametro IR: future handle (I64).
            v_my_fut = fn_->new_value(ir::IrType::I64, "__async_fut");
            fn_->values[v_my_fut].is_param = true;
            fn_->params.push_back(v_my_fut);
        }
        for (size_t pi = 0; pi < n_params; ++pi) {
            auto &p = fd->params[pi];
            // Determinar el IrType del param segun el tipo declarado.
            ir::IrType pt_ir = ir::IrType::I64;
            if (p->type) {
                if (auto *prim = dynamic_cast<ast::NamedTypeNode *>(p->type.get())) {
                    const std::string &nm = prim->name;
                    if      (nm == "i8")    pt_ir = ir::IrType::I8;
                    else if (nm == "i16")   pt_ir = ir::IrType::I16;
                    else if (nm == "i32" || nm == "int32_t")  pt_ir = ir::IrType::I32;
                    else if (nm == "i64" || nm == "int64_t")  pt_ir = ir::IrType::I64;
                    else if (nm == "u8")    pt_ir = ir::IrType::U8;
                    else if (nm == "u16")   pt_ir = ir::IrType::U16;
                    else if (nm == "u32" || nm == "uint32_t") pt_ir = ir::IrType::U32;
                    else if (nm == "u64" || nm == "uint64_t") pt_ir = ir::IrType::U64;
                    else if (nm == "f32" || nm == "float")    pt_ir = ir::IrType::F32;
                    else if (nm == "f64" || nm == "double")   pt_ir = ir::IrType::F64;
                    else if (nm == "bool")  pt_ir = ir::IrType::BOOL;
                    else if (nm == "char")  pt_ir = ir::IrType::I8;
                }
            }
            ir::IrValueId pv = fn_->new_value(pt_ir, p->name);
            fn_->values[pv].is_param = true;
            fn_->params.push_back(pv);
            bind(p->name, pv);
        }

        // 1b. Activar interception de return en lower_return.
        async_fut_id_ = v_my_fut;

        // 1c. Bajar el body original.  Cada `return X` -> fulfill + hlt.
        lower_block(fd->body.get());

        // 1d. Fallback: si el body cae naturalmente sin return, emitir
        //     fulfillhlt(my_fut, 0) para terminar el child con valor 0.
        //     Optimizado: 1 instr en lugar de fulfill+hlt separados.
        if (!block_terminated_) {
            const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, fd->loc.line);
            emit_fulfill_hlt(v_my_fut, v_zero, fd->loc.line);
            block_terminated_ = true;
        }

        pop_scope();
        // Encolamos el helper para volcarse al final de run() (despues de
        // main para preservar el orden de entry point).
        pending_spawn_helpers_.push_back(std::move(helper_fn));

        // Restaurar contexto del lowering.
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        async_fut_id_         = saved_async_fut;

        // ---------------------------------------------------------------
        // 2. Construir el WRAPPER publico con el nombre de la funcion.
        //    El wrapper recibe los args del usuario via la calling convention
        //    normal CALLVM (R1..R12), aloca un Future, spawnea el helper,
        //    serializa (handle, args) en un buffer y los envia al helper
        //    via msgsend.  Devuelve el handle del Future al caller.
        // ---------------------------------------------------------------
        ir::IrFunction wrapper_fn;
        wrapper_fn.name             = fd->name;
        wrapper_fn.ret_type         = ir::IrType::I64; // bytecode level: handle
        const ir::IrBlockId w_entry = wrapper_fn.new_block("entry");

        fn_               = &wrapper_fn;
        current_block_    = w_entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();
        async_fut_id_ = ir::IR_NO_VALUE; // wrapper NO es async body

        // Mejora II: declarar los parametros del wrapper igual que en una
        // funcion normal.  Cada param se mapea a un IrType y se vincula
        // con su nombre para que su SSA value se pueda leer mas abajo
        // cuando serializamos los args al buffer.
        std::vector<ir::IrValueId> param_vals;
        param_vals.reserve(n_params);
        for (size_t pi = 0; pi < n_params; ++pi) {
            auto &p = fd->params[pi];
            ir::IrType pt_ir = ir::IrType::I64;
            if (p->type) {
                if (auto *prim = dynamic_cast<ast::NamedTypeNode *>(p->type.get())) {
                    const std::string &nm = prim->name;
                    if      (nm == "i8")    pt_ir = ir::IrType::I8;
                    else if (nm == "i16")   pt_ir = ir::IrType::I16;
                    else if (nm == "i32" || nm == "int32_t")  pt_ir = ir::IrType::I32;
                    else if (nm == "i64" || nm == "int64_t")  pt_ir = ir::IrType::I64;
                    else if (nm == "u8")    pt_ir = ir::IrType::U8;
                    else if (nm == "u16")   pt_ir = ir::IrType::U16;
                    else if (nm == "u32" || nm == "uint32_t") pt_ir = ir::IrType::U32;
                    else if (nm == "u64" || nm == "uint64_t") pt_ir = ir::IrType::U64;
                    else if (nm == "f32" || nm == "float")    pt_ir = ir::IrType::F32;
                    else if (nm == "f64" || nm == "double")   pt_ir = ir::IrType::F64;
                    else if (nm == "bool")  pt_ir = ir::IrType::BOOL;
                    else if (nm == "char")  pt_ir = ir::IrType::I8;
                }
            }
            const ir::IrValueId pv = fn_->new_value(pt_ir, p->name);
            fn_->values[pv].is_param = true;
            fn_->params.push_back(pv);
            bind(p->name, pv);
            param_vals.push_back(pv);
        }

        // 2a. fut = future_alloc() via IR op FUTURE.
        const ir::IrValueId v_fut = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr fu{};
            fu.op           = ir::IrOp::FUTURE;
            fu.type         = ir::IrType::I64;
            fu.dst          = v_fut;
            fu.set_is_call_site(true);
            fu.source_line  = fd->loc.line;
            fn_->append(current_block_, std::move(fu));
        }

        // Mejora II optimizada: el wrapper usa IrOp::SPAWN_ARGS que aprovecha
        // el parallel-move del regalloc para colocar args correctamente en
        // R1..R[N+1] sin conflictos.  Calling convention:
        //   R1            = handle del Future
        //   R2..R[N+1]    = parametros del usuario coerced a i64
        //   R15           = N+1 (argc total, lo setea el emisor IR)
        //   spawnargs r_pc -> child PID encoded en R0 (devuelto al caller)
        //
        // Esto reemplaza la version previa con buffer + msgsend (~12 instr)
        // por ~2-3 instr fijas + N moves resueltos por parallel-move +
        // 1 spawnargs.  Elimina la contencion del lock del mailbox y la
        // copia de buffer.

        // 2b.1: Coerce cada param del usuario a i64 preservando bits.
        std::vector<ir::IrValueId> qword_args;
        qword_args.reserve(n_params);
        for (size_t pi = 0; pi < n_params; ++pi) {
            const ir::IrValueId v_param = param_vals[pi];
            const ir::IrType    pt_ir   = fn_->values[v_param].type;
            ir::IrValueId       v_qword = v_param;
            if (pt_ir == ir::IrType::F64) {
                v_qword = fn_->new_value(ir::IrType::I64);
                ir::IrInstr bc{};
                bc.op = ir::IrOp::BITCAST;
                bc.type = ir::IrType::I64;
                bc.dst = v_qword;
                bc.operands = {v_param};
                bc.source_line = fd->loc.line;
                fn_->append(current_block_, std::move(bc));
            } else if (pt_ir == ir::IrType::F32) {
                ir::IrValueId v_i32 = fn_->new_value(ir::IrType::I32);
                ir::IrInstr bc{};
                bc.op = ir::IrOp::BITCAST;
                bc.type = ir::IrType::I32;
                bc.dst = v_i32;
                bc.operands = {v_param};
                bc.source_line = fd->loc.line;
                fn_->append(current_block_, std::move(bc));
                v_qword = cast_if_needed(v_i32, ir::IrType::I32, ir::IrType::I64,
                                          fd->loc.line);
            } else if (pt_ir != ir::IrType::I64 && pt_ir != ir::IrType::U64
                    && pt_ir != ir::IrType::PTR) {
                v_qword = cast_if_needed(v_param, pt_ir, ir::IrType::I64,
                                          fd->loc.line);
            }
            qword_args.push_back(v_qword);
        }

        // 2b.2: Cargar la direccion del helper en un SSA value PTR.
        const ir::IrValueId v_pc = emit_label_addr(helper_name, fd->loc.line);

        // SPAWN_ARGS dedicado.  El emisor IR usa parallel-move para
        // resolver conflictos al colocar args en sus regs destino.
        // Operands: [r_pc, fut, arg1, arg2, ..., argN]
        const ir::IrValueId v_child = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::SPAWN_ARGS;
            ins.type        = ir::IrType::I64;
            ins.dst         = v_child;
            ins.operands.reserve(2 + n_params);
            ins.operands.push_back(v_pc);   // r_pc
            ins.operands.push_back(v_fut);  // R1 = fut
            for (auto v : qword_args)        // R2..R[N+1] = args
                ins.operands.push_back(v);
            ins.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ins));
        }
        (void)v_child; // no usado mas; el child ya esta ejecutando

        // 2c. return fut.
        {
            ir::IrInstr ret{};
            ret.op          = ir::IrOp::RET;
            ret.type        = ir::IrType::I64;
            ret.operands    = {v_fut};
            ret.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ret));
            block_terminated_ = true;
        }

        pop_scope();
        propagate_is_gc_object_through_phis(wrapper_fn);
        out.add_function(std::move(wrapper_fn));

        // Restaurar el contexto (aunque ya estamos al final de la funcion).
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        async_fut_id_         = saved_async_fut;
    }

    ir::IrValueId Lowering::lower_string_lit(ast::StringLitExpr *e) {
        if (!out_mod_) {
            error_at(e->loc, "lowering: out_mod_ nulo al bajar StringLitExpr");
            return ir::IR_NO_VALUE;
        }
        // Convertir el contenido resuelto a vector<uint8_t> y registrarlo
        // (deduplicado) en static_data.  Los duplicados retornan el mismo
        // indice, ahorrando bytes en el .vel emitido.
        std::vector<uint8_t> bytes(e->value.begin(), e->value.end());
        const uint64_t       idx = out_mod_->intern_static_data(std::move(bytes));

        // Emitir IrOp::STR_LIT_ADDR -> el emisor genera "mov rDst, @Absolute(\"code.s_<idx>\")".
        const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr         ins{};
        ins.op          = ir::IrOp::STR_LIT_ADDR;
        ins.type        = ir::IrType::PTR;
        ins.dst         = dst;
        ins.imm         = idx;
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        return dst;
    }

    // ---------------------------------------------------------------------
    // POO: clases en Vex. la integracion completa
    // (registro en module init, NEWOBJ + CALLVIRT, GETFIELD/SETFIELD)
    // se implementa por fases.  Cada metodo nuevo emite un error claro
    // hasta que su implementacion concreta este lista.
    // ---------------------------------------------------------------------

    // Forward decls de helpers definidos en lowering_builtin.cpp
    uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);
    uint64_t intern_class_cache_slot(ir::IrModule &mod, const std::string &name);

    // Forward decl: definida mas abajo en este TU
    ir::IrValueId emit_field_addr(ir::IrFunction *fn, ir::IrBlockId block,
                                   ir::IrValueId base, uint64_t offset,
                                   uint32_t source_line);

} // namespace vex
