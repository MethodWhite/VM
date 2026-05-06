/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file ast.cpp
 * @brief Implementaciones auxiliares del AST de Vex.
 *
 * Aqui viven los helpers de mapeo TokenKind -> BinOp / AssignOp.  Se
 * sacan del header para evitar inflar las traducciones que solo
 * incluyen ast.h (parser, type checker, lowering los necesitan, pero
 * los tests unitarios del lexer no).
 *
 * Optimizacion: ambas funciones usan switch sobre TokenKind (jump table)
 * y son ramas pequenyas.  Marcadas en el header como noexcept para
 * facilitar inlining cuando el llamador y el callee comparten TU.
 */

#include "vex/ast.h"

namespace vex::ast {

    bool binop_from_token(TokenKind k, BinOp &out) noexcept {
        // Switch convertido a jump table por el compilador.
        // Devuelve false en cualquier token que no sea operador binario.
        switch (k) {
            case TokenKind::PLUS:    out = BinOp::Add; return true;
            case TokenKind::MINUS:   out = BinOp::Sub; return true;
            case TokenKind::STAR:    out = BinOp::Mul; return true;
            case TokenKind::SLASH:   out = BinOp::Div; return true;
            case TokenKind::PERCENT: out = BinOp::Mod; return true;

            case TokenKind::EQ:  out = BinOp::Eq;  return true;
            case TokenKind::NEQ: out = BinOp::Neq; return true;
            case TokenKind::LT:  out = BinOp::Lt;  return true;
            case TokenKind::LE:  out = BinOp::Le;  return true;
            case TokenKind::GT:  out = BinOp::Gt;  return true;
            case TokenKind::GE:  out = BinOp::Ge;  return true;

            case TokenKind::AND_AND: out = BinOp::LogicalAnd; return true;
            case TokenKind::OR_OR:   out = BinOp::LogicalOr;  return true;

            case TokenKind::AMP:   out = BinOp::BitAnd; return true;
            case TokenKind::PIPE:  out = BinOp::BitOr;  return true;
            case TokenKind::CARET: out = BinOp::BitXor; return true;
            case TokenKind::SHL:   out = BinOp::Shl;    return true;
            case TokenKind::SHR:   out = BinOp::Shr;    return true;

            default: return false;
        }
    }

    bool assignop_from_token(TokenKind k, AssignOp &out) noexcept {
        // Mismo patron: switch + return rapido.
        switch (k) {
            case TokenKind::ASSIGN:         out = AssignOp::Assign;       return true;
            case TokenKind::PLUS_ASSIGN:    out = AssignOp::AddAssign;    return true;
            case TokenKind::MINUS_ASSIGN:   out = AssignOp::SubAssign;    return true;
            case TokenKind::STAR_ASSIGN:    out = AssignOp::MulAssign;    return true;
            case TokenKind::SLASH_ASSIGN:   out = AssignOp::DivAssign;    return true;
            case TokenKind::PERCENT_ASSIGN: out = AssignOp::ModAssign;    return true;
            case TokenKind::AMP_ASSIGN:     out = AssignOp::BitAndAssign; return true;
            case TokenKind::PIPE_ASSIGN:    out = AssignOp::BitOrAssign;  return true;
            case TokenKind::CARET_ASSIGN:   out = AssignOp::BitXorAssign; return true;
            case TokenKind::SHL_ASSIGN:     out = AssignOp::ShlAssign;    return true;
            case TokenKind::SHR_ASSIGN:     out = AssignOp::ShrAssign;    return true;
            default: return false;
        }
    }

} // namespace vex::ast
