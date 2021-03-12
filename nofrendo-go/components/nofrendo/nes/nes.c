/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of version 2 of the GNU Library General
** Public License as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Library General Public License for more details.  To obtain a
** copy of the GNU Library General Public License, write to the Free
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
**
**
** nes.c
**
** NES hardware related routines
** $Id: nes.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/

#include <nofrendo.h>
#include <string.h>
#include "input.h"
#include "nes.h"

static nes_t nes;

nes_t *nes_getptr(void)
{
    return &nes;
}

/* Emulate one frame */
INLINE void renderframe()
{
    int elapsed_cycles;
    mapintf_t *mapintf = nes.mmc->intf;

    while (nes.scanline < nes.scanlines_per_frame)
    {
        nes.cycles += nes.cycles_per_line;

        ppu_scanline(nes.vidbuf, nes.scanline, nes.drawframe);

        if (nes.scanline == 241)
        {
            /* 7-9 cycle delay between when VINT flag goes up and NMI is taken */
            elapsed_cycles = nes6502_execute(7);
            nes.cycles -= elapsed_cycles;

            if (nes.ppu->ctrl0 & PPU_CTRL0F_NMI)
                nes6502_nmi();

            if (mapintf->vblank)
                mapintf->vblank();
        }

        if (mapintf->hblank)
            mapintf->hblank(nes.scanline);

        elapsed_cycles = nes6502_execute(nes.cycles);
        apu_fc_advance(elapsed_cycles);
        nes.cycles -= elapsed_cycles;

        ppu_endscanline();
        nes.scanline++;
    }

    nes.scanline = 0;
}

/* main emulation loop */
void nes_emulate(void)
{
    // Discard the garbage frames
    renderframe();
    renderframe();

    osd_loadstate();

    while (false == nes.poweroff)
    {
        osd_getinput();
        renderframe();

        if (nes.drawframe)
        {
            osd_blitscreen(nes.vidbuf);
            nes.vidbuf = nes.framebuffers[nes.vidbuf == nes.framebuffers[0]];
        }

        apu_emulate();

        osd_vsync();
    }
}

void nes_poweroff(void)
{
    nes.poweroff = true;
}

void nes_togglepause(void)
{
    nes.pause ^= true;
}

void nes_setcompathacks(void)
{
    // Hack to fix many MMC3 games with status bar vertical alignment issues
    // The issue is that the CPU and PPU aren't running in sync
    // if (nes.region == NES_NTSC && nes.cart->mapper_number == 4)
    if (nes.cart->checksum == 0xD8578BFD || // Zen Intergalactic
        nes.cart->checksum == 0x2E6301ED || // Super Mario Bros 3
        nes.cart->checksum == 0x5ED6F221 || // Kirby's Adventure
        nes.cart->checksum == 0xD273B409)   // Power Blade 2
    {
        nes.cycles_per_line += 2.5;
        MESSAGE_INFO("NES: Enabled MMC3 Timing Hack\n");
    }
}

/* insert a cart into the NES */
bool nes_insertcart(const char *filename)
{
    /* rom file */
    nes.cart = rom_loadfile(filename);
    if (NULL == nes.cart)
        goto _fail;

    /* mapper */
    nes.mmc = mmc_init(nes.cart);
    if (NULL == nes.mmc)
        goto _fail;

    /* if there's VRAM, let the PPU know */
    nes.ppu->vram_present = (NULL != nes.cart->chr_ram); // FIX ME: This is always true?

    nes_setregion(nes.region);
    nes_setcompathacks();

    nes_reset(HARD_RESET);

    return true;

_fail:
    nes_shutdown();
    return false;
}

/* insert a disk into the FDS */
bool nes_insertdisk(const char *filename)
{

}

/* Reset NES hardware */
void nes_reset(reset_type_t reset_type)
{
    if (nes.cart->chr_ram)
    {
        memset(nes.cart->chr_ram, 0, 0x2000 * nes.cart->chr_ram_banks);
    }

    apu_reset();
    ppu_reset();
    mem_reset();
    mmc_reset();
    nes6502_reset();

    nes.vidbuf = nes.framebuffers[0];
    nes.scanline = 241;
    nes.cycles = 0;

    MESSAGE_INFO("NES: System reset (%s)\n", (SOFT_RESET == reset_type) ? "soft" : "hard");
}

/* Shutdown NES */
void nes_shutdown(void)
{
    mmc_shutdown();
    mem_shutdown();
    ppu_shutdown();
    apu_shutdown();
    nes6502_shutdown();
    rom_free();
    free(nes.framebuffers[0]);
    free(nes.framebuffers[1]);
}

/* Setup region-dependant timings */
void nes_setregion(region_t region)
{
    // https://wiki.nesdev.com/w/index.php/Cycle_reference_chart
    if (region == NES_PAL)
    {
        nes.region = NES_PAL;
        nes.refresh_rate = NES_REFRESH_RATE_PAL;
        nes.scanlines_per_frame = NES_SCANLINES_PAL;
        nes.overscan = 0;
        nes.cycles_per_line = 341.f * 5 / 16;
        MESSAGE_INFO("NES: System region: PAL\n");
    }
    else
    {
        nes.region = NES_NTSC;
        nes.refresh_rate = NES_REFRESH_RATE_NTSC;
        nes.scanlines_per_frame = NES_SCANLINES_NTSC;
        nes.overscan = 8;
        nes.cycles_per_line = 341.f * 4 / 12;
        MESSAGE_INFO("NES: System region: NTSC\n");
    }
}

region_t nes_getregion(void)
{
    return (nes.region == NES_PAL) ? NES_PAL : NES_NTSC;
}

/* Initialize NES CPU, hardware, etc. */
bool nes_init(region_t region, int sample_rate, bool stereo)
{
    memset(&nes, 0, sizeof(nes_t));

    nes_setregion(region);

    nes.autoframeskip = true;
    nes.poweroff = false;
    nes.pause = false;
    nes.drawframe = true;

    /* Framebuffers */
    nes.framebuffers[0] = rg_alloc(NES_SCREEN_PITCH * NES_SCREEN_HEIGHT, MEM_FAST);
    nes.framebuffers[1] = rg_alloc(NES_SCREEN_PITCH * NES_SCREEN_HEIGHT, MEM_FAST);
    if (NULL == nes.framebuffers[0] || NULL == nes.framebuffers[1])
        goto _fail;

    /* memory */
    nes.mem = mem_create();
    if (NULL == nes.mem)
        goto _fail;

    /* cpu */
    nes.cpu = nes6502_init(nes.mem);
    if (NULL == nes.cpu)
        goto _fail;

    /* ppu */
    nes.ppu = ppu_init();
    if (NULL == nes.ppu)
        goto _fail;

    /* apu */
    nes.apu = apu_init(sample_rate, stereo);
    if (NULL == nes.apu)
        goto _fail;

    return true;

_fail:
    nes_shutdown();
    return false;
}
