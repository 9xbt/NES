/*
 *   This file is part of nes_emu.
 *   Copyright (c) 2019 Franz Flasch.
 *
 *   nes_emu is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   nes_emu is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with nes_emu.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* X11/Xlib */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

/* NES specific */
#include <nes.h>
#include <ppu.h>
#include <cpu.h>
#include <cartridge.h>
#include <controller.h>

#define debug_print(fmt, ...) \
            do { if (DEBUG_MAIN) printf(fmt, __VA_ARGS__); } while (0)

void die (const char * format, ...)
{
    va_list vargs;
    va_start (vargs, format);
    vfprintf (stderr, format, vargs);
    va_end (vargs);
    exit (1);
}

#define FPS 60
#define FPS_UPDATE_TIME_MS (1000/FPS)
#define SCREEN_WIDTH 256
#define SCREEN_HEIGHT 240
#define SCALE 2

/* NES controller key indices */
typedef enum {
    KEY_A = 0,
    KEY_S,
    KEY_C,
    KEY_RETURN,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_COUNT
} nes_key_index_t;

/* X11 Context */
typedef struct {
    Display *display;
    Window window;
    GC gc;
    XImage *ximage;
    int screen;
    Visual *visual;
    int depth;
    uint8_t keys[KEY_COUNT];
    int running;
    uint32_t *scaled_buffer;
} x11_context_t;

/* Get current time in milliseconds */
static uint32_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Map KeySym to our key index */
static int keysym_to_index(KeySym keysym)
{
    switch (keysym)
    {
        case XK_a:      return KEY_A;
        case XK_s:      return KEY_S;
        case XK_c:      return KEY_C;
        case XK_Return: return KEY_RETURN;
        case XK_Up:     return KEY_UP;
        case XK_Down:   return KEY_DOWN;
        case XK_Left:   return KEY_LEFT;
        case XK_Right:  return KEY_RIGHT;
        default:        return -1;
    }
}

/* Initialize X11 */
static void x11_init(x11_context_t *ctx)
{
    memset(ctx, 0, sizeof(x11_context_t));
    ctx->running = 1;

    ctx->display = XOpenDisplay(NULL);
    if (!ctx->display)
    {
        die("Failed to open X display\n");
    }

    ctx->screen = DefaultScreen(ctx->display);
    ctx->visual = DefaultVisual(ctx->display, ctx->screen);
    ctx->depth = DefaultDepth(ctx->display, ctx->screen);

    /* Allocate scaled buffer */
    ctx->scaled_buffer = malloc(SCREEN_WIDTH * SCALE * SCREEN_HEIGHT * SCALE * sizeof(uint32_t));
    if (!ctx->scaled_buffer)
    {
        die("Failed to allocate scaled buffer\n");
    }

    /* Create window */
    ctx->window = XCreateSimpleWindow(
        ctx->display,
        RootWindow(ctx->display, ctx->screen),
        0, 0,
        SCREEN_WIDTH * SCALE, SCREEN_HEIGHT * SCALE,
        0,
        BlackPixel(ctx->display, ctx->screen),
        BlackPixel(ctx->display, ctx->screen)
    );

    /* Set window properties */
    XStoreName(ctx->display, ctx->window, "nes_emu");
    
    /* Set WM_DELETE_WINDOW protocol for proper window closing */
    Atom wm_delete_window = XInternAtom(ctx->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ctx->display, ctx->window, &wm_delete_window, 1);

    /* Select input events */
    XSelectInput(ctx->display, ctx->window, 
                 KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask);

    /* Create graphics context */
    ctx->gc = XCreateGC(ctx->display, ctx->window, 0, NULL);

    /* Create XImage for rendering at scaled size */
    ctx->ximage = XCreateImage(
        ctx->display,
        ctx->visual,
        ctx->depth,
        ZPixmap,
        0,
        (char *)ctx->scaled_buffer,  /* Use scaled buffer */
        SCREEN_WIDTH * SCALE,        /* Scaled width */
        SCREEN_HEIGHT * SCALE,       /* Scaled height */
        32,
        0
    );

    if (!ctx->ximage)
    {
        die("Failed to create XImage\n");
    }

    /* Map window and wait for it to be mapped */
    XMapWindow(ctx->display, ctx->window);
    XFlush(ctx->display);
}

/* Process X11 events */
static void x11_process_events(x11_context_t *ctx)
{
    XEvent event;
    
    while (XPending(ctx->display))
    {
        XNextEvent(ctx->display, &event);
        
        switch (event.type)
        {
            case KeyPress: 
            {
                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                int idx = keysym_to_index(keysym);
                if (idx >= 0)
                    ctx->keys[idx] = 1;
                break;
            }
            case KeyRelease:
            {
                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                int idx = keysym_to_index(keysym);
                if (idx >= 0)
                    ctx->keys[idx] = 0;
                break;
            }
            case ClientMessage:
            {
                /* Handle window close button */
                Atom wm_delete_window = XInternAtom(ctx->display, "WM_DELETE_WINDOW", False);
                if ((Atom)event.xclient.data.l[0] == wm_delete_window)
                {
                    ctx->running = 0;
                }
                break;
            }
            case DestroyNotify:
                ctx->running = 0;
                break;
        }
    }
}

/* Render frame to X11 window with scaling */
static void x11_render(x11_context_t *ctx, uint32_t *framebuffer)
{
    /* Nearest-neighbor upscaling */
    for (int y = 0; y < SCREEN_HEIGHT; y++)
    {
        for (int x = 0; x < SCREEN_WIDTH; x++)
        {
            uint32_t pixel = framebuffer[y * SCREEN_WIDTH + x];
            
            /* Write scaled pixel block */
            for (int sy = 0; sy < SCALE; sy++)
            {
                for (int sx = 0; sx < SCALE; sx++)
                {
                    int dest_x = x * SCALE + sx;
                    int dest_y = y * SCALE + sy;
                    ctx->scaled_buffer[dest_y * (SCREEN_WIDTH * SCALE) + dest_x] = pixel;
                }
            }
        }
    }
    
    /* Draw scaled image to window */
    XPutImage(
        ctx->display,
        ctx->window,
        ctx->gc,
        ctx->ximage,
        0, 0,                          /* source x, y */
        0, 0,                          /* dest x, y */
        SCREEN_WIDTH * SCALE,          /* scaled width */
        SCREEN_HEIGHT * SCALE          /* scaled height */
    );
    
    XFlush(ctx->display);
}

/* Cleanup X11 */
static void x11_cleanup(x11_context_t *ctx)
{
    if (ctx->ximage)
    {
        ctx->ximage->data = NULL;  /* Don't let XDestroyImage free our buffer */
        XDestroyImage(ctx->ximage);
    }
    if (ctx->scaled_buffer)
        free(ctx->scaled_buffer);
    if (ctx->gc)
        XFreeGC(ctx->display, ctx->gc);
    if (ctx->window)
        XDestroyWindow(ctx->display, ctx->window);
    if (ctx->display)
        XCloseDisplay(ctx->display);
}

/* NES controller input - mapped to X11 keyboard */
static x11_context_t *g_x11_ctx = NULL;

uint8_t nes_key_state(uint8_t b)
{
    if (!g_x11_ctx)
        return 0;
    
    switch (b)
    {
        case 0: // On / Off
            return 1;
        case 1: // A
            return g_x11_ctx->keys[KEY_A];
        case 2: // B
            return g_x11_ctx->keys[KEY_S];
        case 3: // SELECT
            return g_x11_ctx->keys[KEY_C];
        case 4: // START
            return g_x11_ctx->keys[KEY_RETURN];
        case 5: // UP
            return g_x11_ctx->keys[KEY_UP];
        case 6: // DOWN
            return g_x11_ctx->keys[KEY_DOWN];
        case 7: // LEFT
            return g_x11_ctx->keys[KEY_LEFT];
        case 8: // RIGHT
            return g_x11_ctx->keys[KEY_RIGHT];
        default:
            return 1;
    }
}

uint8_t nes_key_state_ctrl2(uint8_t b)
{
    switch (b)
    {
        case 0: // On / Off
            return 1;
        case 1: // A
            return 0;
        case 2: // B
            return 0;
        case 3: // SELECT
            return 0;
        case 4: // START
            return 0;
        case 5: // UP
            return 0;
        case 6: // DOWN
            return 0;
        case 7: // LEFT
            return 0;
        case 8: // RIGHT
            return 0;
        default:
            return 1;
    }
}

int main(int argc, char *argv[])
{
    /* NES part */
    static nes_ppu_t nes_ppu;
    static nes_cpu_t nes_cpu;
    static nes_cartridge_t nes_cart;
    static nes_mem_td nes_memory = { 0 };

    uint32_t cpu_clocks = 0;
    uint32_t ppu_clocks = 0;
    uint32_t ppu_rest_clocks = 0;
    uint32_t ppu_clock_index = 0;
    uint8_t ppu_status = 0;

    if(argc != 2)
    {
        die("Please specify rom file\n");
    }

    /* init cartridge */
    nes_cart_init(&nes_cart, &nes_memory);

    /* load rom */
    if(nes_cart_load_rom(&nes_cart, argv[1]) != 0)
    {
        die("ROM does not exist\n");
    }

    /* init cpu */
    nes_cpu_init(&nes_cpu, &nes_memory);
    nes_cpu_reset(&nes_cpu);

    /* init ppu */
    nes_ppu_init(&nes_ppu, &nes_memory);

    /* X11 Initialization */
    static x11_context_t x11_ctx;
    x11_init(&x11_ctx);
    g_x11_ctx = &x11_ctx;

    unsigned int lastTime = 0, currentTime;

    while (x11_ctx.running)
    {
        /* Process X11 events */
        x11_process_events(&x11_ctx);

        /* NES core loop */
        for(;;)
        {
            cpu_clocks = 0;
            if(!ppu_rest_clocks)
            {
                if(ppu_status & PPU_STATUS_NMI)
                    cpu_clocks += nes_cpu_nmi(&nes_cpu);
                cpu_clocks += nes_cpu_run(&nes_cpu);
            }

            /* the ppu runs at a 3 times higher clock rate than the cpu
            so we need to give the ppu some clocks here to catchup */
            ppu_clocks = (cpu_clocks*3) + ppu_rest_clocks;
            ppu_status = 0;
            for(ppu_clock_index=0;ppu_clock_index<ppu_clocks;ppu_clock_index++)
            {
                ppu_status |= nes_ppu_run(&nes_ppu, nes_cpu.num_cycles);
                if(ppu_status & PPU_STATUS_FRAME_READY) break;
                else ppu_rest_clocks = 0;
            }

            ppu_rest_clocks = (ppu_clocks - ppu_clock_index);

            nes_ppu_dump_regs(&nes_ppu);

            if(ppu_status & PPU_STATUS_FRAME_READY) break;
        }

        /* Render frame */
        x11_render(&x11_ctx, (uint32_t *)nes_ppu.screen_bitmap);

        /* 60 FPS framerate limit */
        while ((currentTime = get_time_ms()) < (lastTime + FPS_UPDATE_TIME_MS));
        lastTime = currentTime;
    }

    /* Cleanup */
    x11_cleanup(&x11_ctx);

    return 0;
}
