#include <u.h>
#include <libc.h>

// Set the color of the pixel at the given coordinates
// by writing a "setpixel" command to the the ledpanel
// device.
void setpixel(int fd, uint x, uint y, uint color) {
  fprint(fd, "setpixel %d %d 0x%06x\n", x, y, color);
}

// Set all pixels to the given color, with the "fill" command.
void fill(int fd, uint color) {
  fprint(fd, "fill 0x%06x\n", color);
}


// Iterate through six color mixtures (red,
// red/green, green, green/blue, blue,
// white), pulsing each one from low to
// high intensity.
void demo_pulse(int fd) {
  int i, j;

  static uint masks[] = {
    0xff0000,
    0xffff00,
    0x00ff00,
    0x00ffff,
    0x0000ff,
    0xffffff
  };
  
  for (i = 0; i < 6; i++) {
    for (j = 0; j < 0xff; j++) {
      fill(fd, masks[i] & (j << 16 | j << 8 | j));
      sleep(1);
    }
   for (j = 0xff; j >=0; j--) {
      fill(fd, masks[i] & (j << 16 | j << 8 | j));
      sleep(1);
    }
  }
}

// Show a demo where we use a gradient for each of
// the three colors channels. Go through six generations.
// In the first generation, green is on the x axis, blue is
// on the y axis, and we increase red over time. In the
// other five generations, switch the x, y, and time dimensions.
void demo_gradient(int fd) {
  int g, t, x, y;
  uchar c1, c2, c3;
  uint color;
  
  while (1) {
    for (g = 0; g < 6; g++) {
      for (t = 0; t < 510; t += 8) {
        for (x = 0; x < 32; x++) {
          for (y = 0; y < 16; y++) {
            c1 = 255 - abs(255 - t);
            c2 = 255 * (x / 32.0);
            c3 = 255 * (y / 16.0);
            color = 
              g == 0 ? c1 << 16 | c2 << 8 | c3 :
              g == 1 ? c3 << 16 | c2 << 8 | c1 :
              g == 2 ? c3 << 16 | c1 << 8 | c2 :
              g == 3 ? c2 << 16 | c1 << 8 | c3 :
              g == 4 ? c2 << 16 | c3 << 8 | c1 :
                             c1 << 16 | c3 << 8 | c2;
            setpixel(fd, x, y, color);
          }
        }
        sleep(1);
      }
    }
  }
}

// Animation that assigns a random color to a random pixel,
// with a random sleep in between.
void demo_random(int fd) {
  uint x, y, color;
  while (1) {
    x = nrand(32);
    y = nrand(16);
    color = nrand(0xffffff);
    setpixel(fd, x, y, color);
    sleep(nrand(15));
  }
}


void print_usage(char *name) {
    print("Usage: %s cmd, where command is one of\n", name);
    print("  pulse - show six pulsing solid colors\n");
    print("  gradient - cycle through colors with vertical and horizontal gradients\n");
    print("  random - set random pixels to random colors\n");
}

char *commands[] = { "pulse", "gradient", "random" };

void main(int argc, char **argv) {
  int fd = open("/dev/ledpanel", ORDWR);
  int cmd;

  if (fd < 0) {
    bind("#L", "/dev", MAFTER);
    fd = open("/dev/ledpanel", ORDWR);
    if (fd < 0) {
      print("Error opening ledpanel: %r\n");
    }
  }
 
  if (argc != 2) {
    print_usage(argv[0]);
    return;
  }

  for (cmd = 0; cmd < 3 && strcmp(argv[1], commands[cmd]); cmd++);
  
  if (cmd == 3) {
    print("Bad command %s\n", argv[1]);
    print_usage(argv[0]);
  }

   fprint(fd, "init\n");
  fprint(fd, "clear\n");
  switch (cmd) {
    case 0: demo_pulse(fd); break;
    case 1: demo_gradient(fd); break;
    case 2: demo_random(fd); break;
  }
  fprint(fd, "clear\n");
}

