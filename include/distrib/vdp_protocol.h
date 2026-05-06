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
 * @file vdp_protocol.h
 * @brief Definiciones del protocolo VDP (Vesta Distribution Protocol).
 *
 * VDP es el protocolo de red propietario de VestaVM para comunicacion
 * distribuida entre nodos.  Puede funcionar sobre TCP plano o TLS.
 *
 * Formato del paquete:
 *   [VdpHeader (24 bytes)][payload (variable)][hmac (16 bytes, opcional)]
 *
 * Autenticacion (configurable):
 *   - Token:  challenge-response CRAM sobre SHA-256
 *   - mTLS:   certificados X.509 a nivel de transporte
 *   - Ambos:  token como segunda capa sobre mTLS
 */

#ifndef VDP_PROTOCOL_H
#define VDP_PROTOCOL_H

#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Utilidad de empaquetado de structs
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#define VDP_PACKED
#define VDP_PACKED_BEGIN __pragma(pack(push, 1))
#define VDP_PACKED_END   __pragma(pack(pop))
#else
#define VDP_PACKED       __attribute__((packed))
#define VDP_PACKED_BEGIN
#define VDP_PACKED_END
#endif

namespace distrib {

// ---------------------------------------------------------------------------
// Constantes magicas del protocolo
// ---------------------------------------------------------------------------
static constexpr uint32_t VDP_MAGIC         = 0x00504456u; // "VDP\0" en LE
static constexpr uint8_t  VDP_VERSION       = 0x01;
static constexpr uint16_t VDP_LISTEN_PORT   = 7789;        // puerto por defecto del servidor VDP
static constexpr uint16_t VDP_DISCOVER_PORT = 7790;        // puerto UDP para descubrimiento dinamico
static constexpr uint32_t VDP_PAGE_SIZE     = 4096;        // tamano de pagina para memsync
static constexpr uint32_t VDP_MAX_NODES     = 256;         // limite de nodos en el registro
static constexpr uint32_t VDP_MAX_PAYLOAD   = 64 * 1024 * 1024; // 64 MiB por paquete
static constexpr uint32_t VDP_SEQ_NONE      = 0;           // seq_num sin ACK requerido

// ---------------------------------------------------------------------------
// Flags de cabecera (VdpHeader::flags)
// ---------------------------------------------------------------------------
static constexpr uint8_t VDP_FLAG_TLS       = 0x01; // sesion protegida por TLS
static constexpr uint8_t VDP_FLAG_AUTH_TOKEN= 0x02; // autenticacion por token habilitada
static constexpr uint8_t VDP_FLAG_AUTH_CERT = 0x04; // autenticacion por certificado habilitada
static constexpr uint8_t VDP_FLAG_HMAC      = 0x08; // paquete incluye HMAC-SHA256 al final
static constexpr uint8_t VDP_FLAG_COMPRESSED= 0x10; // payload comprimido (reservado para v2)

// ---------------------------------------------------------------------------
// Flags de autenticacion (VdpPayloadHello::auth_flags)
// ---------------------------------------------------------------------------
static constexpr uint8_t VDP_AUTH_NONE      = 0x00; // sin autenticacion (solo en redes de confianza)
static constexpr uint8_t VDP_AUTH_TOKEN     = 0x01; // acepta autenticacion por token
static constexpr uint8_t VDP_AUTH_CERT      = 0x02; // requiere mTLS (certificado de cliente)
static constexpr uint8_t VDP_AUTH_BOTH      = 0x03; // token + mTLS

// ---------------------------------------------------------------------------
// Tipos de mensaje (VdpHeader::msg_type)
// ---------------------------------------------------------------------------
enum class VdpMsgType : uint16_t {
    // -- Handshake y autenticacion --
    HELLO           = 0x0001, // presentacion inicial del nodo
    HELLO_ACK       = 0x0002, // respuesta del servidor con nonce de desafio
    AUTH_TOKEN      = 0x0003, // respuesta al desafio (CRAM SHA-256)
    AUTH_OK         = 0x0004, // sesion establecida correctamente
    AUTH_FAIL       = 0x0005, // fallo de autenticacion
    PING            = 0x0006, // keepalive
    PONG            = 0x0007, // respuesta a PING
    DISCONNECT      = 0x00FF, // cierre limpio de sesion

    // -- Creacion de procesos remotos --
    RSPAWN          = 0x0010, // crear proceso en nodo remoto
    RSPAWN_ACK      = 0x0011, // PID del proceso creado o error

    // -- Mensajes entre procesos --
    MSGSEND         = 0x0020, // enviar mensaje a un proceso
    MSGSEND_ACK     = 0x0021, // confirmacion de entrega (at-least-once)

    // -- Sincronizacion de memoria --
    MEMSYNC_HASH    = 0x0030, // tabla de hashes de paginas (Adler32 + BLAKE2s)
    MEMSYNC_DIFF    = 0x0031, // indices de paginas que difieren en el destino
    MEMSYNC_DATA    = 0x0032, // datos de paginas que deben actualizarse
    MEMSYNC_ACK     = 0x0033, // sincronizacion completada

    // -- Futuros distribuidos --
    FUTURE_FULFILL  = 0x0040, // resolver un future en el nodo origen
    FUTURE_REJECT   = 0x0041, // rechazar un future en el nodo origen

    // -- Configuracion remota de VM --
    VM_CONFIG       = 0x0050, // cambiar parametro de configuracion
    VM_CONFIG_ACK   = 0x0051, // confirmacion de configuracion

    // -- Descubrimiento de nodos (solo UDP) --
    NODE_DISCOVER   = 0x0060, // broadcast de busqueda
    NODE_ANNOUNCE   = 0x0061, // respuesta de presencia
};

// ---------------------------------------------------------------------------
// Codigos de error VDP
// ---------------------------------------------------------------------------
enum class VdpError : uint32_t {
    OK                  = 0,
    AUTH_FAILED         = 1,  // token incorrecto o certificado rechazado
    SESSION_EXPIRED     = 2,  // sesion caducada
    NODE_NOT_FOUND      = 3,  // nodo desconocido
    PROCESS_NOT_FOUND   = 4,  // proceso destino no existe
    PAYLOAD_TOO_LARGE   = 5,  // payload supera VDP_MAX_PAYLOAD
    SPAWN_FAILED        = 6,  // error al crear el proceso remoto
    MEMSYNC_ADDR_ERR    = 7,  // direccion de memoria invalida en destino
    INTERNAL_ERROR      = 255,
};

// ---------------------------------------------------------------------------
// Cabecera VDP: 24 bytes fijos, precede a cualquier payload
// ---------------------------------------------------------------------------
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpHeader {
    uint8_t  magic[4];    // "VDP\0" = {0x56,0x44,0x50,0x00}
    uint8_t  version;     // VDP_VERSION = 0x01
    uint8_t  flags;       // VDP_FLAG_* bitmask
    uint16_t msg_type;    // VdpMsgType (little-endian)
    uint64_t session_id;  // identificador de sesion aleatorio de 64 bits (0 = sin sesion)
    uint32_t seq_num;     // numero de secuencia; el receptor devuelve el mismo en el ACK
    uint32_t payload_len; // bytes de payload que siguen a la cabecera
} VdpHeader;
VDP_PACKED_END
static_assert(sizeof(VdpHeader) == 24, "VdpHeader debe medir exactamente 24 bytes");

// ---------------------------------------------------------------------------
// Payloads de handshake / autenticacion
// ---------------------------------------------------------------------------

// Payload de VDP_HELLO (enviado por el cliente al conectarse)
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadHello {
    uint64_t node_id;       // identificador unico de 64 bits del nodo emisor
    uint16_t vdp_version;   // version del protocolo VDP soportada por el cliente
    uint16_t listen_port;   // puerto TCP del servidor VDP del cliente (para conexiones inversas)
    uint8_t  auth_flags;    // VDP_AUTH_* bitmask: metodos de autenticacion soportados
    uint8_t  _pad[3];
    char     node_name[24]; // nombre legible del nodo (maximo 23 caracteres + nul)
} VdpPayloadHello;
VDP_PACKED_END

// Payload de VDP_HELLO_ACK (enviado por el servidor como respuesta al HELLO)
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadHelloAck {
    uint64_t session_id;    // ID de sesion asignado por el servidor (aleatorio)
    uint64_t server_node_id;// ID del nodo servidor
    uint8_t  nonce[32];     // desafio aleatorio de 32 bytes para CRAM SHA-256
} VdpPayloadHelloAck;
VDP_PACKED_END

// Payload de VDP_AUTH_TOKEN (respuesta al desafio CRAM)
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadAuthToken {
    // CRAM: SHA256(stored_token_hash || ":" || nonce_as_hex_string)
    uint8_t response[32];
} VdpPayloadAuthToken;
VDP_PACKED_END

// Payload de VDP_AUTH_OK
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadAuthOk {
    uint64_t session_id;    // confirmacion del session_id activo
    uint32_t server_caps;   // capacidades del servidor (reservado, usar 0)
    uint32_t _pad;
} VdpPayloadAuthOk;
VDP_PACKED_END

// Payload de VDP_AUTH_FAIL
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadAuthFail {
    uint32_t error_code;    // VdpError::AUTH_FAILED u otro
    char     message[28];   // descripcion ASCII del motivo
} VdpPayloadAuthFail;
VDP_PACKED_END

// ---------------------------------------------------------------------------
// Payloads de procesos remotos
// ---------------------------------------------------------------------------

// Flags del campo VdpPayloadRspawn::flags
static constexpr uint32_t RSPAWN_FLAG_VELB = 0x01; // el payload es un fichero .velb completo (usa load_executable en remoto)

// Payload de VDP_RSPAWN: cabecera seguida de code_size bytes de bytecode
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadRspawn {
    uint64_t future_id;   // ID opaco del future en el nodo origen (correlacion con ACK)
    uint64_t fn_offset;   // offset del punto de entrada dentro del bloque de bytecode
    uint64_t code_size;   // tamano del bloque de bytecode que sigue al header
    uint32_t argc;        // numero de argumentos pasados (r1..rargc)
    uint32_t flags;       // RSPAWN_FLAG_* bitmask (0 = bytecode raw, RSPAWN_FLAG_VELB = fichero .velb)
    uint64_t args[12];    // valores de r1..r12 para el proceso remoto
} VdpPayloadRspawn;
VDP_PACKED_END

// Payload de VDP_RSPAWN_ACK
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadRspawnAck {
    uint64_t future_id;   // ID del future original (para correlacionar con el pending map)
    uint64_t remote_pid;  // PID del proceso creado (0 si error)
    uint32_t error_code;  // VdpError::OK o codigo de fallo
    uint32_t _pad;
} VdpPayloadRspawnAck;
VDP_PACKED_END

// ---------------------------------------------------------------------------
// Payloads de mensajes entre procesos
// ---------------------------------------------------------------------------

// Payload de VDP_MSGSEND: cabecera seguida de data_size bytes del mensaje
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadMsgsend {
    uint64_t target_pid;  // PID del proceso destino en el nodo remoto
    uint64_t data_size;   // tamano del mensaje en bytes
} VdpPayloadMsgsend;
VDP_PACKED_END

// Payload de VDP_MSGSEND_ACK
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadMsgsendAck {
    uint32_t seq_num;     // numero de secuencia del MSGSEND original
    uint32_t error_code;  // VdpError::OK o codigo de fallo
} VdpPayloadMsgsendAck;
VDP_PACKED_END

// ---------------------------------------------------------------------------
// Payloads de sincronizacion de memoria
// ---------------------------------------------------------------------------

// Estructura de hash de una pagina (usada en VDP_MEMSYNC_HASH)
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPageHash {
    uint32_t page_idx;    // indice de pagina (0-based desde la dir base de la region)
    uint32_t adler32;     // hash rapido Adler-32 para pre-filtrado
    uint8_t  blake2s[32]; // hash fuerte BLAKE2s-256 para verificacion definitiva
} VdpPageHash;
VDP_PACKED_END
static_assert(sizeof(VdpPageHash) == 40, "VdpPageHash debe medir 40 bytes");

// Payload de VDP_MEMSYNC_HASH: cabecera seguida de page_count entradas VdpPageHash
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadMemsyncHash {
    uint64_t local_addr;  // dir virtual local del nodo que envia (informativa)
    uint64_t remote_addr; // dir virtual base en el nodo destino (donde aplicar los datos)
    uint64_t region_size; // tamano total de la region en bytes
    uint32_t page_count;  // numero de entradas VdpPageHash que siguen
    uint32_t _pad;
} VdpPayloadMemsyncHash;
VDP_PACKED_END

// Payload de VDP_MEMSYNC_DIFF: lista de page_idx que difieren, seguida de diff_count uint32_t
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadMemsyncDiff {
    uint32_t diff_count;  // numero de paginas que difieren (y que se esperan en MEMSYNC_DATA)
    uint32_t _pad;
} VdpPayloadMemsyncDiff;
VDP_PACKED_END

// Payload de VDP_MEMSYNC_DATA: cabecera seguida de page_count bloques [uint32_t page_idx + page_size bytes]
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadMemsyncData {
    uint64_t remote_addr; // dir virtual base del destino (donde escribir)
    uint32_t page_count;  // numero de bloques de pagina que siguen
    uint32_t page_size;   // tamano de cada pagina en bytes (normalmente VDP_PAGE_SIZE)
} VdpPayloadMemsyncData;
VDP_PACKED_END

// Payload de VDP_MEMSYNC_ACK
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadMemsyncAck {
    uint32_t pages_written; // paginas realmente escritas en el destino
    uint32_t error_code;    // VdpError::OK o codigo de fallo
} VdpPayloadMemsyncAck;
VDP_PACKED_END

// ---------------------------------------------------------------------------
// Payloads de futuros distribuidos
// ---------------------------------------------------------------------------

// Payload de VDP_FUTURE_FULFILL
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadFutureFulfill {
    uint64_t future_id;   // ID del future en el nodo origen (el que llamo rspawn)
    uint64_t result;      // valor del resultado
} VdpPayloadFutureFulfill;
VDP_PACKED_END

// Payload de VDP_FUTURE_REJECT
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadFutureReject {
    uint64_t future_id;   // ID del future en el nodo origen
    uint64_t error_code;  // codigo de error
} VdpPayloadFutureReject;
VDP_PACKED_END

// ---------------------------------------------------------------------------
// Payloads de configuracion remota de VM
// ---------------------------------------------------------------------------

// Claves de configuracion para VDP_VM_CONFIG
enum class VdpConfigKey : uint32_t {
    MAX_PROCESSES   = 1, // limite de procesos concurrentes
    GC_THRESHOLD    = 2, // umbral del GC en bytes
    LOG_LEVEL       = 3, // nivel de log (0=off, 1=error, 2=info, 3=debug)
    SCHED_QUANTUM   = 4, // quantum del scheduler en reducciones
};

// Payload de VDP_VM_CONFIG
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadVmConfig {
    uint32_t config_key;  // VdpConfigKey
    uint32_t _pad;
    uint64_t int_value;   // valor entero (si aplica)
    char     str_value[48]; // valor en cadena (si aplica, max 47 + nul)
} VdpPayloadVmConfig;
VDP_PACKED_END

// ---------------------------------------------------------------------------
// Payloads de descubrimiento dinamico (UDP broadcast)
// ---------------------------------------------------------------------------

// VDP_NODE_DISCOVER: enviado como broadcast UDP por un nodo que busca pares
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadNodeDiscover {
    uint8_t  magic[4];    // "VDPD"
    uint64_t node_id;     // ID del nodo que busca pares
    uint16_t reply_port;  // puerto UDP al que responder con NODE_ANNOUNCE
    uint16_t _pad;
} VdpPayloadNodeDiscover;
VDP_PACKED_END

// VDP_NODE_ANNOUNCE: enviado como unicast UDP como respuesta a NODE_DISCOVER
VDP_PACKED_BEGIN
typedef struct VDP_PACKED VdpPayloadNodeAnnounce {
    uint8_t  magic[4];    // "VDPA"
    uint64_t node_id;     // ID del nodo anunciante
    uint16_t vdp_port;    // puerto TCP del servidor VDP
    uint8_t  auth_flags;  // metodos de autenticacion aceptados
    uint8_t  _pad;
    char     node_name[24]; // nombre legible (max 23 + nul)
} VdpPayloadNodeAnnounce;
VDP_PACKED_END

// ---------------------------------------------------------------------------
// Estructura MemsyncParams: leida desde memoria VM por la instruccion memsync
//
// El bytecode coloca esta estructura en VM memory y pasa el puntero en r_params.
// Debe estar alineada a 8 bytes en la memoria virtual del proceso.
// ---------------------------------------------------------------------------
VDP_PACKED_BEGIN
typedef struct VDP_PACKED MemsyncParams {
    uint64_t local_addr;    // dir virtual local de inicio de la region a sincronizar
    uint64_t len;           // tamano de la region en bytes (recomendado multiplo de PAGE_SIZE)
    uint32_t node_idx;      // indice del nodo destino en NodeRegistry
    uint32_t _pad;
    uint64_t remote_addr;   // dir virtual en el nodo remoto donde escribir
    uint32_t future_handle; // GcHandle del future a resolver al terminar (0 = sincrono)
    uint32_t _pad2;
} MemsyncParams;
VDP_PACKED_END

// ---------------------------------------------------------------------------
// Codificacion de PIDs remotos como uint64_t (para instruccion msgsend)
//
// bit 63: 1 = PID remoto, 0 = PID local
// bits 62-32: indice del nodo en NodeRegistry (31 bits, max 2147483648 nodos)
// bits 31-0: PID local en el nodo remoto
// ---------------------------------------------------------------------------
static constexpr uint64_t VDP_REMOTE_PID_BIT = (1ULL << 63);

inline uint64_t vdp_make_remote_pid(uint32_t node_idx, uint64_t local_pid) {
    return VDP_REMOTE_PID_BIT
         | ((static_cast<uint64_t>(node_idx) & 0x7FFFFFFFull) << 32)
         | (local_pid & 0xFFFFFFFFull);
}

inline bool vdp_is_remote_pid(uint64_t pid) {
    return (pid & VDP_REMOTE_PID_BIT) != 0;
}

inline uint32_t vdp_pid_node_idx(uint64_t pid) {
    return static_cast<uint32_t>((pid >> 32) & 0x7FFFFFFFull);
}

inline uint32_t vdp_pid_local(uint64_t pid) {
    return static_cast<uint32_t>(pid & 0xFFFFFFFFull);
}

} // namespace distrib

#endif // VDP_PROTOCOL_H
