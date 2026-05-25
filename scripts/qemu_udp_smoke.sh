#!/usr/bin/env bash
set -u

LOG_FILE="${MICROK_QEMU_LOG:-/tmp/microk-qemu.log}"
PCAP_FILE="${MICROK_QEMU_PCAP:-/tmp/microk-net.pcap}"
PORT="${MICROK_LLM_PORT:-1234}"

rm -f "$LOG_FILE"
rm -f "$PCAP_FILE"

timeout 35s qemu-system-i386 \
  -kernel build/kernel.bin \
  -initrd stories15M.gguf \
  -drive file=storage.img,format=raw,index=0,media=disk \
  -netdev user,id=net0,hostfwd=udp::"$PORT"-:"$PORT" \
  -object filter-dump,id=netdump0,netdev=net0,file="$PCAP_FILE" \
  -device e1000,netdev=net0 \
  -m 256M \
  -serial file:"$LOG_FILE" \
  -display none \
  -monitor none \
  -no-reboot \
  -accel tcg &
qpid=$!

for _ in $(seq 1 25); do
  if grep -q "MicroK >" "$LOG_FILE" 2>/dev/null; then
    break
  fi
  sleep 1
done

python3 scripts/llm_udp_client.py --port "$PORT" --timeout 5 PING
client_rc=$?

kill "$qpid" 2>/dev/null || true
wait "$qpid" 2>/dev/null || true

echo "CLIENT_RC=$client_rc"
ls -l "$PCAP_FILE" 2>/dev/null || true
tail -80 "$LOG_FILE"
exit "$client_rc"
