#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define MAX_BUFFER 1024

// Función auxiliar para guardar logs
void guardar_log(const char *archivo, const char *tipo, const char *ip, int puerto, const char *mensaje) {
    FILE *file = fopen(archivo, "a");
    if (file != NULL) {
        printf("[%s] %s:%d -> %s", tipo, ip, puerto, mensaje);
        if (mensaje[strlen(mensaje)-1] != '\n') {
            printf("\n");
            fprintf(file, "[%s] %s:%d -> %s\n", tipo, ip, puerto, mensaje);
        } else {
            fprintf(file, "[%s] %s:%d -> %s", tipo, ip, puerto, mensaje);
        }
        fclose(file);
    } else {
        perror("Error al abrir el archivo de log");
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Uso: %s <puerto> <archivoDeLogs>\n", argv[0]);
        exit(1);
    }

    int puerto = atoi(argv[1]);
    char *archivo_logs = argv[2];

    printf("Iniciando servidor en el puerto %d...\n", puerto);
    printf("Los logs se guardarán en: %s\n", archivo_logs);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Error al crear el socket");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0) {
        perror("Error en setsockopt");
        exit(1);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(puerto);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error en bind");
        exit(1);
    }

    if (listen(server_fd, 5) < 0) {
        perror("Error en listen");
        exit(1);
    }

    printf("Servidor escuchando y listo para recibir conexiones...\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) continue;

        char *client_ip = inet_ntoa(client_addr.sin_addr);
        int client_port = ntohs(client_addr.sin_port);

        char buffer[MAX_BUFFER] = {0};
        int valread = read(client_socket, buffer, MAX_BUFFER - 1);
        
        if (valread > 0) {
            guardar_log(archivo_logs, "ENTRANTE", client_ip, client_port, buffer);

            // INTERPRETACION DE MENSAJES (FASE 2: Parseo Básico)
            char respuesta[MAX_BUFFER] = {0};
            
            // Hacemos una copia del buffer para usar strtok de forma segura
            char buffer_copy[MAX_BUFFER];
            strcpy(buffer_copy, buffer);
            buffer_copy[strcspn(buffer_copy, "\n")] = 0; // Limpiamos el salto de linea final

            // Extraemos el primer campo (El Comando)
            char *comando = strtok(buffer_copy, "|");

            if (comando == NULL) {
                strcpy(respuesta, "ERR|SERVER|400|Formato Invalido\n");
            } 
            else if (strcmp(comando, "REG") == 0) {
                char *id_nodo = strtok(NULL, "|");
                if (id_nodo) {
                    sprintf(respuesta, "REG_ACK|SERVER|%s|SUCCESS\n", id_nodo);
                } else {
                    strcpy(respuesta, "ERR|SERVER|400|Faltan parametros en REG\n");
                }
            } 
            else if (strcmp(comando, "DATA") == 0) {
                char *id_nodo = strtok(NULL, "|");
                char *cpu = strtok(NULL, "|");
                char *ram = strtok(NULL, "|");
                if (id_nodo && cpu && ram) {
                    sprintf(respuesta, "OK|SERVER|Datos de %s recibidos (CPU:%s RAM:%s)\n", id_nodo, cpu, ram);
                } else {
                    strcpy(respuesta, "ERR|SERVER|400|Faltan parametros en DATA\n");
                }
            } 
            else if (strcmp(comando, "GET") == 0) {
                char *id_cliente = strtok(NULL, "|");
                char *tipo_consulta = strtok(NULL, "|");
                char *id_nodo = strtok(NULL, "|");
                if (id_cliente && tipo_consulta && id_nodo) {
                    // Para la Fase 2, respondemos con datos estáticos de prueba (dummy)
                    if (strcmp(tipo_consulta, "STATUS") == 0) {
                        sprintf(respuesta, "RESP|SERVER|STATUS|%s|45.0|60.0\n", id_nodo);
                    } else if (strcmp(tipo_consulta, "HISTORY") == 0) {
                        sprintf(respuesta, "RESP|SERVER|HISTORY|%s|40,60;41,61;45,60;44,59;45,60\n", id_nodo);
                    } else {
                        strcpy(respuesta, "ERR|SERVER|400|Tipo de consulta invalido\n");
                    }
                } else {
                    strcpy(respuesta, "ERR|SERVER|400|Faltan parametros en GET\n");
                }
            } 
            else {
                sprintf(respuesta, "ERR|SERVER|400|Comando no reconocido\n");
            }

            // Enviar la respuesta construida
            write(client_socket, respuesta, strlen(respuesta));

            guardar_log(archivo_logs, "SALIENTE", client_ip, client_port, respuesta);
            printf("------------------------------------------------\n");
        }

        close(client_socket);
    }

    close(server_fd);
    return 0;
}
