#include <fcft/fcft.h>

static const unsigned int borderpx = 4; // 0 to disable border
static const char *font = "monospace:size=10";

static const pixman_color_t col_gray1  = {0x2200, 0x2200, 0x2200, 0xffff};
static const pixman_color_t col_gray2  = {0xBB00, 0xBB00, 0xBB00, 0xffff};
static const pixman_color_t col_gray3  = {0xEE00, 0xEE00, 0xEE00, 0xffff};
static const pixman_color_t col_accent = {0x0000, 0x5500, 0x7700, 0xffff};
static pixman_color_t colors[2][2] = {
  //fg         bg
  { col_gray2, col_gray1 }, // Normal scheme
  { col_gray3, col_accent }, // Selected scheme
};
