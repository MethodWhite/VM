/**
 * @file vesta_math.c
 * @brief Modulo matematico de la libreria estandar nativa de VestaVM.
 *
 * Todas las funciones operan exclusivamente sobre valores numericos
 * (uint64_t o bits IEEE 754 de double); ningun argumento es puntero a
 * memoria VM, por lo que no se necesita proc_ptr en este modulo.
 *
 * Convencion de llamada:
 *   - Argumentos en r1..r12 como uint64_t; ningun contexto implicito.
 *   - Enteros        : uint64_t directo.
 *   - Floats/doubles : bits IEEE 754 en uint64_t (usar memcpy para convertir).
 *   - Retorno        : uint64_t (bits IEEE 754 cuando el resultado es double).
 *
 * Importar desde bytecode .vel:
 * @code
 *   @Import {
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_sqrt")   }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_pow")    }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_abs")    }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_fabs")   }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_floor")  }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_ceil")   }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_round")  }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_min")    }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_max")    }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_clamp")  }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_fmin")   }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_fmax")   }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_log")    }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_log2")   }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_log10")  }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_sin")    }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_cos")    }
 *       @Method { @Lib("vesta_math.dll")  @Name("vmath_tan")    }
 *   }
 * @endcode
 */

/**
 * @def VESTA_MATH_DEBUG
 * @brief Activa el mensaje de carga del modulo en vesta_init cuando vale 1.
 *
 * Definir como 1 para depuracion; dejar en 0 para produccion.
 * Se puede sobreescribir desde la linea de compilacion con -DVESTA_MATH_DEBUG=1.
 */
#ifndef VESTA_MATH_DEBUG
#  define VESTA_MATH_DEBUG 0
#endif

#include "../../../include/ffi/vesta_plugin.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Helpers de conversion double <-> uint64_t por valor de bits (sin UB)
 * ----------------------------------------------------------------------- */

/**
 * @brief Convierte bits IEEE 754 almacenados en uint64_t a double.
 * @param u  Representacion de bits del double.
 * @return   El double correspondiente.
 */
static inline double u64_to_f64(uint64_t u) {
    double d;
    memcpy(&d, &u, 8);
    return d;
}

/**
 * @brief Convierte un double a su representacion de bits IEEE 754 en uint64_t.
 * @param d  Valor double a convertir.
 * @return   Bits IEEE 754 del double como uint64_t.
 */
static inline uint64_t f64_to_u64(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

/* -----------------------------------------------------------------------
 * Punto de entrada del plugin
 * ----------------------------------------------------------------------- */

/**
 * @brief Punto de entrada del plugin; llamado una vez por VestaVM al cargar.
 *
 * El mensaje de carga solo se emite si VESTA_MATH_DEBUG esta definido como 1.
 *
 * @param api  Puntero a la tabla de funciones de VestaVM.
 */
VESTA_PLUGIN_EXPORT void vesta_init(const VestaPluginAPI *api) {
#if VESTA_MATH_DEBUG
    if (api) api->log("[vesta_math] cargado");
#else
    (void) api;
#endif
}

/* -----------------------------------------------------------------------
 * Raiz cuadrada y potencia
 * ----------------------------------------------------------------------- */

/**
 * @brief Calcula la raiz cuadrada de un double.
 *
 * @param bits  Bits IEEE 754 del operando (double >= 0).
 * @return      Bits IEEE 754 del resultado; NaN si la entrada es negativa.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_sqrt(uint64_t bits) {
    return f64_to_u64(sqrt(u64_to_f64(bits)));
}

/**
 * @brief Calcula base elevado a exp (ambos doubles).
 *
 * @param base_bits  Bits IEEE 754 de la base.
 * @param exp_bits   Bits IEEE 754 del exponente.
 * @return           Bits IEEE 754 del resultado.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_pow(uint64_t base_bits, uint64_t exp_bits) {
    return f64_to_u64(pow(u64_to_f64(base_bits), u64_to_f64(exp_bits)));
}

/* -----------------------------------------------------------------------
 * Valor absoluto
 * ----------------------------------------------------------------------- */

/**
 * @brief Valor absoluto de un entero de 64 bits con signo.
 *
 * @param n  Valor de entrada como uint64_t (interpretado como int64_t).
 * @return   |n| como uint64_t.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_abs(uint64_t n) {
    int64_t s = (int64_t) n;
    return (uint64_t) (s < 0 ? -s : s);
}

/**
 * @brief Valor absoluto de un double.
 *
 * @param bits  Bits IEEE 754 del operando.
 * @return      Bits IEEE 754 de |operando|.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_fabs(uint64_t bits) {
    return f64_to_u64(fabs(u64_to_f64(bits)));
}

/* -----------------------------------------------------------------------
 * Redondeo
 * ----------------------------------------------------------------------- */

/**
 * @brief Redondeo hacia menos infinito (floor).
 *
 * @param bits  Bits IEEE 754 del operando.
 * @return      Bits IEEE 754 del resultado.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_floor(uint64_t bits) {
    return f64_to_u64(floor(u64_to_f64(bits)));
}

/**
 * @brief Redondeo hacia mas infinito (ceil).
 *
 * @param bits  Bits IEEE 754 del operando.
 * @return      Bits IEEE 754 del resultado.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_ceil(uint64_t bits) {
    return f64_to_u64(ceil(u64_to_f64(bits)));
}

/**
 * @brief Redondeo al entero mas cercano (0.5 redondea hacia mas infinito).
 *
 * @param bits  Bits IEEE 754 del operando.
 * @return      Bits IEEE 754 del resultado.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_round(uint64_t bits) {
    return f64_to_u64(round(u64_to_f64(bits)));
}

/* -----------------------------------------------------------------------
 * Minimo, maximo y acotacion
 * ----------------------------------------------------------------------- */

/**
 * @brief Minimo de dos enteros de 64 bits sin signo.
 *
 * @param a  Primer operando.
 * @param b  Segundo operando.
 * @return   El menor de a y b.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_min(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

/**
 * @brief Maximo de dos enteros de 64 bits sin signo.
 *
 * @param a  Primer operando.
 * @param b  Segundo operando.
 * @return   El mayor de a y b.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_max(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

/**
 * @brief Acota un entero de 64 bits sin signo en el intervalo [lo, hi].
 *
 * @param x   Valor a acotar.
 * @param lo  Limite inferior (inclusive).
 * @param hi  Limite superior (inclusive).
 * @return    lo si x < lo; hi si x > hi; x en caso contrario.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_clamp(uint64_t x, uint64_t lo, uint64_t hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/**
 * @brief Minimo de dos doubles.
 *
 * @param a_bits  Bits IEEE 754 del primer operando.
 * @param b_bits  Bits IEEE 754 del segundo operando.
 * @return        Bits IEEE 754 del menor de los dos valores.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_fmin(uint64_t a_bits, uint64_t b_bits) {
    return f64_to_u64(fmin(u64_to_f64(a_bits), u64_to_f64(b_bits)));
}

/**
 * @brief Maximo de dos doubles.
 *
 * @param a_bits  Bits IEEE 754 del primer operando.
 * @param b_bits  Bits IEEE 754 del segundo operando.
 * @return        Bits IEEE 754 del mayor de los dos valores.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_fmax(uint64_t a_bits, uint64_t b_bits) {
    return f64_to_u64(fmax(u64_to_f64(a_bits), u64_to_f64(b_bits)));
}

/* -----------------------------------------------------------------------
 * Logaritmos
 * ----------------------------------------------------------------------- */

/**
 * @brief Logaritmo natural (base e) de un double.
 *
 * @param bits  Bits IEEE 754 del operando (debe ser > 0).
 * @return      Bits IEEE 754 del resultado; -Inf si bits = 0; NaN si bits < 0.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_log(uint64_t bits) {
    return f64_to_u64(log(u64_to_f64(bits)));
}

/**
 * @brief Logaritmo en base 2 de un double.
 *
 * @param bits  Bits IEEE 754 del operando (debe ser > 0).
 * @return      Bits IEEE 754 del resultado.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_log2(uint64_t bits) {
    return f64_to_u64(log2(u64_to_f64(bits)));
}

/**
 * @brief Logaritmo en base 10 de un double.
 *
 * @param bits  Bits IEEE 754 del operando (debe ser > 0).
 * @return      Bits IEEE 754 del resultado.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_log10(uint64_t bits) {
    return f64_to_u64(log10(u64_to_f64(bits)));
}

/* -----------------------------------------------------------------------
 * Trigonometria (angulos en radianes como double)
 * ----------------------------------------------------------------------- */

/**
 * @brief Seno de un angulo expresado en radianes como double.
 *
 * @param bits  Bits IEEE 754 del angulo en radianes.
 * @return      Bits IEEE 754 del seno del angulo; resultado en [-1.0, 1.0].
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_sin(uint64_t bits) {
    return f64_to_u64(sin(u64_to_f64(bits)));
}

/**
 * @brief Coseno de un angulo expresado en radianes como double.
 *
 * @param bits  Bits IEEE 754 del angulo en radianes.
 * @return      Bits IEEE 754 del coseno del angulo; resultado en [-1.0, 1.0].
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_cos(uint64_t bits) {
    return f64_to_u64(cos(u64_to_f64(bits)));
}

/**
 * @brief Tangente de un angulo expresado en radianes como double.
 *
 * @param bits  Bits IEEE 754 del angulo en radianes.
 * @return      Bits IEEE 754 de la tangente del angulo.
 */
VESTA_PLUGIN_EXPORT uint64_t vmath_tan(uint64_t bits) {
    return f64_to_u64(tan(u64_to_f64(bits)));
}
