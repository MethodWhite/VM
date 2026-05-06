# Terminal interactiva de VestaVM (REPL)

La terminal interactiva de VestaVM es un REPL (Read-Eval-Print Loop) completo
que combina gestion de la maquina virtual con utilidades de sistema de ficheros,
aliases persistentes, ejecucion de comandos de shell y control de tareas en background.

---

## Inicio rapido

```bash
# Abrir el REPL (sin argumentos)
./vm
vesta [directorio]>
```

El prompt muestra dinamicamente el nombre del directorio de trabajo actual.
Para salir escribe `exit` o pulsa `Ctrl+D`.

---

## Edicion de linea y navegacion

| Tecla | Accion |
|-------|--------|
| `Flecha arriba / abajo` | Navegar por el historial de comandos |
| `Ctrl+R` | Busqueda incremental inversa en el historial |
| `TAB` | Completar nombre de comando o ruta de archivo |
| `Ctrl+A` | Ir al inicio de la linea |
| `Ctrl+E` | Ir al final de la linea |
| `Ctrl+K` | Borrar desde el cursor hasta el final de la linea |
| `Ctrl+U` | Borrar desde el inicio hasta el cursor |
| `Ctrl+W` | Borrar la palabra anterior |
| `Ctrl+L` | Limpiar pantalla y repintar la linea actual |
| `Ctrl+C` | Cancelar la linea actual |
| `Ctrl+D` | EOF (salir si la linea esta vacia) |
| `Flecha izquierda / derecha` | Mover cursor caracter a caracter |
| `Inicio / Fin` | Inicio o fin de linea |
| `Supr` | Borrar caracter bajo el cursor |
| `Backspace` | Borrar caracter anterior |

### Busqueda inversa con Ctrl+R

```
vesta [src]> ^R
(reverse-search)`jmp': jmp r1, label_end
```

- Pulsa `Ctrl+R` para activar la busqueda.
- Escribe parte del comando buscado: el historial se filtra en tiempo real.
- Vuelve a pulsar `Ctrl+R` para saltar a la coincidencia anterior.
- `Enter` acepta la entrada encontrada.
- `ESC` o `Ctrl+G` cancela y restaura el buffer original.

### TAB completion

- **Primer token**: completa con nombres de comandos y aliases definidos.
- **Resto de tokens**: completa con rutas del sistema de ficheros.
- Si hay una sola coincidencia se completa directamente.
- Si hay varias, la primera pulsacion extiende al prefijo comun y la segunda muestra la lista.

---

## Historial

El historial se guarda automaticamente en `vm_history.txt` al cerrar el REPL
y se carga al abrirlo. Las entradas consecutivas duplicadas no se almacenan.

| Comando | Descripcion |
|---------|-------------|
| `history` | Lista todas las entradas con su indice |
| `!N` | Re-ejecuta la entrada numero N del historial |
| `:prev` / `:next` | Navegar por el historial (modo legacy; usa flechas en su lugar) |

Cambia la ruta del fichero de historial con:
```
set history_max 5000
```

---

## Comandos especiales del REPL

| Comando | Descripcion |
|---------|-------------|
| `help` | Muestra la referencia completa de todos los comandos |
| `history` | Lista el historial con indices |
| `!N` | Re-ejecuta la entrada N |
| `interprete` | Abre el interprete interactivo de Vesta (modo REPL de bytecode) |
| `complete <prefijo>` | Lista completions para el prefijo dado |
| `exit` | Salir del REPL |

---

## Comportamiento frente a comandos desconocidos

Si escribes un comando que no esta en la tabla interna del REPL (ni es un alias
expandido a un comando interno), **se ejecuta directamente como comando del sistema
operativo**. Esto hace que el REPL se comporte como una shell ligera:

```
vesta [src]> dir
vesta [src]> ls -la
vesta [src]> python --version
```

Esto tambien es lo que hace que los aliases funcionen correctamente cuando
apuntan a comandos externos:

```
vesta [src]> alias ll="ls -la"
vesta [src]> ll              # ejecuta: ls -la
```

Para ejecutar un comando de shell de forma asincrona (sin bloquear el prompt)
usa el comando `cmd`:

```
vesta [src]> cmd dir         # asincronos: el prompt vuelve inmediatamente
```

---

## Aliases

Los aliases permiten crear atajos para comandos frecuentes. Se guardan en
`vm_aliases.txt` y persisten entre sesiones.

### Definir un alias

```
alias nombre='expansion'
alias nombre="expansion con espacios"
alias ll="ls -la"
alias gs="git status"
```

### Listar aliases activos

```
alias
```

### Eliminar un alias

```
unalias nombre
unalias ll gs
```

### Notas sobre aliases

- La expansion es recursiva hasta 16 niveles para evitar bucles infinitos.
- Un alias puede apuntar a si mismo (`alias ls="ls -la"`) sin causar recursion
  infinita: se detecta la autoreferencia y se rompe el ciclo.
- Los aliases se expanden antes de buscar en la tabla de comandos internos y
  antes de ejecutar como comando de shell.

---

## Variables de entorno

```
env [list]              # listar todas las variables de entorno
env get KEY             # mostrar el valor de KEY
env set KEY VALUE       # establecer KEY=VALUE
env set KEY=VALUE       # forma alternativa
env unset KEY           # eliminar KEY del entorno
env save [fichero]      # guardar entorno en vm_env.txt (o fichero indicado)
env load [fichero]      # cargar entorno desde vm_env.txt (o fichero indicado)
```

El entorno se carga automaticamente de `vm_env.txt` al arrancar el REPL
y se guarda en el mismo fichero al cerrarlo.

Las variables se pueden usar en cualquier comando con `echo`:

```
env set SALIDA /tmp/build
echo El directorio de salida es $SALIDA
```

---

## Configuracion del REPL (set)

El comando `set` permite leer y modificar parametros del REPL sin reiniciar.

```
set                         # listar todos los parametros y sus valores
set history_max             # ver valor actual
set history_max 5000        # cambiar valor
set multiline_end ;;        # terminador de bloque multilinea
set aliases_file mi.txt     # cambiar fichero de aliases
set env_file entorno.txt    # cambiar fichero de entorno persistente
```

| Parametro | Defecto | Descripcion |
|-----------|---------|-------------|
| `history_max` | 2000 | Numero maximo de entradas en el historial |
| `multiline_end` | `;;` | Cadena que cierra un bloque multilinea |
| `aliases_file` | `vm_aliases.txt` | Fichero de persistencia de aliases |
| `env_file` | `vm_env.txt` | Fichero de entorno persistente |

---

## Scripts de inicio

Al arrancar, el REPL busca y ejecuta automaticamente:

1. `vm_init.vel` en el directorio de trabajo actual.
2. `~/.vestarc` en el directorio HOME del usuario.

El primer fichero que exista se ejecuta; si `vm_init.vel` no existe se intenta
`~/.vestarc`. Ninguno de los dos es obligatorio.

Ejemplo de `~/.vestarc`:

```
# Aliases globales
alias ll="ls -la"
alias gs="git status"
alias build-all="build src/main.vel -o programa"

# Variables de entorno
env set VESTA_PATH /opt/vm/stdlib

# Configuracion del REPL
set history_max 10000
```

---

## Source: ejecutar un script de comandos

```
source <fichero>
```

Ejecuta linea a linea un fichero de comandos del REPL. Las lineas vacias y
las que empiezan por `#` se ignoran. Util para cargar configuraciones adicionales:

```
vesta [src]> source ~/.vestarc
vesta [src]> source proyectos/setup.vel
```

---

## Tareas en background (jobs / wait)

Los comandos `exec` y `run` lanzan programas `.velb` en background y registran
una entrada en la tabla de jobs.

```
jobs                    # listar tareas con ID, estado, duracion y nombre
wait <job_id>           # bloquear hasta que el job termine
```

Ejemplo:

```
vesta [src]> run servidor servidor.velb --schedulers 4
[run] 'servidor' lanzado en background (id=1)
vesta [src]> jobs
ID     ESTADO   DURACION    NOMBRE
--------------------------------------------------
1      RUN      2.3s        servidor
vesta [src]> wait 1
[wait] Esperando job 1 (servidor)...
[wait] Job 1 completado: OK
```

---

## watch: repetir un comando con intervalo

```
watch <intervalo_ms> <comando ...>
```

Ejecuta un comando del REPL repetidamente con el intervalo indicado (en milisegundos),
mostrando la hora de cada ejecucion. Pulsa cualquier tecla para detenerlo.

```
vesta [src]> watch 1000 vms            # actualizar la lista de VMs cada segundo
vesta [src]> watch 2000 ls *.vel       # listar .vel cada 2 segundos
```

---

## Operaciones con archivos

| Comando | Descripcion |
|---------|-------------|
| `ls [ruta\|patron]` | Listar directorio con soporte de patrones glob (`*.vel`, `src/*.velb`) |
| `cd [ruta\|-]` | Cambiar directorio; sin argumento va al HOME; `-` al directorio anterior |
| `pwd` | Mostrar el directorio actual |
| `mkdir <dir> [dir2...]` | Crear directorios (con padres intermedios) |
| `touch <archivo> [arch2...]` | Crear archivo vacio o actualizar su timestamp |
| `cp [-r] <src> <dst>` | Copiar archivo o directorio (-r para recursivo) |
| `mv <src> <dst>` | Mover o renombrar archivo o directorio |
| `rm [-r] <ruta> [ruta2...]` | Eliminar archivos; -r para directorios (pide confirmacion) |
| `cat [-n] [--raw] <arch> [...]` | Mostrar contenido; -n numera lineas; archivos .vel con syntax highlight |
| `head [-n N] <arch> [...]` | Mostrar las primeras N lineas (defecto: 10) |
| `tail [-n N] [-f] <arch> [...]` | Mostrar las ultimas N lineas; -f sigue el archivo en tiempo real |
| `echo <texto>` | Imprimir texto expandiendo `$VAR` y `${VAR}` |

### Syntax highlight en cat

Los archivos `.vel` se muestran con colores automaticamente:

- **Cian negrita**: mnemonicos (`mov`, `add`, `jmp`, `await`, `msgsend`...)
- **Verde**: registros (`r0`-`r15`, `rip`, `rsp`, `rbp`)
- **Amarillo**: literales numericos (`42`, `0xFF`, `-1`)
- **Cian**: directivas (`@Module`, `@Export`, `@Lib`...)
- **Amarillo**: etiquetas (`mi_funcion:`)
- **Gris**: comentarios (desde `;` hasta fin de linea)

Para desactivar el coloreado usa `--raw`:
```
cat --raw archivo.vel
```

---

## type: identificar un nombre

```
type <nombre> [nombre2 ...]
```

Muestra si un nombre es un alias, un comando interno del REPL, o desconocido:

```
vesta [src]> type ll ls build exit
ll es un alias -> 'ls -la'
ls es un comando interno
build es un comando interno
exit es un comando especial del REPL
```

---

## Bloques multilinea

El REPL detecta bloques de codigo que se extienden en varias lineas cuando
la linea abierta termina en `{` o `(` con balance desigual. Para cerrar
el bloque manualmente usa el terminador configurado (por defecto `;;`):

```
vesta [src]> mov r1, 42
... add r1, r1
... ;;
```

Cambia el terminador con `set multiline_end <cadena>`.
