"""
Cliente de administracion. Permite autenticarse y consultar el estado de los nodos.

Uso: python cliente.py [host] [puerto]      (por defecto: localhost 8080)

Usuarios de prueba (servicio de autenticacion):
    admin / admin123     -> perfil ADMIN  (LIST, STATUS, HISTORY)
    operador / oper123   -> perfil VIEWER (LIST, STATUS)
"""
import getpass
import socket
import sys

TIMEOUT = 10  # segundos maximos esperando respuesta del servidor


class Conexion:
    def __init__(self, host, puerto):
        ip = socket.gethostbyname(host)      # puede lanzar socket.gaierror
        self.sock = socket.create_connection((ip, puerto), timeout=TIMEOUT)
        self.sock.settimeout(TIMEOUT)
        self.lector = self.sock.makefile("r", encoding="utf-8", newline="\n")
        self.ip = ip

    def enviar(self, mensaje):
        self.sock.sendall(mensaje.encode("utf-8"))
        respuesta = self.lector.readline()
        if not respuesta:
            raise ConnectionError("El servidor cerro la conexion")
        return respuesta.strip()

    def cerrar(self):
        try:
            self.sock.close()
        except OSError:
            pass


def mostrar_respuesta(respuesta):
    campos = respuesta.split("|")
    tipo = campos[0]

    if tipo == "RESP" and len(campos) >= 4:
        subtipo = campos[2]
        if subtipo == "LIST":
            print("\n  Nodos registrados:")
            if campos[3] == "NINGUNO":
                print("    (ninguno)")
            else:
                for item in campos[3].split(","):
                    nombre, _, estado = item.partition(":")
                    print(f"    - {nombre:<15} {estado}")
        elif subtipo == "STATUS" and len(campos) >= 7:
            print(f"\n  Nodo   : {campos[3]}")
            print(f"  Estado : {campos[6]}")
            print(f"  CPU    : {campos[4]}%")
            print(f"  RAM    : {campos[5]}%")
        elif subtipo == "HISTORY" and len(campos) >= 5:
            registros = campos[4].split(";")
            print(f"\n  Historial de {campos[3]} (del mas antiguo al mas reciente):")
            for i, reg in enumerate(registros, 1):
                cpu, _, ram = reg.partition(",")
                print(f"    [{i}] CPU: {cpu}%   RAM: {ram}%")
        else:
            print(f"\n  Respuesta: {respuesta}")
    elif tipo == "ERR" and len(campos) >= 4:
        print(f"\n  Error {campos[2]}: {campos[3]}")
    else:
        print(f"\n  Respuesta no reconocida: {respuesta}")


def iniciar_sesion(host, puerto):
    """Conecta y autentica. Devuelve (conexion, usuario, rol) o None si el usuario desiste."""
    while True:
        con = None
        try:
            con = Conexion(host, puerto)
            print(f"\nConectado a {host} ({con.ip}:{puerto})")
            usuario = input("  Usuario: ").strip()
            clave = getpass.getpass("  Clave: ")
            if not usuario or not clave or "|" in usuario or "|" in clave:
                print("  Usuario o clave invalidos (no pueden estar vacios ni contener '|').")
            else:
                resp = con.enviar(f"LOGIN|{usuario}|{clave}\n")
                campos = resp.split("|")
                if campos[0] == "LOGIN_OK" and len(campos) >= 4:
                    return con, campos[2], campos[3]
                mostrar_respuesta(resp)
        except socket.gaierror as e:
            print(f"\n  No se pudo resolver el nombre '{host}': {e}")
        except (socket.timeout, TimeoutError):
            print(f"\n  El servidor no respondio en {TIMEOUT}s.")
        except (ConnectionError, OSError) as e:
            print(f"\n  Error de conexion: {e}")

        if con:
            con.cerrar()
        if input("\n  Reintentar? (s/n): ").strip().lower() != "s":
            return None


def menu(host, puerto):
    print("=== Cliente de Administracion - Monitoreo CPU/RAM ===")

    while True:
        sesion = iniciar_sesion(host, puerto)
        if sesion is None:
            print("Saliendo...")
            return
        con, usuario, rol = sesion
        print(f"\n  Sesion iniciada como {usuario} (perfil {rol})")

        while True:
            print("\n  [1] Listar nodos (LIST)")
            print("  [2] Estado actual de un nodo (STATUS)")
            print("  [3] Historial de un nodo (HISTORY)")
            print("  [4] Salir")
            opcion = input("\n  Opcion: ").strip()

            try:
                if opcion == "4":
                    con.enviar("BYE|" + usuario + "\n")
                    con.cerrar()
                    print("Sesion cerrada.")
                    return
                if opcion == "1":
                    mostrar_respuesta(con.enviar(f"GET|{usuario}|LIST|ALL\n"))
                elif opcion in ("2", "3"):
                    id_nodo = input("  ID del nodo (ej: NODE_01): ").strip()
                    if not id_nodo or "|" in id_nodo:
                        print("  ID invalido.")
                        continue
                    tipo = "STATUS" if opcion == "2" else "HISTORY"
                    mostrar_respuesta(con.enviar(f"GET|{usuario}|{tipo}|{id_nodo}\n"))
                else:
                    print("  Opcion invalida.")
            except (socket.timeout, TimeoutError):
                print(f"\n  El servidor no respondio en {TIMEOUT}s. Se debe reconectar.")
                con.cerrar()
                break
            except (ConnectionError, OSError) as e:
                print(f"\n  Se perdio la conexion con el servidor: {e}")
                con.cerrar()
                break


if __name__ == "__main__":
    host_servidor   = sys.argv[1] if len(sys.argv) >= 2 else "localhost"
    puerto_servidor = int(sys.argv[2]) if len(sys.argv) >= 3 else 8080
    try:
        menu(host_servidor, puerto_servidor)
    except (KeyboardInterrupt, EOFError):
        print("\nSaliendo...")
