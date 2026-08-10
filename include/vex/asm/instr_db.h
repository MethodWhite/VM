/**
 * @file instr_db.h
 * @brief Base de datos de instrucciones EMBEBIDA en el compilador (autocontenida,
 *        sin archivos externos).
 *
 * Las tablas (sintaxis por-ISA, y mas adelante coste por microarquitectura) las
 * genera @c tools/import/gen_cpp_db.py a partir de la DB (@c arch-data) y se
 * compilan DENTRO del compilador como @c .rodata estatica.  Asi el analisis de
 * asm (ASA), el LSP (hover) y el optimizer JIT/AOT resuelven cada instruccion a
 * su FORMA (identidad ISA) sin depender de ficheros ni parsear texto en runtime.
 *
 * El FormID es el indice denso del orden lexicografico de la clave estructural
 * (misma identidad que la DB); un @c match(mnemonico, operandos) resuelve el
 * texto de un @c asm { } a su FormID.
 */
#ifndef VEX_INSTR_DB_H
#define VEX_INSTR_DB_H

#include <cstdint>
#include <string>
#include <vector>

namespace vex {
namespace instr_db {

/// Clase de operando (debe casar con @c _KIND del generador).
enum DbOpKind : uint8_t {
    OP_REG = 0, OP_MEM = 1, OP_IMM = 2, OP_AGEN = 3, OP_RELBR = 4,
    OP_ABSBR = 5, OP_FLAGS = 7,
};

/// Bits de overlay (semantica que el encoding no da; casa con @c _OVL).
enum DbOverlayBit : uint16_t {
    OVL_BARRIER = 1u << 0, OVL_SERIALIZING = 1u << 1, OVL_ATOMIC = 1u << 2,
    OVL_LL_SC = 1u << 3, OVL_MEM_ACQUIRE = 1u << 4, OVL_MEM_RELEASE = 1u << 5,
    OVL_MEM_SEQ_CST = 1u << 6, OVL_NO_REORDER = 1u << 7, OVL_BRANCH = 1u << 8,
    OVL_CALL = 1u << 9, OVL_RET = 1u << 10, OVL_SYSCALL = 1u << 11,
};

/// Un operando de una forma (kind + ancho en bits + flags r/w/impl/suppr).
struct DbOperand {
    uint8_t kind;    ///< DbOpKind.
    uint16_t width;  ///< bits (0 si no aplica).
    uint8_t flags;   ///< bit0=read, bit1=write, bit2=implicit, bit3=suppressed.
    uint16_t regset; ///< indice a kStr del conjunto de registros permitido
                     ///< (p.ej. "AX", "DX", "GPR64", "-").  Para operandos
                     ///< IMPLICITOS de registro fijo nombra el registro
                     ///< concreto (rax:rdx de div, rax de cmpxchg) sin
                     ///< re-derivarlo a mano.
};

/// Una FORMA (encoding) indexada por FormID.  Los campos string son indices al
/// pool @c kStr de la ISA.
struct DbForm {
    uint32_t iclass;   ///< mnemonico (indice a kStr).
    uint16_t ext;      ///< extension (indice a kStr).
    uint16_t overlay;  ///< bitmask DbOverlayBit.
    uint8_t rmask;     ///< operandos leidos (bit i = operando i).
    uint8_t wmask;     ///< operandos escritos.
    uint8_t memflags;  ///< bit0=mem bit1=imm bit2=wflags bit3=rflags.
    uint32_t ops_off;  ///< offset en el pool de operandos (>64K -> 32 bits).
    uint8_t ops_count; ///< numero de operandos.
    uint32_t opcode;   ///< opcode (indice a kStr; documentacion).
};

/// Rango de FormIDs con el mismo iclass (para el matcher; ordenado por nombre).
struct DbIclassRange {
    uint32_t iclass;    ///< indice a kStr.
    uint32_t first_fid; ///< primer FormID del rango.
    uint32_t count;     ///< numero de formas.
};

/// Operando parseado de una linea de asm (entrada del matcher).
struct ParsedOp {
    DbOpKind kind = OP_REG;
    uint16_t width = 0; ///< 0 = desconocido (no restringe).
};

/// ISA soportadas por la DB embebida (ARM64 = AArch64, ARM32 = A32/T32).
enum class Isa { X86, ARM64, ARM32, RISCV };

/// Punteros a las tablas generadas de una ISA.  Lo rellena el @c .cpp generado
/// (@c gen/instr_db_<isa>_gen.cpp) via su accesor @c db_<isa>(); el resto del
/// modulo lo consume sin conocer las tablas concretas.
struct IsaData {
    const char *const *str = nullptr;
    unsigned str_count = 0;
    const DbOperand *ops = nullptr;
    unsigned ops_count = 0;
    const DbForm *forms = nullptr;
    unsigned form_count = 0;
    const DbIclassRange *iclass = nullptr;
    unsigned iclass_count = 0;
};

/// Accesores de las tablas de cada ISA (definidos en @c gen/instr_db_*_gen.cpp).
const IsaData &db_x86();
const IsaData &db_arm64();
const IsaData &db_arm32();
const IsaData &db_riscv();

// ------------------------------------------------------------------------
// Capa de COSTE por microarquitectura (latencia + puertos = ejecucion
// paralela superescalar).  La consume el optimizer (scheduling) y el LSP
// (hover: coste por microarq).  Tablas generadas en gen/instr_db_<isa>_cost_gen.cpp.
// ------------------------------------------------------------------------

/// Uso de un puerto de ejecucion por una clase (para el modelo superescalar):
/// @c port indexa el legado de puertos de la microarquitectura.
struct AsmPortSlot {
    uint8_t port;  ///< indice al legado de puertos de la microarq.
    float uops;    ///< uops repartidos a ese grupo de puertos.
};

/// Clase de scheduling deduplicada (formas con el mismo coste comparten clase).
struct AsmClass {
    float recip_tp;    ///< throughput reciproco (1/IPC).
    float latency;     ///< latencia maxima (proxy del camino critico del nodo).
    float div_cycles;  ///< ciclos de division (-1 si no aplica).
    uint16_t uops;     ///< uops emitidas.
    uint8_t flags;     ///< bit0 microcoded, bit1 macro_fusible.
    uint16_t ports_off; ///< offset en el pool de AsmPortSlot de la microarq.
    uint8_t ports_count;
};

/// Tablas de coste de UNA microarquitectura (las rellena el .cpp generado).
struct MicroarchData {
    const char *name;
    const char *const *port_names; ///< legado: indice -> nombre de puerto.
    uint16_t port_count;
    const AsmClass *classes;
    uint16_t class_count;
    const AsmPortSlot *slots; ///< pool de puertos de todas las clases.
    const int16_t *form_class; ///< FormID -> class_id (-1 = sin coste aqui).
    uint32_t form_count;
};

/// Lista de microarquitecturas de una ISA (accesor del .cpp de coste generado).
struct CostData {
    const MicroarchData *uarchs = nullptr;
    uint32_t count = 0;
};
const CostData &cost_x86();
const CostData &cost_arm64();
const CostData &cost_arm32();
const CostData &cost_riscv();

/// Coste resuelto de una forma en una microarquitectura (resultado publico).
struct AsmCost {
    bool found = false;         ///< false = la microarq no cronometra esta forma.
    float recip_tp = 0.0f;      ///< throughput reciproco.
    float latency = 0.0f;       ///< latencia (camino critico del nodo).
    float div_cycles = -1.0f;
    uint16_t uops = 0;
    bool microcoded = false;
    bool macro_fusible = false;
    /// Puertos usados (para el modelo de ejecucion paralela).  Punteros a la
    /// tabla de la microarq (sin copia); @c ports_count entradas.
    const AsmPortSlot *ports = nullptr;
    uint8_t ports_count = 0;
    const char *const *port_names = nullptr; ///< resuelve port index -> nombre.
};

/// Numero de microarquitecturas con coste para la ISA.
uint32_t microarch_count(Isa isa);
/// Nombre de la microarquitectura @p ua_id (o "" si fuera de rango).
const char *microarch_name(Isa isa, uint32_t ua_id);
/// Indice de una microarquitectura por nombre (-1 si no existe).
int32_t microarch_by_name(Isa isa, const std::string &name);
/// Coste de la forma @p form_id en la microarquitectura @p ua_id.
AsmCost cost(Isa isa, int32_t form_id, uint32_t ua_id);

// ------------------------------------------------------------------------
// Capa de FEATURES por CPU: que extensiones de ISA implementa cada core
// (para saber que instrucciones admite -- especializacion de codegen).
// Tablas generadas en gen/instr_db_<isa>_feat_gen.cpp.
// ------------------------------------------------------------------------

/// Features (extensiones de ISA) que implementa una CPU real.
struct CpuFeatures {
    const char *name;      ///< nombre de la CPU.
    const char *sched;     ///< modelo de scheduling asociado (documentacion).
    const uint16_t *feats; ///< indices al pool de nombres de feature.
    uint16_t feat_count;
};

/// Tabla de features de una ISA (accesor del .cpp de features generado).
struct FeatData {
    const char *const *feat_names = nullptr;
    uint16_t feat_name_count = 0;
    const CpuFeatures *cpus = nullptr;
    uint16_t cpu_count = 0;
};
const FeatData &feat_x86();
const FeatData &feat_arm(); ///< AArch64 y A32/T32 comparten features.
const FeatData &feat_riscv();

/// Numero de CPUs con features para la ISA.
uint32_t cpu_count(Isa isa);
/// Nombre de la CPU @p cpu_id (o "" si fuera de rango).
const char *cpu_name(Isa isa, uint32_t cpu_id);
/// Indice de una CPU por nombre (-1 si no existe).
int32_t cpu_by_name(Isa isa, const std::string &name);
/// La CPU @p cpu_id implementa la feature @p feature (extension de ISA).
bool cpu_has_feature(Isa isa, uint32_t cpu_id, const std::string &feature);

// ------------------------------------------------------------------------
// Coste de un BLOQUE de asm: modelo superescalar (latencia + presion de
// puertos = ejecucion en paralelo).  Lo consumen el LSP (hover: coste del
// bloque) y el optimizer (scheduling).
// ------------------------------------------------------------------------

/// Coste agregado de un bloque de asm en una microarquitectura.
struct AsmBlockCost {
    uint32_t instr_count = 0; ///< instrucciones (lineas no vacias/label).
    uint32_t matched = 0;     ///< con forma reconocida en la DB.
    uint32_t costed = 0;      ///< con coste en esta microarq.
    uint32_t total_uops = 0;  ///< suma de uops.
    /// Cota SUPERIOR de latencia (suma serie).  El camino critico EXACTO
    /// necesita el grafo de dependencias entre registros (futuro, ASA.2).
    float latency_sum = 0.0f;
    /// Ciclos por el modelo SUPERESCALAR: max entre la presion del puerto mas
    /// cargado (ejecucion paralela) y la suma de throughput reciproco.  Es la
    /// cota INFERIOR de ciclos del bloque bien planificado.
    float throughput = 0.0f;
    /// Presion por puerto (uops acumuladas por grupo de puertos): muestra que
    /// unidad de ejecucion es el cuello de botella.
    std::vector<std::pair<std::string, float>> port_pressure;
};

/// Analiza el coste de @p body (cuerpo de un @c asm) en la microarq @p ua_id:
/// empareja cada linea (@ref match_asm_line), acumula uops/latencia y reparte
/// las uops por puerto (modelo de ejecucion paralela superescalar).
AsmBlockCost analyze_asm_cost(Isa isa, const std::string &body, uint32_t ua_id);

// ------------------------------------------------------------------------
// Planificacion (scheduling) de un bloque de asm: analisis de dependencias
// + reordenacion valida por latencia/puertos.  SEGURIDAD: toda reordenacion
// respeta dependencias, barreras y memoria; lo no modelable es conservador.
// Funcion PURA (no reescribe codegen todavia): se valida antes de cablear.
// ------------------------------------------------------------------------

/// Semantica de UNA instruccion de asm (para el grafo de dependencias).
struct AsmInsnSem {
    int32_t form_id = -1;
    bool modeled = false;   ///< false = operandos implicitos / no emparejada ->
                            ///< se trata CONSERVADOR (no se reordena alrededor).
    bool barrier = false;   ///< overlay barrera/serializante/atomica/rama/call/
                            ///< ret/syscall: nada la cruza.
    std::vector<std::string> reads;  ///< registros canonicos leidos.
    std::vector<std::string> writes; ///< registros canonicos escritos.
    bool reads_mem = false, writes_mem = false;
    bool reads_flags = false, writes_flags = false;
    float latency = 0.0f;   ///< latencia en la microarq (prioridad del scheduler).
    std::string text;       ///< linea original (para reemitir).
};

/// Semantica de una instruccion de asm (@p line) en la microarq @p ua_id.
AsmInsnSem asm_insn_sem(Isa isa, const std::string &line, uint32_t ua_id);

/// ¿Hay dependencia entre @p a (antes) y @p b (despues) que OBLIGA a conservar
/// su orden?  True si RAW/WAR/WAW en registros, dependencia de flags, ambas
/// tocan memoria (conservador: no se sabe si solapan), o alguna es barrera / no
/// modelada.  Es la regla de SEGURIDAD del scheduler.
bool asm_dep_conflict(const AsmInsnSem &a, const AsmInsnSem &b);

/// Resultado de planificar un bloque de asm.
struct AsmSchedule {
    std::vector<uint32_t> order; ///< permutacion de indices (orden nuevo).
    bool moved = false;          ///< true si el orden cambio.
    bool valid = true;           ///< invariante: respeta TODAS las dependencias.
};

/// Planifica (list scheduling) las instrucciones de @p body por su altura de
/// camino critico (latencia), respetando dependencias y barreras.  No cambia la
/// semantica; devuelve una permutacion valida.  @p ua_id da las latencias.
AsmSchedule schedule_asm_block(Isa isa, const std::string &body, uint32_t ua_id);

/// Reordena las instrucciones de @p body (via @ref schedule_asm_block) y
/// devuelve el cuerpo REORDENADO **solo si es seguro**: sin labels (un salto a
/// una etiqueta rompe si algo la cruza), invariante @c valid, y el orden
/// realmente cambio.  En CUALQUIER otro caso devuelve @p body SIN TOCAR.  Es la
/// funcion que cablea el reoptimizador de asm de forma conservadora.
std::string reschedule_asm(Isa isa, const std::string &body, uint32_t ua_id);

/**
 * @brief Clasifica un operando de asm (token) a su @c (kind, width) para el
 *        matcher.  Conoce los registros de la ISA (x86 rax/eax/xmm...; ARM
 *        x/w/v...; RISC-V x0-31/ABI/f), la memoria (x86/ARM @c [...], RISC-V
 *        @c disp(reg)) y los inmediatos (numero, @c #imm de ARM).
 */
ParsedOp parse_operand(Isa isa, const std::string &token);

/**
 * @brief Empareja una LINEA de asm completa (mnemonico + operandos, sintaxis
 *        del ensamblador) a su FormID.  Parte el mnemonico, trocea los
 *        operandos (respetando @c [...]/@c (...)), los clasifica con
 *        @ref parse_operand y llama a @ref match.  Devuelve -1 si la linea no
 *        tiene mnemonico (label/vacia) o el mnemonico no existe.
 */
int32_t match_asm_line(Isa isa, const std::string &line);

/**
 * @brief Resuelve el texto de una instruccion (mnemonico + operandos) a su
 *        FormID en la DB de la ISA dada.  Devuelve -1 si el mnemonico no existe.
 *
 * Espeja el emparejador del analizador: busca el rango del iclass (binaria) y
 * puntua por clase+ancho de operando (aridad exacta); la forma de mayor
 * puntuacion gana.  Si el mnemonico existe pero ninguna forma casa por
 * operandos, devuelve la primera del rango (nivel mnemonico).
 */
int32_t match(Isa isa, const std::string &mnemonic,
              const std::vector<ParsedOp> &ops);

/// Nombre del iclass de una forma (o "" si el FormID no es valido).
const char *iclass_name(Isa isa, int32_t form_id);
/// Bitmask de overlay de una forma (0 si el FormID no es valido).
uint16_t overlay_of(Isa isa, int32_t form_id);
/// Numero de formas de la ISA.
uint32_t form_count(Isa isa);

} // namespace instr_db
} // namespace vex

#endif // VEX_INSTR_DB_H
