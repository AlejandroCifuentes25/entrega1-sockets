#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAX_BUFFER   1024
#define MAX_NODOS    10
#define MAX_HISTORIAL 5

// ---------------------------------------------------------------------------
// Estructuras de datos compartidas
// ---------------------------------------------------------------------------

// Un registro de telemetría puntual
typedef struct {
    float cpu;
    float ram;
} Registro;

// Un nodo activo registrado en el servidor
typedef struct {
    char id[64];
    int  activo;
    Registro historial[MAX_HISTORIAL];
    int  historial_count; // cuántos registros hay (máximo MAX_HISTORIAL)
    int  historial_idx;   // índice circular para inserción
} Nodo;

// Tabla global de nodos activos (memoria compartida entre hilos)
Nodo tabla_nodos[MAX_NODOS];
int  total_nodos = 0;

// Mutex para proteger la tabla de nodos
pthread_mutex_t mutex_tabla = PTHREAD_MUTEX_INITIALIZER;

// Nombre del archivo de logs (global para acceder desde los hilos)
char archivo_logs[256];

// ---------------------------------------------------------------------------
// Funciones auxiliares
// ---------------------------------------------------------------------------

void guardar_log(const char *tipo, const char *ip, int puerto, const char *mensaje) {
    FILE *file = fopen(archivo_logs, "a");
    if (file != NULL) {
        printf("[%s] %s:%d -> %s", tipo, ip, puerto, mensaje);
        if (mensaje[strlen(mensaje) - 1] != '\n') {
            printf("\n");
            fprintf(file, "[%s] %s:%d -> %s\n", tipo, ip, puerto, mensaje);
        } else {
            fprintf(file, "[%s] %s:%d -> %s", tipo, ip, puerto, mensaje);
        }
        fclose(file);
    }
}

// Busca un nodo por ID. Devuelve el índice o -1 si no existe.
// IMPORTANTE: llamar siempre con el mutex bloqueado.
int buscar_nodo(const char *id) {
    for (int i = 0; i < total_nodos; i++) {
        if (strcmp(tabla_nodos[i].id, id) == 0)
            return i;
    }
    return -1;
}

// Registra un nodo nuevo. Devuelve el índice o -1 si la tabla está llena.
// IMPORTANTE: llamar siempre con el mutex bloqueado.
int registrar_nodo(const char *id) {
    if (total_nodos >= MAX_NODOS) return -1;
    int idx = total_nodos++;
    memset(&tabla_nodos[idx], 0, sizeof(Nodo));
    strncpy(tabla_nodos[idx].id, id, sizeof(tabla_nodos[idx].id) - 1);
    tabla_nodos[idx].activo = 1;
    return idx;
}

// Inserta un nuevo registro en el historial circular del nodo.
// IMPORTANTE: llamar siempre con el mutex bloqueado.
void insertar_historial(int idx, float cpu, float ram) {
    int pos = tabla_nodos[idx].historial_idx;
    tabla_nodos[idx].historial[pos].cpu = cpu;
    tabla_nodos[idx].historial[pos].ram = ram;
    tabla_nodos[idx].historial_idx = (pos + 1) % MAX_HISTORIAL;
    if (tabla_nodos[idx].historial_count < MAX_HISTORIAL)
        tabla_nodos[idx].historial_count++;
}

// ---------------------------------------------------------------------------
// Estructura que le pasamos a cada hilo con los datos de su conexión
// ---------------------------------------------------------------------------
typedef struct {
    int    socket;
    char   ip[INET_ADDRSTRLEN];
    int    puerto;
} ConexionArgs;

// ---------------------------------------------------------------------------
// Función que ejecuta cada hilo: atiende a UN cliente o nodo
// ---------------------------------------------------------------------------
void *manejar_cliente(void *arg) {
    ConexionArgs *conn = (ConexionArgs *)arg;
    int    sock   = conn->socket;
    char   ip[INET_ADDRSTRLEN];
    int    puerto = conn->puerto;
    strncpy(ip, conn->ip, INET_ADDRSTRLEN);
    free(conn); // ya copiamos lo que necesitamos

    char buffer[MAX_BUFFER]   = {0};
    char respuesta[MAX_BUFFER] = {0};

    // Leemos mensajes en bucle hasta que el cliente cierre la conexión
    int valread;
    while ((valread = read(sock, buffer, MAX_BUFFER - 1)) > 0) {
        buffer[valread] = '\0';
        guardar_log("ENTRANTE", ip, puerto, buffer);

        // Copia segura para strtok
        char copia[MAX_BUFFER];
        strncpy(copia, buffer, MAX_BUFFER);
        copia[strcspn(copia, "\n")] = '\0';

        char *comando = strtok(copia, "|");
        memset(respuesta, 0, MAX_BUFFER);

        if (comando == NULL) {
            strcpy(respuesta, "ERR|SERVER|400|Formato invalido\n");

        // ------------------------------------------------------------------
        } else if (strcmp(comando, "REG") == 0) {
            char *id_nodo = strtok(NULL, "|");
            if (!id_nodo) {
                strcpy(respuesta, "ERR|SERVER|400|Faltan parametros en REG\n");
            } else {
                pthread_mutex_lock(&mutex_tabla);
                int idx = buscar_nodo(id_nodo);
                if (idx == -1) {
                    idx = registrar_nodo(id_nodo);
                }
                if (idx == -1) {
                    strcpy(respuesta, "ERR|SERVER|503|Tabla de nodos llena\n");
                } else {
                    tabla_nodos[idx].activo = 1;
                    sprintf(respuesta, "REG_ACK|SERVER|%s|SUCCESS\n", id_nodo);
                }
                pthread_mutex_unlock(&mutex_tabla);
            }

        // ------------------------------------------------------------------
        } else if (strcmp(comando, "DATA") == 0) {
            char *id_nodo = strtok(NULL, "|");
            char *s_cpu   = strtok(NULL, "|");
            char *s_ram   = strtok(NULL, "|");
            if (!id_nodo || !s_cpu || !s_ram) {
                strcpy(respuesta, "ERR|SERVER|400|Faltan parametros en DATA\n");
            } else {
                float cpu = atof(s_cpu);
                float ram = atof(s_ram);

                pthread_mutex_lock(&mutex_tabla);
                int idx = buscar_nodo(id_nodo);
                if (idx == -1) {
                    strcpy(respuesta, "ERR|SERVER|404|Nodo no registrado\n");
                } else {
                    insertar_historial(idx, cpu, ram);
                    sprintf(respuesta, "OK|SERVER|Datos de %s recibidos (CPU:%.1f RAM:%.1f)\n",
                            id_nodo, cpu, ram);
                }
                pthread_mutex_unlock(&mutex_tabla);
            }

        // ------------------------------------------------------------------
        } else if (strcmp(comando, "ALARM") == 0) {
            char *id_nodo    = strtok(NULL, "|");
            char *tipo_alarm = strtok(NULL, "|");
            char *valor      = strtok(NULL, "|");
            char *descripcion = strtok(NULL, "|");
            if (!id_nodo || !tipo_alarm) {
                strcpy(respuesta, "ERR|SERVER|400|Faltan parametros en ALARM\n");
            } else {
                printf("[ALARMA] Nodo:%s Tipo:%s Valor:%s Desc:%s\n",
                       id_nodo,
                       tipo_alarm,
                       valor       ? valor       : "N/A",
                       descripcion ? descripcion : "N/A");
                sprintf(respuesta, "OK|SERVER|ALARM recibida para %s\n", id_nodo);
            }

        // ------------------------------------------------------------------
        } else if (strcmp(comando, "GET") == 0) {
            char *id_cliente    = strtok(NULL, "|");
            char *tipo_consulta = strtok(NULL, "|");
            char *id_nodo       = strtok(NULL, "|");

            if (!id_cliente || !tipo_consulta || !id_nodo) {
                strcpy(respuesta, "ERR|SERVER|400|Faltan parametros en GET\n");
            } else {
                pthread_mutex_lock(&mutex_tabla);
                int idx = buscar_nodo(id_nodo);

                if (idx == -1) {
                    strcpy(respuesta, "ERR|SERVER|404|Nodo no registrado\n");
                } else if (strcmp(tipo_consulta, "STATUS") == 0) {
                    // Último dato disponible
                    int last = (tabla_nodos[idx].historial_idx - 1 + MAX_HISTORIAL) % MAX_HISTORIAL;
                    if (tabla_nodos[idx].historial_count == 0) {
                        strcpy(respuesta, "ERR|SERVER|204|Sin datos aun\n");
                    } else {
                        sprintf(respuesta, "RESP|SERVER|STATUS|%s|%.1f|%.1f\n",
                                id_nodo,
                                tabla_nodos[idx].historial[last].cpu,
                                tabla_nodos[idx].historial[last].ram);
                    }
                } else if (strcmp(tipo_consulta, "HISTORY") == 0) {
                    if (tabla_nodos[idx].historial_count == 0) {
                        strcpy(respuesta, "ERR|SERVER|204|Sin datos aun\n");
                    } else {
                        // Construimos la cadena de historial ordenado más antiguo -> más nuevo
                        char hist_str[MAX_BUFFER] = {0};
                        int count = tabla_nodos[idx].historial_count;
                        int start = (tabla_nodos[idx].historial_idx - count + MAX_HISTORIAL) % MAX_HISTORIAL;
                        for (int i = 0; i < count; i++) {
                            int pos = (start + i) % MAX_HISTORIAL;
                            char entry[64];
                            snprintf(entry, sizeof(entry), "%.1f,%.1f",
                                     tabla_nodos[idx].historial[pos].cpu,
                                     tabla_nodos[idx].historial[pos].ram);
                            strncat(hist_str, entry, MAX_BUFFER - strlen(hist_str) - 1);
                            if (i < count - 1)
                                strncat(hist_str, ";", MAX_BUFFER - strlen(hist_str) - 1);
                        }
                        snprintf(respuesta, MAX_BUFFER, "RESP|SERVER|HISTORY|%s|%s\n", id_nodo, hist_str);
                    }
                } else {
                    strcpy(respuesta, "ERR|SERVER|400|Tipo de consulta invalido\n");
                }
                pthread_mutex_unlock(&mutex_tabla);
            }

        // ------------------------------------------------------------------
        } else {
            sprintf(respuesta, "ERR|SERVER|400|Comando no reconocido\n");
        }

        write(sock, respuesta, strlen(respuesta));
        guardar_log("SALIENTE", ip, puerto, respuesta);
        printf("------------------------------------------------\n");

        memset(buffer, 0, MAX_BUFFER);
    }

    printf("[INFO] Conexion cerrada: %s:%d\n", ip, puerto);
    close(sock);
    return NULL;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Uso: %s <puerto> <archivoDeLogs>\n", argv[0]);
        exit(1);
    }

    int puerto = atoi(argv[1]);
    strncpy(archivo_logs, argv[2], sizeof(archivo_logs) - 1);

    printf("Iniciando servidor en el puerto %d...\n", puerto);
    printf("Los logs se guardarán en: %s\n\n", archivo_logs);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(puerto);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen"); exit(1);
    }

    printf("Servidor escuchando... (Ctrl+C para detener)\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_sock = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) continue;

        // Preparamos los argumentos para el hilo
        ConexionArgs *args = malloc(sizeof(ConexionArgs));
        args->socket = client_sock;
        args->puerto = ntohs(client_addr.sin_port);
        inet_ntop(AF_INET, &client_addr.sin_addr, args->ip, INET_ADDRSTRLEN);

        printf("[INFO] Nueva conexion: %s:%d\n", args->ip, args->puerto);

        // Creamos el hilo y lo dejamos correr de forma independiente
        pthread_t hilo;
        pthread_create(&hilo, NULL, manejar_cliente, (void *)args);
        pthread_detach(hilo); // El hilo se limpia solo al terminar
    }

    close(server_fd);
    return 0;
}
