import socket
import sys

def conectar(host, puerto):
    ip = socket.gethostbyname(host)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((ip, puerto))
    return s

def enviar_y_recibir(s, mensaje):
    s.sendall(mensaje.encode())
    return s.recv(4096).decode().strip()

def mostrar_respuesta(respuesta):
    campos = respuesta.split("|")
    tipo = campos[0] if len(campos) > 0 else ""

    if tipo == "RESP" and len(campos) >= 5:
        subtipo = campos[2]
        nodo    = campos[3]
        if subtipo == "STATUS":
            cpu = campos[4]
            ram = campos[5] if len(campos) > 5 else "?"
            print(f"\n  Nodo   : {nodo}")
            print(f"  CPU    : {cpu}%")
            print(f"  RAM    : {ram}%")
        elif subtipo == "HISTORY":
            registros = campos[4].split(";")
            print(f"\n  Historial de {nodo} (últimos {len(registros)} registros):")
            for i, reg in enumerate(registros, 1):
                partes = reg.split(",")
                cpu = partes[0] if len(partes) > 0 else "?"
                ram = partes[1] if len(partes) > 1 else "?"
                print(f"    [{i}] CPU: {cpu}%  RAM: {ram}%")
    elif tipo == "ERR":
        codigo = campos[2] if len(campos) > 2 else "?"
        msg    = campos[3] if len(campos) > 3 else "?"
        print(f"\n  Error {codigo}: {msg}")
    else:
        print(f"\n  Respuesta: {respuesta}")

def menu(host, puerto):
    print(f"\n=== Cliente de Administración ===")
    print(f"Servidor: {host}:{puerto}\n")

    while True:
        print("\n  [1] Consultar estado actual de un nodo (STATUS)")
        print("  [2] Consultar historial de un nodo (HISTORY)")
        print("  [3] Salir")
        opcion = input("\n  Opción: ").strip()

        if opcion == "3":
            print("Saliendo...")
            break

        if opcion not in ("1", "2"):
            print("  Opción inválida.")
            continue

        id_nodo   = input("  ID del nodo (ej: NODE_01): ").strip()
        id_cliente = "CLI_ADMIN"

        tipo = "STATUS" if opcion == "1" else "HISTORY"
        mensaje = f"GET|{id_cliente}|{tipo}|{id_nodo}\n"

        try:
            s = conectar(host, puerto)
            respuesta = enviar_y_recibir(s, mensaje)
            s.close()
            mostrar_respuesta(respuesta)
        except Exception as e:
            print(f"\n  Error de conexión: {e}")

if __name__ == "__main__":
    host_servidor   = 'localhost'
    puerto_servidor = 8080

    if len(sys.argv) >= 2:
        host_servidor = sys.argv[1]
    if len(sys.argv) >= 3:
        puerto_servidor = int(sys.argv[2])

    menu(host_servidor, puerto_servidor)
