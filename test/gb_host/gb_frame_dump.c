/**
 * @file gb_frame_dump.c
 * @brief Host check of the Game Boy core through GbSession: runs a ROM headless, writes the last frame.
 *
 * Usage: gb_frame_dump <rom> <frames> <out.pgm>
 * The frame is written as 160x144 8-bit grey (shade 0 = white). Used with the MIT test ROMs
 * dmg-acid2 and cgb-acid2, whose reference images ship with them. See run_gb_host_test.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../../src/games/gb/GbSession.h"

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s <rom> <frames> <out.pgm>\n", argv[0]);
    return 2;
  }
  FILE* f = fopen(argv[1], "rb");
  if (!f) return perror(argv[1]), 1;
  fseek(f, 0, SEEK_END);
  const long rom_size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* rom = malloc((size_t)rom_size);
  if (!rom || fread(rom, 1, (size_t)rom_size, f) != (size_t)rom_size) return fprintf(stderr, "read failed\n"), 1;
  fclose(f);

  if (!gbs_open(rom, (size_t)rom_size)) return 1;
  const int frames = atoi(argv[2]);
  const clock_t started = clock();
  for (int i = 0; i < frames; ++i) gbs_run_frame();
  const double seconds = (double)(clock() - started) / CLOCKS_PER_SEC;

  FILE* out = fopen(argv[3], "wb");
  if (!out) return perror(argv[3]), 1;
  fprintf(out, "P5\n%d %d\n255\n", GBS_LCD_W, GBS_LCD_H);
  const uint8_t* lcd = gbs_lcd();
  for (int y = 0; y < GBS_LCD_H; ++y) {
    for (int x = 0; x < GBS_LCD_W; ++x) {
      const int shade = (lcd[y * GBS_LCD_ROW_BYTES + x / 4] >> ((x & 3) * 2)) & 3;
      fputc(255 - shade * 85, out);
    }
  }
  fclose(out);
  size_t ram = 0;
  gbs_cart_ram(&ram);
  printf("%s: %s mode, %d frames (%.0f fps on this host), cart RAM %zu\n", argv[1], gbs_is_color() ? "Color" : "DMG",
         frames, seconds > 0 ? frames / seconds : 0.0, ram);
  gbs_close();
  return 0;
}
