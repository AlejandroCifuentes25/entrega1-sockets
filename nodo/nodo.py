"""
Nodo (agente de monitoreo). Reporta CPU y RAM reales al servidor central.

Uso: python nodo.py [host] [puerto] [id_nodo]
     por defecto: localhost 8080 NODE_01

Comportamiento ante fallas:
  - Si falla la resolucion DNS, la conexion o el servidor no responde en
    TIMEOUT segundos, el nodo NO termina: cierra el socket, espera
    ESPERA_REINTENTO segundos y vuelve a conectarse y registrarse.
"""
import platform
import socket
import sys
import time

import psutil

UMBRAL_CPU       = 90.0  # % de CPU que dispara una ALARM
UMBRAL_RAM       = 90.0  # % de RAM que dispara una ALARM
INTERVALO        = 5     # segundos entre cada DATA
TIMEOUT          = 5     # segundos maximos esperando respuesta del servidor
ESPERA_REINTENTO = 5     # segundos antes de reintentar la conexion


class ErrorProtocolo(Exception):
    pass


def conectar(host, puerto):
    ip = socket.gethostbyname(host)          # puede lanzar socket.gaierror
    s = socket.create_connection((ip, puerto), timeout=TIMEOUT)
    s.settimeout(TIMEOUT)
    return s, ip


def transaccion(s, lector, mensaje):
    """Envia un mensaje y espera una linea de respuesta (con timeout)."""
    s.sendall(mensaje.encode("utf-8"))
    respuesta = lector.readline()
    if not respuesta:
        raise ConnectionError("El servidor cerro la conexion")
    return respuesta.strip()


def ejecutar_nodo(host, puerto, id_nodo):
    tipo = platform.system().upper() or "DESCONOCIDO"

    while True:
        s = None
        try:
            s, ip = conectar(host, puerto)
            lector = s.makefile("r", encoding="utf-8", newline="\n")
            print(f"[NODO] Conectado a {host} ({ip}:{puerto})")

            # 1. Registro obligatorio
            resp = transaccion(s, lector, f"REG|{id_nodo}|{tipo}\n")
            print(f"[NODO] REG -> {resp}")
            if not resp.startswith("REG_ACK"):
                raise ErrorProtocolo(f"Registro rechazado: {resp}")

            print(f"[NODO] Monitoreo cada {INTERVALO}s (Ctrl+C para detener)\n")
            psutil.cpu_percent(interval=None)  # primera lectura de referencia

            # 2. Ciclo de monitoreo
            while True:
                cpu = psutil.cpu_percent(interval=1)
                ram = psutil.virtual_memory().percent

                resp = transaccion(s, lector, f"DATA|{id_nodo}|{cpu:.1f}|{ram:.1f}\n")
                print(f"[NODO] CPU:{cpu:.1f}% RAM:{ram:.1f}% -> {resp}")
                if resp.startswith("ERR"):
                    print(f"[NODO] El servidor reporto un error: {resp}")

                # 3. Eventos criticos: se envian de inmediato
                if cpu > UMBRAL_CPU:
                    resp = transaccion(s, lector,
                        f"ALARM|{id_nodo}|CPU_OVERLOAD|{cpu:.1f}|CPU supera el {UMBRAL_CPU:.0f} por ciento\n")
                    print(f"[NODO] *** ALARMA CPU -> {resp}")
                if ram > UMBRAL_RAM:
                    resp = transaccion(s, lector,
                        f"ALARM|{id_nodo}|RAM_OVERLOAD|{ram:.1f}|RAM supera el {UMBRAL_RAM:.0f} por ciento\n")
                    print(f"[NODO] *** ALARMA RAM -> {resp}")

                time.sleep(INTERVALO)

        except KeyboardInterrupt:
            print("\n[NODO] Detenido por el usuario.")
            if s:
                try:
                    s.sendall(b"BYE|" + id_nodo.encode() + b"\n")
                except OSError:
                    pass
                s.close()
            return
        except socket.gaierror as e:
            print(f"[NODO] No se pudo resolver el nombre '{host}': {e}")
        except (socket.timeout, TimeoutError):
            print(f"[NODO] Temporizador expirado: el servidor no respondio en {TIMEOUT}s")
        except ErrorProtocolo as e:
            print(f"[NODO] {e}")
        except (ConnectionError, OSError) as e:
            print(f"[NODO] Falla de comunicacion: {e}")

        if s:
            s.close()
        print(f"[NODO] Reintentando en {ESPERA_REINTENTO}s...\n")
        try:
            time.sleep(ESPERA_REINTENTO)
        except KeyboardInterrupt:
            print("\n[NODO] Detenido por el usuario.")
            return


if __name__ == "__main__":
    host_servidor   = sys.argv[1] if len(sys.argv) >= 2 else "localhost"
    puerto_servidor = int(sys.argv[2]) if len(sys.argv) >= 3 else 8080
    id_nodo         = sys.argv[3] if len(sys.argv) >= 4 else "NODE_01"

    if "|" in id_nodo:
        print("El ID del nodo no puede contener el caracter '|'")
        sys.exit(1)

    ejecutar_nodo(host_servidor, puerto_servidor, id_nodo)
