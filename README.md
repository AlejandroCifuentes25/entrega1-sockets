# Sistema de Monitoreo Distribuido de CPU y RAM

Proyecto de **Internet: Arquitectura y Protocolos (2026-2)**.

Varios nodos reportan su uso de CPU y RAM a un servidor central escrito en C (sockets Berkeley, TCP, un hilo por conexión). Los clientes de administración se autentican contra un servicio de autenticación independiente y consultan el estado y el historial de los nodos.

📄 **Especificación completa del protocolo:** [docs/PROTOCOLO.md](docs/PROTOCOLO.md)

## Estructura

```
servidor/   servidor.c + Makefile     Servidor central (C, pthreads)
auth/       auth_server.py            Servicio de autenticación (usuarios y perfiles)
nodo/       nodo.py                   Agente de monitoreo (Python + psutil)
cliente/    cliente.py                Cliente de administración (consola)
docs/       PROTOCOLO.md              Especificación del protocolo
```

## Requisitos
- Linux o WSL con `gcc` y `make` (servidor).
- Python 3.8+ (nodos, cliente y servicio de autenticación).
- `pip install psutil` (nodos).

## Ejecución
Cada componente se ejecuta en una terminal distinta.

**1. Servicio de autenticación**
```bash
python3 auth/auth_server.py 9000
```

**2. Servidor central**
```bash
cd servidor
make
./servidor <puerto> <archivoDeLogs> [hostAuth] [puertoAuth]
# ejemplo:
./servidor 8080 logs.txt localhost 9000
```

**3. Nodos** (mínimo dos)
```bash
python nodo/nodo.py <host> <puerto> <id_nodo>
python nodo/nodo.py localhost 8080 NODE_01
python nodo/nodo.py localhost 8080 NODE_02
```

**4. Cliente de administración**
```bash
python cliente/cliente.py localhost 8080
```

### Usuarios de prueba
| Usuario | Clave | Perfil | Permisos |
|---|---|---|---|
| admin | admin123 | ADMIN | LIST, STATUS, HISTORY |
| operador | oper123 | VIEWER | LIST, STATUS |

Se guardan en `auth/usuarios.json` (claves en SHA-256), que se crea automáticamente la primera vez que se ejecuta el servicio.

## Resumen del protocolo
Mensajes de texto, una línea por mensaje (`\n`) y campos separados por `|`:

| Origen | Mensajes |
|---|---|
| Nodo | `REG`, `DATA`, `ALARM`, `BYE` |
| Cliente | `LOGIN`, `GET` (LIST / STATUS / HISTORY), `BYE` |
| Servidor | `REG_ACK`, `ACK`, `LOGIN_OK`, `RESP`, `BYE_ACK`, `ERR` |

Los detalles, los códigos de error, los temporizadores y las máquinas de estado están en [docs/PROTOCOLO.md](docs/PROTOCOLO.md).
