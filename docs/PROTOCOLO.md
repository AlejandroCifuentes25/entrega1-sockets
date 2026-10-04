# Especificación del Protocolo MCR (Monitoreo de CPU y RAM)

**Versión:** 1.0  
**Curso:** Internet: Arquitectura y Protocolos – 2026-2  
**Estructura:** basada en el formato de un RFC (visión general, servicio, formato, procedimientos, ejemplos).

---

## 1. Visión general del protocolo

### 1.1 Propósito
MCR es un protocolo de capa de aplicación que permite que varios **nodos** (agentes instalados en máquinas) reporten sus métricas de uso de CPU y memoria RAM a un **servidor central**. Además, permite que los **clientes de administración** autenticados consulten el estado actual y el historial de esos nodos.

### 1.2 Modelo de funcionamiento
Usa el modelo **cliente-servidor** con un servidor central que hace de intermediario. Los nodos y los clientes nunca se comunican directamente.

```
 Nodo 1 ─┐                            ┌─ Cliente admin 1
 Nodo 2 ─┼── TCP ──> Servidor central <── TCP ──┼─ Cliente admin 2
 Nodo N ─┘           (C, pthreads)    └─ ...
                          │
                          │ TCP (protocolo AUTH)
                          v
                  Servicio de autenticación
```

| Entidad | Implementación | Función |
|---|---|---|
| Nodo | Python (`nodo/nodo.py`) | Se registra, envía telemetría periódica y alarmas |
| Servidor central | C, sockets Berkeley (`servidor/servidor.c`) | Registra nodos, guarda el estado y el historial y atiende consultas |
| Cliente de administración | Python (`cliente/cliente.py`) | Se autentica y consulta información |
| Servicio de autenticación | Python (`auth/auth_server.py`) | Valida credenciales y devuelve el perfil del usuario |

### 1.3 Capa y transporte
MCR opera en la **capa de aplicación** de la arquitectura TCP/IP, sobre **TCP** (`SOCK_STREAM`). El puerto del servidor central se indica por consola; el del servicio de autenticación es 9000 por defecto.

### 1.4 Justificación del transporte
| Criterio | Telemetría (DATA) | Alarmas (ALARM) | Registro / Login / Consultas |
|---|---|---|---|
| Frecuencia | Alta (cada 5 s) | Baja, esporádica | Baja |
| Tamaño | ~30 bytes | ~80 bytes | Hasta ~200 bytes (HISTORY) |
| Criticidad | Baja | **Alta** | **Alta** |
| Tolerancia a pérdidas | Sí (llega otra en 5 s) | **No** | **No** |
| Necesidad de orden | Deseable (historial) | Sí | Sí |
| Necesidad de conexión | Útil para detectar caídas | Sí | Sí (sesión autenticada) |

UDP sería suficiente para la telemetría, pero las alarmas, el registro, el login y las consultas necesitan entrega confiable y ordenada. Con UDP habría que implementar en la aplicación confirmaciones, identificadores de mensaje, temporizadores, retransmisión y detección de duplicados. Se eligió **TCP para todo** porque:
1. garantiza la entrega y el orden de las alarmas y consultas;
2. la conexión permite mantener una **sesión** (nodo registrado o cliente autenticado);
3. el servidor detecta la caída de un nodo cuando se cierra el socket o vence el temporizador;
4. el servidor solo necesita un socket de escucha.

El costo es una sobrecarga mayor por mensaje, que es despreciable con el volumen de este sistema (un mensaje cada 5 s por nodo).

---

## 2. Especificación del servicio (primitivas)

| Primitiva | Usuario | Mensaje | Descripción |
|---|---|---|---|
| REGISTRAR | Nodo | `REG` | Darse de alta en el servidor |
| REPORTAR | Nodo | `DATA` | Enviar CPU y RAM actuales |
| ALERTAR | Nodo | `ALARM` | Notificar un evento crítico |
| AUTENTICAR | Cliente | `LOGIN` | Iniciar sesión con usuario y clave |
| LISTAR | Cliente | `GET ... LIST` | Obtener los nodos conocidos y su estado |
| CONSULTAR_ESTADO | Cliente | `GET ... STATUS` | Obtener la última lectura de un nodo |
| CONSULTAR_HISTORIAL | Cliente (solo ADMIN) | `GET ... HISTORY` | Obtener las últimas 5 lecturas |
| TERMINAR | Nodo / Cliente | `BYE` | Cerrar la sesión de forma ordenada |

---

## 3. Formato de los mensajes

### 3.1 Reglas generales
- Texto **ASCII**. Cada mensaje es **una línea** terminada en `\n` (0x0A). Se tolera un `\r` antes del `\n`.
- Los campos se separan con `|` (0x7C). Ningún campo puede contener `|` ni `\n`.
- El primer campo es siempre el **comando**, en mayúsculas.
- Tamaño máximo de un mensaje: **1024 bytes**, incluido el `\n`.
- TCP es un flujo de bytes, por lo que el receptor acumula lo recibido y separa los mensajes por `\n`. Varios mensajes pueden llegar en una sola lectura, y un mensaje puede llegar en varias.

```
COMANDO|CAMPO_1|CAMPO_2|...|CAMPO_N\n
```

### 3.2 Mensajes Nodo → Servidor

| Comando | Formato | Campos |
|---|---|---|
| REG | `REG\|<id_nodo>\|<tipo_sistema>` | `id_nodo`: identificador único (ej. `NODE_01`). `tipo_sistema`: SO del nodo (ej. `LINUX`, `WINDOWS`) |
| DATA | `DATA\|<id_nodo>\|<cpu>\|<ram>` | `cpu`, `ram`: porcentaje decimal entre 0 y 100 (ej. `45.5`) |
| ALARM | `ALARM\|<id_nodo>\|<tipo>\|<valor>\|<descripcion>` | `tipo`: `CPU_OVERLOAD` o `RAM_OVERLOAD`. `valor`: medición que disparó la alarma. `descripcion`: texto libre |
| BYE | `BYE\|<id_nodo>` | Cierre ordenado |

### 3.3 Mensajes Cliente → Servidor

| Comando | Formato | Campos |
|---|---|---|
| LOGIN | `LOGIN\|<usuario>\|<clave>` | Credenciales del usuario |
| GET | `GET\|<id_cliente>\|<tipo>\|<id_nodo>` | `tipo`: `LIST`, `STATUS` o `HISTORY`. Con `LIST`, `id_nodo` se ignora (se envía `ALL`) |
| BYE | `BYE\|<id_cliente>` | Cierre ordenado |

### 3.4 Mensajes Servidor → Nodo / Cliente

| Comando | Formato | Cuándo |
|---|---|---|
| REG_ACK | `REG_ACK\|SERVER\|<id_nodo>\|SUCCESS` | Registro aceptado |
| ACK | `ACK\|SERVER\|<DATA o ALARM>\|<id_nodo>` | Confirmación de telemetría o alarma |
| LOGIN_OK | `LOGIN_OK\|SERVER\|<usuario>\|<rol>` | Autenticación exitosa. `rol`: `ADMIN` o `VIEWER` |
| RESP (LIST) | `RESP\|SERVER\|LIST\|<id>:<estado>,<id>:<estado>,...` | `estado`: `ACTIVO` o `INACTIVO`. Si no hay nodos: `NINGUNO` |
| RESP (STATUS) | `RESP\|SERVER\|STATUS\|<id_nodo>\|<cpu>\|<ram>\|<estado>` | Última lectura |
| RESP (HISTORY) | `RESP\|SERVER\|HISTORY\|<id_nodo>\|<cpu>,<ram>;<cpu>,<ram>;...` | Hasta 5 lecturas, de la más antigua a la más reciente |
| BYE_ACK | `BYE_ACK\|SERVER` | Confirmación de cierre |
| ERR | `ERR\|SERVER\|<codigo>\|<descripcion>` | Cualquier error (ver 3.5) |

### 3.5 Códigos de error

| Código | Significado | Ejemplos |
|---|---|---|
| 204 | Sin datos | El nodo existe pero aún no ha enviado `DATA` |
| 400 | Petición mal formada | Comando desconocido, faltan campos, CPU/RAM no numérica o fuera de 0–100, tipo de consulta inválido, mensaje de más de 1024 bytes |
| 401 | No autenticado / no registrado | `GET` sin `LOGIN`, `DATA`/`ALARM` sin `REG` previo en esa conexión, credenciales inválidas |
| 403 | Sin permiso | Un perfil `VIEWER` pide `HISTORY` |
| 404 | No encontrado | Se consulta un nodo que nunca se registró |
| 409 | Conflicto de sesión | Un cliente envía `REG`, un nodo envía `LOGIN`, o una conexión intenta registrar un segundo nodo |
| 503 | Servicio no disponible | El servicio de autenticación no responde o no se resuelve su nombre; tabla de nodos llena |

### 3.6 Protocolo auxiliar AUTH (Servidor central → Servicio de autenticación)
Una conexión TCP por consulta, con el mismo formato de línea:

| Mensaje | Formato |
|---|---|
| Petición | `AUTH\|<usuario>\|<clave>` |
| Éxito | `AUTH_OK\|<usuario>\|<rol>` |
| Fallo | `AUTH_FAIL\|<usuario>\|<motivo>` |
| Mal formado | `AUTH_ERR\|-\|<motivo>` |

Los usuarios y sus perfiles se guardan **solo en el servicio de autenticación** (claves cifradas con SHA-256), no en la aplicación principal.

---

## 4. Reglas de procedimiento

### 4.1 Reglas generales
1. Toda comunicación va sobre una conexión TCP previamente establecida.
2. Cada petición recibe **exactamente una respuesta**, en el mismo orden en que se enviaron.
3. Una conexión pertenece a **un solo tipo de entidad**: nodo (después de `REG`) o cliente (después de `LOGIN`). No se puede cambiar de tipo.
4. Los nombres de host se resuelven por DNS; no hay direcciones IP escritas en el código.

### 4.2 Nodo
1. Abre la conexión y envía `REG`. No puede enviar `DATA` ni `ALARM` antes de recibir `REG_ACK`.
2. Cada 5 s envía `DATA` y espera su `ACK`.
3. Si CPU > 90 % o RAM > 90 %, envía de inmediato `ALARM` y espera su `ACK`.
4. Si `REG` se repite con el mismo `id_nodo` (por ejemplo, tras una reconexión), el registro se actualiza y el nodo vuelve a quedar `ACTIVO`. El historial se conserva.
5. Un nodo solo puede reportar datos con el `id_nodo` con el que se registró en esa conexión (evita la suplantación).

### 4.3 Cliente
1. Abre la conexión y envía `LOGIN`. Cualquier `GET` anterior se responde con `ERR 401`.
2. El servidor reenvía las credenciales al servicio de autenticación (`AUTH`) y responde `LOGIN_OK` con el perfil, o `ERR 401` / `ERR 503`.
3. Una vez autenticado, el cliente puede enviar varios `GET` por la misma conexión.
4. **Perfiles:** `ADMIN` puede usar LIST, STATUS y HISTORY; `VIEWER` puede usar LIST y STATUS.

### 4.4 Temporizadores
| Temporizador | Valor | Acción al expirar |
|---|---|---|
| Inactividad de nodo (servidor) | 30 s sin recibir mensajes de un nodo registrado | El servidor cierra la conexión y marca el nodo `INACTIVO` |
| Respuesta del servidor (nodo) | 5 s | El nodo considera la conexión caída, la cierra y reintenta |
| Reintento de conexión (nodo) | 5 s | Nueva conexión y nuevo `REG` |
| Respuesta del servidor (cliente) | 10 s | El cliente avisa al usuario y pide reconexión |
| Respuesta del servicio AUTH (servidor) | 5 s | Responde `ERR 503` al cliente |

### 4.5 Manejo de excepciones
| Situación | Comportamiento |
|---|---|
| Mensaje con formato incorrecto | `ERR 400`; la conexión sigue abierta |
| Parámetros inválidos | `ERR 400` con la descripción del problema |
| Nodo desconocido | `ERR 404` |
| Desconexión del nodo (cierre o caída) | El servidor libera el hilo y marca el nodo `INACTIVO`; el estado queda en el log |
| Desconexión del cliente | El servidor libera el hilo y los recursos |
| Falla de DNS / conexión en el nodo | El nodo **no termina**: avisa y reintenta cada 5 s |
| Falla de DNS / conexión en el cliente | El cliente avisa y ofrece reintentar |
| Falla de DNS / caída del servicio AUTH | El servidor **no termina**: responde `ERR 503` |
| Escritura en un socket cerrado | El servidor ignora `SIGPIPE` y sigue funcionando |
| Duplicación de mensajes | TCP elimina los duplicados de la red. Un `REG` repetido es idempotente |
| Pérdida de mensajes | TCP la corrige con retransmisión. Si la conexión cae, los temporizadores la detectan |

### 4.6 Máquinas de estado

**Nodo**
| Estado | Evento | Acción | Siguiente estado |
|---|---|---|---|
| DESCONECTADO | Conexión establecida | Enviar `REG` | ESPERANDO_REGISTRO |
| DESCONECTADO | Falla DNS/conexión | Esperar 5 s | DESCONECTADO |
| ESPERANDO_REGISTRO | `REG_ACK` | — | OPERATIVO |
| ESPERANDO_REGISTRO | `ERR` / timeout | Cerrar socket | DESCONECTADO |
| OPERATIVO | Vence intervalo de 5 s | Enviar `DATA` | OPERATIVO |
| OPERATIVO | Umbral superado | Enviar `ALARM` | OPERATIVO |
| OPERATIVO | Timeout / cierre | Cerrar socket | DESCONECTADO |

**Servidor (por conexión)**
| Estado | Evento | Acción | Siguiente estado |
|---|---|---|---|
| ESCUCHANDO | `accept()` | Crear hilo | IDENTIFICANDO |
| IDENTIFICANDO | `REG` válido | `REG_ACK`, activar temporizador de 30 s | SESION_NODO |
| IDENTIFICANDO | `LOGIN` válido | Consultar AUTH, `LOGIN_OK` | SESION_CLIENTE |
| IDENTIFICANDO | `LOGIN` inválido / mensaje inválido | `ERR` | IDENTIFICANDO |
| SESION_NODO | `DATA` / `ALARM` | Guardar, `ACK` | SESION_NODO |
| SESION_CLIENTE | `GET` | `RESP` o `ERR` | SESION_CLIENTE |
| Cualquiera | `BYE` | `BYE_ACK` | CERRADA |
| Cualquiera | Cierre / timeout / error | Liberar recursos (nodo → INACTIVO) | CERRADA |

**Cliente**
| Estado | Evento | Acción | Siguiente estado |
|---|---|---|---|
| DESCONECTADO | Conexión + credenciales | Enviar `LOGIN` | AUTENTICANDO |
| AUTENTICANDO | `LOGIN_OK` | Mostrar menú | AUTENTICADO |
| AUTENTICANDO | `ERR` | Mostrar error, ofrecer reintento | DESCONECTADO |
| AUTENTICADO | Opción del menú | Enviar `GET` | ESPERANDO_RESPUESTA |
| ESPERANDO_RESPUESTA | `RESP` / `ERR` | Mostrar resultado | AUTENTICADO |
| ESPERANDO_RESPUESTA | Timeout / cierre | Avisar | DESCONECTADO |
| AUTENTICADO | Salir | Enviar `BYE` | DESCONECTADO |

---

## 5. Ejemplos (diagramas de secuencia)

### 5.1 Nodo se registra y reporta
```
Nodo                                   Servidor
 |--- REG|NODE_01|LINUX ---------------->|
 |<-- REG_ACK|SERVER|NODE_01|SUCCESS ----|
 |--- DATA|NODE_01|23.5|61.2 ----------->|
 |<-- ACK|SERVER|DATA|NODE_01 -----------|
 |--- ALARM|NODE_01|CPU_OVERLOAD|95.0|CPU supera el 90 por ciento -->|
 |<-- ACK|SERVER|ALARM|NODE_01 ----------|
```

### 5.2 Cliente se autentica y consulta
```
Cliente                       Servidor                       Servicio AUTH
 |-- LOGIN|admin|admin123 ---->|                                  |
 |                             |-- AUTH|admin|admin123 ---------->|
 |                             |<- AUTH_OK|admin|ADMIN -----------|
 |<- LOGIN_OK|SERVER|admin|ADMIN|                                  |
 |-- GET|admin|LIST|ALL ------>|
 |<- RESP|SERVER|LIST|NODE_01:ACTIVO,NODE_02:INACTIVO
 |-- GET|admin|HISTORY|NODE_01>|
 |<- RESP|SERVER|HISTORY|NODE_01|20.1,60.0;22.4,60.3;23.5,61.2
 |-- BYE|admin --------------->|
 |<- BYE_ACK|SERVER -----------|
```

### 5.3 Errores
```
Cliente                                Servidor
 |--- GET|x|STATUS|NODE_01 ------------->|
 |<-- ERR|SERVER|401|Debe autenticarse con LOGIN antes de consultar
 |--- LOGIN|operador|oper123 ---------->|
 |<-- LOGIN_OK|SERVER|operador|VIEWER --|
 |--- GET|operador|HISTORY|NODE_01 ---->|
 |<-- ERR|SERVER|403|Su perfil no tiene permiso para consultar historiales
 |--- GET|operador|STATUS|NODE_99 ----->|
 |<-- ERR|SERVER|404|Nodo no registrado-|
```

---

## 6. Concurrencia (justificación)
El servidor crea **un hilo POSIX por conexión** (`pthread_create` + `pthread_detach`). Se eligió este modelo porque:
- cada conexión es una sesión larga (los nodos permanecen conectados), y un hilo bloqueado en `read()` no afecta a los demás;
- el código de cada sesión queda secuencial y fácil de seguir, siguiendo la máquina de estados;
- el número de entidades es pequeño, así que el costo de un hilo por conexión es aceptable.

El estado compartido (tabla de nodos e historiales) se protege con `pthread_mutex_t`, y la escritura del log con un segundo mutex. Se usa `strtok_r` en lugar de `strtok` porque este último no es seguro entre hilos.
