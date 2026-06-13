/*
 * mi_plugin.c - Plugin de ejemplo para VestaVM (Modelo A)
 *
 * Compilar en Windows (MinGW):
 *   gcc -shared -o mi_plugin.dll mi_plugin.c
 *
 * Compilar en Linux:
 *   gcc -shared -fPIC -o mi_plugin.so mi_plugin.c
 *
 * Importar desde bytecode .vel:
 *   @Import {
 *       @Method { @Lib("mi_plugin.dll")  @Name("suma")      }
 *       @Method { @Lib("mi_plugin.dll")  @Name("mi_saludo") }
 *   }
 */

#include "../../include/ffi/vesta_plugin.h"
#include <stdio.h>
#include <stdint.h>

/* puntero a la API conservado para uso en funciones nativas avanzadas */
static const VestaPluginAPI *g_api = NULL;

/* -----------------------------------------------------------------------
 * Punto de entrada del plugin: VestaVM lo llama una sola vez al cargar
 * el ejecutable, justo despues de resolver la tabla de importaciones.
 * ----------------------------------------------------------------------- */
void vesta_init(const VestaPluginAPI *api) {
    g_api = api;

    /* verificar version de la API para compatibilidad hacia adelante */
    if (api->api_version != VESTA_PLUGIN_API_VERSION) {
        api->log("[mi_plugin] AVISO: version de API inesperada");
        return;
    }

    api->log("[mi_plugin] inicializado correctamente");
    api->log("[mi_plugin] VMs activas al cargar: ");
}

/* -----------------------------------------------------------------------
 * Funcion normal: disponible via @Import en el bytecode.
 * Convencion de llamada estandar de VestaVM: primer param = contexto void*,
 * argumentos en r1..r12 como uint64_t, retorno en r0 como uint64_t.
 * ----------------------------------------------------------------------- */

/**
 * suma(a, b) -> a + b
 * Uso en .vel:
 *   mov  r15, 2
 *   mov  r1,  10
 *   mov  r2,  32
 *   calln @Method("mi_plugin.dll:suma")
 *   ; r0 = 42
 */
uint64_t suma(uint64_t a, uint64_t b) {
    return a + b;
}

/**
 * mi_saludo() -> 0
 * Imprime un saludo usando la API del manager si esta disponible.
 * Uso en .vel:
 *   mov  r15, 0
 *   calln @Method("mi_plugin.dll:mi_saludo")
 */
uint64_t mi_saludo() {
    if (g_api) {
        g_api->log("Hola desde mi_plugin!");
    } else {
        puts("Hola desde mi_plugin! (sin API)");
    }
    return 0;
}

/**
 * crear_vm_auxiliar(n_schedulers) -> vm_id
 * Crea una VM auxiliar con n_schedulers schedulers y devuelve su ID.
 * Demuestra el uso de la API del manager desde una funcion nativa.
 * Uso en .vel:
 *   mov  r15, 1
 *   mov  r1,  2    ; 2 schedulers
 *   calln @Method("mi_plugin.dll:crear_vm_auxiliar")
 *   ; r0 = vm_id de la nueva VM
 */
uint64_t crear_vm_auxiliar(uint64_t n_schedulers) {
    if (!g_api) return (uint64_t)-1; /* API no disponible */

    uint64_t vm_id = g_api->create_vm(g_api->manager, (uint32_t)n_schedulers);
    g_api->log("[mi_plugin] VM auxiliar creada");
    return vm_id;
}
