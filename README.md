# VestaVM

<div style="display: flex; align-items: center; gap: 20px;">
  <img src="./Component 1.svg" width="250" height="250" />
  <p>
    Vesta es una VM de bajo nivel y distribuida que está siendo desarrollada para un lenguaje de programación.
    Esta VM se basa en el concepto de registros y memoria. Puede encontrar más información y documentación en el repositorio:<br><br>
    <a href="https://github.com/desmonHak/VMdoc">https://github.com/desmonHak/VMdoc</a>
  </p>
</div>

----

## Enlaces de interes

- [Licencia del proyecto](./LICENSE.md)  
  Aqui puede encontrar que puede y que no puede hacer con el codigo.

- [Como contribuir de forma correcta](./doc/CONTRIBUTING.md)  
  Guia para aportar al proyecto de manera ordenada y segura.

- [Informacion sobre el gitflow que manejamos](./doc/github_work.md)  
  Explicacion del flujo de trabajo con ramas y versiones.

- [Que dependencias usamos](./doc/DEPENDENCIES.md)  
  Lista de librerias y herramientas necesarias para compilar.

- [Guia de la terminal interactiva (REPL)](./doc/CLI_REPL.md)  
  Edicion de linea, historial, TAB completion, Ctrl+R, aliases, variables de
  entorno, scripts de inicio (`~/.vestarc`), bloques multilinea y configuracion.

- [Referencia completa de comandos del REPL](./doc/CLI_COMMANDS.md)  
  Todos los comandos con sintaxis detallada y ejemplos: archivos, compilacion,
  ejecucion de VMs, aliases, entorno, jobs, watch, type y mas.

- [Runtime distribuido — comandos del REPL](./doc/CLI_DIST.md)  
  Subcomando `dist`: crear nodos, conectar pares, autenticacion TLS/token,
  descubrimiento UDP y envio de bytecode a nodos remotos.

- [VestaShell — lenguaje de scripting (.vsh)](./doc/CLI_VSH.md)  
  Referencia completa del lenguaje de scripting embebido: tipos, control de
  flujo, funciones con closures, listas, mapas, try/catch, importacion de
  modulos, funciones integradas e integracion con el REPL.


----

## Compilacion

clonar usando:

```bash
git clone  --recursive https://github.com/desmonHak/VM.git
```

## Instalacion en linux

```bash
sudo apt install build-essential cmake libssl-dev
```

### Arch linux

```bash
sudo pacman -S openssl
```

## Compilacion en linux

compilacion con CMAKE:
```bash
mkdir build
cd build
cmake -Wno-dev ..
cmake --build .
```
> En caso de que CMAKE le de error por usar una version no compatible con la version CMAKE de Keystone, puede intentar forzar el uso
> de la version que usted use añadiendo la flag ``-DCMAKE_POLICY_VERSION_MINIMUM=3.5`` para cambiar la version minima.
> Si compila en Windows, debe Mingw32/64 o TDM-GCC, y usar la flag ``-G "MinGW Makefiles"``

Si prefiere usar XMAKE:
```bash
curl -fsSL https://xmake.io/shget.text | bash
xmake f --toolchain=clang -m debug -v
xmake run
```

En caso de un error similar a este en linux:

```c
Enabling CAPSTONE_ARC_SUPPORT
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 6:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 8:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 28:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 29:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 40:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 42:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 54:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 65:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 68:
/root/WinDiskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 71: Syntax error: word unexpected (expecting "in")
CMake Error at libs/SourceCode/keystone/llvm/cmake/modules/GetHostTriple.cmake:24 (message):
  Failed to execute
  /root/WinDiskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess
Call Stack (most recent call first):
  libs/SourceCode/keystone/llvm/cmake/config-ix.cmake:293 (get_host_triple)
  libs/SourceCode/keystone/llvm/CMakeLists.txt:325 (include)
```

puede necesitar convertir los CRLF de Windows en LF de linux usando el siguiente comandos:

```bash
find ../libs/SourceCode -type f -exec dos2unix {} \;
```

generalmente esto permite que el proyecto se compile correctamente si ocurre el anterior error.

### Elegir modo 'Debug' o 'Release'

debug:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

release:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Valgrind/profiling:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./vm```
```

----

## Uso del binario principal

El ejecutable `vm` (o `vm.exe` en Windows) acepta distintos modos de operacion mediante flags.
Los modos son mutuamente excluyentes: solo uno es activo por invocacion.

### Compilacion de fuentes Vesta (`.vel` → `.velb`)

```bash
# Compilar un unico archivo fuente
vm --worker src/main.vel -o programa
vm --build  src/main.vel -o programa        # alias de --worker

# Compilar un directorio completo en paralelo
vm --driver src/ -j 8 -o programa.velb      # 8 hilos
vm --driver src/ -j 0 -o programa.velb      # todos los nucleos disponibles
```

El compilador realiza tres pasadas sobre el fuente: recoleccion de simbolos,
evaluacion de expresiones y generacion de bytecode `.velb`.

### Ejecucion de bytecode (`.velb`)

```bash
# Ejecucion basica
vm --run programa.velb

# Con varios schedulers (hilos de ejecucion)
vm --run programa.velb --schedulers 4

# Con estadisticas al finalizar (tiempo, instrucciones, MIPS)
vm --run programa.velb --stats
```

Cada scheduler ejecuta procesos virtuales de forma cooperativa.
La VM arranca con un DistRuntime minimo que permite IPC local
(instrucciones `msgsend`/`msgrecv`) sin abrir ningun puerto de red.

### Scripting VestaShell (`.vsh`)

```bash
# Ejecutar un script .vsh sin abrir el REPL interactivo
vm --script mi_script.vsh
```

Los scripts `.vsh` tienen acceso a las mismas funciones integradas que el REPL
(filesystem, matematicas, shell, import) ademas de poder llamar a cualquier
comando interno de la terminal. Ver [doc/CLI_VSH.md](./doc/CLI_VSH.md) para
la referencia completa del lenguaje.

### Ensamblado y desensamblado nativo (Keystone / Capstone)

```bash
# Ensamblar un archivo .asm nativo (x86, ARM, etc.) con Keystone
vm --asm-file src/main.asm --arch X86-64 -o salida

# Desensamblar un binario con Capstone
vm --disasm-file programa.bin --arch X86-64

# Ver arquitecturas soportadas
vm --list-arch
```

Arquitecturas soportadas: `X86-32`, `X86-64`, `ARM`, `AArch64` (y mas segun Capstone/Keystone).

### Preprocesador (solo con `VESTA_BUILD_PREPROCESSOR=ON`)

```bash
# Expandir macros y directivas sin compilar
vm --preprocess-only src/main.vel -o resultado.vel
vm --preprocess-only src/main.vel                   # imprime a stdout
```

----

## Runtime distribuido — flags de linea de comandos

VestaVM implementa un protocolo propio llamado **VDP** (Vesta Distribution Protocol)
sobre TCP (con o sin TLS) para comunicar instancias VM entre si.
Cada nodo puede enviar procesos remotos (`rspawn`), intercambiar mensajes (`msgsend`/`msgrecv`)
y sincronizar regiones de memoria (`memsync`).

### Modo servidor distribuido puro

Arranca un nodo VDP que acepta conexiones entrantes sin ejecutar bytecode propio.
Util para servidores de infraestructura o nodos de computo dedicados.

```bash
# Servidor sin cifrado
vm --dist-server --dist-port 7789 --dist-name nodo-1

# Servidor con descubrimiento UDP automatico de pares en la LAN
vm --dist-server --dist-port 7789 --dist-discover --dist-name nodo-1

# Servidor con autenticacion por token
vm --dist-server --dist-port 7789 --dist-token mi-secreto

# Servidor con TLS (certificados en formato PEM)
vm --dist-server --dist-port 7789 \
   --dist-tls --dist-cert cert.pem --dist-key key.pem --dist-ca ca.pem

# Servidor que ademas conecta a un nodo estatico al arrancar
vm --dist-server --dist-port 7789 --dist-add-node 192.168.1.100:7789
```

El proceso queda esperando conexiones indefinidamente hasta recibir `Ctrl+C`.

### Ejecutar bytecode en modo distribuido

Combina `--run` con los flags `--dist-*` para que el programa Vesta pueda
usar las instrucciones distribuidas (`rspawn`, `msgsend`, `msgrecv`, `memsync`).

```bash
# Ejecutar y conectarse a un nodo remoto
vm --run programa.velb --dist-add-node 192.168.1.100:7789

# Ser servidor Y cliente a la vez
vm --run programa.velb --dist-port 7789 --dist-add-node 192.168.1.100:7789

# Con autenticacion y descubrimiento
vm --run programa.velb \
   --dist-port 7789 \
   --dist-token mi-secreto \
   --dist-discover \
   --schedulers 4
```

### Referencia de todos los flags `--dist-*`

| Flag | Tipo | Valor por defecto | Descripcion |
|------|------|-------------------|-------------|
| `--dist-server` | bool | — | Modo servidor puro; espera conexiones VDP sin bytecode |
| `--dist-port N` | uint16 | 0 | Puerto TCP del servidor VDP local (0 = sin servidor) |
| `--dist-discover` | bool | — | Activa descubrimiento UDP de nodos en la LAN |
| `--dist-discover-port N` | uint16 | 7790 | Puerto UDP para el descubrimiento |
| `--dist-name NOMBRE` | string | `""` | Nombre legible del nodo (para logs y registros) |
| `--dist-node-id ID` | uint64 | 0 | ID de 64 bits del nodo (0 = generar automaticamente) |
| `--dist-add-node IP:PUERTO` | string | — | Nodo estatico a registrar y conectar al arrancar (repetible) |
| `--dist-token TOKEN` | string | `""` | Token de autenticacion en texto plano (se almacena como SHA-256) |
| `--dist-tls` | bool | — | Usar TLS en conexiones VDP salientes y entrantes |
| `--dist-cert RUTA` | string | `""` | Ruta al certificado TLS del nodo local (PEM) |
| `--dist-key RUTA` | string | `""` | Ruta a la clave privada TLS (PEM) |
| `--dist-ca RUTA` | string | `""` | Ruta al CA bundle para verificar pares (PEM) |

**Autenticacion:** si se usa `--dist-token`, el token se hashea con SHA-256 y se
intercambia via CRAM durante el handshake VDP. Si se usa `--dist-tls`, el handshake
TLS ocurre antes del handshake VDP. Ambos mecanismos son combinables (mTLS + token).

**Descubrimiento UDP:** con `--dist-discover` cada nodo emite broadcasts `NODE_DISCOVER`
periodicamente y escucha respuestas `NODE_ANNOUNCE`. Los nodos descubiertos se conectan
automaticamente. Requiere acceso de red a la LAN.

----

## Terminal interactiva (REPL)

Ejecutar `vm` sin argumentos abre el interprete interactivo:

```bash
./vm
vesta [directorio]>
```

El prompt muestra dinamicamente el directorio de trabajo actual.
Para una guia completa de uso, edicion de linea, aliases y todas las funciones
consulta la documentacion detallada:

- **[Guia de la terminal (CLI_REPL.md)](./doc/CLI_REPL.md)** — edicion de linea,
  historial, TAB completion, Ctrl+R, aliases, variables de entorno, scripts de inicio
- **[Referencia de comandos (CLI_COMMANDS.md)](./doc/CLI_COMMANDS.md)** — todos los
  comandos con su sintaxis completa y ejemplos
- **[Runtime distribuido — REPL (CLI_DIST.md)](./doc/CLI_DIST.md)** — subcomando
  `dist` con todos los flujos y opciones de seguridad
- **[VestaShell (.vsh) (CLI_VSH.md)](./doc/CLI_VSH.md)** — lenguaje de scripting
  embebido: tipos, funciones, closures, listas, mapas, try/catch, import, builtins

### Edicion de linea y navegacion

| Tecla | Accion |
|-------|--------|
| Flechas arriba/abajo | Navegar por el historial |
| `Ctrl+R` | Busqueda incremental inversa en el historial |
| `TAB` | Completar nombre de comando o ruta de archivo |
| `Ctrl+A` / `Ctrl+E` | Inicio / fin de linea |
| `Ctrl+K` / `Ctrl+U` | Borrar hasta fin / hasta inicio de linea |
| `Ctrl+W` | Borrar la palabra anterior |
| `Ctrl+L` | Limpiar pantalla |

### Resumen de comandos disponibles

| Categoria | Comandos |
|-----------|----------|
| Navegacion | `ls`, `cd`, `pwd`, `cls` / `clear` |
| Archivos | `touch`, `cp`, `mv`, `rm`, `mkdir`, `cat`, `head`, `tail`, `echo` |
| Compilacion | `build`, `vpp`, `disasm` |
| Ejecucion VM | `exec`, `run`, `vms`, `kill`, `mgrinfo`, `vmstat`, `schedtop`, `procinfo` |
| Aliases | `alias`, `unalias` |
| Entorno | `env` |
| REPL | `set`, `source`, `type`, `watch`, `jobs`, `wait`, `cmd` |
| Scripting | `script` (VestaShell `.vsh`) |
| Distribuido | `dist` |

Los comandos no reconocidos se ejecutan directamente como comandos del sistema
operativo, lo que permite usar aliases que apunten a herramientas externas:

```
vesta [src]> alias ll="ls -la"
vesta [src]> ll              # ejecuta: ls -la
vesta [src]> git status      # ejecutado como comando del SO
```

----

## Runtime distribuido — comandos del REPL

> Referencia completa en [doc/CLI_DIST.md](./doc/CLI_DIST.md).

Los comandos `dist` permiten gestionar el runtime distribuido de cualquier VM
activa desde el propio REPL, sin necesidad de reiniciar el proceso.

> **Nota:** `<mgr_id>` es el numero que muestra `vms`. El `vm_id` por defecto es `1`
> (el primer VM creado en un manager). Usar `--vm-id N` para apuntar a otro VM.

### Crear un nodo servidor (forma mas directa)

```
dist new [--port PUERTO] [--discover] [--discover-port PUERTO]
         [--name NOMBRE] [--node-id ID] [--schedulers N]
         [--token TOKEN] [--tls] [--cert C] [--key K] [--ca CA]
```

Crea un manager nuevo con una VM, configura y arranca el DistRuntime.
El manager persiste hasta que el usuario lo elimine con `kill <id>`.
Imprime el `mgr_id` asignado para usarlo con los demas subcomandos.

```
vesta> dist new --port 7789 --name servidor-local
[dist] Nodo creado (mgr_id=1 vm_id=1) puerto=7789
  dist info 1           -> estado
  dist add-node 1 <ip> <p>  -> conectar nodo
  dist stop 1           -> detener servidor VDP
  kill 1                -> eliminar
```

### Iniciar DistRuntime en un manager existente

```
dist start <mgr_id> [--port P] [--discover] [--discover-port P]
           [--name N] [--node-id ID] [--vm-id N]
           [--token T] [--tls] [--cert C] [--key K] [--ca CA]
```

Reconfigura y arranca el DistRuntime de la VM indicada.
Util cuando se lanza un programa con `run` y luego se quiere activar la red.
Si el DistRuntime ya estaba activo, lo detiene y lo reemplaza con la nueva configuracion.

```
vesta> run mi-app programa.velb
[run] 'mi-app' lanzado en background (id=1)
vesta> dist start 1 --port 7789 --token secreto
[dist] DistRuntime iniciado en puerto 7789
```

### Detener el servidor VDP

```
dist stop <mgr_id> [--vm-id N]
```

Cierra el servidor TCP, todas las sesiones activas y el hilo de descubrimiento UDP.
El manager y la VM siguen existiendo; solo se desactiva la red distribuida.

### Inspeccionar el estado

```
dist info <mgr_id> [--vm-id N]
```

Muestra el ID de nodo local (64 bits), el puerto, y un resumen de todos los nodos
del registro con su estado actual (`ACTIVO`, `DESCONECTADO`, etc.).

```
vesta> dist info 1
=== DistRuntime | mgr=1  vm=1 ===
  Node ID local : 0x7614fed977ada27f
  Nodos en registro : 2
    [0] 192.168.1.100:7789  ACTIVO  (nodo-remoto)
    [1] 192.168.1.101:7789  DESCONECTADO  (nodo-caido)
```

### Listar nodos

```
dist list <mgr_id> [--vm-id N]
```

Tabla completa con indice, IP:puerto, nombre, estado y tipo (estatico/dinamico)
de cada nodo del registro.

```
vesta> dist list 1
IDX  IP:PUERTO              NOMBRE             ESTADO          TIPO
----------------------------------------------------------------------
0    192.168.1.100:7789     nodo-remoto        ACTIVO          estatico
1    192.168.1.101:7789     nodo-caido         DESCONECTADO    estatico
```

### Registrar y conectar un nodo

```
dist add-node <mgr_id> <ip> <puerto>
             [--name NOMBRE] [--vm-id N]
             [--token TOKEN] [--tls] [--cert C] [--key K] [--ca CA]
```

Agrega un nodo al registro de forma estatica e intenta conectarse de inmediato.
La autenticacion (token y/o TLS) debe coincidir con la configuracion del servidor remoto.
El indice asignado (`idx`) se usa en `dist connect` y en las instrucciones de bytecode.

```
vesta> dist add-node 1 192.168.1.100 7789 --name servidor-A --token secreto
[dist] Nodo registrado: 192.168.1.100:7789  (idx=0)
```

### Reconectar a un nodo ya registrado

```
dist connect <mgr_id> <node_idx> [--vm-id N]
```

Intenta reconectar a un nodo que ya existe en el registro (por su indice).
Util cuando un nodo se desconecto y volvio a estar disponible.

### Descubrimiento de nodos en la LAN

```
dist discover <mgr_id> [--vm-id N]
```

Emite un broadcast UDP `NODE_DISCOVER` de forma inmediata (sin esperar al ciclo
periodico). Los nodos que escuchan en el puerto de descubrimiento responden con
`NODE_ANNOUNCE` y quedan registrados automaticamente.
Requiere que el DistRuntime haya sido iniciado con `--discover` o `dist start --discover`.

### Enviar bytecode a ejecutar en un nodo remoto

```
dist run <mgr_id> <node_idx> <archivo.velb> [--vm-id N]
```

Carga el archivo `.velb` en la VM local, mapea su bytecode en la memoria virtual de la VM
y lo envia al nodo remoto indicado por `node_idx` mediante el protocolo VDP (`VDP_RSPAWN`).

El nodo remoto recibe el bytecode, crea un proceso local y lo ejecuta.
La respuesta llega de forma asincrona: `dist run` devuelve de inmediato un `GcHandle`
de un `FutureObject` que quedara resuelto cuando el nodo remoto termine y envie `VDP_RSPAWN_ACK`.

**Requisitos previos:**
- El nodo (`node_idx`) debe estar en estado `ACTIVO` en `dist list`.
- La VM local debe tener un `dist_runtime` activo (creado con `dist new` o `dist start`).
- El archivo `.velb` debe ser un ejecutable VestaLangBinary valido.

**Ejemplo:**

```
# Compilar el programa
vesta> build src/tarea.vel -o tarea.velb

# Arrancar nodo local y conectar al remoto
vesta> dist new --port 7790 --name nodo-local
[dist] Nodo creado (mgr_id=1 vm_id=1) puerto=7790
vesta> dist add-node 1 192.168.1.100 7789 --name nodo-remoto
[dist] Nodo registrado: 192.168.1.100:7789  (idx=0)

# Enviar ejecucion al nodo remoto (idx=0)
vesta> dist run 1 0 tarea.velb
[dist run] Bytecode enviado a nodo[0] desde tarea.velb
  FutureObject GcHandle = 0x0000001a
  Usa 'await' en bytecode para obtener el resultado cuando el nodo responda.
```

El `GcHandle` devuelto puede ser referenciado desde bytecode `.vel` con la instruccion
`await` para bloquear el proceso actual hasta que el nodo remoto complete la tarea:

```asm
; En el proceso que lanzo dist run (GcHandle = 0x1a en este ejemplo)
mov  r1, 0x1a
await r1          ; bloquea hasta recibir VDP_RSPAWN_ACK; r0 = resultado del proceso remoto
```

### Ayuda del subcomando dist

```
dist help
```

### Referencia rapida de subcomandos `dist`

| Subcomando | Argumentos | Descripcion |
|------------|------------|-------------|
| `dist new` | `[--port P] [--name N] [--token T] [--tls] ...` | Crea y arranca un nodo servidor persistente |
| `dist start` | `<mgr_id> [--port P] [--discover] ...` | Configura y arranca DistRuntime en VM existente |
| `dist stop` | `<mgr_id> [--vm-id N]` | Detiene el servidor VDP y sesiones |
| `dist info` | `<mgr_id> [--vm-id N]` | Estado del nodo: ID, puerto, lista de pares |
| `dist list` | `<mgr_id> [--vm-id N]` | Tabla de nodos registrados con su estado |
| `dist add-node` | `<mgr_id> <ip> <puerto> [--name N] [--token T] ...` | Registra y conecta un nodo estatico |
| `dist connect` | `<mgr_id> <node_idx> [--vm-id N]` | Reconecta a un nodo ya registrado |
| `dist discover` | `<mgr_id> [--vm-id N]` | Emite broadcast UDP de descubrimiento |
| `dist run` | `<mgr_id> <node_idx> <archivo.velb> [--vm-id N]` | Carga .velb y lo envia a ejecutar en nodo remoto |
| `dist help` | — | Muestra la ayuda completa |

### Flujo tipico: dos nodos locales

```bash
# Terminal 1: arrancar el servidor
vm --dist-server --dist-port 7789 --dist-name servidor-A

# Terminal 2: REPL que se conecta al servidor
vm
vesta> dist new --port 7790 --name cliente-B
[dist] Nodo creado (mgr_id=1 vm_id=1) puerto=7790
vesta> dist add-node 1 127.0.0.1 7789 --name servidor-A
[dist] Nodo registrado: 127.0.0.1:7789  (idx=0)
vesta> dist list 1
IDX  IP:PUERTO         NOMBRE      ESTADO   TIPO
--------------------------------------------------
0    127.0.0.1:7789    servidor-A  ACTIVO   estatico
```

### Flujo tipico: nodo con TLS y token

```bash
# Generar certificados (ejemplo con openssl)
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes

# Servidor con TLS + token
vm --dist-server --dist-port 7789 \
   --dist-tls --dist-cert cert.pem --dist-key key.pem \
   --dist-token mi-secreto-compartido

# Cliente que se conecta
vesta> dist new --port 7790 --name cliente
vesta> dist add-node 1 192.168.1.100 7789 --name servidor \
       --tls --cert cert.pem --ca cert.pem --token mi-secreto-compartido
```

### Instrucciones de bytecode distribuidas

Desde codigo Vesta (`.vel`) se accede a la red distribuida mediante estas instrucciones:

| Instruccion | Descripcion |
|-------------|-------------|
| `rspawn r_fn, r_node` | Crear un proceso en el nodo remoto `r_node`; devuelve un FutureObject en R0 |
| `msgsend r_pid, r_addr, r_len` | Enviar un mensaje al proceso `r_pid` (local o remoto); R0 = 1 si OK |
| `msgrecv r_buf, r_max` | Recibir del propio mailbox; bloquea hasta que llegue un mensaje |
| `memsync r_params` | Sincronizar una region de memoria VM con un nodo remoto |

El indice de nodo que usan estas instrucciones (`node_idx`) es el que asigna
`dist add-node` o `dist new`. Se puede consultar con `dist list`.

----

## Forma de trabajo

Siga lellendo en: [github_work.md](./doc/github_work.md)

----
