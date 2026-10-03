import socket
import sys
import time
import psutil

UMBRAL_CPU = 90.0  # % de CPU para disparar alarma
UMBRAL_RAM = 90.0  # % de RAM para disparar alarma
INTERVALO  = 5     # segundos entre cada DATA

def iniciar_nodo(host, puerto, id_nodo):
    # Resolver nombre de dominio (sin IPs hardcodeadas)
    ip_resuelta = socket.gethostbyname(host)
    
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect((ip_resuelta, puerto))
        print(f"[NODO] Conectado al servidor {host} ({ip_resuelta}:{puerto})")
    except Exception as e:
        print(f"[NODO] No se pudo conectar: {e}")
        sys.exit(1)

    # 1. Registrarse en el servidor
    reg_msg = f"REG|{id_nodo}|LINUX_SERVER\n"
    s.sendall(reg_msg.encode())
    respuesta = s.recv(1024).decode().strip()
    print(f"[NODO] Registro -> {respuesta}")

    if "ERROR" in respuesta or "ERR" in respuesta:
        print("[NODO] El servidor rechazó el registro. Saliendo.")
        s.close()
        sys.exit(1)

    print(f"[NODO] Iniciando monitoreo cada {INTERVALO}s (Ctrl+C para detener)...\n")

    # 2. Bucle de monitoreo periódico
    try:
        while True:
            # Leer métricas reales con psutil
            cpu = psutil.cpu_percent(interval=1)       # % uso CPU
            ram = psutil.virtual_memory().percent      # % uso RAM

            # Enviar telemetría periódica
            data_msg = f"DATA|{id_nodo}|{cpu}|{ram}\n"
            s.sendall(data_msg.encode())
            resp_data = s.recv(1024).decode().strip()
            print(f"[NODO] CPU:{cpu}% RAM:{ram}% -> {resp_data}")

            # Detectar umbral crítico y enviar ALARM de inmediato
            if cpu > UMBRAL_CPU:
                alarm_msg = f"ALARM|{id_nodo}|CPU_OVERLOAD|{cpu}|CPU supera el {UMBRAL_CPU}%\n"
                s.sendall(alarm_msg.encode())
                resp_alarm = s.recv(1024).decode().strip()
                print(f"[NODO] *** ALARMA CPU enviada -> {resp_alarm}")

            if ram > UMBRAL_RAM:
                alarm_msg = f"ALARM|{id_nodo}|RAM_OVERLOAD|{ram}|RAM supera el {UMBRAL_RAM}%\n"
                s.sendall(alarm_msg.encode())
                resp_alarm = s.recv(1024).decode().strip()
                print(f"[NODO] *** ALARMA RAM enviada -> {resp_alarm}")

            time.sleep(INTERVALO)

    except KeyboardInterrupt:
        print("\n[NODO] Detenido por el usuario.")
    except Exception as e:
        print(f"[NODO] Error en el bucle: {e}")
    finally:
        s.close()

if __name__ == "__main__":
    host_servidor  = 'localhost'
    puerto_servidor = 8080
    id_nodo        = 'NODE_01'

    if len(sys.argv) >= 2:
        host_servidor = sys.argv[1]
    if len(sys.argv) >= 3:
        puerto_servidor = int(sys.argv[2])
    if len(sys.argv) >= 4:
        id_nodo = sys.argv[3]

    iniciar_nodo(host_servidor, puerto_servidor, id_nodo)
