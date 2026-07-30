#!/bin/sh
set -e
cd /mnt/d/sistemas/microk

printf "Hola desde un archivo con nombre largo y acentos.\n" > /tmp/testfile.txt

make image-gguf

LONGNAME="café del día muy largo y con ñ.txt"
mcopy -i storage.img /tmp/testfile.txt "::/$LONGNAME"

echo "---- mdir output (host-side mtools view) ----"
mdir -i storage.img ::/

echo "---- done ----"
