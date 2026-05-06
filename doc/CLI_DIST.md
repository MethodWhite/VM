# Comando dist: runtime distribuido desde el REPL

El subcomando `dist` gestiona el runtime distribuido (DistRuntime) de cualquier
VM activa sin necesidad de reiniciar el proceso. Para la referencia de los flags
de linea de comandos (`--dist-*`) consulta el README principal.

---

## Referencia rapida

| Subcomando | Argumentos | Descripcion |
|------------|------------|-------------|
| `dist new` | `[opciones]` | Crea y arranca un nodo servidor persistente |
| `dist start` | `<mgr_id> [opciones]` | Configura y arranca DistRuntime en VM existente |
| `dist stop` | `<mgr_id> [--vm-id N]` | Detiene el servidor VDP y sesiones activas |
| `dist info` | `<mgr_id> [--vm-id N]` | Estado del nodo: ID, puerto, lista de pares |
| `dist list` | `<mgr_id> [--vm-id N]` | Tabla de nodos registrados con su estado |
| `dist add-node` | `<mgr_id> <ip> <puerto> [opts]` | Registra y conecta un nodo estatico |
| `dist connect` | `<mgr_id> <node_idx> [--vm-id N]` | Reconecta a un nodo ya registrado |
| `dist discover` | `<mgr_id> [--vm-id N]` | Emite broadcast UDP de descubrimiento |
| `dist run` | `<mgr_id> <node_idx> <archivo.velb>` | Envia bytecode a ejecutar en nodo remoto |
| `dist help` | — | Ayuda completa del subcomando |

---

## dist new

```
dist new [--port PUERTO] [--discover] [--discover-port PUERTO]
         [--name NOMBRE] [--node-id ID] [--schedulers N]
         [--token TOKEN] [--tls] [--cert C] [--key K] [--ca CA]
```

Crea un manager nuevo con una VM, configura y arranca su DistRuntime de una vez.
Imprime el `mgr_id` asignado para usarlo con los demas subcomandos.

```
vesta> dist new --port 7789 --name servidor-local
[dist] Nodo creado (mgr_id=1 vm_id=1) puerto=7789
  dist info 1           -> ver estado
  dist add-node 1 <ip> <puerto>  -> conectar a otro nodo
  dist stop 1           -> detener servidor VDP
  kill 1                -> eliminar manager
```

---

## dist start

```
dist start <mgr_id> [--port P] [--discover] [--discover-port P]
           [--name N] [--node-id ID] [--vm-id N]
           [--token T] [--tls] [--cert C] [--key K] [--ca CA]
```

Reconfigura y arranca el DistRuntime de la VM indicada. Si ya estaba activo
lo detiene y lo reemplaza. Util cuando se lanza un programa con `run` y
luego se quiere activar la red:

```
vesta> run mi-app programa.velb
[run] 'mi-app' lanzado en background (id=1)
vesta> dist start 1 --port 7789 --token secreto
[dist] DistRuntime iniciado en puerto 7789
```

---

## dist stop

```
dist stop <mgr_id> [--vm-id N]
```

Cierra el servidor TCP, todas las sesiones activas y el hilo de descubrimiento UDP.
El manager y la VM siguen existiendo; solo se desactiva la red.

---

## dist info

```
dist info <mgr_id> [--vm-id N]
```

Muestra el ID de nodo local (64 bits), el puerto en escucha y el estado de todos
los nodos del registro:

```
vesta> dist info 1
=== DistRuntime | mgr=1  vm=1 ===
  Node ID local : 0x7614fed977ada27f
  Nodos en registro : 2
    [0] 192.168.1.100:7789  ACTIVO  (nodo-remoto)
    [1] 192.168.1.101:7789  DESCONECTADO  (nodo-caido)
```

---

## dist list

```
dist list <mgr_id> [--vm-id N]
```

Tabla detallada de todos los nodos registrados:

```
vesta> dist list 1
IDX  IP:PUERTO              NOMBRE             ESTADO          TIPO
----------------------------------------------------------------------
0    192.168.1.100:7789     nodo-remoto        ACTIVO          estatico
1    192.168.1.101:7789     nodo-caido         DESCONECTADO    estatico
```

---

## dist add-node

```
dist add-node <mgr_id> <ip> <puerto>
             [--name NOMBRE] [--vm-id N]
             [--token TOKEN] [--tls] [--cert C] [--key K] [--ca CA]
```

Registra un nodo de forma estatica e intenta conectarse de inmediato.
El indice asignado (`idx`) se usa en `dist connect` y en las instrucciones
`rspawn`/`msgsend` del bytecode.

```
vesta> dist add-node 1 192.168.1.100 7789 --name servidor-A --token secreto
[dist] Nodo registrado: 192.168.1.100:7789  (idx=0)
```

---

## dist connect

```
dist connect <mgr_id> <node_idx> [--vm-id N]
```

Reconecta a un nodo que ya existe en el registro por su indice. Util cuando
un nodo se desconecto temporalmente y volvio a estar disponible.

---

## dist discover

```
dist discover <mgr_id> [--vm-id N]
```

Emite un broadcast UDP `NODE_DISCOVER` de forma inmediata. Los nodos que
escuchan en el puerto de descubrimiento responden y quedan registrados.
Requiere que el DistRuntime haya sido iniciado con `--discover`.

---

## dist run

```
dist run <mgr_id> <node_idx> <archivo.velb> [--vm-id N]
```

Carga el `.velb` en la VM local y lo envia al nodo remoto para que lo ejecute.
Devuelve de inmediato un `GcHandle` de un `FutureObject` que se resuelve
cuando el nodo remoto termina y envia `VDP_RSPAWN_ACK`.

```
vesta> build src/tarea.vel -o tarea.velb
vesta> dist new --port 7790 --name nodo-local
[dist] Nodo creado (mgr_id=1 vm_id=1) puerto=7790
vesta> dist add-node 1 192.168.1.100 7789 --name nodo-remoto
[dist] Nodo registrado: 192.168.1.100:7789  (idx=0)
vesta> dist run 1 0 tarea.velb
[dist run] Bytecode enviado a nodo[0] desde tarea.velb
  FutureObject GcHandle = 0x0000001a
```

Para esperar el resultado desde bytecode `.vel`:

```asm
mov  r1, 0x1a
await r1          ; bloquea hasta VDP_RSPAWN_ACK; r0 = resultado
```

---

## Instrucciones de bytecode distribuidas

Desde codigo Vesta (`.vel`) se accede a la red mediante:

| Instruccion | Descripcion |
|-------------|-------------|
| `rspawn r_fn, r_node` | Crear proceso en el nodo `r_node`; R0 = FutureObject |
| `msgsend r_pid, r_addr, r_len` | Enviar mensaje al proceso `r_pid`; R0=1 si OK |
| `msgrecv r_buf, r_max` | Recibir del propio mailbox; bloquea (WAIT_IO) si vacio |
| `memsync r_params` | Sincronizar region de memoria VM con nodo remoto |

El `node_idx` que usan `rspawn` y `msgsend` (para PIDs remotos) es el que
asigna `dist add-node` y que muestra `dist list`.

---

## Seguridad: token y TLS

| Mecanismo | Descripcion |
|-----------|-------------|
| Token (CRAM) | El token se hashea con SHA-256 y se intercambia durante el handshake VDP |
| TLS | El handshake TLS ocurre antes del handshake VDP. Requiere certificados PEM |
| mTLS + token | Ambos mecanismos son combinables para maxima seguridad |

Generar certificados autofirmados para desarrollo:

```bash
openssl req -x509 -newkey rsa:4096 \
  -keyout key.pem -out cert.pem -days 365 -nodes \
  -subj "/CN=vesta-node"
```

---

## Flujos tipicos

### Dos nodos locales

```bash
# Terminal 1: servidor
./vm --dist-server --dist-port 7789 --dist-name servidor-A

# Terminal 2: cliente REPL
./vm
vesta> dist new --port 7790 --name cliente-B
vesta> dist add-node 1 127.0.0.1 7789 --name servidor-A
vesta> dist list 1
IDX  IP:PUERTO         NOMBRE      ESTADO   TIPO
--------------------------------------------------
0    127.0.0.1:7789    servidor-A  ACTIVO   estatico
```

### Nodo con TLS y token

```bash
# Servidor
./vm --dist-server --dist-port 7789 \
  --dist-tls --dist-cert cert.pem --dist-key key.pem \
  --dist-token mi-secreto

# Cliente
vesta> dist new --port 7790
vesta> dist add-node 1 192.168.1.100 7789 --name servidor \
       --tls --cert cert.pem --ca cert.pem --token mi-secreto
```

### Descubrimiento automatico en LAN

```bash
# Nodo A
./vm --dist-server --dist-port 7789 --dist-discover

# Nodo B (REPL)
vesta> dist new --port 7790 --discover
vesta> dist discover 1
[dist] Broadcast NODE_DISCOVER enviado
[dist] Nodo descubierto: 192.168.1.10:7789  (idx=0)
```
