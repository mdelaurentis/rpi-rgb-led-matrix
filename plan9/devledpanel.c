
#include "u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

// Registers
enum {
  GPFSEL0 = 0x7e200000,
  GPFSEL1 = 0x7e200004,
  GPSET0  = 0x7e20001c,
  GPCLR0  = 0x7e200028,

  // PWM registers. The actual addresses aren't listed in the
  // datasheet, but the offsets are.
  PWMCTL    = 0x7e20c000,
  PWMSTA    = 0x7e20c004,
  PWMRNG1   = 0x7e20c010,
  PWMFIF1   = 0x7e20c018,

  // Clock management registers. The actual addresses aren't listed in the
  // datasheet, but the offsets are.
  CM_PWMCTL = 0x7e1010a0,
  CM_PWMDIV = 0x7e1010a4
};

// Clock register bits
enum {
  CLK_PASSWD = 0x5a << 24,
  CLK_KILL = 1 << 5,
  CLK_ENAB = 1 << 4,
};

// PWM CTL bits
enum {
  PWEN1 = 1 << 0,
  POLA1 = 1 << 4,
  USEF1 = 1 << 5,
  CLRF1 = 1 << 6
};

// hzeller's library uses a CIE1931 algorithm to map the input color values to different values. This mapping is derived from his.
char cie1931_lookup[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 15, 16, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 28, 28, 29, 29, 30, 31, 31, 32, 33, 33, 34, 35, 35, 36, 37, 37, 38, 39, 40, 40, 41, 42, 43, 44, 44, 45, 46, 47, 48, 49, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 75, 76, 77, 78, 79, 80, 82, 83, 84, 85, 87, 88, 89, 90, 92, 93, 94, 96, 97, 99, 100, 101, 103, 104, 106, 107, 108, 110, 111, 113, 114, 116, 118, 119, 121, 122, 124, 125, 127, 129, 130, 132, 134, 135, 137, 139, 141, 142, 144, 146, 148, 149, 151, 153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 174, 176, 178, 180, 182, 185, 187, 189, 191, 193, 195, 197, 200, 202, 204, 206, 208, 211, 213, 215, 218, 220, 222, 225, 227, 230, 232, 234, 237, 239, 242, 244, 247, 249, 252, 255};

// The buffer where we store the color values. We set the LEDs by pulsing in one "bit plane" at a time. We have 16 rows, 8 bit planes, and 3 color channels. There are 32 columns, so we can store the on/off value for column x, row y, bit plane bp, color channel c as the xth bit of color_buffer[y][bp][c].
uint color_buffer[16][8][3];
static int initialized = 0;

// Clear all the pixels, just setting all the slots in our buffer to 0.
void ledpanel_clear(void) {
  int row, bp, color;
  for (row = 0; row < 16; row++)
    for (bp = 0; bp < 8; bp++)
      for (color = 0; color < 3; color++)
        color_buffer[row][bp][color] = 0;
}

// Fill the whole display with one color. I added this
// function because I wrote a demo program that showed
// pulses of solid colors, and I found that writing the "setpixel x y color"
// command introduced more latency than I wanted. Having a 
// kernel-level function to set all the pixels seemed to make the
// pulsing smoother. See the "demo_pulse" function in demo.c.
void ledpanel_fill(uint color) {
  int y;
  uchar bp, c;
  uchar r = color >> 16;
  uchar g = color >> 8;
  uchar b = color;

  // Use the CIE1931 mapping
  char rgb[] = { cie1931_lookup[r],
                 cie1931_lookup[g],
                 cie1931_lookup[b] };
  
  for (bp = 0; bp < 8; bp++) {
    for (c = 0; c < 3; c++) {
      for (y = 0; y < 16; y++) {
        // If this color channel is "on" in this bit plane,
        // set all columns to 1, otherwise set all columns to 0.
        if (rgb[c] & 1) {
          color_buffer[y][bp][c] = 0xffffffff;
        }
        else {
          color_buffer[y][bp][c] = 0x00000000;
        }
      }
      // Shift down one to get the next bit plane.
       rgb[c] >>= 1;
    }
  }
}

// Initialize the panel.
void ledpanel_init(void) {
  
  // Alias all the registers we'll use in this function here
  // so we don't have to cast every time we access them.
  uint
      *gpfsel = (uint*)GPFSEL0,
      *cm_pwmctl = (uint*)CM_PWMCTL,
      *cm_pwmdiv = (uint*)CM_PWMDIV,
     *reg;
  
  uint fld, i, b;
  
  // Set the function to 1 for all of the output pins we'll use
  int output_bits[] = {
    4,  // strobe
    17, // clock
    18, // output enable

    22, 23, 24, // a, b, c

    7,  // b1
    8,  // r2
    9,  // g2
    10, // b2
    11, // r1
    27 // g1
  };
  for (i = 0; i < 12; i++) {
    b = output_bits[i];
    reg = gpfsel + b / 10;
    fld = (b % 10) * 3;
    *reg &= ~(7<< fld);
    *reg |=  (1<< fld);       
  }

  // Set the FSEL18 field of register GPFSEL1 to 010 (GPIO Pin 18
  // takes alternate function 5).
  reg = gpfsel + 18 / 10;
  fld = (18 % 10) * 3;
  *reg &= ~(7<< fld);
  *reg |=  (2<< fld);

  // Kill the PWM clock, then set the source as 500 MHz PLLD, then set
  // the divider, then enable it again.
  *cm_pwmctl = CLK_PASSWD | CLK_KILL;
  *cm_pwmctl = CLK_PASSWD | 6;
  *cm_pwmdiv = CLK_PASSWD | ((130 / 4) << 12);
  *cm_pwmctl = CLK_PASSWD | CLK_ENAB | 6;
}

// Set the color for one pixel
void ledpanel_setpixel(uint x, uint y, uint color) {

  // bit plane, color channel
  uchar bp, c;

  // red is bits 16 - 23, green is 8 - 15, blue is 0 - 7
  uchar r = color >> 16;
  uchar g = color >> 8;
  uchar b = color;

  if (x > 31 || y > 15)
    return;

  // Map the colors with the CIE1931 lookup
  char rgb[] = { cie1931_lookup[r],
                 cie1931_lookup[g],
                 cie1931_lookup[b] };
  
  // The ith bit for color channel c for column x, row y is
  // color_buffer[y][i][c] & 1 << x.
  for (bp = 0; bp < 8; bp++) {
    for (c = 0; c < 3; c++) {
      color_buffer[y][bp][c] &= ~(1 << x);
      color_buffer[y][bp][c] |= (rgb[c] & 1) << x;
      rgb[c] >>= 1;
    }
  }
}

// Constantly refresh the panel
static void ledpanel_refresh(void *) {
  uint x, y1, y2, bp;
  
  uint color_pins = 1 << 7 | 1 << 8 | 1 << 9 | 1 << 10 | 1 << 11 | 1 << 27;

  uint *pwmctl  = (uint*) PWMCTL;
  uint *pwmsta  = (uint*) PWMSTA;
  uint *pwmrng1 = (uint*) PWMRNG1;
  uint *pwmfif1 = (uint*) PWMFIF1;
  uint *gpset0 = (uint*) GPSET0;
  uint *gpclr0 = (uint*) GPCLR0;
  
  while (1) {
  for (y1 = 0; y1 < 8; ++y1) {
    
    // Set row address (A, B, C). ABC are bits 22-24.
    *gpclr0 = 7 << 22;
    *gpset0 = y1 << 22;
    
    // Rows can't be switched very quickly without ghosting, so we do the
    // full PWM of one row before switching rows.
    for (bp = 0; bp < 8; ++bp) {
      
      y2 = y1 + 8;
      uint r1 = color_buffer[y1][bp][0];
      uint g1 = color_buffer[y1][bp][1];
      uint b1 = color_buffer[y1][bp][2];
      uint r2 = color_buffer[y2][bp][0];
      uint g2 = color_buffer[y2][bp][1];
      uint b2 = color_buffer[y2][bp][2];
      
      // While the output enable is still on, we can already clock in the next
      // data.
      for (x = 0; x < 32; ++x) {

        // Set the pins that should be on for this bit plane, and set the clock.
        *gpclr0 = color_pins | 1 << 17;
        *gpset0 = ((r1 & 1) << 11 | (r2 & 1) <<  8 |
                   (g1 & 1) << 27 | (g2 & 1) <<  9 |
                   (b1 & 1) <<  7 | (b2 & 1) << 10 |
                   1 << 17);
        r1 >>= 1;
        g1 >>= 1;
        b1 >>= 1;
        r2 >>= 1;
        g2 >>= 1;
        b2 >>= 1;
      }
      // Clear the clock and color
      *gpclr0 = color_pins | 1 << 17;
      
      // OE of the previous row-data must be finished before strobe.
      while (!(*pwmsta & 0x2));
      *pwmctl = USEF1 | POLA1 | CLRF1;      

      // Set and clear the strobe (bit 4)
      *gpset0 = 1 << 4;
      *gpclr0 = 1 << 4;

      // Now switch on for the sleep time necessary for that bit-plane.
      *pwmrng1 = 1 << (bp + 1);
      *pwmfif1 = 1 << (bp + 1);
      *pwmfif1 = 0;
      *pwmfif1 = 0;
      *pwmctl =  PWEN1 | POLA1 | USEF1;

    }
    while (!(*pwmsta & 0x2));
    *pwmctl = USEF1 | POLA1 | CLRF1;      
  }
  }
}

//
// The rest of this file is the part that provides the Dev structure.
// Much of the code below is mostly copied from devgpio.c and
// changed slightly.
//

enum{
	Qdir = 0,
	Qledpanel,
};

Dirtab ledpaneldir[]={
	".",	{Qdir, 0, QTDIR},	0,	0555,
	"ledpanel",	{Qledpanel, 0},	0,	0664,
};

enum {
	// commands
	CMinit,
	CMclear,
         CMfill,
	CMsetpixel
};

// We'll support four commands. init initializes, clear sets all pixels to 0,
// fill fils the display with a solid color, and setpixel sets one pixel.
static Cmdtab ledpanelcmd[] = {
	{CMinit, "init", 1},
	{CMclear, "clear", 1},
         {CMfill, "fill", 2},
	{CMsetpixel, "setpixel", 4}
};

static Chan*
ledpanelattach(char* spec)
{
	return devattach('L', spec);
}

static Walkqid*	 
ledpanelwalk(Chan* c, Chan *nc, char** name, int nname)
{
	return devwalk(c, nc, name, nname, ledpaneldir, nelem(ledpaneldir), devgen);
}

static int	 
ledpanelstat(Chan* c, uchar* dp, int n)
{
	return devstat(c, dp, n, ledpaneldir, nelem(ledpaneldir), devgen);
}

static Chan*
ledpanelopen(Chan* c, int omode)
{
	return devopen(c, omode, ledpaneldir, nelem(ledpaneldir), devgen);
}

static void	 
ledpanelclose(Chan*)
{
}

// We don't support any read operations.
static long	 
ledpanelread(Chan* c, void *buf, long n, vlong)
{
	USED(c);
	USED(n);
	USED(buf);
	return 0;
}

static long	 
ledpanelwrite(Chan* c, void *buf, long n, vlong)
{
	uint x, y, color;
	Cmdbuf *cb;
	Cmdtab *ct;

	USED(c);

	if(c->qid.type & QTDIR)
		error(Eperm);
	cb = parsecmd(buf, n);
	ct = lookupcmd(cb, ledpanelcmd, nelem(ledpanelcmd));

	switch(ct->index) {

	// If it's "init", initialize the panel and start the kproc that
	// refreshes it. Avoid initializing multiple times. NOTE: I
	// realize that there's a race condition where we could be
	// initializing multiple times, but I'm not sure how to do an
	// atomic compare-and-set in C on Plan 9. I'm assuming for
	// the purposes of this project this is ok.
	case CMinit:
		if (!initialized) {
			initialized = 1;
			ledpanel_init();
			kproc("ledpanelrefresh", ledpanel_refresh, nil);
		}
		break;

	case CMclear:
		ledpanel_clear();
		break;

	// The argument is the color as a string
	// in the format "0xffffff".
         case CMfill:
		color = atoi(cb->f[1]);
		ledpanel_fill(color);
		break;

	// Set pixel (x, y) to the given color. The color
	// argument is in the format 0xRRBBGG. For example
	// 0xffff00 is purple.
	case CMsetpixel:
		x = atoi(cb->f[1]);
		y = atoi(cb->f[2]);
		color = atoi(cb->f[3]);
		ledpanel_setpixel(x, y, color);
		break;
	}
	return n;
}

Dev ledpaneldevtab = {
	'L',
	"ledpanel",

	devreset,
	devinit,
	devshutdown,
	ledpanelattach,
	ledpanelwalk,
	ledpanelstat,
	ledpanelopen,
	devcreate,
	ledpanelclose,
	ledpanelread,
	devbread,
	ledpanelwrite,
	devbwrite,
	devremove,
	devwstat,
};
