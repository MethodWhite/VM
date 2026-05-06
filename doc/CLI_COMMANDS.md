# Referencia de comandos del REPL de VestaVM

Referencia completa de todos los comandos disponibles en la terminal interactiva.
Para la guia de uso y edicion de linea, consulta [CLI_REPL.md](./CLI_REPL.md).

---

## Comandos especiales (no aparecen en cmd_table)

| Comando | Uso | Descripcion |
|---------|-----|-------------|
| `exit` | `exit` | Salir del REPL y guardar historial |
| `help` | `help` | Mostrar la referencia de todos los comandos |
| `history` | `history` | Listar el historial con indices |
| `!N` | `!3` | Re-ejecutar la entrada N del historial |
| `:prev` | `:prev` | Entrada anterior del historial (legacy) |
| `:next` | `:next` | Entrada siguiente del historial (legacy) |
| `complete` | `complete PREF` | Listar completions para un prefijo |
| `interprete` | `interprete` | Abrir el interprete interactivo de Vesta |

---

## Navegacion del sistema de ficheros

### ls

```
ls [ruta|patron]
```

Lista el directorio actual o la ruta indicada. Soporta patrones glob:

```
ls                  # directorio actual
ls src/             # contenido de src/
ls *.vel            # todos los .vel en el directorio actual
ls tests/**/*.velb  # recursivo
```

La salida distingue directorios (con `/` al final) de archivos regulares.

### cd

```
cd [ruta|-]
```

Cambia el directorio de trabajo. Sin argumento va al HOME. `-` vuelve al
directorio anterior (equivalente a `cd -` en bash).

### pwd

```
pwd
```

Imprime el directorio de trabajo actual (ruta absoluta).

### mkdir

```
mkdir <dir> [dir2 ...]
```

Crea uno o mas directorios, incluyendo todos los padres intermedios necesarios
(equivalente a `mkdir -p`).

### touch

```
touch <archivo> [archivo2 ...]
```

Crea el archivo si no existe (vacio) o actualiza su timestamp si ya existe.

### cp

```
cp [-r] <src> <dst>
```

Copia un archivo o directorio. Usa `-r` para copiar directorios de forma recursiva.
Si el destino es un directorio existente, el archivo se copia dentro.

### mv

```
mv <src> <dst>
```

Mueve o renombra un archivo o directorio. Si el destino es un directorio existente,
el origen se mueve dentro.

### rm

```
rm [-r] <ruta> [ruta2 ...]
```

Elimina archivos. Con `-r` elimina directorios de forma recursiva (pide confirmacion
antes de borrar directorios no vacios). `-rf` omite la confirmacion.

### cat

```
cat [-n] [--raw] <archivo> [archivo2 ...]
```

Muestra el contenido de uno o varios archivos. Con `-n` numera las lineas.
Los archivos `.vel` se muestran con syntax highlight automaticamente; usa `--raw`
para desactivarlo.

### head

```
head [-n N] <archivo> [archivo2 ...]
```

Muestra las primeras N lineas de cada archivo (defecto: 10).

### tail

```
tail [-n N] [-f] <archivo> [archivo2 ...]
```

Muestra las ultimas N lineas (defecto: 10). Con `-f` sigue el archivo en tiempo
real (modo follow), mostrando nuevas lineas a medida que se escriben.
Pulsa cualquier tecla para detener `-f`.

### echo

```
echo <texto con $VARS>
```

Imprime el texto expandiendo referencias `$VAR` y `${VAR}` con los valores
del entorno del proceso.

---

## Compilacion y bytecode

### build

```
build <archivo.vel> [-o salida]
```

Compila un archivo Vesta (`.vel`) a bytecode (`.velb`). El compilador realiza
tres pasadas: recoleccion de simbolos, evaluacion de expresiones y generacion
de codigo. Si no se especifica `-o`, la salida toma el nombre del fuente
con extension `.velb`.

### vpp

```
vpp <archivo.vel> [-o salida] [-D N] [-I ruta] [-M ruta]
```

Preprocesa un fuente Vesta y muestra o guarda el resultado expandido, sin llegar
a la fase de compilacion.

| Flag | Descripcion |
|------|-------------|
| `-o salida` | Guardar resultado en fichero en lugar de imprimirlo |
| `-D N` | Nivel de expansion de macros (0 = sin macros) |
| `-I ruta` | Directorio de busqueda para includes |
| `-M ruta` | Directorio de busqueda para modulos |

### disasm

```
disasm <archivo.velb>
```

Desensambla un archivo `.velb` usando las tablas de decodificacion internas
de VestaVM. La salida muestra el offset, los bytes raw y el mnemonico de cada
instruccion.

---

## Ejecucion de programas

### exec

```
exec <archivo.velb> [--schedulers N]
```

Carga y ejecuta un archivo `.velb` en un nuevo manager en background. El numero
de schedulers (hilos de ejecucion cooperativa) por defecto es 1.

### run

```
run <nombre> <ruta.velb> [--schedulers N] [--stats]
```

Como `exec` pero asigna un nombre identificativo al manager y opcionalmente
imprime estadisticas (tiempo, instrucciones ejecutadas, MIPS) al finalizar.

```
vesta [src]> run servidor servidor.velb --schedulers 4 --stats
[run] 'servidor' lanzado en background (id=1)
```

---

## Gestion de managers y VMs

### vms

```
vms
```

Lista todos los managers activos con su ID numerico, nombre, numero de VMs
internas y estado del servidor TCP.

### kill

```
kill <id>
```

Detiene el scheduler de la VM, espera a que todos los procesos virtuales
terminen y libera el manager con el ID indicado.

### mgrinfo

```
mgrinfo <id>
```

Informacion detallada de un manager: configuracion del loader, VMs internas,
estado del listener TCP y parametros del DistRuntime.

### vmstat

```
vmstat <mgr_id> <vm_id>
```

Estadisticas en tiempo real de una instancia VM: schedulers activos, procesos
en cola, instrucciones ejecutadas, tiempo de wall-clock.

### schedtop

```
schedtop <mgr_id> <vm_id>
```

Vista interactiva estilo `htop` de los schedulers y procesos virtuales de una VM.

**Navegacion en schedtop:**

| Tecla | Accion |
|-------|--------|
| `j` / `k` o flechas | Moverse entre schedulers o procesos |
| `Enter` | Entrar en la lista de procesos de un scheduler |
| `i` | Ver informacion completa del proceso seleccionado |
| `x` | Matar el proceso seleccionado |
| `q` / `Esc` | Volver o salir |

### procinfo

```
procinfo <mgr_id> <vm_id> <sched_id> <pid>
```

Estado completo de un proceso virtual: todos los registros (R00-R15, RIP, RSP, RBP),
flags, estado del proceso (NEW, READY, RUN, HALT, DEAD) y estadisticas.

---

## Aliases

### alias

```
alias [nombre[='expansion']]
```

Sin argumentos lista todos los aliases activos con su expansion.
Con `nombre='expansion'` define o redefine un alias.

```
alias ll="ls -la"
alias gs="git status"
alias build-debug="build src/main.vel -o prog"
alias              # listar todos
```

Los aliases se guardan en `vm_aliases.txt` y persisten entre sesiones.

### unalias

```
unalias <nombre> [nombre2 ...]
```

Elimina uno o varios aliases del almacen persistente.

```
unalias ll
unalias ll gs build-debug
```

---

## Variables de entorno

### env

```
env [list|get KEY|set KEY VALUE|unset KEY|save [f]|load [f]]
```

| Subcomando | Descripcion |
|------------|-------------|
| `env` / `env list` | Listar todas las variables con colores |
| `env get KEY` | Mostrar el valor de KEY |
| `env set KEY VALUE` | Establecer KEY=VALUE |
| `env set KEY=VALUE` | Forma alternativa con `=` |
| `env unset KEY` | Eliminar KEY del entorno |
| `env save [f]` | Guardar entorno en `vm_env.txt` (o fichero indicado) |
| `env load [f]` | Cargar entorno desde `vm_env.txt` (o fichero indicado) |

El entorno se carga de `vm_env.txt` al arrancar y se guarda al cerrar.

---

## Configuracion del REPL

### set

```
set [clave [= valor]]
```

Sin argumentos lista todos los parametros. Con solo la clave muestra su valor
actual. Con clave y valor aplica el cambio inmediatamente.

| Parametro | Tipo | Defecto | Descripcion |
|-----------|------|---------|-------------|
| `history_max` | entero | 2000 | Numero maximo de entradas en el historial |
| `multiline_end` | cadena | `;;` | Terminador de bloque multilinea |
| `aliases_file` | ruta | `vm_aliases.txt` | Fichero de aliases persistente |
| `env_file` | ruta | `vm_env.txt` | Fichero de entorno persistente |

### source

```
source <archivo>
```

Ejecuta linea a linea un fichero de comandos del REPL. Las lineas vacias
y las que comienzan por `#` se ignoran.

---

## Tareas en background

### jobs

```
jobs
```

Muestra la tabla de tareas registradas por `run` y `exec`:

```
ID     ESTADO   DURACION    NOMBRE
--------------------------------------------------
1      RUN      5.2s        servidor
2      DONE     12.0s       batch  [OK]
```

### wait

```
wait <job_id>
```

Bloquea el prompt hasta que el job indicado complete su ejecucion.
Si el job ya ha terminado, informa inmediatamente.

---

## Utilidades

### type

```
type <nombre> [nombre2 ...]
```

Identifica la naturaleza de cada nombre:

- **alias**: muestra la expansion.
- **comando especial del REPL**: `exit`, `help`, `history`, etc.
- **comando interno**: encontrado en la tabla de comandos.
- **no encontrado**: no esta registrado en ningun sitio.

### watch

```
watch <intervalo_ms> <comando ...>
```

Ejecuta repetidamente un comando del REPL con el intervalo indicado en
milisegundos. Muestra la hora de cada ejecucion en la cabecera. Pulsa
cualquier tecla para detenerlo.

```
watch 1000 vms              # estado de VMs cada segundo
watch 500  ls *.vel         # listar .vel cada 500 ms
watch 2000 vmstat 1 1       # estadisticas de VM cada 2 s
```

### cmd

```
cmd <comando de shell>
```

Ejecuta un comando del sistema operativo en un hilo separado de forma
asincrona. El prompt vuelve inmediatamente sin esperar a que el comando
termine. La salida aparece cuando el comando completa.

Para ejecucion sincrona de comandos externos simplemente escribe el
comando directamente (o como expansion de un alias).

---

## Scripting VestaShell

### script

```
script <archivo.vsh>
```

Ejecuta un fichero VestaShell (`.vsh`) con el interprete integrado. El script
tiene acceso a todos los comandos del REPL como si fueran funciones:

```
vesta> script ejemplos/despliegue.vsh
```

Para ejecutar un script sin abrir el REPL interactivo, usa el flag de linea
de comandos:

```bash
./vm --script mi_script.vsh
```

Ver [CLI_VSH.md](./CLI_VSH.md) para la referencia completa del lenguaje VestaShell.

---

## Runtime distribuido

Ver [CLI_DIST.md](./CLI_DIST.md) para la referencia completa del subcomando `dist`.
