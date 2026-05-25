# EXT2 read-only test

MicroK has an experimental ext2 read-only path for simple root-directory files.

## Current support

- Detects ext2/ext3/ext4 superblocks.
- Mounts the first compatible EXT filesystem found by the storage layer.
- `extls` lists entries in the root directory.
- `extcat <file>` reads regular files stored in direct, single-indirect,
  double-indirect, or triple-indirect blocks.
- `extcat dir/file` supports simple subdirectory traversal.
- `loadmodel <file>` can load MKLM models from EXT if FAT32 lookup fails.

## Limits

- ext3/ext4 journaling/extents are not implemented.
- This is intended for simple ext2 test images first.

## Run

```bash
make qemu-ext2
```

The target creates `storage-ext2.img`, formats it as ext2, copies `model.mklm`
to `/model.mklm` and `/models/model.mklm`, then boots QEMU with that disk.

## Commands inside MicroK

```text
extls
extcat model.txt
extcat models/model.txt
loadmodel models/model.mklm
```
