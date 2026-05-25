#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>
#include "multiboot.h"

void video_init(multiboot_info_t *mbi);
void kprint(const char *msg);
void klog(const char *msg);
void video_putpixel(int x, int y, uint32_t color);
void video_clear(uint32_t color);
void video_putc(char c, uint32_t color);
void video_draw_char(int x, int y, char c, uint32_t color);
void video_draw_string(int x, int y, const char *s, uint32_t color);

#endif
