#include "video.h"
#include "serial.h"
#include "font.h"
#include "spinlock.h"
#include "string.h"

static uint16_t *video_memory = (uint16_t *)0xB8000;
static int cursor_x = 0;
static int cursor_y = 0;
static spinlock_t video_lock;

/* Framebuffer configuration */
static uint32_t *framebuffer = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;
static uint8_t fb_bpp = 0;
static int use_framebuffer = 0;

static uint32_t vga_to_rgb(uint8_t color) {
    static const uint32_t colors[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
    };
    return colors[color & 0x0F];
}

static void fb_putpixel_locked(int x, int y, uint32_t color) {
    if (x < 0 || x >= (int)fb_width || y < 0 || y >= (int)fb_height) return;
    if (fb_bpp == 32) {
        uint32_t *pixel_address = (uint32_t *)((uint8_t *)framebuffer + y * fb_pitch + x * 4);
        *pixel_address = color;
    } else if (fb_bpp == 24) {
        uint8_t *pixel_address = (uint8_t *)framebuffer + y * fb_pitch + x * 3;
        pixel_address[0] = color & 0xFF;         // B
        pixel_address[1] = (color >> 8) & 0xFF;  // G
        pixel_address[2] = (color >> 16) & 0xFF; // R
    } else if (fb_bpp == 16) {
        uint16_t *pixel_address = (uint16_t *)((uint8_t *)framebuffer + y * fb_pitch + x * 2);
        uint16_t r = (color >> 16) & 0xF8;
        uint16_t g = (color >> 8) & 0xFC;
        uint16_t b = color & 0xF8;
        *pixel_address = (r << 8) | (g << 3) | (b >> 3);
    }
}

static void fb_draw_char_locked(int x, int y, char c, uint32_t color) {
    if ((unsigned char)c > 127) c = ' ';
    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[(int)c][row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                fb_putpixel_locked(x + col, y + row, color);
            } else {
                fb_putpixel_locked(x + col, y + row, 0x00000000);
            }
        }
    }
}

static void fb_clear_locked(uint32_t color) {
    if (fb_bpp == 32) {
        uint32_t *fb = (uint32_t *)framebuffer;
        int num_pixels = fb_pitch / 4 * fb_height;
        for (int i = 0; i < num_pixels; i++) {
            fb[i] = color;
        }
    } else {
        for (int y = 0; y < (int)fb_height; y++) {
            for (int x = 0; x < (int)fb_width; x++) {
                fb_putpixel_locked(x, y, color);
            }
        }
    }
}

/* Previously, filling the last character row just cleared the entire
 * framebuffer and reset the cursor to (0,0) -- losing all prior output
 * instead of scrolling like a real terminal. Shifts the whole framebuffer
 * up by one character row (8 pixels) and clears only the newly exposed
 * bottom row, matching the VGA text-mode path's actual scrolling. */
static void fb_scroll_locked(uint32_t bg_color) {
    uint32_t row_bytes = fb_pitch * 8;
    uint32_t total_bytes = fb_pitch * fb_height;
    uint8_t *fb_bytes = (uint8_t *)framebuffer;

    if (fb_height <= 8 || row_bytes >= total_bytes) {
        fb_clear_locked(bg_color);
        return;
    }

    memmove(fb_bytes, fb_bytes + row_bytes, total_bytes - row_bytes);
    for (uint32_t y = fb_height - 8; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            fb_putpixel_locked((int)x, (int)y, bg_color);
        }
    }
}

void video_init(multiboot_info_t *mbi) {
    spin_init(&video_lock);
    
    use_framebuffer = 0;
    if (mbi && (mbi->flags & 0x800) && mbi->framebuffer_type == 1) {
        framebuffer = (uint32_t *)(uintptr_t)mbi->framebuffer_addr;
        fb_width = mbi->framebuffer_width;
        fb_height = mbi->framebuffer_height;
        fb_pitch = mbi->framebuffer_pitch;
        fb_bpp = mbi->framebuffer_bpp;
        use_framebuffer = 1;
        
        fb_clear_locked(0x00000000);
        cursor_x = 0;
        cursor_y = 0;
    } else {
        video_clear(0x07);
    }
}

void video_putpixel(int x, int y, uint32_t color) {
    uint32_t flags = spin_lock_irqsave(&video_lock);
    if (use_framebuffer) {
        fb_putpixel_locked(x, y, color);
    }
    spin_unlock_irqrestore(&video_lock, flags);
}

void video_draw_char(int x, int y, char c, uint32_t color) {
    uint32_t flags = spin_lock_irqsave(&video_lock);
    if (!use_framebuffer) {
        // VGA text fallback
        int old_x = cursor_x;
        int old_y = cursor_y;
        cursor_x = x / 8;
        cursor_y = y / 8;
        
        uint8_t attr = (uint8_t)color;
        if (attr == 0) attr = 0x07;
        
        if (c == '\n') {
            cursor_x = 0;
            cursor_y++;
        } else if (c == '\b') {
            if (cursor_x > 0) cursor_x--;
            video_memory[cursor_y * 80 + cursor_x] = (uint16_t)(' ' | (attr << 8));
        } else {
            if (cursor_y < 25 && cursor_x < 80) {
                video_memory[cursor_y * 80 + cursor_x] = (uint16_t)(c | (attr << 8));
            }
            cursor_x++;
        }
        
        cursor_x = old_x;
        cursor_y = old_y;
    } else {
        uint32_t rgb_color = color;
        if (color < 16) {
            rgb_color = vga_to_rgb(color);
        }
        fb_draw_char_locked(x, y, c, rgb_color);
    }
    spin_unlock_irqrestore(&video_lock, flags);
}

void video_draw_string(int x, int y, const char *s, uint32_t color) {
    while (s && *s) {
        video_draw_char(x, y, *s, color);
        x += 8;
        s++;
    }
}

void video_clear(uint32_t color) {
    uint32_t flags = spin_lock_irqsave(&video_lock);
    if (!use_framebuffer) {
        uint8_t attr = (uint8_t)color;
        if (attr == 0) attr = 0x07;
        uint16_t blank = (uint16_t)(' ' | (attr << 8));
        for (int i = 0; i < 80 * 25; i++) {
            video_memory[i] = blank;
        }
    } else {
        uint32_t rgb_color = color;
        if (color < 16) {
            rgb_color = vga_to_rgb(color);
        }
        fb_clear_locked(rgb_color);
    }
    cursor_x = 0;
    cursor_y = 0;
    spin_unlock_irqrestore(&video_lock, flags);
}

void video_putc(char c, uint32_t color) {
    uint32_t flags = spin_lock_irqsave(&video_lock);
    
    if (!use_framebuffer) {
        uint8_t attr = (uint8_t)color;
        if (attr == 0) attr = 0x07;
        
        if (c == '\n') {
            cursor_x = 0;
            cursor_y++;
        } else if (c == '\b') {
            if (cursor_x > 0) cursor_x--;
            video_memory[cursor_y * 80 + cursor_x] = (uint16_t)(' ' | (attr << 8));
        } else {
            if (cursor_y < 25 && cursor_x < 80) {
                video_memory[cursor_y * 80 + cursor_x] = (uint16_t)(c | (attr << 8));
            }
            cursor_x++;
        }
        
        if (cursor_x >= 80) {
            cursor_x = 0;
            cursor_y++;
        }
        
        if (cursor_y >= 25) {
            uint16_t blank = (uint16_t)(' ' | (attr << 8));
            for (int i = 0; i < 80 * 25; i++) {
                video_memory[i] = blank;
            }
            cursor_x = 0;
            cursor_y = 0;
        }
        spin_unlock_irqrestore(&video_lock, flags);
        return;
    }
    
    int cols = fb_width / 8;
    int rows = fb_height / 8;
    
    uint32_t rgb_color = color;
    if (color < 16) {
        rgb_color = vga_to_rgb(color);
    }
    
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            fb_draw_char_locked(cursor_x * 8, cursor_y * 8, ' ', rgb_color);
        }
    } else {
        if (cursor_y < rows && cursor_x < cols) {
            fb_draw_char_locked(cursor_x * 8, cursor_y * 8, c, rgb_color);
        }
        cursor_x++;
    }
    
    if (cursor_x >= cols) {
        cursor_x = 0;
        cursor_y++;
    }
    
    while (cursor_y >= rows) {
        fb_scroll_locked(0x00000000);
        cursor_y--;
    }

    spin_unlock_irqrestore(&video_lock, flags);
}

void kprint(const char *msg) {
    serial_puts(msg);
    while (*msg) {
        video_putc(*msg, 0x07);
        msg++;
    }
}

void klog(const char *msg) {
    kprint(msg);
    video_putc('\n', 0x07);
    serial_putc('\n');
}
