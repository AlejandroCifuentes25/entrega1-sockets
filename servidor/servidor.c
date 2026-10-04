/*
 * Servidor central de monitoreo distribuido (CPU / RAM)
 * Internet: Arquitectura y Protocolos - Proyecto Sockets
 *
 * Uso: ./servidor <puerto> <archivoDeLogs> [hostAuth] [puertoAuth]
 *
 * - Sockets Berkeley, TCP (SOCK_STREAM).
 * - Concurrencia: un hilo (pthread) por conexion.
 * - Estado compartido (tabla de nodos) protegido con mutex.
 * - Autenticacion delegada a un servicio externo (protocolo AUTH).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_BUFFER     1024
#define MAX_NODOS      10
#define MAX_HISTORIAL  5
#define TIMEOUT_NODO   30   /* segundos sin mensajes antes de declarar un nodo caido */
#define TIMEOUT_AUTH   5    /* segundos maximos de espera al servicio de autenticacion */

/* ------------------------------------------------------------------------- */
/* Estructuras de datos compartidas                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    float cpu;
    float ram;
} Registro;

typedef struct {
    char     id[64];
    char     tipo[64];
    int      activo;                     /* 1 = conectado, 0 = desconectado */
    time_t   ultimo_reporte;
    char     ultima_alarma[128];
    Registro historial[MAX_HISTORIAL];   /* buffer circular */
    int      historial_count;
    int      historial_idx;
} Nodo;

Nodo tabla_nodos[MAX_NODOS];
int  total_nodos = 0;

pthread_mutex_t mutex_tabla = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_log   = PTHREAD_MUTEX_INITIALIZER;

char archivo_logs[256];
char auth_host[256]  = "localhost";
char auth_puerto[16] = "9000";

/* Tipo de sesion de cada conexion (maquina de estados del servidor) */
typedef enum {
    SESION_IDENTIFICANDO,   /* aun no se sabe si es nodo o cliente */
    SESION_NODO,            /* nodo registrado con REG */
    SESION_CLIENTE          /* cliente autenticado con LOGIN */
} TipoSesion;

typedef struct {
    int        sock;
    char       ip[INET_ADDRSTRLEN];
    int        puerto;
    TipoSesion tipo;
    char       id_nodo[64];
    char       usuario[64];
    char       rol[16];
    int        cerrar;      /* 1 si se debe terminar la sesion (BYE) */
} Sesion;

/* ------------------------------------------------------------------------- */
/* Logging (consola + archivo), protegido con mutex                           */
/* ------------------------------------------------------------------------- */

void guardar_log(const char *tipo, const char *ip, int puerto, const char *mensaje) {
    char fecha[32];
    time_t ahora = time(NULL);
    struct tm tm_info;
    localtime_r(&ahora, &tm_info);
    strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", &tm_info);

    /* Quitamos el salto de linea final para que el log quede en una sola linea */
    char limpio[MAX_BUFFER];
    snprintf(limpio, sizeof(limpio), "%s", mensaje);
    limpio[strcspn(limpio, "\r\n")] = '\0';

    pthread_mutex_lock(&mutex_log);
    printf("%s [%s] %s:%d -> %s\n", fecha, tipo, ip, puerto, limpio);
    fflush(stdout);
    FILE *file = fopen(archivo_logs, "a");
    if (file != NULL) {
        fprintf(file, "%s [%s] %s:%d -> %s\n", fecha, tipo, ip, puerto, limpio);
        fclose(file);
    } else {
        perror("Error al abrir el archivo de log");
    }
    pthread_mutex_unlock(&mutex_log);
}

/* Envia una respuesta y la registra en el log */
void enviar(Sesion *s, const char *respuesta) {
    if (write(s->sock, respuesta, strlen(respuesta)) < 0) {
        guardar_log("ERROR", s->ip, s->puerto, "Fallo al enviar respuesta");
        return;
    }
    guardar_log("SALIENTE", s->ip, s->puerto, respuesta);
}

/* ------------------------------------------------------------------------- */
/* Funciones sobre la tabla de nodos (llamar SIEMPRE con mutex_tabla tomado)  */
/* ------------------------------------------------------------------------- */

int buscar_nodo(const char *id) {
    for (int i = 0; i < total_nodos; i++)
        if (strcmp(tabla_nodos[i].id, id) == 0)
            return i;
    return -1;
}

int registrar_nodo(const char *id, const char *tipo) {
    if (total_nodos >= MAX_NODOS) return -1;
    int idx = total_nodos++;
    memset(&tabla_nodos[idx], 0, sizeof(Nodo));
    snprintf(tabla_nodos[idx].id,   sizeof(tabla_nodos[idx].id),   "%s", id);
    snprintf(tabla_nodos[idx].tipo, sizeof(tabla_nodos[idx].tipo), "%s", tipo);
    return idx;
}

void insertar_historial(int idx, float cpu, float ram) {
    Nodo *n = &tabla_nodos[idx];
    n->historial[n->historial_idx].cpu = cpu;
    n->historial[n->historial_idx].ram = ram;
    n->historial_idx = (n->historial_idx + 1) % MAX_HISTORIAL;
    if (n->historial_count < MAX_HISTORIAL) n->historial_count++;
    n->ultimo_reporte = time(NULL);
}

/* ------------------------------------------------------------------------- */
/* Validaciones                                                               */
/* ------------------------------------------------------------------------- */

/* Convierte un porcentaje en texto. Devuelve 1 si es valido (0..100). */
int parsear_porcentaje(const char *txt, float *valor) {
    if (txt == NULL || *txt == '\0') return 0;
    char *fin;
    errno = 0;
    float v = strtof(txt, &fin);
    if (errno != 0 || *fin != '\0' || v < 0.0f || v > 100.0f) return 0;
    *valor = v;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Cliente del servicio de autenticacion (protocolo AUTH)                     */
/* Devuelve: 1 = credenciales validas, 0 = invalidas, -1 = servicio caido     */
/* ------------------------------------------------------------------------- */

int consultar_auth(const char *usuario, const char *clave, char *rol, size_t rol_len) {
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* Resolucion de nombres: si falla, NO se termina el servidor */
    int rc = getaddrinfo(auth_host, auth_puerto, &hints, &res);
    if (rc != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "No se pudo resolver %s: %s", auth_host, gai_strerror(rc));
        guardar_log("AUTH", auth_host, atoi(auth_puerto), msg);
        return -1;
    }

    int s = -1;
    struct timeval tv = { TIMEOUT_AUTH, 0 };
    for (p = res; p != NULL; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) continue;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(s, p->ai_addr, p->ai_addrlen) == 0) break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s < 0) {
        guardar_log("AUTH", auth_host, atoi(auth_puerto), "Servicio de autenticacion no disponible");
        return -1;
    }

    char peticion[256];
    snprintf(peticion, sizeof(peticion), "AUTH|%s|%s\n", usuario, clave);
    if (write(s, peticion, strlen(peticion)) < 0) { close(s); return -1; }

    char resp[256] = {0};
    int n = read(s, resp, sizeof(resp) - 1);
    close(s);
    if (n <= 0) return -1;   /* timeout o cierre: servicio no disponible */
    resp[strcspn(resp, "\r\n")] = '\0';

    /* Respuestas: AUTH_OK|<usuario>|<rol>  o  AUTH_FAIL|<usuario>|<motivo> */
    char *save;
    char *cmd = strtok_r(resp, "|", &save);
    if (cmd && strcmp(cmd, "AUTH_OK") == 0) {
        strtok_r(NULL, "|", &save);               /* usuario */
        char *r = strtok_r(NULL, "|", &save);      /* rol */
        if (!r) return -1;
        snprintf(rol, rol_len, "%s", r);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Procesamiento de un mensaje (una linea sin '\n')                            */
/* ------------------------------------------------------------------------- */

void procesar_mensaje(Sesion *s, char *linea) {
    char respuesta[MAX_BUFFER];
    char *save;
    char *comando = strtok_r(linea, "|", &save);

    if (comando == NULL) {
        enviar(s, "ERR|SERVER|400|Formato invalido\n");
        return;
    }

    /* ---------------------------- REG ---------------------------------- */
    if (strcmp(comando, "REG") == 0) {
        char *id_nodo = strtok_r(NULL, "|", &save);
        char *tipo    = strtok_r(NULL, "|", &save);
        if (!id_nodo || !tipo) {
            enviar(s, "ERR|SERVER|400|Faltan parametros en REG\n");
            return;
        }
        if (s->tipo == SESION_CLIENTE) {
            enviar(s, "ERR|SERVER|409|Una sesion de cliente no puede registrar nodos\n");
            return;
        }
        if (s->tipo == SESION_NODO && strcmp(s->id_nodo, id_nodo) != 0) {
            enviar(s, "ERR|SERVER|409|Esta conexion ya pertenece a otro nodo\n");
            return;
        }

        pthread_mutex_lock(&mutex_tabla);
        int idx = buscar_nodo(id_nodo);
        if (idx == -1) idx = registrar_nodo(id_nodo, tipo);
        if (idx != -1) {
            tabla_nodos[idx].activo = 1;
            tabla_nodos[idx].ultimo_reporte = time(NULL);
            snprintf(tabla_nodos[idx].tipo, sizeof(tabla_nodos[idx].tipo), "%s", tipo);
        }
        pthread_mutex_unlock(&mutex_tabla);

        if (idx == -1) {
            enviar(s, "ERR|SERVER|503|Tabla de nodos llena\n");
            return;
        }

        s->tipo = SESION_NODO;
        snprintf(s->id_nodo, sizeof(s->id_nodo), "%s", id_nodo);

        /* Temporizador: si el nodo no envia nada en TIMEOUT_NODO segundos, read() expira */
        struct timeval tv = { TIMEOUT_NODO, 0 };
        setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        snprintf(respuesta, sizeof(respuesta), "REG_ACK|SERVER|%s|SUCCESS\n", id_nodo);
        enviar(s, respuesta);
        return;
    }

    /* ---------------------------- DATA --------------------------------- */
    if (strcmp(comando, "DATA") == 0) {
        char *id_nodo = strtok_r(NULL, "|", &save);
        char *s_cpu   = strtok_r(NULL, "|", &save);
        char *s_ram   = strtok_r(NULL, "|", &save);
        float cpu, ram;

        if (!id_nodo || !s_cpu || !s_ram) {
            enviar(s, "ERR|SERVER|400|Faltan parametros en DATA\n");
            return;
        }
        if (s->tipo != SESION_NODO || strcmp(s->id_nodo, id_nodo) != 0) {
            enviar(s, "ERR|SERVER|401|Nodo no registrado en esta conexion (enviar REG primero)\n");
            return;
        }
        if (!parsear_porcentaje(s_cpu, &cpu) || !parsear_porcentaje(s_ram, &ram)) {
            enviar(s, "ERR|SERVER|400|Parametros invalidos: CPU y RAM deben estar entre 0 y 100\n");
            return;
        }

        pthread_mutex_lock(&mutex_tabla);
        int idx = buscar_nodo(id_nodo);
        if (idx != -1) insertar_historial(idx, cpu, ram);
        pthread_mutex_unlock(&mutex_tabla);

        if (idx == -1) {
            enviar(s, "ERR|SERVER|404|Nodo no encontrado\n");
            return;
        }
        snprintf(respuesta, sizeof(respuesta), "ACK|SERVER|DATA|%s\n", id_nodo);
        enviar(s, respuesta);
        return;
    }

    /* ---------------------------- ALARM -------------------------------- */
    if (strcmp(comando, "ALARM") == 0) {
        char *id_nodo     = strtok_r(NULL, "|", &save);
        char *tipo_alarma = strtok_r(NULL, "|", &save);
        char *valor       = strtok_r(NULL, "|", &save);
        char *descripcion = strtok_r(NULL, "|", &save);

        if (!id_nodo || !tipo_alarma || !valor || !descripcion) {
            enviar(s, "ERR|SERVER|400|Faltan parametros en ALARM\n");
            return;
        }
        if (s->tipo != SESION_NODO || strcmp(s->id_nodo, id_nodo) != 0) {
            enviar(s, "ERR|SERVER|401|Nodo no registrado en esta conexion (enviar REG primero)\n");
            return;
        }

        pthread_mutex_lock(&mutex_tabla);
        int idx = buscar_nodo(id_nodo);
        if (idx != -1) {
            snprintf(tabla_nodos[idx].ultima_alarma, sizeof(tabla_nodos[idx].ultima_alarma),
                     "%s=%s", tipo_alarma, valor);
            tabla_nodos[idx].ultimo_reporte = time(NULL);
        }
        pthread_mutex_unlock(&mutex_tabla);

        char aviso[MAX_BUFFER];
        snprintf(aviso, sizeof(aviso), "*** ALARMA *** Nodo:%s Tipo:%s Valor:%s Desc:%s",
                 id_nodo, tipo_alarma, valor, descripcion);
        guardar_log("ALARMA", s->ip, s->puerto, aviso);

        snprintf(respuesta, sizeof(respuesta), "ACK|SERVER|ALARM|%s\n", id_nodo);
        enviar(s, respuesta);
        return;
    }

    /* ---------------------------- LOGIN -------------------------------- */
    if (strcmp(comando, "LOGIN") == 0) {
        char *usuario = strtok_r(NULL, "|", &save);
        char *clave   = strtok_r(NULL, "|", &save);
        if (!usuario || !clave) {
            enviar(s, "ERR|SERVER|400|Faltan parametros en LOGIN\n");
            return;
        }
        if (s->tipo == SESION_NODO) {
            enviar(s, "ERR|SERVER|409|Un nodo no puede iniciar sesion como cliente\n");
            return;
        }

        char rol[16] = {0};
        int r = consultar_auth(usuario, clave, rol, sizeof(rol));
        if (r == 1) {
            s->tipo = SESION_CLIENTE;
            snprintf(s->usuario, sizeof(s->usuario), "%s", usuario);
            snprintf(s->rol, sizeof(s->rol), "%s", rol);
            snprintf(respuesta, sizeof(respuesta), "LOGIN_OK|SERVER|%s|%s\n", usuario, rol);
            enviar(s, respuesta);
        } else if (r == 0) {
            enviar(s, "ERR|SERVER|401|Credenciales invalidas\n");
        } else {
            enviar(s, "ERR|SERVER|503|Servicio de autenticacion no disponible\n");
        }
        return;
    }

    /* ---------------------------- GET ---------------------------------- */
    if (strcmp(comando, "GET") == 0) {
        char *id_cliente    = strtok_r(NULL, "|", &save);
        char *tipo_consulta = strtok_r(NULL, "|", &save);
        char *id_nodo       = strtok_r(NULL, "|", &save);

        if (!id_cliente || !tipo_consulta || !id_nodo) {
            enviar(s, "ERR|SERVER|400|Faltan parametros en GET\n");
            return;
        }
        if (s->tipo != SESION_CLIENTE) {
            enviar(s, "ERR|SERVER|401|Debe autenticarse con LOGIN antes de consultar\n");
            return;
        }

        /* LIST: lista de nodos conocidos y su estado */
        if (strcmp(tipo_consulta, "LIST") == 0) {
            char lista[900] = {0};
            pthread_mutex_lock(&mutex_tabla);
            for (int i = 0; i < total_nodos; i++) {
                char item[96];
                snprintf(item, sizeof(item), "%s%s:%s", (i > 0 ? "," : ""),
                         tabla_nodos[i].id, tabla_nodos[i].activo ? "ACTIVO" : "INACTIVO");
                strncat(lista, item, sizeof(lista) - strlen(lista) - 1);
            }
            pthread_mutex_unlock(&mutex_tabla);
            snprintf(respuesta, sizeof(respuesta), "RESP|SERVER|LIST|%s\n",
                     lista[0] ? lista : "NINGUNO");
            enviar(s, respuesta);
            return;
        }

        if (strcmp(tipo_consulta, "STATUS") != 0 && strcmp(tipo_consulta, "HISTORY") != 0) {
            enviar(s, "ERR|SERVER|400|Tipo de consulta invalido (LIST, STATUS o HISTORY)\n");
            return;
        }

        /* Control de perfiles: solo ADMIN puede consultar historiales */
        if (strcmp(tipo_consulta, "HISTORY") == 0 && strcmp(s->rol, "ADMIN") != 0) {
            enviar(s, "ERR|SERVER|403|Su perfil no tiene permiso para consultar historiales\n");
            return;
        }

        pthread_mutex_lock(&mutex_tabla);
        int idx = buscar_nodo(id_nodo);
        if (idx == -1) {
            pthread_mutex_unlock(&mutex_tabla);
            enviar(s, "ERR|SERVER|404|Nodo no registrado\n");
            return;
        }
        Nodo n = tabla_nodos[idx];   /* copia local para liberar pronto el mutex */
        pthread_mutex_unlock(&mutex_tabla);

        if (n.historial_count == 0) {
            enviar(s, "ERR|SERVER|204|El nodo aun no ha reportado datos\n");
            return;
        }

        if (strcmp(tipo_consulta, "STATUS") == 0) {
            int last = (n.historial_idx - 1 + MAX_HISTORIAL) % MAX_HISTORIAL;
            snprintf(respuesta, sizeof(respuesta), "RESP|SERVER|STATUS|%s|%.1f|%.1f|%s\n",
                     n.id, n.historial[last].cpu, n.historial[last].ram,
                     n.activo ? "ACTIVO" : "INACTIVO");
        } else {
            char hist[512] = {0};
            int start = (n.historial_idx - n.historial_count + MAX_HISTORIAL) % MAX_HISTORIAL;
            for (int i = 0; i < n.historial_count; i++) {
                int pos = (start + i) % MAX_HISTORIAL;
                char item[32];
                snprintf(item, sizeof(item), "%s%.1f,%.1f", (i > 0 ? ";" : ""),
                         n.historial[pos].cpu, n.historial[pos].ram);
                strncat(hist, item, sizeof(hist) - strlen(hist) - 1);
            }
            snprintf(respuesta, sizeof(respuesta), "RESP|SERVER|HISTORY|%s|%s\n", n.id, hist);
        }
        enviar(s, respuesta);
        return;
    }

    /* ---------------------------- BYE ---------------------------------- */
    if (strcmp(comando, "BYE") == 0) {
        enviar(s, "BYE_ACK|SERVER\n");
        s->cerrar = 1;
        return;
    }

    enviar(s, "ERR|SERVER|400|Comando no reconocido\n");
}

/* ------------------------------------------------------------------------- */
/* Hilo que atiende una conexion                                              */
/* ------------------------------------------------------------------------- */

void *manejar_conexion(void *arg) {
    Sesion *s = (Sesion *)arg;
    char acumulado[MAX_BUFFER * 2];
    size_t len = 0;
    const char *motivo = "El otro extremo cerro la conexion";

    while (!s->cerrar) {
        ssize_t n = read(s->sock, acumulado + len, sizeof(acumulado) - 1 - len);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                motivo = "Temporizador expirado: el nodo dejo de reportar";
            else
                motivo = "Error de comunicacion en el socket";
            break;
        }
        len += n;
        acumulado[len] = '\0';

        /* TCP es un flujo de bytes: separamos los mensajes por '\n' */
        char *inicio = acumulado;
        char *nl;
        while (!s->cerrar && (nl = strchr(inicio, '\n')) != NULL) {
            *nl = '\0';
            if (nl > inicio && *(nl - 1) == '\r') *(nl - 1) = '\0';
            if (*inicio != '\0') {
                guardar_log("ENTRANTE", s->ip, s->puerto, inicio);
                procesar_mensaje(s, inicio);
            }
            inicio = nl + 1;
        }

        /* Guardamos lo que quedo incompleto para la siguiente lectura */
        size_t resto = len - (inicio - acumulado);
        memmove(acumulado, inicio, resto);
        len = resto;

        if (len >= sizeof(acumulado) - 1) {
            enviar(s, "ERR|SERVER|400|Mensaje demasiado largo o sin terminador\n");
            len = 0;
        }
    }

    if (s->cerrar) motivo = "Cierre ordenado (BYE)";

    /* Si era un nodo, lo marcamos como inactivo */
    if (s->tipo == SESION_NODO) {
        pthread_mutex_lock(&mutex_tabla);
        int idx = buscar_nodo(s->id_nodo);
        if (idx != -1) tabla_nodos[idx].activo = 0;
        pthread_mutex_unlock(&mutex_tabla);

        char aviso[256];
        snprintf(aviso, sizeof(aviso), "Nodo %s marcado como INACTIVO (%s)", s->id_nodo, motivo);
        guardar_log("INFO", s->ip, s->puerto, aviso);
    } else {
        guardar_log("INFO", s->ip, s->puerto, motivo);
    }

    close(s->sock);
    free(s);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Main                                                                       */
/* ------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 5) {
        printf("Uso: %s <puerto> <archivoDeLogs> [hostAuth] [puertoAuth]\n", argv[0]);
        printf("     hostAuth por defecto: localhost, puertoAuth por defecto: 9000\n");
        exit(1);
    }

    int puerto = atoi(argv[1]);
    if (puerto <= 0 || puerto > 65535) {
        printf("Puerto invalido: %s\n", argv[1]);
        exit(1);
    }
    snprintf(archivo_logs, sizeof(archivo_logs), "%s", argv[2]);
    if (argc >= 4) snprintf(auth_host,   sizeof(auth_host),   "%s", argv[3]);
    if (argc >= 5) snprintf(auth_puerto, sizeof(auth_puerto), "%s", argv[4]);

    /* Evita que el proceso muera si se escribe en un socket cerrado */
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(puerto);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen"); exit(1);
    }

    printf("Servidor escuchando en el puerto %d\n", puerto);
    printf("Archivo de logs: %s\n", archivo_logs);
    printf("Servicio de autenticacion: %s:%s\n\n", auth_host, auth_puerto);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_sock = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            perror("accept");
            continue;
        }

        Sesion *s = calloc(1, sizeof(Sesion));
        if (s == NULL) { close(client_sock); continue; }
        s->sock   = client_sock;
        s->puerto = ntohs(client_addr.sin_port);
        s->tipo   = SESION_IDENTIFICANDO;
        inet_ntop(AF_INET, &client_addr.sin_addr, s->ip, INET_ADDRSTRLEN);

        guardar_log("INFO", s->ip, s->puerto, "Nueva conexion aceptada");

        pthread_t hilo;
        if (pthread_create(&hilo, NULL, manejar_conexion, s) != 0) {
            guardar_log("ERROR", s->ip, s->puerto, "No se pudo crear el hilo");
            close(client_sock);
            free(s);
            continue;
        }
        pthread_detach(hilo);
    }

    close(server_fd);
    return 0;
}
