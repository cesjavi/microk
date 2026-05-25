# FAT32 loadmodel test

This test boots MicroK with a raw FAT32 disk image and loads `model.mklm` from
the filesystem instead of relying on a Multiboot initrd module.

## Run

```bash
make qemu
```

The `qemu` target creates `storage.img`, formats it as FAT32, and copies the
same model to:

- `/model.mklm`
- `/models/model.mklm`

## Commands inside MicroK

```text
ls
ls models
loadmodel model.mklm
llm status
llm ask hola
loadmodel models/model.mklm
llm status
llm ask fat32
```

Expected result: `loadmodel` reports that the model loaded, and `llm status`
continues to report a valid MKLM/MKNN model.

## Current limits

- Paths support relative navigation from the shell (`cd`, `.`, `..`).
- Basic ASCII VFAT long names are decoded for listing and shell completion.
- The test image is unpartitioned FAT32, which keeps the QEMU path simple.
