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
 * @file native_ffi.cpp
 * @brief Implementacion del sistema FFI de VestaVM.
 *
 * Implementa el internado de strings (intern_module, intern_function,
 * intern_signature), la carga de modulos nativos (load_native_module)
 * y la resolucion de simbolos y aplicacion de patches en el bytecode
 * (resolve_native_symbol, resolve_all).
 */
#include "ffi/native_ffi.h"
#include "ffi/virtual_lib_registry.h"  // Phase MC.20: lookup_virtual_fn

#ifdef _WIN32
#include "windows.h"
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#include <vector>

namespace ffi {

    uint32_t FFI::intern_module(const std::string &name) {
        auto it = module_map.find(name);
        if (it != module_map.end())
            return it->second; /* ya existe: devolver indice almacenado */

        /* nuevo modulo: agregar al vector y registrar su indice */
        uint32_t idx     = static_cast<uint32_t>(modules.size());
        modules.push_back(name);
        module_map[name] = idx;
        return idx;
    }

    uint32_t FFI::intern_function(const std::string &name) {
        auto it = function_map.find(name);
        if (it != function_map.end())
            return it->second;

        uint32_t idx       = static_cast<uint32_t>(functions.size());
        functions.push_back(name);
        function_map[name] = idx;
        return idx;
    }

    uint32_t FFI::intern_signature(const std::string &sig) {
        auto it = signature_map.find(sig);
        if (it != signature_map.end())
            return it->second;

        uint32_t idx        = static_cast<uint32_t>(signatures.size());
        signatures.push_back(sig);
        signature_map[sig]  = idx;
        return idx;
    }

#ifdef _WIN32
    /* Devuelve el directorio donde reside @c vm.exe (sin trailing slash). */
    static std::string vm_exe_dir(void) {
        char buf[1024];
        DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
        if (n == 0 || n == sizeof(buf)) return std::string();
        std::string p(buf, n);
        size_t pos = p.find_last_of("/\\");
        if (pos == std::string::npos) return std::string();
        return p.substr(0, pos);
    }
#else
    static std::string vm_exe_dir(void) {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return std::string();
        buf[n] = 0;
        std::string p(buf);
        size_t pos = p.find_last_of('/');
        if (pos == std::string::npos) return std::string();
        return p.substr(0, pos);
    }
#endif

    void *FFI::load_native_module(const std::string &name) {
        /* consultar cache antes de llamar al SO */
        auto it = native_modules.find(name);
        if (it != native_modules.end())
            return it->second;

        /* Intentar varios paths candidatos en orden:
         *   1. name tal cual (compatibilidad: usuario puede pasar path completo)
         *   2. <vm_exe_dir>/<name>(.dll/.so)
         *   3. <cwd>/<name>(.dll/.so)
         *
         * Para programas Vesta tipicos, las DLLs nativas viven en
         * @c <vm_exe_dir>/stdlib/native/<subdir>/lib.dll mientras que el
         * usuario puede ejecutar @c vesta --run desde cualquier dir.
         * El @c name viene ya con el subdir (e.g. @c "stdlib/native/io/vesta_io")
         * asi que basta con prefixar @c vm_exe_dir/ . */
        std::vector<std::string> candidates;
        candidates.push_back(name);
        {
            std::string dir = vm_exe_dir();
            if (!dir.empty()) {
                candidates.push_back(dir + "/" + name);
            }
        }
        std::string last_err;

        for (const auto &cand : candidates) {
#ifdef _WIN32
            HMODULE h = LoadLibraryA(cand.c_str());
            if (h) {
                native_modules[name] = static_cast<void *>(h);
                return static_cast<void *>(h);
            }
            DWORD err = GetLastError();
            last_err  = "LoadLibraryA('" + cand + "') codigo " + std::to_string(err);
#else
            void *h = dlopen(cand.c_str(), RTLD_LAZY);
            if (h) {
                native_modules[name] = h;
                return h;
            }
            const char *e = dlerror();
            last_err = std::string("dlopen('") + cand + "') " + (e ? e : "");
#endif
        }
        throw FFIError(
            "FFI: No se pudo cargar la libreria '" + name +
            "' tras intentar " + std::to_string(candidates.size()) +
            " paths. Ultimo error: " + last_err
        );
    }

    void *FFI::resolve_native_symbol(void *module, const std::string &func) {
#ifdef _WIN32
        FARPROC p = GetProcAddress(static_cast<HMODULE>(module), func.c_str());
        if (!p) {
            DWORD err = GetLastError();
            throw FFIError(
                "FFI: No se pudo resolver el simbolo '" + func +
                "' (GetProcAddress fallo, codigo: " + std::to_string(err) + ")"
            );
        }
        return reinterpret_cast<void *>(p);
#else
        dlerror(); /* limpiar cualquier error previo de dlsym */
        void *p = dlsym(module, func.c_str());
        if (!p) {
            const char *err = dlerror();
            throw FFIError(
                "FFI: No se pudo resolver el simbolo '" + func +
                "' (dlsym fallo: " + std::string(err ? err : "error desconocido") + ")"
            );
        }
        return p;
#endif
    }

    void FFI::resolve_all(uint8_t *file_base, uint64_t offset_real_bytecode) {
        for (auto &[key, entry]: imports) {
            const std::string &mod_name  = modules[key.module_idx];
            const std::string &func_name = functions[key.function_idx];

            /* virtual lib registry FIRST.  Si el par
             * @c (mod_name, func_name) esta registrado in-process,
             * usamos ese fn_ptr y skipeamos LoadLibrary.  Esto permite
             * que macros invoquen `extern "vesta_comptime" fn ...`
             * desde codigo lowered al VM. */
            void *fn = ffi::lookup_virtual_fn(mod_name, func_name);
            if (!fn) {
                /* cargar modulo y resolver la funcion nativa */
                void *mod = load_native_module(mod_name);
                fn        = resolve_native_symbol(mod, func_name);
            }
            entry.resolved_ptr = fn;

            /* parchear cada instruccion CALLN que referencia este simbolo */
            for (uint32_t off : entry.patch_sites) {
                uint64_t real_offset = off + offset_real_bytecode;
                patch_call(file_base, static_cast<uint32_t>(real_offset), fn);
            }
        }
    }

    void FFI::call_plugin_inits(const VestaPluginAPI *api) {
        for (auto &[mod_name, mod_handle] : native_modules) {
            /* omitir modulos ya inicializados como plugins en sesiones anteriores */
            if (initialized_plugins.count(mod_name))
                continue;

            /* buscar el simbolo "vesta_init" en el modulo sin lanzar excepcion si no existe */
#ifdef _WIN32
            FARPROC sym = GetProcAddress(static_cast<HMODULE>(mod_handle), "vesta_init");
            void   *fn  = reinterpret_cast<void *>(sym);
#else
            dlerror(); /* limpiar error previo para distinguir NULL real de error */
            void *fn = dlsym(mod_handle, "vesta_init");
            if (!fn && dlerror())
                fn = nullptr; /* solo omitir si fue un error; NULL puede ser simbolo valido en edge cases */
#endif
            if (!fn)
                continue; /* el modulo no es un plugin Vesta */

            /* llamar al punto de entrada del plugin con la API del manager */
            VestaInitFn init_fn = reinterpret_cast<VestaInitFn>(fn);
            init_fn(api);

            /* marcar como inicializado para no repetir en cargas sucesivas */
            initialized_plugins.insert(mod_name);
        }
    }

} // namespace ffi
