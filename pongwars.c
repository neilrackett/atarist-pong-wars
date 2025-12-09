/**
 * PONGWARS.TOS - Pong Wars for Atari ST
 *
 * - Runs in ST low resolution (320x200)
 * - 200x200 game area, centred horizontally
 * - Left half = "day", right half = "night"
 * - Day & night counters in white text to left/right
 * - ESC key to exit
 *
 * @author  Neil Rackett <https://github.com/neilrackett>
 */

#include <osbind.h> /* TOS BIOS/XBIOS/GEMDOS bindings */
#include <string.h> /* memset */

/* --- Screen/geometry constants ------------------------------------- */

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200
#define PLANES 4

#define WORDS_PER_LINE ((SCREEN_WIDTH / 16) * PLANES) /* 20 * 4 = 80 */
#define SCREEN_BYTES (WORDS_PER_LINE * SCREEN_HEIGHT * 2)

#define GRID_SIZE 10
#define SQUARE_SIZE 20                        /* pixels */
#define GAME_PIXELS (GRID_SIZE * SQUARE_SIZE) /* Result should be 200 */

#define GAME_LEFT ((SCREEN_WIDTH - GAME_PIXELS) / 2)
#define GAME_TOP ((SCREEN_HEIGHT - GAME_PIXELS) / 2)

#define BALL_SIZE 10
#define BALL_VELOCITY 10 /* pixels per frame */

#define DIGIT_WIDTH 5
#define DIGIT_HEIGHT 7
#define MAX_SCORE_DIGITS 3

#define SCORE_AREA_WIDTH (GAME_LEFT)
#define MAX_SCORE_WIDTH (MAX_SCORE_DIGITS * (DIGIT_WIDTH + 1) - 1)

#define LEFT_SCORE_X ((SCORE_AREA_WIDTH - MAX_SCORE_WIDTH) / 2)
#define RIGHT_SCORE_X (GAME_LEFT + GAME_PIXELS + (SCORE_AREA_WIDTH - MAX_SCORE_WIDTH) / 2 + DIGIT_WIDTH)
#define SCORE_CENTER_Y (SCREEN_HEIGHT / 2)

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* --- Colour indices and palette ------------------------------------ */
/* ST palette format: 0b0000 rrr0 ggg0 bbb0 (3 bits per component) */

#define COLOR_BG 0 /* background / borders */

/* Match original scheme: DAY_COLOR / NIGHT_COLOR and ball colours */
#define DAY_COLOR 1        /* approx #D9E8E3 */
#define NIGHT_COLOR 2      /* approx #172B36 */
#define DAY_BALL_COLOR 3   /* dark ball on day side  */
#define NIGHT_BALL_COLOR 4 /* light ball on night side */

#define COLOR_TEXT 15 /* white text */

#define ST_COLOR(r, g, b) (((r & 7) << 8) | ((g & 7) << 4) | ((b & 7) << 0))

/* --- Ownership enum for board cells -------------------------------- */

enum
{
  OWNER_DAY = 1,
  OWNER_NIGHT = 2
};

/* --- Globals -------------------------------------------------------- */

static unsigned char *screen;                   /* offscreen drawing buffer */
static unsigned char *phys_screen;              /* physical screen base address */
static unsigned char framebuffer[SCREEN_BYTES]; /* offscreen frame buffer */
static unsigned short old_palette[16];          /* saved palette for restore on exit */
static unsigned char pi1_buffer[34 + 32000];    /* buffer for PI1 file data */

/* Territory grid ("squares") */
static int squares[GRID_SIZE][GRID_SIZE];
static long dayScore = 0;
static long nightScore = 0;
static int scores_changed = 1; /* flag to redraw scores only when changed */

static long iteration = 0; /* not displayed, just for fun */

/* --- Ball structure ("balls" array like original) ------------------ */
/* x,y are pixel coordinates relative to the game area (0..GAME_PIXELS-1),
   vx,vy are pixel velocities. */

typedef struct
{
  int x, y;   /* top-left in game pixels */
  int vx, vy; /* velocity in pixels per frame */
  int owner;  /* OWNER_DAY / OWNER_NIGHT */
  int color;  /* DAY_BALL_COLOR / NIGHT_BALL_COLOR */
} Ball;

static Ball balls[2];

/* --- Digit font (5x7) ---------------------------------------------- */

static const unsigned char digit_font[10][DIGIT_HEIGHT] = {
    /* 0 */
    {0x1E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x1E},
    /* 1 */
    {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F},
    /* 2 */
    {0x1E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F},
    /* 3 */
    {0x1E, 0x11, 0x01, 0x0E, 0x01, 0x11, 0x1E},
    /* 4 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    /* 5 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x1E},
    /* 6 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x1E},
    /* 7 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    /* 8 */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    /* 9 */
    {0x1E, 0x11, 0x11, 0x1F, 0x01, 0x02, 0x0C}};

/* --- Palette helpers (using XBIOS Setcolor) ------------------------ */

static void save_palette(void)
{
  int i;
  for (i = 0; i < 16; ++i)
  {
    /* Setcolor(col, -1) returns current value without changing it */
    old_palette[i] = (unsigned short)Setcolor(i, -1);
  }
}

static void restore_palette(void)
{
  int i;
  for (i = 0; i < 16; ++i)
  {
    Setcolor(i, old_palette[i]);
  }
}

static void set_game_palette(void)
{
  int i;

  for (i = 0; i < 16; ++i)
  {
    Setcolor(i, ST_COLOR(0, 1, 1));
  }

  Setcolor(COLOR_BG, ST_COLOR(0, 1, 1));         /* background      */
  Setcolor(DAY_COLOR, ST_COLOR(6, 7, 6));        /* light mint      */
  Setcolor(NIGHT_COLOR, ST_COLOR(1, 2, 3));      /* dark blue       */
  Setcolor(DAY_BALL_COLOR, ST_COLOR(1, 2, 3));   /* dark ball (day) */
  Setcolor(NIGHT_BALL_COLOR, ST_COLOR(6, 7, 6)); /* light ball (night) */
  Setcolor(COLOR_TEXT, ST_COLOR(7, 7, 7));       /* white           */
}

/* --- PI1 Splash Screen ---------------------------------------------- */

static int load_and_display_pi1(const char *filename)
{
  long handle;
  long bytes_read;
  unsigned short *pi1_palette;
  unsigned char *pi1_image;

  /* Open file */
  handle = Fopen(filename, 0); /* read-only */
  if (handle < 0)
  {
    return -1; /* error */
  }

  /* Read header + palette + image */
  bytes_read = Fread(handle, sizeof(pi1_buffer), pi1_buffer);
  Fclose(handle);

  if (bytes_read != sizeof(pi1_buffer))
  {
    return -1; /* error */
  }

  /* PI1 format: 2 bytes resolution (skip), 32 bytes palette (16 words), then 32000 bytes image */
  pi1_palette = (unsigned short *)(pi1_buffer + 2);
  pi1_image = pi1_buffer + 34;

  /* Set PI1 palette */
  {
    int i;
    for (i = 0; i < 16; ++i)
    {
      Setcolor(i, pi1_palette[i]);
    }
  }

  memcpy(phys_screen, pi1_image, 32000);

  return 0;
}

static void show_splash(void)
{
  if (load_and_display_pi1("PONGWARS.PI1") == 0)
  {
    /* Wait 3 seconds (150 frames at 50Hz) */
    int i;
    for (i = 0; i < 150; ++i)
    {
      Vsync();
    }
  }
}

/* --- Low-level drawing: put_pixel, fill_rect, clear_screen --------- */

static void put_pixel(int x, int y, int color)
{
  if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
  {
    return;
  }

  unsigned short *scr = (unsigned short *)screen;

  int group = x >> 4;      /* 16-pixel group index */
  int bit = 15 - (x & 15); /* bit in word, left = bit 15 */
  unsigned short mask = (unsigned short)(1u << bit);

  int offset_words = y * WORDS_PER_LINE + group * PLANES;
  unsigned short *p = scr + offset_words;

  int colorBits = color & 0x0F;
  int plane;
  for (plane = 0; plane < PLANES; ++plane)
  {
    if (colorBits & (1 << plane))
    {
      p[plane] |= mask;
    }
    else
    {
      p[plane] &= (unsigned short)~mask;
    }
  }
}

static void fill_rect(int x, int y, int w, int h, int color)
{
  int yy, xx;
  for (yy = y; yy < y + h; ++yy)
  {
    for (xx = x; xx < x + w; ++xx)
    {
      put_pixel(xx, yy, color);
    }
  }
}

static void clear_screen(void)
{
  memset(screen, 0, SCREEN_BYTES);
}

/* --- Board / cells ("squares") ------------------------------------- */

static void init_board(void)
{
  int x, y;
  dayScore = nightScore = 0;

  for (y = 0; y < GRID_SIZE; ++y)
  {
    for (x = 0; x < GRID_SIZE; ++x)
    {
      int owner = (x < GRID_SIZE / 2) ? OWNER_DAY : OWNER_NIGHT;
      squares[y][x] = owner;
      if (owner == OWNER_DAY)
      {
        dayScore++;
      }
      else
      {
        nightScore++;
      }
    }
  }
  scores_changed = 1;
}

static void draw_cell(int gx, int gy)
{
  if (gx < 0 || gx >= GRID_SIZE || gy < 0 || gy >= GRID_SIZE)
  {
    return;
  }

  int owner = squares[gy][gx];
  int color = COLOR_BG;

  if (owner == OWNER_DAY)
  {
    color = DAY_COLOR;
  }
  else if (owner == OWNER_NIGHT)
  {
    color = NIGHT_COLOR;
  }

  {
    int x = GAME_LEFT + gx * SQUARE_SIZE;
    int y = GAME_TOP + gy * SQUARE_SIZE;
    fill_rect(x, y, SQUARE_SIZE, SQUARE_SIZE, color);
  }
}

/* Full redraw of all squares; used at startup */
static void drawSquares(void)
{
  int x, y;
  for (y = 0; y < GRID_SIZE; ++y)
  {
    for (x = 0; x < GRID_SIZE; ++x)
    {
      draw_cell(x, y);
    }
  }
}

/* Paint a cell for given owner, update scores, and redraw the cell */
static void paint_cell(int gx, int gy, int owner)
{
  if (gx < 0 || gx >= GRID_SIZE || gy < 0 || gy >= GRID_SIZE)
  {
    return;
  }

  if (squares[gy][gx] == owner)
  {
    return;
  }

  if (squares[gy][gx] == OWNER_DAY)
  {
    dayScore--;
  }
  else if (squares[gy][gx] == OWNER_NIGHT)
  {
    nightScore--;
  }

  squares[gy][gx] = owner;

  if (owner == OWNER_DAY)
  {
    dayScore++;
  }
  else if (owner == OWNER_NIGHT)
  {
    nightScore++;
  }

  draw_cell(gx, gy);
  scores_changed = 1;
}

/* --- Balls ---------------------------------------------------------- */

static void init_balls(void)
{
  /* day ball (left) */
  balls[0].x = GAME_PIXELS / 4; /* 50 */
  balls[0].y = GAME_PIXELS / 2; /* 100 */
  balls[0].vx = BALL_VELOCITY / 2;
  balls[0].vy = BALL_VELOCITY;
  balls[0].owner = OWNER_DAY;
  balls[0].color = DAY_BALL_COLOR;

  /* night ball (right) */
  balls[1].x = (GAME_PIXELS * 3) / 4; /* 150 */
  balls[1].y = GAME_PIXELS / 2;       /* 100 */
  balls[1].vx = -BALL_VELOCITY;
  balls[1].vy = -BALL_VELOCITY / 2;
  balls[1].owner = OWNER_NIGHT;
  balls[1].color = NIGHT_BALL_COLOR;
}

/* Erase ball by redrawing underlying territory pixels */
static void eraseBall(const Ball *ball)
{
  int min_gx = ball->x / SQUARE_SIZE;
  int max_gx = (ball->x + BALL_SIZE - 1) / SQUARE_SIZE;
  int min_gy = ball->y / SQUARE_SIZE;
  int max_gy = (ball->y + BALL_SIZE - 1) / SQUARE_SIZE;

  int gy, gx;
  for (gy = min_gy; gy <= max_gy; ++gy)
  {
    for (gx = min_gx; gx <= max_gx; ++gx)
    {
      int cell_owner = (gx >= 0 && gx < GRID_SIZE && gy >= 0 && gy < GRID_SIZE) ? squares[gy][gx] : COLOR_BG;
      int cell_color = COLOR_BG;
      if (cell_owner == OWNER_DAY)
      {
        cell_color = DAY_COLOR;
      }
      else if (cell_owner == OWNER_NIGHT)
      {
        cell_color = NIGHT_COLOR;
      }

      int cell_left = gx * SQUARE_SIZE;
      int cell_top = gy * SQUARE_SIZE;
      int cell_right = cell_left + SQUARE_SIZE - 1;
      int cell_bottom = cell_top + SQUARE_SIZE - 1;

      int ball_left = ball->x;
      int ball_top = ball->y;
      int ball_right = ball_left + BALL_SIZE - 1;
      int ball_bottom = ball_top + BALL_SIZE - 1;

      int overlap_left = MAX(cell_left, ball_left);
      int overlap_top = MAX(cell_top, ball_top);
      int overlap_right = MIN(cell_right, ball_right);
      int overlap_bottom = MIN(cell_bottom, ball_bottom);

      if (overlap_left <= overlap_right && overlap_top <= overlap_bottom)
      {
        int overlap_x = GAME_LEFT + overlap_left;
        int overlap_y = GAME_TOP + overlap_top;
        int overlap_w = overlap_right - overlap_left + 1;
        int overlap_h = overlap_bottom - overlap_top + 1;
        fill_rect(overlap_x, overlap_y, overlap_w, overlap_h, cell_color);
      }
    }
  }
}

/**
 * Here balls are 4x4 pixels in game coordinates.
 */
static void drawBall(const Ball *ball)
{
  int x = GAME_LEFT + ball->x;
  int y = GAME_TOP + ball->y;
  fill_rect(x, y, BALL_SIZE, BALL_SIZE, ball->color);
}

/**
 * Reflect off outer edges of the 200x200 game area.
 */
static void checkBoundaryCollision(Ball *ball)
{
  if (ball->x + ball->vx < 0 || ball->x + BALL_SIZE + ball->vx > GAME_PIXELS)
  {
    ball->vx = -ball->vx;
  }
  if (ball->y + ball->vy < 0 || ball->y + BALL_SIZE + ball->vy > GAME_PIXELS)
  {
    ball->vy = -ball->vy;
  }
}

/**
 * Ball moves freely in pixels, but territory is on a 20x20 grid.
 * When the ball's centre enters an enemy-owned cell, we repaint it and bounce.
 */
static void checkSquareCollision(Ball *ball, int old_gx, int old_gy)
{
  int center_x = ball->x + BALL_SIZE / 2;
  int center_y = ball->y + BALL_SIZE / 2;

  int gx = center_x / SQUARE_SIZE;
  int gy = center_y / SQUARE_SIZE;

  if (gx < 0 || gx >= GRID_SIZE || gy < 0 || gy >= GRID_SIZE)
  {
    return;
  }

  if (squares[gy][gx] != ball->owner)
  {
    /* capture territory */
    paint_cell(gx, gy, ball->owner);

    /* decide bounce axis based on grid movement */
    {
      int dxg = gx - old_gx;
      int dyg = gy - old_gy;

      if (dxg != 0 && dyg == 0)
      {
        ball->vx = -ball->vx; /* horizontal wall */
      }
      else if (dxg == 0 && dyg != 0)
      {
        ball->vy = -ball->vy; /* vertical wall   */
      }
      else
      {
        ball->vx = -ball->vx; /* corner/ambiguous */
        ball->vy = -ball->vy;
      }
    }
  }
  else
  {
    /* ensure cell is drawn with correct owner colour */
    paint_cell(gx, gy, ball->owner);
  }
}

/**
 * Still a stub; movement is deterministic but non-grid.
 */
static void addRandomness(Ball *ball)
{
  (void)ball;
}

/**
 * Ball/ball collision in pixel space
 */
static void handle_ball_collision(void)
{
  int dx = balls[0].x - balls[1].x;
  int dy = balls[0].y - balls[1].y;

  if (dx < 0)
    dx = -dx;
  if (dy < 0)
    dy = -dy;

  if (dx < BALL_SIZE && dy < BALL_SIZE)
  {
    /* simple exchange of velocities */
    int tmpvx = balls[0].vx;
    int tmpvy = balls[0].vy;
    balls[0].vx = balls[1].vx;
    balls[0].vy = balls[1].vy;
    balls[1].vx = tmpvx;
    balls[1].vy = tmpvy;
  }
}

/* --- Text / counters ------------------------------------------------ */

static void long_to_string(long value, char *buffer)
{
  char tmp[16];
  int i = 0;

  if (value <= 0)
  {
    buffer[0] = '0';
    buffer[1] = '\0';
    return;
  }

  while (value > 0 && i < (int)sizeof(tmp) - 1)
  {
    int d = (int)(value % 10L);
    tmp[i++] = (char)('0' + d);
    value /= 10L;
  }

  {
    int j = 0;
    while (i > 0)
    {
      buffer[j++] = tmp[--i];
    }
    buffer[j] = '\0';
  }
}

static void draw_digit(int x, int y, char ch, int color)
{
  if (ch < '0' || ch > '9')
  {
    return;
  }

  {
    int d = ch - '0';
    int dx, dy;

    for (dy = 0; dy < DIGIT_HEIGHT; ++dy)
    {
      unsigned char row = digit_font[d][dy];
      for (dx = 0; dx < DIGIT_WIDTH; ++dx)
      {
        unsigned char bit = (unsigned char)(1u << (DIGIT_WIDTH - 1 - dx));
        if (row & bit)
        {
          put_pixel(x + dx, y + dy, color);
        }
      }
    }
  }
}

/* x = left edge of text box; center_y = vertical centre */
static void draw_number(int x, int center_y, long value)
{
  char buf[16];
  int len = 0;
  long_to_string(value, buf);

  while (buf[len] != '\0')
  {
    len++;
  }
  if (len == 0)
  {
    return;
  }

  {
    int width = len * (DIGIT_WIDTH + 1) - 1;
    int top_y = center_y - (DIGIT_HEIGHT / 2);

    /* Clear background behind text to black */
    fill_rect(x, top_y, width, DIGIT_HEIGHT, COLOR_BG);

    /* Draw digits */
    {
      int cx = x;
      int i;
      for (i = 0; i < len; ++i)
      {
        draw_digit(cx, top_y, buf[i], COLOR_TEXT);
        cx += DIGIT_WIDTH + 1;
      }
    }
  }
}

static void draw_counters(void)
{
  draw_number(LEFT_SCORE_X, SCORE_CENTER_Y, dayScore);
  draw_number(RIGHT_SCORE_X, SCORE_CENTER_Y, nightScore);
}

/* --- One frame of the simulation (original name: draw) ------------- */

/**
 * Draw the next frame of the simulation
 */
static void draw(void)
{
  int i;

  /* Overdraw cursor area each frame just in case */
  fill_rect(0, 0, 8, 16, COLOR_BG);

  /* Erase balls at old positions by redrawing underlying cells */
  for (i = 0; i < 2; ++i)
  {
    eraseBall(&balls[i]);
  }

  /* Move balls and territory collisions */
  for (i = 0; i < 2; ++i)
  {
    int center_x_old = balls[i].x + BALL_SIZE / 2;
    int center_y_old = balls[i].y + BALL_SIZE / 2;
    int old_gx = center_x_old / SQUARE_SIZE;
    int old_gy = center_y_old / SQUARE_SIZE;

    checkBoundaryCollision(&balls[i]);

    balls[i].x += balls[i].vx;
    balls[i].y += balls[i].vy;

    /* clamp for safety */
    if (balls[i].x < 0)
    {
      balls[i].x = 0;
      balls[i].vx = -balls[i].vx;
    }
    if (balls[i].y < 0)
    {
      balls[i].y = 0;
      balls[i].vy = -balls[i].vy;
    }
    if (balls[i].x > GAME_PIXELS - BALL_SIZE)
    {
      balls[i].x = GAME_PIXELS - BALL_SIZE;
      balls[i].vx = -balls[i].vx;
    }
    if (balls[i].y > GAME_PIXELS - BALL_SIZE)
    {
      balls[i].y = GAME_PIXELS - BALL_SIZE;
      balls[i].vy = -balls[i].vy;
    }

    checkSquareCollision(&balls[i], old_gx, old_gy);
    addRandomness(&balls[i]);
  }

  /* Ball/ball collision */
  handle_ball_collision();

  /* Draw balls on top */
  for (i = 0; i < 2; ++i)
  {
    drawBall(&balls[i]);
  }

  /* Update counters */
  if (scores_changed)
  {
    draw_counters();
    scores_changed = 0;
  }

  iteration++;
}

/* --- Main ----------------------------------------------------------- */

int main(void)
{
  int prev_rez = Getrez(); /* 0 = low, 1 = medium, 2 = high */
  int rez = prev_rez;

  if (rez != 0)
  {
    /* Switch to low res (0) */
    Setscreen(-1L, -1L, 0);
    rez = Getrez();
    if (rez != 0)
    {
      Cconws("Pong Wars requires ST low res:\r\nplease switch to low res and try again.\r\n");
      return 0;
    }
  }

  save_palette();

  phys_screen = (unsigned char *)Physbase(); /* XBIOS 2 */
  show_splash();
  set_game_palette();
  screen = framebuffer; /* draw into offscreen buffer */
  clear_screen();

  /* Hide VT52 cursor (ESC f) */
  Cconws("\033f");

  init_board();
  drawSquares();
  init_balls();
  drawBall(&balls[0]);
  drawBall(&balls[1]);
  draw_counters();

  /* Present initial frame */
  memcpy(phys_screen, screen, SCREEN_BYTES);

  for (;;)
  {
    draw();
    Vsync(); /* frame pacing, then present fully rendered frame */

    memcpy(phys_screen, screen, SCREEN_BYTES);

    /* ESC to quit */
    if (Cconis())
    {
      long key = Crawcin();
      if ((key & 0xFF) == 0x1B)
      { /* ESC */
        break;
      }
    }
  }

  restore_palette();

  if (prev_rez != rez)
  {
    Setscreen(-1L, -1L, prev_rez);
  }

  /* Show cursor again (ESC e) */
  Cconws("\033e");

  return 0;
}
