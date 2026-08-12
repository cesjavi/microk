#ifndef IO_H
#define IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ( "outw %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ( "inw %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ( "inl %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

// Bulk port I/O via REP INSW/OUTSW: one repeated instruction instead of a
// manual per-word loop of inw()/outw(). Under software emulation (QEMU TCG)
// this barely matters, but under hardware virtualization (VT-x/AMD-V) each
// individual in/out is its own VM-exit, while REP INSW/OUTSW is handled by
// the hypervisor as (at most) a handful of exits for the whole block -- the
// standard real-world fix for exactly the ATA PIO slowness documented in
// ROADMAP.md's "Arranque en Hardware Real" findings.
static inline void insw(uint16_t port, void *addr, uint32_t count) {
    asm volatile ( "rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory" );
}

static inline void outsw(uint16_t port, const void *addr, uint32_t count) {
    asm volatile ( "rep outsw" : "+S"(addr), "+c"(count) : "d"(port) : "memory" );
}

#endif
