import socket
import sys

def iniciar_nodo(host, puerto):
    # 1. Crear el socket TCP
    cliente = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
    try:
        # 2. Resolver el nombre de dominio a IP y conectar (Requisito PDF)
        ip_resuelta = socket.gethostbyname(host)
        cliente.connect((ip_resuelta, puerto))
        print(f"Conectado al servidor {host} ({ip_resuelta}:{puerto})")
        
        # 3. Enviar mensaje de registro siguiendo nuestra sintaxis
        mensaje = "REG|NODE_01|LINUX_SERVER\n"
        cliente.sendall(mensaje.encode('utf-8'))
        print(f"Enviado: {mensaje.strip()}")
        
        # 4. Esperar la respuesta (REG_ACK)
        respuesta = cliente.recv(1024).decode('utf-8')
        print(f"Respuesta del servidor: {respuesta.strip()}")
        
    except Exception as e:
        print(f"Error de conexión: {e}")
    finally:
        cliente.close()

if __name__ == "__main__":
    host_servidor = 'localhost'
    puerto_servidor = 8080
    if len(sys.argv) == 3:
        host_servidor = sys.argv[1]
        puerto_servidor = int(sys.argv[2])
    
    iniciar_nodo(host_servidor, puerto_servidor)
