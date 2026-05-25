#include "ata.h"
#include "io.h"
#include <string.h>

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_SECCOUNT0   2
#define ATA_REG_LBA0        3
#define ATA_REG_LBA1        4
#define ATA_REG_LBA2        5
#define ATA_REG_HDDEVSEL    6
#define ATA_REG_COMMAND     7
#define ATA_REG_STATUS      7

#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_BSY          0x80
#define ATA_SR_DRDY         0x40
#define ATA_SR_DF           0x20
#define ATA_SR_DRQ          0x08
#define ATA_SR_ERR          0x01

typedef struct {
    block_device_t device;
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t drive_select;
    uint32_t sectors;
    int present;
} ata_drive_t;

static ata_drive_t primary_master;

static void ata_io_wait(ata_drive_t *drive) {
    inb(drive->ctrl_base);
    inb(drive->ctrl_base);
    inb(drive->ctrl_base);
    inb(drive->ctrl_base);
}

static int ata_wait_ready(ata_drive_t *drive) {
    for (uint32_t i = 0; i < 100000; i++) {
        uint8_t status = inb(drive->io_base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_drq(ata_drive_t *drive) {
    for (uint32_t i = 0; i < 100000; i++) {
        uint8_t status = inb(drive->io_base + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return 0;
        }
    }
    return -1;
}

static int ata_read_sector(ata_drive_t *drive, uint32_t lba, uint8_t *buffer) {
    if (!drive->present || !buffer) return -1;
    if (ata_wait_ready(drive) != 0) return -1;

    outb(drive->io_base + ATA_REG_HDDEVSEL, drive->drive_select | ((lba >> 24) & 0x0F));
    ata_io_wait(drive);
    outb(drive->io_base + ATA_REG_SECCOUNT0, 1);
    outb(drive->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(drive->io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(drive->io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(drive->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (ata_wait_drq(drive) != 0) return -1;

    uint16_t *words = (uint16_t *)buffer;
    for (int i = 0; i < 256; i++) {
        words[i] = inw(drive->io_base + ATA_REG_DATA);
    }
    ata_io_wait(drive);
    return 0;
}

static int ata_write_sector(ata_drive_t *drive, uint32_t lba, uint8_t *buffer) {
    if (!drive->present || !buffer) return -1;
    if (ata_wait_ready(drive) != 0) return -1;

    outb(drive->io_base + ATA_REG_HDDEVSEL, drive->drive_select | ((lba >> 24) & 0x0F));
    ata_io_wait(drive);
    outb(drive->io_base + ATA_REG_SECCOUNT0, 1);
    outb(drive->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(drive->io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(drive->io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(drive->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    if (ata_wait_drq(drive) != 0) return -1;

    uint16_t *words = (uint16_t *)buffer;
    for (int i = 0; i < 256; i++) {
        outw(drive->io_base + ATA_REG_DATA, words[i]);
    }
    ata_io_wait(drive);

    if (ata_wait_ready(drive) != 0) return -1;
    
    outb(drive->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_ready(drive);
    return 0;
}

static uint32_t ata_block_read(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer) {
    ata_drive_t *drive = (ata_drive_t *)device->driver_data;
    if (!drive || !buffer) return 0;

    uint8_t sector[512];
    uint32_t done = 0;
    while (done < size) {
        uint32_t absolute = offset + done;
        uint32_t lba = absolute / 512;
        uint32_t sector_offset = absolute % 512;
        uint32_t chunk = 512 - sector_offset;
        if (chunk > size - done) chunk = size - done;

        if (ata_read_sector(drive, lba, sector) != 0) {
            break;
        }
        memcpy(buffer + done, sector + sector_offset, chunk);
        done += chunk;
    }
    return done;
}

static uint32_t ata_block_write(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer) {
    ata_drive_t *drive = (ata_drive_t *)device->driver_data;
    if (!drive || !buffer) return 0;

    uint8_t sector[512];
    uint32_t done = 0;
    while (done < size) {
        uint32_t absolute = offset + done;
        uint32_t lba = absolute / 512;
        uint32_t sector_offset = absolute % 512;
        uint32_t chunk = 512 - sector_offset;
        if (chunk > size - done) chunk = size - done;

        if (chunk < 512) {
            // Partial write: read-modify-write
            if (ata_read_sector(drive, lba, sector) != 0) break;
            memcpy(sector + sector_offset, buffer + done, chunk);
            if (ata_write_sector(drive, lba, sector) != 0) break;
        } else {
            // Full sector write
            if (ata_write_sector(drive, lba, buffer + done) != 0) break;
        }
        done += chunk;
    }
    return done;
}

static int ata_identify(ata_drive_t *drive) {
    uint16_t identify[256];

    outb(drive->io_base + ATA_REG_HDDEVSEL, drive->drive_select);
    ata_io_wait(drive);
    outb(drive->io_base + ATA_REG_SECCOUNT0, 0);
    outb(drive->io_base + ATA_REG_LBA0, 0);
    outb(drive->io_base + ATA_REG_LBA1, 0);
    outb(drive->io_base + ATA_REG_LBA2, 0);
    outb(drive->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait(drive);

    if (inb(drive->io_base + ATA_REG_STATUS) == 0) {
        return -1;
    }
    if (ata_wait_drq(drive) != 0) {
        return -1;
    }

    for (int i = 0; i < 256; i++) {
        identify[i] = inw(drive->io_base + ATA_REG_DATA);
    }

    drive->sectors = ((uint32_t)identify[61] << 16) | identify[60];
    if (drive->sectors == 0) {
        return -1;
    }
    drive->present = 1;
    return 0;
}

int ata_init() {
    memset(&primary_master, 0, sizeof(primary_master));
    primary_master.io_base = ATA_PRIMARY_IO;
    primary_master.ctrl_base = ATA_PRIMARY_CTRL;
    primary_master.drive_select = 0xE0;
    primary_master.device.name = "ata0";
    primary_master.device.block_size = 512;
    primary_master.device.driver_data = &primary_master;
    primary_master.device.read = ata_block_read;
    primary_master.device.write = ata_block_write;

    if (ata_identify(&primary_master) != 0) {
        return -1;
    }
    primary_master.device.block_count = primary_master.sectors;
    return 0;
}

block_device_t *ata_primary_master() {
    return primary_master.present ? &primary_master.device : 0;
}
