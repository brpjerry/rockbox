/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2006 by Barry Wardell
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include "cpu.h"
#include "lcd.h"
#include "kernel.h"
#include "system.h"

/** Initialized in lcd_init_device() **/
/* Is the power turned on? */
static bool power_on;
/* Is the display turned on? */
static bool display_on;
/* Amount of vertical offset. Used for flip offset correction/detection. */
static int y_offset = 4;
static int x_offset = 16;
/* Reverse flag. Must be remembered when display is turned off. */
static unsigned short disp_control_rev;
/* Contrast setting << 8 */
static int lcd_contrast;

/* Forward declarations */
#if defined(HAVE_LCD_ENABLE) || defined(HAVE_LCD_SLEEP)
static void lcd_display_off(void);
#endif

/* register defines for the Renesas HD66773R */
#define R_START_OSC             0x00
#define R_DEVICE_CODE_READ      0x00
#define R_DRV_OUTPUT_CONTROL    0x01
#define R_DRV_AC_CONTROL        0x02
#define R_POWER_CONTROL1        0x03
#define R_POWER_CONTROL2        0x04
#define R_ENTRY_MODE            0x05
#define R_COMPARE_REG           0x06
#define R_DISP_CONTROL          0x07
#define R_FRAME_CYCLE_CONTROL   0x0b
#define R_POWER_CONTROL3        0x0c
#define R_POWER_CONTROL4        0x0d
#define R_POWER_CONTROL5        0x0e
#define R_GATE_SCAN_START_POS   0x0f
#define R_VERT_SCROLL_CONTROL   0x11
#define R_1ST_SCR_DRV_POS       0x14
#define R_2ND_SCR_DRV_POS       0x15
#define R_HORIZ_RAM_ADDR_POS    0x16
#define R_VERT_RAM_ADDR_POS     0x17
#define R_RAM_WRITE_DATA_MASK   0x20
#define R_RAM_ADDR_SET          0x21
#define R_WRITE_DATA_2_GRAM     0x22
#define R_RAM_READ_DATA         0x22
#define R_GAMMA_FINE_ADJ_POS1   0x30
#define R_GAMMA_FINE_ADJ_POS2   0x31
#define R_GAMMA_FINE_ADJ_POS3   0x32
#define R_GAMMA_GRAD_ADJ_POS    0x33
#define R_GAMMA_FINE_ADJ_NEG1   0x34
#define R_GAMMA_FINE_ADJ_NEG2   0x35
#define R_GAMMA_FINE_ADJ_NEG3   0x36
#define R_GAMMA_GRAD_ADJ_NEG    0x37
#define R_GAMMA_AMP_ADJ_POS     0x3a
#define R_GAMMA_AMP_ADJ_NEG     0x3b

#define R_DRV_OUTPUT_CONTROL_NORMAL     (1<<9|1<<4|1<<2|1<<0)
#define R_DRV_OUTPUT_CONTROL_FLIPPED    (1<<11|1<<8|1<<5|1<<4|1<<3)
static inline void lcd_wait_write(void)
{
    while (LCD2_PORT & LCD2_BUSY_MASK);
}

/* Send command */
static inline void lcd_send_cmd(unsigned cmd)
{
    lcd_wait_write();
    LCD2_PORT = LCD2_CMD_MASK;  /* Send MSB first (should always be 0) */
    lcd_wait_write();
    LCD2_PORT = LCD2_CMD_MASK | cmd;
}

/* Send 16-bit data */
static inline void lcd_send_data(unsigned data)
{
    lcd_wait_write();
    LCD2_PORT = (data >> 8) | LCD2_DATA_MASK;   /* Send MSB first */
    lcd_wait_write();
    LCD2_PORT = (data & 0xff) | LCD2_DATA_MASK;
}

/* Send 16-bit data byte-swapped. Only needed until we can use block transfer. */
static inline void lcd_send_data_swapped(unsigned v)
{
    lcd_wait_write();
    LCD2_PORT = LCD2_DATA_MASK | (v & 0xff);  /* Send LSB first */
    LCD2_PORT = LCD2_DATA_MASK | (v >> 8);    
}

/* Write value to register */
static void lcd_write_reg(int reg, int val)
{
    lcd_send_cmd(reg);
    lcd_send_data(val);
}

/*** hardware configuration ***/

int lcd_default_contrast(void)
{
    return DEFAULT_CONTRAST_SETTING;
}

void lcd_set_contrast(int val)
{
    /* Clamp val in range 0-14, 16-30 */
    if (val < 1)
        val = 0;
    else if (val <= 15)
        --val;
    else if (val > 30)
        val = 30;

    lcd_contrast = val << 8;

    if (!power_on)
             return;

    /* VCOMG=1, VDV4-0=xxxxx, VCM4-0=11000 */
    lcd_write_reg(R_POWER_CONTROL5, 0x2018 | lcd_contrast);
}

void lcd_set_invert_display(bool yesno)
{
    if (yesno == (disp_control_rev == 0x0000))
        return;

    disp_control_rev = yesno ? 0x0000 : 0x0004;

    if (!display_on)
        return;

    /* PT1-0=00, VLE2-1=00, SPT=0, GON=1, DTE=1, REV=x, D1-0=11 */
    lcd_write_reg(R_DISP_CONTROL, 0x0033 | disp_control_rev);
}

/* turn the display upside down (call lcd_update() afterwards) */
void lcd_set_flip(bool flip)
{
    int r_drv_output_control;
    if (flip == (y_offset != 4))
        return;
    /* The LCD controller is 132x160 while the LCD itself is 128x160, so we need
     * to shift the origin by 4 when we flip the LCD
     *
     * the higher bits are the key bits for flipping */
    if (flip)
    {
        x_offset = 0; y_offset = 0;
        r_drv_output_control = R_DRV_OUTPUT_CONTROL_FLIPPED;
    }
    else
    {
        x_offset = 16; y_offset = 4;
        r_drv_output_control = R_DRV_OUTPUT_CONTROL_NORMAL;
    }

    if (!power_on)
        return;

    lcd_write_reg(R_1ST_SCR_DRV_POS, ( (LCD_WIDTH + x_offset - 1) << 8) | x_offset);
    lcd_write_reg(R_HORIZ_RAM_ADDR_POS, (LCD_HEIGHT - 1) << 8 | 0);
    lcd_write_reg(R_VERT_RAM_ADDR_POS, ((LCD_WIDTH + x_offset- 1) << 8) | x_offset);
    lcd_write_reg(R_DRV_OUTPUT_CONTROL, r_drv_output_control );
    lcd_write_reg(R_ENTRY_MODE, 0x1028);
}

/* Reset the LCD prior to booting the OF */
#ifdef BOOTLOADER
void lcd_reset(void)
{
    lcd_write_reg(R_DRV_OUTPUT_CONTROL, 0x0315);
    lcd_write_reg(R_ENTRY_MODE, 0x1028);
    lcd_write_reg(R_HORIZ_RAM_ADDR_POS, 0x7f00);
    lcd_write_reg(R_VERT_RAM_ADDR_POS, 0xaf10);
    lcd_write_reg(R_RAM_ADDR_SET, 0x0000);
}
#endif

/* LCD init */
void lcd_init_device(void)
{
#if 0
    /* This is the OF LCD init */
    int i;

    DEV_EN2 |= 0x2000;
    DEV_INIT1 |= 0xfc000000;
    CLCD_CLOCK_SRC &= ~0xf;
    DEV_INIT2 &= ~0x400;

    GPIOF_ENABLE |= 0x4;
    GPIOF_OUTPUT_EN |= 0x4;
    GPIOF_OUTPUT_VAL &= ~0x4;
    udelay(300000);
    GPIOF_OUTPUT_VAL |= 0x4;

    outl(0x220, 0x70008a00);
    outl(0x1f00, 0x70008a04);
    LCD2_BLOCK_CTRL = 0x10008080;
    LCD2_BLOCK_CONFIG = 0xf00000;
    udelay(100000);

    for (i = 0; i < 8; i++)
    {
        lcd_send_data(0);
        udelay(1);
    }

    lcd_write_reg(R_START_OSC, 0x0001);
    udelay(20000);
    lcd_write_reg(R_DRV_OUTPUT_CONTROL, 0x0315);
    lcd_write_reg(R_DRV_AC_CONTROL, 0x0700);
    lcd_write_reg(R_ENTRY_MODE, 0x1028);
    lcd_write_reg(R_COMPARE_REG, 0x0000);
    lcd_write_reg(R_FRAME_CYCLE_CONTROL, 0x4008);
    lcd_write_reg(R_RAM_ADDR_SET, 0x0000);
    lcd_write_reg(R_GAMMA_FINE_ADJ_POS1, 0x0700);
    lcd_write_reg(R_GAMMA_FINE_ADJ_POS2, 0x0000);
    lcd_write_reg(R_GAMMA_FINE_ADJ_POS3, 0x0000);
    lcd_write_reg(R_GAMMA_GRAD_ADJ_POS, 0x0100);
    lcd_write_reg(R_GAMMA_FINE_ADJ_NEG1, 0x0707);
    lcd_write_reg(R_GAMMA_FINE_ADJ_NEG2, 0x0007);
    lcd_write_reg(R_GAMMA_FINE_ADJ_NEG3, 0x0700);
    lcd_write_reg(R_GAMMA_GRAD_ADJ_NEG, 0x0001);
    lcd_write_reg(R_GATE_SCAN_START_POS, 0x0000);
    lcd_write_reg(R_VERT_SCROLL_CONTROL, 0x0000);
    lcd_write_reg(R_1ST_SCR_DRV_POS, ( (LCD_WIDTH + 16 - 1) << 8) | 16);
    lcd_write_reg(R_2ND_SCR_DRV_POS, 0x0000);
    lcd_write_reg(R_HORIZ_RAM_ADDR_POS, (LCD_HEIGHT - 1) << 8 | 0);
    lcd_write_reg(R_VERT_RAM_ADDR_POS, ((LCD_WIDTH + 16- 1) << 8) | 16);
    lcd_write_reg(R_GAMMA_AMP_ADJ_POS, 0x1600);
    lcd_write_reg(R_GAMMA_AMP_ADJ_NEG, 0x0006);
    lcd_write_reg(R_DISP_CONTROL, 0x0104);
    lcd_write_reg(R_POWER_CONTROL4, 0x0100);
    lcd_write_reg(R_POWER_CONTROL5, 0x0d18);
    lcd_write_reg(R_POWER_CONTROL3, 0x0000);
    udelay(50000);
    lcd_write_reg(R_POWER_CONTROL1, 0x0008);
    lcd_write_reg(R_POWER_CONTROL3, 0x0000);
    lcd_write_reg(R_POWER_CONTROL2, 0x0000);
    udelay(50000);
    lcd_write_reg(R_POWER_CONTROL5, 0x2d18);
    lcd_write_reg(R_POWER_CONTROL3, 0x0000);
    udelay(50000);
    lcd_write_reg(R_POWER_CONTROL4, 0x0110);
    //~ lcd_clear_display();
    lcd_write_reg(R_DISP_CONTROL, 0x0105);
    udelay(20000);
    lcd_write_reg(R_DISP_CONTROL, 0x0125);
    lcd_write_reg(R_DISP_CONTROL, 0x0127);
    udelay(20000);
    lcd_write_reg(R_DISP_CONTROL, 0x0137);
    lcd_send_cmd(R_WRITE_DATA_2_GRAM);

    GPIOF_ENABLE     |= 0x02;
    GPIOF_OUTPUT_EN  |= 0x02;
    GPIOF_OUTPUT_VAL |= 0x02;
#endif

#ifndef BOOTLOADER
    /* The OF won't boot if this is done in the bootloader - ideally we should 
       tweak the lcd controller speed settings but this will do for now */
    CLCD_CLOCK_SRC |= 0xc0000000; /* Set LCD interface clock to PLL */
#endif

    /* only these bits are needed from the OF init */
    lcd_write_reg(R_DRV_OUTPUT_CONTROL, R_DRV_OUTPUT_CONTROL_NORMAL);
    lcd_write_reg(R_ENTRY_MODE, 0x1028);

    power_on = true;
    display_on = true;
    disp_control_rev = 0x0004;
    lcd_contrast     = DEFAULT_CONTRAST_SETTING << 8;
}

/* THE SLEEP AND POWER FUNCTIONS HAVE NOT BEEN MODIFIED FOR THE YH-925 */

#ifdef HAVE_LCD_SLEEP
static void lcd_power_on(void)
{
    /* Be sure standby bit is clear. */
    /* BT2-0=000, DC2-0=000, AP2-0=000, SLP=0, STB=0 */
    lcd_write_reg(R_POWER_CONTROL1, 0x0000);

    /** Power ON Sequence **/
    /* Per datasheet Rev.1.10, Jun.21.2003, p. 99 */

    lcd_write_reg(R_START_OSC, 0x0001); /* Start Oscillation */
    /* 10ms or more for oscillation circuit to stabilize */
    sleep(HZ/50);
    /* Instruction (1) for power setting; VC2-0, VRH3-0, CAD,
       VRL3-0, VCM4-0, VDV4-0 */
    /* VC2-0=001 */
    lcd_write_reg(R_POWER_CONTROL3, 0x0001);
    /* VRL3-0=0100, PON=0, VRH3-0=0001 */
    lcd_write_reg(R_POWER_CONTROL4, 0x0401);
    /* CAD=1 */
    lcd_write_reg(R_POWER_CONTROL2, 0x8000);
    /* VCOMG=0, VDV4-0=xxxxx (19), VCM4-0=11000 */
    lcd_write_reg(R_POWER_CONTROL5, 0x0018 | lcd_contrast);
    /* Instruction (2) for power setting; BT2-0, DC2-0, AP2-0 */
    /* BT2-0=000, DC2-0=001, AP2-0=011, SLP=0, STB=0 */
    lcd_write_reg(R_POWER_CONTROL1, 0x002c);
    /* Instruction (3) for power setting; VCOMG = "1" */
    /* VCOMG=1, VDV4-0=xxxxx (19), VCM4-0=11000 */
    lcd_write_reg(R_POWER_CONTROL5, 0x2018 | lcd_contrast);

    /* 40ms or more; time for step-up circuits 1,2 to stabilize */
    sleep(HZ/25);

    /* Instruction (4) for power setting; PON = "1" */
    /* VRL3-0=0100, PON=1, VRH3-0=0001 */
    lcd_write_reg(R_POWER_CONTROL4, 0x0411);

    /* 40ms or more; time for step-up circuit 4 to stabilize */
    sleep(HZ/25);

    /* Instructions for other mode settings (in register order). */
    /* SM=0, GS=x, SS=0, NL4-0=10011 (G1-G160)*/
    lcd_write_reg(R_DRV_OUTPUT_CONTROL, y_offset ? 0x0013 : 0x0113); /* different to X5 */
    /* FLD1-0=01 (1 field), B/C=1, EOR=1 (C-pat), NW5-0=000000 (1 row) */
    lcd_write_reg(R_DRV_AC_CONTROL, 0x0700);
    /* DIT=0, BGR=1, HWM=0, I/D1-0=10, AM=1, LG2-0=000 */
    lcd_write_reg(R_ENTRY_MODE, 0x1028); /* different to X5 */
    /* CP15-0=0000000000000000 */
    lcd_write_reg(R_COMPARE_REG, 0x0000);
    /* NO1-0=01, SDT1-0=00, EQ1-0=00, DIV1-0=00, RTN3-00000 */
    lcd_write_reg(R_FRAME_CYCLE_CONTROL, 0x4000);
    /* SCN4-0=000x0 (G1/G160) */
/*    lcd_write_reg(R_GATE_SCAN_START_POS, y_offset ? 0x0000 : 0x0002); */
    /* VL7-0=0x00 */
    lcd_write_reg(R_VERT_SCROLL_CONTROL, 0x0000);
    /* SE17-10(End)=0x9f (159), SS17-10(Start)=0x00 */
    lcd_write_reg(R_1ST_SCR_DRV_POS, 0x9f00);
    /* SE27-20(End)=0x5c (92), SS27-20(Start)=0x00 */
    lcd_write_reg(R_2ND_SCR_DRV_POS, 0x5c00);
    /* HEA7-0=7f, HSA7-0=00 */
    lcd_write_reg(R_HORIZ_RAM_ADDR_POS, 0x7f00);
    /* PKP12-10=0x0, PKP02-00=0x0 */
    lcd_write_reg(R_GAMMA_FINE_ADJ_POS1, 0x0003);
    /* PKP32-30=0x4, PKP22-20=0x0 */
    lcd_write_reg(R_GAMMA_FINE_ADJ_POS2, 0x0400);
    /* PKP52-50=0x4, PKP42-40=0x7 */
    lcd_write_reg(R_GAMMA_FINE_ADJ_POS3, 0x0407);
    /* PRP12-10=0x3, PRP02-00=0x5 */
    lcd_write_reg(R_GAMMA_GRAD_ADJ_POS, 0x0305);
    /* PKN12-10=0x0, PKN02-00=0x3 */
    lcd_write_reg(R_GAMMA_FINE_ADJ_NEG1, 0x0003);
    /* PKN32-30=0x7, PKN22-20=0x4 */
    lcd_write_reg(R_GAMMA_FINE_ADJ_NEG2, 0x0704);
    /* PKN52-50=0x4, PRN42-40=0x7 */
    lcd_write_reg(R_GAMMA_FINE_ADJ_NEG3, 0x0407);
    /* PRN12-10=0x5, PRN02-00=0x3 */
    lcd_write_reg(R_GAMMA_GRAD_ADJ_NEG, 0x0503);
    /* VRP14-10=0x14, VRP03-00=0x09 */
    lcd_write_reg(R_GAMMA_AMP_ADJ_POS, 0x1409);
    /* VRN14-00=0x06, VRN03-00=0x02 */
    lcd_write_reg(R_GAMMA_AMP_ADJ_NEG, 0x0602);

    /* 100ms or more; time for step-up circuits to stabilize */
    sleep(HZ/10);

    power_on = true;
}

static void lcd_power_off(void)
{
    /* Display must be off first */
    if (display_on)
        lcd_display_off();

    power_on = false;

    /** Power OFF sequence **/
    /* Per datasheet Rev.1.10, Jun.21.2003, p. 99 */

    /* Step-up1 halt setting bit */
    /* BT2-0=110, DC2-0=001, AP2-0=011, SLP=0, STB=0 */
    lcd_write_reg(R_POWER_CONTROL1, 0x062c);
    /* Step-up3,4 halt setting bit */
    /* VRL3-0=0100, PON=0, VRH3-0=0001 */
    lcd_write_reg(R_POWER_CONTROL4, 0x0401);
    /* VCOMG=0, VDV4-0=10011, VCM4-0=11000 */
    lcd_write_reg(R_POWER_CONTROL5, 0x0018 | lcd_contrast);

    /* Wait 100ms or more */
    sleep(HZ/10);

    /* Step-up2,amp halt setting bit */
    /* BT2-0=000, DC2-0=000, AP2-0=000, SLP=0, STB=0 */
    lcd_write_reg(R_POWER_CONTROL1, 0x0000);
}

void lcd_sleep(void)
{
    if (power_on)
        lcd_power_off();

    /* Set standby mode */
    /* BT2-0=000, DC2-0=000, AP2-0=000, SLP=0, STB=1 */
    lcd_write_reg(R_POWER_CONTROL1, 0x0001);
}
#endif

#if defined(HAVE_LCD_ENABLE) || defined(HAVE_LCD_SLEEP)
static void lcd_display_off(void)
{
    display_on = false;

    /** Display OFF sequence **/
    /* Per datasheet Rev.1.10, Jun.21.2003, p. 97 */

    /* EQ1-0=00 already */

    /* PT1-0=00, VLE2-1=00, SPT=0, GON=1, DTE=1, REV=x, D1-0=10 */
    lcd_write_reg(R_DISP_CONTROL, 0x0032 | disp_control_rev);

    sleep(HZ/25); /* Wait 2 frames or more */

    /* PT1-0=00, VLE2-1=00, SPT=0, GON=1, DTE=0, REV=x, D1-0=10 */
    lcd_write_reg(R_DISP_CONTROL, 0x0022 | disp_control_rev);

    sleep(HZ/25); /* Wait 2 frames or more */

    /* PT1-0=00, VLE2-1=00, SPT=0, GON=0, DTE=0, REV=0, D1-0=00 */
    lcd_write_reg(R_DISP_CONTROL, 0x0000);
}
#endif

#if defined(HAVE_LCD_ENABLE)
static void lcd_display_on(void)
{
    /* Be sure power is on first */
    if (!power_on)
        lcd_power_on();

    /** Display ON Sequence **/
    /* Per datasheet Rev.1.10, Jun.21.2003, p. 97 */

    /* PT1-0=00, VLE2-1=00, SPT=0, GON=0, DTE=0, REV=0, D1-0=01 */
    lcd_write_reg(R_DISP_CONTROL, 0x0001);

    sleep(HZ/25); /* Wait 2 frames or more */

    /* PT1-0=00, VLE2-1=00, SPT=0, GON=1, DTE=0, REV=x, D1-0=01 */
    lcd_write_reg(R_DISP_CONTROL, 0x0021 | disp_control_rev);
    /* PT1-0=00, VLE2-1=00, SPT=0, GON=1, DTE=0, REV=x, D1-0=11 */
    lcd_write_reg(R_DISP_CONTROL, 0x0023 | disp_control_rev);

    sleep(HZ/25); /* Wait 2 frames or more */

    /* PT1-0=00, VLE2-1=00, SPT=0, GON=1, DTE=1, REV=x, D1-0=11 */
    lcd_write_reg(R_DISP_CONTROL, 0x0033 | disp_control_rev);

    display_on = true;
}

void lcd_enable(bool on)
{
    if (on == display_on)
        return;

    if (on)
    {
        lcd_display_on();
        /* Probably out of sync and we don't wanna pepper the code with
           lcd_update() calls for this. */
        lcd_update();
        send_event(LCD_EVENT_ACTIVATION, NULL);
    }
    else
    {
        lcd_display_off();
    }
}
#endif

#if defined(HAVE_LCD_ENABLE) || defined(HAVE_LCD_SLEEP)
bool lcd_active(void)
{
    return display_on;
}
#endif

/*** update functions ***/

/* Update a fraction of the display. */
void lcd_update_rect(int x0, int y0, int width, int height)
{
    int x1, y1;
    int lcd_x0, lcd_x1, lcd_y0, lcd_y1;
    unsigned short *addr;

    if (!display_on)
        return;

    /* calculate the drawing region */
    y1 = (y0 + height) - 1;     /* max vert */
    x1 = (x0 + width) - 1;      /* max horiz */

    if(x1 >= LCD_WIDTH)
        x1 = LCD_WIDTH - 1;
    if (x1 <= 0)
        return; /* nothing left to do, 0 is harmful to lcd_write_data() */
    if(y1 >= LCD_HEIGHT)
        y1 = LCD_HEIGHT-1;
        
    /* The LCD is actually 128x160 rotated 90 degrees */
    lcd_x0 = (LCD_HEIGHT - 1) - y1 + y_offset;
    lcd_x1 = (LCD_HEIGHT - 1) - y0 + y_offset;
    lcd_y0 = x0 + x_offset;
    lcd_y1 = x1 + x_offset;

    /* set the drawing window */
    lcd_write_reg(R_HORIZ_RAM_ADDR_POS, (lcd_x1 << 8) | lcd_x0);
    lcd_write_reg(R_VERT_RAM_ADDR_POS,  (lcd_y1 << 8) | lcd_y0);

    /* set the starting position */
    lcd_write_reg(R_RAM_ADDR_SET, (lcd_y0 << 8) | lcd_x1);

    /* start drawing */
    lcd_send_cmd(R_WRITE_DATA_2_GRAM);

    addr = (unsigned short*)FBADDR(x0,y0);

    int c, r;

    /* for each row */
    for (r = 0; r < height; r++) {
        /* for each column */
        for (c = 0; c < width; c++) {
            /* output 1 pixel */
            lcd_send_data(*addr++);
        }

        addr += (LCD_WIDTH - width);
    }
}

/* Update the display.
   This must be called after all other LCD functions that change the display. */
void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}
