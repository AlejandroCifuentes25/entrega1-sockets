"""
Servicio de autenticacion (independiente del servidor central).

Protocolo AUTH (TCP, texto ASCII, campos separados por '|', fin de mensaje '\n'):
    Peticion :  AUTH|<usuario>|<clave>\n
    Respuesta:  AUTH_OK|<usuario>|<rol>\n
                AUTH_FAIL|<usuario>|<motivo>\n
                AUTH_ERR|-|<motivo>\n          (peticion mal formada)

Los usuarios se guardan en usuarios.json con la clave cifrada (SHA-256),
fuera de la aplicacion principal. Roles disponibles: ADMIN, VIEWER.

Uso: python auth_server.py [puerto]     (por defecto 9000)
"""
import hashlib
import json
import os
import socketserver
import sys
import threading
from datetime import datetime

ARCHIVO_USUARIOS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "usuarios.json")
lock_print = threading.Lock()


def hash_clave(clave):
    return hashlib.sha256(clave.encode("utf-8")).hexdigest()


def cargar_usuarios():
    # Si no existe el archivo, se crean usuarios por defecto
    if not os.path.exists(ARCHIVO_USUARIOS):
        por_defecto = {
            "admin":    {"clave": hash_clave("admin123"), "rol": "ADMIN"},
            "operador": {"clave": hash_clave("oper123"),  "rol": "VIEWER"},
        }
        with open(ARCHIVO_USUARIOS, "w", encoding="utf-8") as f:
            json.dump(por_defecto, f, indent=2)
    with open(ARCHIVO_USUARIOS, "r", encoding="utf-8") as f:
        return json.load(f)


def log(origen, texto):
    with lock_print:
        print(f"{datetime.now():%Y-%m-%d %H:%M:%S} [AUTH] {origen[0]}:{origen[1]} -> {texto}", flush=True)


class ManejadorAuth(socketserver.StreamRequestHandler):
    timeout = 10  # segundos maximos esperando la peticion

    def handle(self):
        try:
            linea = self.rfile.readline().decode("utf-8", errors="replace").strip()
        except Exception:
            return
        if not linea:
            return
        log(self.client_address, f"ENTRANTE {linea.split('|')[0]}|{'|'.join(linea.split('|')[1:2])}|***")

        campos = linea.split("|")
        if len(campos) != 3 or campos[0] != "AUTH" or not campos[1] or not campos[2]:
            respuesta = "AUTH_ERR|-|Formato invalido\n"
        else:
            usuario, clave = campos[1], campos[2]
            usuarios = cargar_usuarios()
            datos = usuarios.get(usuario)
            if datos and datos["clave"] == hash_clave(clave):
                respuesta = f"AUTH_OK|{usuario}|{datos['rol']}\n"
            else:
                respuesta = f"AUTH_FAIL|{usuario}|Credenciales invalidas\n"

        log(self.client_address, f"SALIENTE {respuesta.strip()}")
        try:
            self.wfile.write(respuesta.encode("utf-8"))
        except OSError:
            pass


class ServidorAuth(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    puerto = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    cargar_usuarios()
    with ServidorAuth(("0.0.0.0", puerto), ManejadorAuth) as srv:
        print(f"Servicio de autenticacion escuchando en el puerto {puerto}")
        print(f"Usuarios en: {ARCHIVO_USUARIOS}")
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nServicio de autenticacion detenido.")
