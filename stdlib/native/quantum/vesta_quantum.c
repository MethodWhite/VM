/**
 * @file vesta_quantum.c
 * @brief Plugin nativo de simulacion cuantica para VestaVM.
 *
 * Simulador de estado vectorial completo (state vector) con soporte
 * para hasta 20 qubits (2^20 amplitudes complejas).
 *
 * Puertas soportadas: H, X, Y, Z, CNOT, SWAP, Toffoli, T, S,
 * Phase, Rx, Ry, Rz.
 *
 * Medicion con colapso de funcion de onda y seguimiento de
 * amplitudes de probabilidad.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../../../include/ffi/vesta_plugin.h"

/* -----------------------------------------------------------------------
 * Configuracion
 * ----------------------------------------------------------------------- */

#ifndef VESTA_QUANTUM_DEBUG
#  define VESTA_QUANTUM_DEBUG 0
#endif

#define MAX_QUBITS         20
#define MAX_AMPLITUDES     (1ULL << MAX_QUBITS)
#define TWO_PI             6.28318530717958647693
#define SQRT2_INV          0.70710678118654752440

/* -----------------------------------------------------------------------
 * Tipos internos
 * ----------------------------------------------------------------------- */

typedef struct {
    double re;
    double im;
} Complex;

typedef struct {
    uint64_t n_qubits;
    uint64_t n_amplitudes;
    Complex *state;
    uint64_t *measurements;
} Simulator;

typedef struct {
    Simulator **handles;
    uint64_t    count;
    uint64_t    capacity;
} HandleTable;

/* -----------------------------------------------------------------------
 * Estado global del plugin
 * ----------------------------------------------------------------------- */

static const VestaPluginAPI *g_api = NULL;
static HandleTable g_handles = { NULL, 0, 0 };
static int g_random_seeded = 0;

/* -----------------------------------------------------------------------
 * Helpers de conversion double <-> uint64_t
 * ----------------------------------------------------------------------- */

static inline double u64_to_f64(uint64_t u) {
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static inline uint64_t f64_to_u64(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

/* -----------------------------------------------------------------------
 * Manejo de tabla de handles
 * ----------------------------------------------------------------------- */

static uint64_t handle_store(Simulator *sim) {
    if (g_handles.count == g_handles.capacity) {
        uint64_t new_cap = g_handles.capacity ? g_handles.capacity * 2 : 16;
        Simulator **new_handles = (Simulator**)realloc(
            g_handles.handles, new_cap * sizeof(Simulator*));
        if (!new_handles) return 0;
        g_handles.handles = new_handles;
        g_handles.capacity = new_cap;
    }
    uint64_t id = g_handles.count + 1;
    g_handles.handles[g_handles.count++] = sim;
    return id;
}

static Simulator *handle_get(uint64_t id) {
    if (id == 0 || id > g_handles.count) return NULL;
    return g_handles.handles[id - 1];
}

static void handle_remove(uint64_t id) {
    if (id == 0 || id > g_handles.count) return;
    Simulator *sim = g_handles.handles[id - 1];
    if (sim) {
        if (sim->state) free(sim->state);
        if (sim->measurements) free(sim->measurements);
        free(sim);
    }
    g_handles.handles[id - 1] = NULL;
}

/* -----------------------------------------------------------------------
 * Complejos inline
 * ----------------------------------------------------------------------- */

static inline Complex cpack(double re, double im) {
    Complex z;
    z.re = re;
    z.im = im;
    return z;
}

static inline Complex cadd(Complex a, Complex b) {
    return cpack(a.re + b.re, a.im + b.im);
}

static inline Complex csub(Complex a, Complex b) {
    return cpack(a.re - b.re, a.im - b.im);
}

static inline Complex cmul(Complex a, Complex b) {
    return cpack(a.re * b.re - a.im * b.im,
                 a.re * b.im + a.im * b.re);
}

static inline Complex cscale(Complex a, double s) {
    return cpack(a.re * s, a.im * s);
}

static inline double cnorm(Complex a) {
    return a.re * a.re + a.im * a.im;
}

/* -----------------------------------------------------------------------
 * RNG para colapso
 * ----------------------------------------------------------------------- */

static double random_uniform(void) {
    if (!g_random_seeded) {
        srand((unsigned int)(time(NULL) ^ (uintptr_t)&g_random_seeded));
        g_random_seeded = 1;
    }
    return (double)rand() / (double)RAND_MAX;
}

/* -----------------------------------------------------------------------
 * Construir / destruir simulador
 * ----------------------------------------------------------------------- */

VESTA_PLUGIN_EXPORT uint64_t vquantum_create(uint64_t n_qubits) {
    if (n_qubits == 0 || n_qubits > MAX_QUBITS) {
        if (g_api) g_api->log("[vesta_quantum] n_qubits fuera de rango");
        return 0;
    }
    Simulator *sim = (Simulator*)calloc(1, sizeof(Simulator));
    if (!sim) return 0;

    sim->n_qubits = n_qubits;
    sim->n_amplitudes = 1ULL << n_qubits;

    sim->state = (Complex*)calloc(sim->n_amplitudes, sizeof(Complex));
    if (!sim->state) { free(sim); return 0; }

    sim->state[0] = cpack(1.0, 0.0);

    sim->measurements = (uint64_t*)calloc(n_qubits, sizeof(uint64_t));
    if (!sim->measurements) { free(sim->state); free(sim); return 0; }

    uint64_t handle = handle_store(sim);
#if VESTA_QUANTUM_DEBUG
    if (g_api) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[vesta_quantum] creado simulador %llu con %llu qubits",
                 (unsigned long long)handle, (unsigned long long)n_qubits);
        g_api->log(buf);
    }
#endif
    return handle;
}

VESTA_PLUGIN_EXPORT void vquantum_destroy(uint64_t handle) {
    handle_remove(handle);
}

/* -----------------------------------------------------------------------
 * Aplicacion de puertas cuanticas
 * ----------------------------------------------------------------------- */

static void apply_gate_1q(Simulator *sim, uint64_t qubit,
                          Complex m00, Complex m01,
                          Complex m10, Complex m11)
{
    uint64_t n = sim->n_amplitudes;
    uint64_t step = 1ULL << qubit;
    Complex *s = sim->state;

    for (uint64_t i = 0; i < n; i += (step << 1)) {
        for (uint64_t j = 0; j < step; ++j) {
            uint64_t idx0 = i + j;
            uint64_t idx1 = i + j + step;
            Complex a = s[idx0];
            Complex b = s[idx1];
            s[idx0] = cadd(cmul(m00, a), cmul(m01, b));
            s[idx1] = cadd(cmul(m10, a), cmul(m11, b));
        }
    }
}

static void apply_gate_2q(Simulator *sim,
                          uint64_t q0, uint64_t q1,
                          int gate_type)
{
    uint64_t n = sim->n_amplitudes;
    uint64_t step0 = 1ULL << q0;
    uint64_t step1 = 1ULL << q1;
    Complex *s = sim->state;

    for (uint64_t i = 0; i < n; ++i) {
        uint64_t bit0 = (i >> q0) & 1;
        uint64_t bit1 = (i >> q1) & 1;

        if (gate_type == 0) {
            if (bit0) {
                uint64_t j = i ^ step1;
                if (j > i) {
                    Complex tmp = s[i];
                    s[i] = s[j];
                    s[j] = tmp;
                }
            }
        } else if (gate_type == 1) {
            if (bit0 != bit1) {
                uint64_t j = i ^ step0 ^ step1;
                if (j > i) {
                    Complex tmp = s[i];
                    s[i] = s[j];
                    s[j] = tmp;
                }
            }
        }
    }
}

static void apply_toffoli(Simulator *sim,
                          uint64_t c0, uint64_t c1, uint64_t target)
{
    uint64_t n = sim->n_amplitudes;
    uint64_t step_t = 1ULL << target;
    Complex *s = sim->state;

    for (uint64_t i = 0; i < n; ++i) {
        uint64_t bit_c0 = (i >> c0) & 1;
        uint64_t bit_c1 = (i >> c1) & 1;
        if (bit_c0 && bit_c1) {
            uint64_t j = i ^ step_t;
            if (j > i) {
                Complex tmp = s[i];
                s[i] = s[j];
                s[j] = tmp;
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Puertas de 1 qubit predefinidas
 * ----------------------------------------------------------------------- */

static void gate_hadamard(Simulator *sim, uint64_t qubit) {
    Complex a = cpack(SQRT2_INV, 0.0);
    Complex b = cpack(SQRT2_INV, 0.0);
    Complex c = cpack(SQRT2_INV, 0.0);
    Complex d = cpack(-SQRT2_INV, 0.0);
    apply_gate_1q(sim, qubit, a, b, c, d);
}

static void gate_paulix(Simulator *sim, uint64_t qubit) {
    apply_gate_1q(sim, qubit,
                  cpack(0.0, 0.0), cpack(1.0, 0.0),
                  cpack(1.0, 0.0), cpack(0.0, 0.0));
}

static void gate_pauliy(Simulator *sim, uint64_t qubit) {
    apply_gate_1q(sim, qubit,
                  cpack(0.0, 0.0), cpack(0.0, -1.0),
                  cpack(0.0, 1.0), cpack(0.0, 0.0));
}

static void gate_pauliz(Simulator *sim, uint64_t qubit) {
    apply_gate_1q(sim, qubit,
                  cpack(1.0, 0.0), cpack(0.0, 0.0),
                  cpack(0.0, 0.0), cpack(-1.0, 0.0));
}

static void gate_t(Simulator *sim, uint64_t qubit) {
    double phi = 3.14159265358979323846 / 4.0;
    apply_gate_1q(sim, qubit,
                  cpack(1.0, 0.0), cpack(0.0, 0.0),
                  cpack(0.0, 0.0), cpack(cos(phi), sin(phi)));
}

static void gate_s(Simulator *sim, uint64_t qubit) {
    apply_gate_1q(sim, qubit,
                  cpack(1.0, 0.0), cpack(0.0, 0.0),
                  cpack(0.0, 0.0), cpack(0.0, 1.0));
}

static void gate_phase(Simulator *sim, uint64_t qubit, double phi) {
    apply_gate_1q(sim, qubit,
                  cpack(1.0, 0.0), cpack(0.0, 0.0),
                  cpack(0.0, 0.0), cpack(cos(phi), sin(phi)));
}

static void gate_rx(Simulator *sim, uint64_t qubit, double theta) {
    double half = theta * 0.5;
    double c = cos(half);
    double s = sin(half);
    apply_gate_1q(sim, qubit,
                  cpack(c, 0.0),   cpack(0.0, -s),
                  cpack(0.0, -s),  cpack(c, 0.0));
}

static void gate_ry(Simulator *sim, uint64_t qubit, double theta) {
    double half = theta * 0.5;
    double c = cos(half);
    double s = sin(half);
    apply_gate_1q(sim, qubit,
                  cpack(c, 0.0), cpack(-s, 0.0),
                  cpack(s, 0.0), cpack(c, 0.0));
}

static void gate_rz(Simulator *sim, uint64_t qubit, double theta) {
    double half = theta * 0.5;
    apply_gate_1q(sim, qubit,
                  cpack(cos(-half), sin(-half)), cpack(0.0, 0.0),
                  cpack(0.0, 0.0),               cpack(cos(half), sin(half)));
}

/* -----------------------------------------------------------------------
 * API: vquantum_apply_gate
 *
 * gate_type: 0=H, 1=X, 2=Y, 3=Z, 4=CNOT, 5=SWAP, 6=Toffoli,
 *            7=Phase, 8=T, 9=S, 10=Rx, 11=Ry, 12=Rz
 * ----------------------------------------------------------------------- */

VESTA_PLUGIN_EXPORT void vquantum_apply_gate(uint64_t handle,
                                              uint64_t gate_type,
                                              uint64_t targets,
                                              uint64_t n_targets,
                                              uint64_t params,
                                              uint64_t n_params)
{
    Simulator *sim = handle_get(handle);
    if (!sim) return;

    uint64_t t0 = 0, t1 = 0, t2 = 0;
    if (n_targets >= 1) {
        t0 = targets & 0xFFFFFFFF;
    }
    if (n_targets >= 2) {
        t1 = (targets >> 32) & 0xFFFFFFFF;
    }
    if (n_targets >= 3) {
        t2 = params;
    }

    double p0 = 0.0;
    if (n_params >= 1) {
        p0 = u64_to_f64(params);
    }

    switch (gate_type) {
    case 0:  gate_hadamard(sim, t0); break;
    case 1:  gate_paulix(sim, t0);   break;
    case 2:  gate_pauliy(sim, t0);   break;
    case 3:  gate_pauliz(sim, t0);   break;
    case 4:  apply_gate_2q(sim, t0, t1, 0); break;
    case 5:  apply_gate_2q(sim, t0, t1, 1); break;
    case 6:  apply_toffoli(sim, t0, t1, t2); break;
    case 7:  gate_phase(sim, t0, p0); break;
    case 8:  gate_t(sim, t0); break;
    case 9:  gate_s(sim, t0); break;
    case 10: gate_rx(sim, t0, p0); break;
    case 11: gate_ry(sim, t0, p0); break;
    case 12: gate_rz(sim, t0, p0); break;
    default: break;
    }
}

/* -----------------------------------------------------------------------
 * API: vquantum_measure
 *
 * Mide un qubit, colapsando el state vector.
 * Retorna 0 o 1.
 * ----------------------------------------------------------------------- */

VESTA_PLUGIN_EXPORT uint64_t vquantum_measure(uint64_t handle, uint64_t qubit) {
    Simulator *sim = handle_get(handle);
    if (!sim || qubit >= sim->n_qubits) return 0;

    uint64_t n = sim->n_amplitudes;
    uint64_t step = 1ULL << qubit;
    Complex *s = sim->state;

    double prob_one = 0.0;
    for (uint64_t i = 0; i < n; ++i) {
        if ((i >> qubit) & 1) {
            prob_one += cnorm(s[i]);
        }
    }

    uint64_t result = (random_uniform() < prob_one) ? 1 : 0;
    sim->measurements[qubit] = result;

    double norm = 0.0;
    for (uint64_t i = 0; i < n; ++i) {
        if (((i >> qubit) & 1) != result) {
            s[i] = cpack(0.0, 0.0);
        } else {
            norm += cnorm(s[i]);
        }
    }
    if (norm > 0.0) {
        double inv_norm = 1.0 / sqrt(norm);
        for (uint64_t i = 0; i < n; ++i) {
            s[i] = cscale(s[i], inv_norm);
        }
    }

    return result;
}

/* -----------------------------------------------------------------------
 * API: vquantum_get_probabilities
 *
 * Retorna las probabilidades de cada estado base como f64[]
 * (bits IEEE 754). Escritura en memoria VM via vm_write_bytes.
 * ----------------------------------------------------------------------- */

VESTA_PLUGIN_EXPORT uint64_t vquantum_get_probabilities(uint64_t handle,
                                                         uint64_t proc_ptr,
                                                         uint64_t vm_addr,
                                                         uint64_t max_count)
{
    Simulator *sim = handle_get(handle);
    if (!sim || !g_api) return 0;

    uint64_t n = sim->n_amplitudes;
    uint64_t count = n < max_count ? n : max_count;

    double *probs = (double*)malloc(count * sizeof(double));
    if (!probs) return 0;

    for (uint64_t i = 0; i < count; ++i) {
        probs[i] = cnorm(sim->state[i]);
    }

    uint64_t written = g_api->vm_write_bytes(proc_ptr, vm_addr,
                                              (const void*)probs,
                                              count * sizeof(double));
    free(probs);
    return written;
}

/* -----------------------------------------------------------------------
 * API: vquantum_get_statevector
 *
 * Retorna el state vector completo como Complex[].
 * ----------------------------------------------------------------------- */

VESTA_PLUGIN_EXPORT uint64_t vquantum_get_statevector(uint64_t handle,
                                                       uint64_t proc_ptr,
                                                       uint64_t vm_addr)
{
    Simulator *sim = handle_get(handle);
    if (!sim || !g_api) return 0;

    uint64_t n = sim->n_amplitudes;
    uint64_t byte_count = n * sizeof(Complex);

    return g_api->vm_write_bytes(proc_ptr, vm_addr,
                                  (const void*)sim->state, byte_count);
}

/* -----------------------------------------------------------------------
 * API: vquantum_run_simulator
 * ----------------------------------------------------------------------- */

VESTA_PLUGIN_EXPORT uint64_t vquantum_run_simulator(uint64_t handle) {
    return handle;
}

/* -----------------------------------------------------------------------
 * Punto de entrada del plugin
 * ----------------------------------------------------------------------- */

VESTA_PLUGIN_EXPORT void vesta_init(const VestaPluginAPI *api) {
    g_api = api;
#if VESTA_QUANTUM_DEBUG
    if (g_api) g_api->log("[vesta_quantum] plugin de simulacion cuantica cargado");
#else
    (void)api;
#endif
}
