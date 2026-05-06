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
 * @file mailbox.h
 * @brief Buzon de mensajes por proceso para comunicacion entre procesos (IPC).
 *
 * Cada proceso que usa msgsend/msgrecv tiene un Mailbox asociado.
 * El Mailbox es una cola FIFO thread-safe con bloqueo en recepcion.
 *
 * Mensajes locales: el proceso emisor escribe directamente en el mailbox del receptor.
 * Mensajes remotos: el DistRuntime recibe el paquete VDP_MSGSEND y lo deposita
 *   en el mailbox del proceso destino via Mailbox::push().
 *
 * Un proceso bloqueado en msgrecv es despertado por make_ready() al llegar un mensaje.
 */

#ifndef MAILBOX_H
#define MAILBOX_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace distrib {

/**
 * @brief Mensaje almacenado en el buzon.
 *
 * Contiene el PID codificado del emisor y los bytes del mensaje.
 * Los bytes se copian al construir el mensaje (el emisor puede liberar su buffer).
 */
struct MailboxMsg {
    uint64_t             sender_pid; // PID codificado del emisor (local o remoto)
    std::vector<uint8_t> data;       // copia de los bytes del mensaje

    /**
     * @brief Construye un mensaje copiando los datos del buffer indicado.
     * @param pid    PID codificado del emisor.
     * @param buf    Puntero al buffer con los datos.
     * @param len    Longitud del buffer en bytes.
     */
    MailboxMsg(uint64_t pid, const uint8_t *buf, size_t len)
        : sender_pid(pid), data(buf, buf + len) {}
};

/**
 * @brief Buzon de mensajes thread-safe para un ProcessVM.
 *
 * Usa un mutex + condicion para bloquear al receptor cuando el buzon esta vacio.
 * El tamano maximo evita que un proceso malintencionado llene la memoria.
 */
class Mailbox {
public:
    static constexpr size_t DEFAULT_MAX_MSGS = 1024; // limite de mensajes encolados
    static constexpr size_t DEFAULT_MAX_BYTES = 64 * 1024 * 1024; // 64 MiB total encolados

    /**
     * @brief Construye el buzon con los limites indicados.
     * @param max_msgs  Maximo de mensajes encolados simultaneamente.
     * @param max_bytes Maximo de bytes totales encolados.
     */
    explicit Mailbox(size_t max_msgs = DEFAULT_MAX_MSGS,
                     size_t max_bytes = DEFAULT_MAX_BYTES);

    /**
     * @brief Encola un mensaje en el buzon.
     *
     * No bloquea.  Si el buzon esta lleno (limite de mensajes o bytes),
     * descarta el mensaje y retorna false.
     *
     * @param pid  PID codificado del emisor.
     * @param buf  Puntero a los datos del mensaje.
     * @param len  Longitud del mensaje en bytes.
     * @return true si el mensaje fue encolado; false si el buzon esta lleno.
     */
    bool push(uint64_t pid, const uint8_t *buf, size_t len);

    /**
     * @brief Extrae el primer mensaje de la cola (no bloquea).
     *
     * Si el buzon esta vacio retorna un MailboxMsg con sender_pid=0 y data vacia.
     * El llamador debe verificar data.empty() para detectar buzon vacio.
     *
     * @return Primer mensaje de la cola, o mensaje vacio si no hay ninguno.
     */
    MailboxMsg try_pop();

    /**
     * @brief Indica si el buzon no tiene mensajes encolados.
     * @return true si el buzon esta vacio.
     */
    bool empty() const;

    /**
     * @brief Numero de mensajes encolados actualmente.
     * @return Cantidad de mensajes pendientes.
     */
    size_t size() const;

    /**
     * @brief Bytes totales encolados actualmente.
     * @return Suma de los tamanos de todos los mensajes encolados.
     */
    size_t bytes_used() const;

private:
    mutable std::mutex   mtx_;        // protege el acceso a queue_ y bytes_used_
    std::deque<MailboxMsg> queue_;    // cola FIFO de mensajes
    size_t               bytes_used_; // bytes totales de datos encolados
    const size_t         max_msgs_;   // limite de mensajes
    const size_t         max_bytes_;  // limite de bytes
};

} // namespace distrib

#endif // MAILBOX_H
