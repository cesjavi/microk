import socket
import time

IP = '192.168.1.22'

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5)

# Step 1: quick PING to confirm the machine is actually reachable and RSH
# is listening, before waiting on a slow LLM generation.
print(f"Sending PING to {IP}:2323 ...")
s.sendto(b'PING', (IP, 2323))
try:
    data, addr = s.recvfrom(1024)
    print("PING reply:", data)
except socket.timeout:
    print("No reply to PING within 5s -- IP wrong, RSH not running, or blocked by a firewall.")
    print("Not attempting the LLM prompt since basic connectivity already failed.")
    raise SystemExit(1)

# Step 2: the real prompt. Generation on real hardware is not instant --
# give it a couple of minutes and print progress so it's obvious the
# script is still waiting, not frozen.
print(f"\nSending 'llm ask' prompt to {IP}:2323 ...")
s.sendto(b'llm ask tell me about the sun', (IP, 2323))
s.settimeout(10)
start = time.time()
while True:
    try:
        data, addr = s.recvfrom(2048)
        print(f"\nResponse after {time.time()-start:.1f}s:", data)
        break
    except socket.timeout:
        elapsed = time.time() - start
        if elapsed > 180:
            print(f"\nStill no response after {elapsed:.0f}s -- giving up.")
            break
        print(f"  ...still waiting ({elapsed:.0f}s elapsed)")
