import socket
import sys

def query_microk(prompt, ip="10.0.2.15", port=1234):
    """
    Envia un prompt de IA al MicroK Kernel via UDP.
    """
    print(f"[*] Enviando prompt a MicroK AI-OS ({ip}:{port}): {prompt}")
    
    # Crear socket UDP
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)
    
    try:
        # Enviar prompt
        sock.sendto(prompt.encode(), (ip, port))
        
        print("[+] Paquete enviado. MicroK procesara la inferencia neuronal.")
        print("[TIP] Revisa la consola de QEMU (o el log serial) para ver la generacion de tokens en tiempo real.")
        
    except Exception as e:
        print(f"[!] Error de red: {e}")
    finally:
        sock.close()

if __name__ == "__main__":
    p = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "Hello from Host"
    query_microk(p)
