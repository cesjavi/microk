#!/bin/sh
set -e
cd /mnt/d/sistemas/microk

dd if=/dev/zero of=storage.img bs=1M count=1536
mkfs.fat -F 32 storage.img

mcopy -i storage.img model.mklm ::/model.mklm
mmd -i storage.img ::/models
mcopy -i storage.img model.mklm ::/models/model.mklm

# Small fixtures: keep both root + /models copies like the Makefile does.
for f in tin1.gguf tiny.gguf stories15M.gguf; do
  [ -f "$f" ] || continue
  mcopy -o -i storage.img "$f" ::/"$f"
  mcopy -o -i storage.img "$f" ::/models/"$f"
done

# Big model: only one copy, root only, to avoid wasting ~640MB twice.
mcopy -o -i storage.img tinyllama-1.1b-chat-v1.0.Q4_0.gguf ::/tinyllama-1.1b-chat-v1.0.Q4_0.gguf

echo "---- mdir ----"
mdir -i storage.img ::/
