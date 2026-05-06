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
 * @file mailbox.cpp
 * @brief Implementacion del buzon de mensajes por proceso (IPC/distribuido).
 */

#include "distrib/mailbox.h"

namespace distrib {

Mailbox::Mailbox(size_t max_msgs, size_t max_bytes)
    : bytes_used_(0)
    , max_msgs_(max_msgs)
    , max_bytes_(max_bytes)
{}

bool Mailbox::push(uint64_t pid, const uint8_t *buf, size_t len) {
    std::lock_guard<std::mutex> lk(mtx_);

    // verificar limites antes de encolar para evitar agotamiento de memoria
    if (queue_.size() >= max_msgs_) return false;
    if (bytes_used_ + len > max_bytes_) return false;

    queue_.emplace_back(pid, buf, len); // copia los datos al MailboxMsg
    bytes_used_ += len;
    return true;
}

MailboxMsg Mailbox::try_pop() {
    std::lock_guard<std::mutex> lk(mtx_);

    if (queue_.empty()) {
        // retornar mensaje vacio: el llamador detecta buzon vacio con data.empty()
        return MailboxMsg(0, nullptr, 0);
    }

    MailboxMsg msg = std::move(queue_.front());
    queue_.pop_front();
    bytes_used_ -= msg.data.size();
    return msg;
}

bool Mailbox::empty() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.empty();
}

size_t Mailbox::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.size();
}

size_t Mailbox::bytes_used() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return bytes_used_;
}

} // namespace distrib
