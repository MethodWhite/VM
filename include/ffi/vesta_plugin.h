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
 * @file vesta_plugin.h
 * @brief API publica en C puro para plugins nativos de VestaVM (Modelo A).
 *
 * Un plugin es una libreria dinamica (.dll / .so) que exporta una funcion
 * de inicializacion con la siguiente firma:
 *
 * @code
 *   void vesta_init(const VestaPluginAPI *api);
 * @endcode
 *
 * VestaVM llama a @c vesta_init una sola vez, justo despues de resolver la
 * tabla de importaciones del ejecutable.  El plugin recibe un puntero a
 * @c VestaPluginAPI, que contiene punteros de funcion para operar con el
 * manager y las instancias VM.
 *
 * Las funciones normales del plugin (las registradas via @c @Import en el
 * bytecode) mantienen la convencion de llamada habitual de VestaVM sin
 * cambios: r1-r12 como argumentos, r0 como valor de retorno.
 *
 * El header es puro C (sin C++) para que sea compilable con GCC, Clang o
 * MSVC sin necesidad de flags C++ ni dependencias adicionales.
 */

#ifndef VESTA_PLUGIN_H
#define VESTA_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Version de la API expuesta al plugin. Incrementar en cambios incompatibles. */
#define VESTA_PLUGIN_API_VERSION 2

/**
 * @brief Handle opaco al ManageVM de VestaVM.
 *
 * El plugin no accede directamente a la clase C++; usa los punteros de
 * funcion de @c VestaPluginAPI que reciben este handle como primer argumento.
 */
typedef void VestaManager;

/**
 * @brief Handle opaco a una instancia VM de VestaVM.
 *
 * Obtenido mediante @c VestaPluginAPI::get_vm() o @c VestaPluginAPI::create_vm().
 * Pasado a las funciones de ciclo de vida (@c start_vm, @c stop_vm, etc.).
 */
typedef void VestaVM_t;

/**
 * @struct VestaPluginAPI
 * @brief Tabla de funciones de la API que VestaVM entrega al plugin en @c vesta_init.
 *
 * Todos los punteros de funcion son validos durante toda la vida del ejecutable.
 * El plugin NO debe guardar punteros a instancias VM mas alla de @c stop_vm.
 *
 * Uso tipico:
 * @code
 *   static const VestaPluginAPI *g_api;
 *
 *   void vesta_init(const VestaPluginAPI *api) {
 *       g_api = api;
 *       // registrar funciones, crear VMs auxiliares, etc.
 *   }
 *
 *   uint64_t mi_funcion(void *ctx, uint64_t a, uint64_t b) {
 *       return a + b;  // funcion normal: sin cambios en la convencion
 *   }
 * @endcode
 */
typedef struct VestaPluginAPI {

    /** @brief Version de la API. Comparar con VESTA_PLUGIN_API_VERSION para compatibilidad. */
    uint32_t       api_version;

    /** @brief Handle al ManageVM activo. Pasarlo a todas las funciones del manager. */
    VestaManager  *manager;

    /* --- operaciones del manager --- */

    /**
     * @brief Crea una nueva instancia VM con @p num_schedulers hilos nativos.
     * @param mgr            Handle al manager (usar @c api->manager).
     * @param num_schedulers Numero de schedulers de la nueva VM.
     * @return ID unico de la VM creada.
     */
    uint64_t   (*create_vm)  (VestaManager *mgr, uint32_t num_schedulers);

    /**
     * @brief Destruye la instancia VM con el ID indicado.
     * @param mgr   Handle al manager.
     * @param vm_id ID de la VM a destruir.
     * @return 1 si se destruyo; 0 si no existia.
     */
    int        (*destroy_vm) (VestaManager *mgr, uint64_t vm_id);

    /**
     * @brief Devuelve el handle a la instancia VM con el ID indicado.
     * @param mgr   Handle al manager.
     * @param vm_id ID de la VM.
     * @return Handle a la VM, o NULL si no existe.
     */
    VestaVM_t *(*get_vm)     (VestaManager *mgr, uint64_t vm_id);

    /**
     * @brief Comprueba si existe una VM con el ID indicado.
     * @param mgr   Handle al manager.
     * @param vm_id ID a verificar.
     * @return 1 si existe; 0 si no.
     */
    int        (*has_vm)     (VestaManager *mgr, uint64_t vm_id);

    /**
     * @brief Devuelve el numero de instancias VM activas en el manager.
     * @param mgr Handle al manager.
     * @return Numero de VMs activas.
     */
    uint64_t   (*vm_count)   (VestaManager *mgr);

    /* --- ciclo de vida de la VM --- */

    /**
     * @brief Lanza los schedulers de la VM (no bloquea).
     * @param vm Handle a la instancia VM.
     */
    void       (*start_vm)   (VestaVM_t *vm);

    /**
     * @brief Solicita la parada cooperativa de todos los schedulers de la VM.
     * @param vm Handle a la instancia VM.
     */
    void       (*stop_vm)    (VestaVM_t *vm);

    /**
     * @brief Bloquea hasta que todos los schedulers de la VM finalicen.
     * @param vm Handle a la instancia VM.
     */
    void       (*wait_vm)    (VestaVM_t *vm);

    /* --- procesos virtuales --- */

    /**
     * @brief Crea un nuevo proceso virtual dentro de la VM.
     *
     * El proceso queda en estado NEW; llamar a @c make_ready para activarlo.
     *
     * @param vm        Handle a la instancia VM.
     * @param out_sched Recibe el ID del scheduler asignado al nuevo proceso.
     * @param out_pid   Recibe el PID local del nuevo proceso.
     */
    void       (*spawn_process) (VestaVM_t *vm, uint32_t *out_sched, uint64_t *out_pid);

    /**
     * @brief Marca un proceso como READY para que el scheduler lo ejecute.
     * @param vm       Handle a la instancia VM.
     * @param sched_id ID del scheduler propietario del proceso.
     * @param local_pid PID local del proceso (obtenido con @c spawn_process).
     */
    void       (*make_ready)    (VestaVM_t *vm, uint32_t sched_id, uint64_t local_pid);

    /* --- acceso a memoria virtual de la VM --- */

    /**
     * @brief Lee bytes desde la memoria virtual de la VM al buffer host.
     *
     * @p proc_ptr es el valor uint64_t obtenido con la instruccion @c getproc
     * desde el bytecode .vel, que codifica el ProcessVM* del proceso activo.
     * Se obtiene tipicamente asi:
     *
     * @code
     *   getproc r1               ; r1 = ProcessVM* como uint64_t
     *   mov     r2, vm_address   ; direccion virtual del dato
     *   mov     r3, longitud
     *   mov     r15, 3
     *   calln   @Method("mi_lib.dll:mi_funcion")
     *   ; en mi_funcion: g_api->vm_read_bytes((void*)proc_ptr, vm_addr, buf, len)
     * @endcode
     *
     * @param proc_ptr  Valor uint64_t con el ProcessVM* (resultado de getproc).
     *                  Se interpreta internamente como ProcessVM* mediante cast.
     * @param vm_addr   Direccion virtual dentro del espacio de la VM.
     * @param dst       Buffer host de destino (debe tener al menos @p len bytes).
     * @param len       Numero de bytes a leer.
     * @return Numero de bytes leidos (igual a @p len si no hay error).
     */
    uint64_t   (*vm_read_bytes) (uint64_t proc_ptr, uint64_t vm_addr,
                                 void *dst, uint64_t len);

    /**
     * @brief Escribe bytes desde un buffer host a la memoria virtual de la VM.
     *
     * Simetrico de @c vm_read_bytes.  @p proc_ptr se obtiene con @c getproc.
     *
     * @param proc_ptr  Valor uint64_t con el ProcessVM* (resultado de getproc).
     * @param vm_addr   Direccion virtual de destino dentro de la VM.
     * @param src       Buffer host de origen.
     * @param len       Numero de bytes a escribir.
     * @return Numero de bytes escritos (igual a @p len si no hay error).
     */
    uint64_t   (*vm_write_bytes)(uint64_t proc_ptr, uint64_t vm_addr,
                                 const void *src, uint64_t len);

    /* --- utilidades --- */

    /**
     * @brief Emite un mensaje de log hilo-seguro a la salida de VestaVM.
     * @param msg Cadena terminada en '\0' a imprimir.
     */
    void       (*log)           (const char *msg);

    /* --- - GC roots externos (write-barrier para colecciones nativas) --- */

    /**
     * @brief Incrementa el refcount externo de @p gc_handle.
     *
     * Cualquier estructura nativa que retenga un GcHandle (e.g.
     * `ArrayList<string>` que hace push de un StringObject) DEBE llamar
     * a esta funcion al adquirir la referencia, y a @c gc_release al
     * liberarla.  Mientras el refcount sea >0, el GC tratara el handle
     * como root vivo durante el mark phase y NO recolectara el objeto
     * aunque ningun root normal (HandleTable bytecode) lo referencie.
     *
     * Coste: O(1) amortizado (1 lookup + increment en un hashmap del
     * GcHeap del proceso activo).  Cero overhead en el GC cycle excepto
     * iterar el map (mucho mas pequeno que escanear todos los slots).
     *
     * Thread-safety: el GcHeap es per-process y NO multi-thread; el
     * plugin se ejecuta en el mismo thread que ejecuta bytecode del
     * proceso activo.  Si el plugin necesita llamar desde otro thread,
     * debe sincronizar externamente.
     *
     * @param proc_ptr   Handle al ProcessVM activo (usar `getproc` desde
     *                   bytecode o pasarlo como arg explicito).
     * @param gc_handle  GcHandle a pinnar (uint32_t encoded como uint64_t).
     *                   Si @c gc_handle == 0 (GC_NULL_HANDLE), no-op.
     */
    void       (*gc_addref)     (uint64_t proc_ptr, uint64_t gc_handle);

    /**
     * @brief Decrementa el refcount externo de @p gc_handle.
     *
     * Llamar cuando la estructura nativa deja de retener el handle (pop,
     * remove, clear, free).  Si el refcount llega a 0, la entrada se
     * elimina del map del GcHeap; el objeto sera colectado en el proximo
     * major_gc si nadie mas lo referencia.
     *
     * @param proc_ptr   Handle al ProcessVM activo.
     * @param gc_handle  GcHandle a despinnar.  Si no estaba pinnado, no-op.
     */
    void       (*gc_release)    (uint64_t proc_ptr, uint64_t gc_handle);

} VestaPluginAPI;

/**
 * @brief Tipo del punto de entrada que el plugin debe exportar.
 *
 * El plugin debe definir y exportar:
 * @code
 *   void vesta_init(const VestaPluginAPI *api) { ... }
 * @endcode
 */
typedef void (*VestaInitFn)(const VestaPluginAPI *api);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VESTA_PLUGIN_H */
