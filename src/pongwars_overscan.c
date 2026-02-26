/**
 * PONGWRS2.TOS - Pong Wars for Atari STE (vertical overscan)
 *
 * - Runs in STE vertical overscan (320x268)
 * - 264x264 game area (12x12 grid of 22px squares), centred
 * - Left half = "day", right half = "night"
 * - Day & night counters near left/right screen edges
 * - ESC key to exit
 *
 * @author  Neil Rackett <https://github.com/neilrackett>
 */

#include <osbind.h>
#include <string.h>
#include "fastcpy.h"

/* --- Overscan assembly interface ----------------------------------- */
/*
 * overscan_ste_vbl: sets hardware screen address from scraddr1,
 *   swaps scraddr1 <-> scraddr2, increments vblcnt.
 *
 * We use it purely for timing (vblcnt) and display address management.
 * We point scraddr1 == scraddr2 == display_buf so the VBL always
 * shows display_buf regardless of which way the swap goes.
 * We draw directly into display_buf each frame — no per-frame copy needed.
 */

extern void overscan_ste_setup(void);
extern void overscan_ste_restore(void);
extern volatile unsigned short vblcnt;
extern volatile unsigned long scraddr1;
extern volatile unsigned long scraddr2;

/* --- Screen/geometry constants ------------------------------------ */

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 273  /* vertical overscan: top+bottom borders removed */
#define BYTES_PER_LINE 160 /* 320px / 8 * 4 planes = 160 bytes/line        */
#define PLANES 4

/* Buffer size: 280 lines worth to safely cover all overscan output   */
#define SCREEN_BYTES (BYTES_PER_LINE * 280) /* 44800 bytes          */

#define GRID_SIZE 13
#define SQUARE_SIZE 21
#define GAME_PIXELS (GRID_SIZE * SQUARE_SIZE) /* 273                  */

#define GAME_LEFT ((SCREEN_WIDTH - GAME_PIXELS) / 2)       /* 28            */
#define GAME_TOP (((SCREEN_HEIGHT - GAME_PIXELS) / 2) + 1) /* 0          */

#define BALL_SIZE 11
#define BALL_VELOCITY 11

#define DIGIT_WIDTH 5
#define DIGIT_HEIGHT 7
#define MAX_SCORE_DIGITS 3
#define MAX_SCORE_WIDTH (MAX_SCORE_DIGITS * (DIGIT_WIDTH + 1) - 1) /* 17px */

/* Scores near screen edges (only 28px margin each side)              */
#define LEFT_SCORE_X 0
#define RIGHT_SCORE_X SCREEN_WIDTH - MAX_SCORE_WIDTH
#define SCORE_CENTER_Y (SCREEN_HEIGHT / 2)

/* WORDS_PER_LINE for put_pixel address arithmetic                    */
#define WORDS_PER_LINE ((SCREEN_WIDTH / 16) * PLANES) /* 80           */

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define ALIGN_LEFT 0
#define ALIGN_RIGHT 1

/* --- Colour indices ----------------------------------------------- */

#define COLOR_BG 0
#define DAY_COLOR 1
#define NIGHT_COLOR 2
#define COLOR_TEXT 15

#define ST_COLOR(r, g, b) (((r & 7) << 8) | ((g & 7) << 4) | (b & 7))

/* --- Ownership ---------------------------------------------------- */

enum
{
  OWNER_DAY = 1,
  OWNER_NIGHT = 2
};

/* --- Screen buffers ----------------------------------------------- */
/*
 * display_buf: live screen, 256-byte aligned.  scraddr1==scraddr2==display_buf.
 * board_buf:   offscreen scratch used only at init to pre-render the
 *              game board invisibly; fastcpy'd into display_buf after splash.
 */
static unsigned char disp_raw[SCREEN_BYTES + 255];
static unsigned char board_raw[SCREEN_BYTES + 255];
static unsigned char *display_buf;
static unsigned char *board_buf;

static unsigned char pi1_buffer[34 + 32000];

/* --- Game state --------------------------------------------------- */

static unsigned short old_palette[16];

static int squares[GRID_SIZE][GRID_SIZE];
static long dayScore = 0;
static long nightScore = 0;
static int scores_changed = 1;
static long iteration = 0;

/* Dirty cell queue: at most 1 cell per ball per frame = 2, sized to 4 */
#define DIRTY_MAX 4
static unsigned char dirty_gx[DIRTY_MAX];
static unsigned char dirty_gy[DIRTY_MAX];
static int dirty_count = 0;

/* --- Ball --------------------------------------------------------- */

typedef struct
{
  int x, y;
  int vx, vy;
  int owner;
  int color;
} Ball;

static Ball balls[2];

/* --- Forward declarations ----------------------------------------- */

static void init_board(void);
static void drawSquares(void);
static void init_balls(void);
static void drawBall(const Ball *ball);
static void draw_counters(void);
static void fast_fill_rect(int x, int y, int w, int h, int color);
static void step_physics(void);

/* --- Digit font (5x7) --------------------------------------------- */

static const unsigned char digit_font[10][DIGIT_HEIGHT] = {
    {0x1E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x1E}, /* 0 */
    {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F}, /* 1 */
    {0x1E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}, /* 2 */
    {0x1E, 0x11, 0x01, 0x0E, 0x01, 0x11, 0x1E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x1E}, /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x1E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* 8 */
    {0x1E, 0x11, 0x11, 0x1F, 0x01, 0x02, 0x0C}  /* 9 */
};

/* --- Palette ------------------------------------------------------- */

static void save_palette(void)
{
  int i;
  for (i = 0; i < 16; ++i)
    old_palette[i] = (unsigned short)Setcolor(i, -1);
}

static void restore_palette(void)
{
  int i;
  for (i = 0; i < 16; ++i)
    Setcolor(i, old_palette[i]);
}

static void set_game_palette(void)
{
  int i;
  /* Colours sampled from PONGWRS2.PI1 splash palette (raw STE values) */
  for (i = 0; i < 16; ++i)
    Setcolor(i, 0x0088);     /* background: near-black with slight teal */
  Setcolor(COLOR_BG, 0x0088);
  Setcolor(DAY_COLOR, 0x0675);   /* PI1 color 12: bright yellow-green streaks */
  Setcolor(NIGHT_COLOR, 0x012A); /* PI1 color 5: dark teal */
  Setcolor(COLOR_TEXT, 0x077D);  /* PI1 color 14: bright warm white */
}

/* --- Frame sync via overscan VBL counter -------------------------- */

static void wait_vbl(void)
{
  long timeout = 200000L; /* watchdog: ~200ms of spins, well over 1 frame */
  vblcnt = 0;             /* discard any stale count from previous frame */
  while (vblcnt == 0)     /* spin until the next VBL fires */
  {
    if (--timeout <= 0)
    {
      /* VBL missed — Timer A may have overrun and blocked it.
       * Stop Timer A so the next natural VBL can fire unblocked. */
      *(volatile unsigned char *)0xFFFFFA19 = 0; /* stop Timer A */
      timeout = 200000L;
      vblcnt = 0;
    }
  }
}

/* --- Splash screen ------------------------------------------------ */

static void load_and_display_pi1(const char *filename)
{
  long handle = Fopen(filename, 0);
  long bytes_read;

  if (handle < 0)
    return;

  bytes_read = Fread(handle, sizeof(pi1_buffer), pi1_buffer);
  Fclose(handle);

  if (bytes_read != (long)sizeof(pi1_buffer))
    return;

  /* PI1: 2 bytes res, 32 bytes palette (16 words), 32000 bytes image */
  {
    unsigned short *pi1_palette = (unsigned short *)(pi1_buffer + 2);
    unsigned char *pi1_image = pi1_buffer + 34;
    int i;

    for (i = 0; i < 16; ++i)
      Setcolor(i, pi1_palette[i]);

    /* Display in the overscan display buffer (first 32000 bytes = standard res) */
    /* Vertically center: offset by (268-200)/2 = 34 lines = 34*160 = 5440 bytes */
    fastcpy(display_buf + 34 * BYTES_PER_LINE, pi1_image, 32000);
  }
}

/* --- Drawing ------------------------------------------------------- */

static void put_pixel(int x, int y, int color)
{
  unsigned short *p;
  unsigned short mask;
  int plane, colorBits;

  if ((unsigned)x >= SCREEN_WIDTH || (unsigned)y >= SCREEN_HEIGHT)
    return;

  {
    int group = x >> 4;
    int bit = 15 - (x & 15);
    mask = (unsigned short)(1u << bit);
    p = (unsigned short *)display_buf + y * WORDS_PER_LINE + group * PLANES;
  }

  colorBits = color & 0x0F;
  for (plane = 0; plane < PLANES; ++plane)
  {
    if (colorBits & (1 << plane))
      p[plane] |= mask;
    else
      p[plane] &= (unsigned short)~mask;
  }
}

/* Word-at-a-time rect fill — replaces pixel-by-pixel fill_rect.
 * For a solid colour, each plane word is 0xFFFF or 0x0000.
 * Handles left/right partial-word edges and full middle words. */
static void fast_fill_rect(int x, int y, int w, int h, int color)
{
  unsigned short pw[4];
  int p, row;
  int x_end, grp_l, grp_r, bit_l, bit_r;

  if (w <= 0 || h <= 0)
    return;

  for (p = 0; p < 4; ++p)
    pw[p] = (color & (1 << p)) ? (unsigned short)0xFFFF : 0x0000;

  x_end = x + w - 1;
  grp_l = x >> 4;
  grp_r = x_end >> 4;
  bit_l = x & 15;
  bit_r = x_end & 15;

  for (row = y; row < y + h; ++row)
  {
    unsigned short *base = (unsigned short *)display_buf + row * WORDS_PER_LINE;

    if (grp_l == grp_r)
    {
      /* Entire width fits within one 16-px word group */
      unsigned short mask = (unsigned short)((0xFFFFu >> bit_l) & ~(0x7FFFu >> bit_r));
      unsigned short *ptr = base + grp_l * PLANES;
      for (p = 0; p < PLANES; ++p)
      {
        if (pw[p])
          ptr[p] |= mask;
        else
          ptr[p] &= (unsigned short)~mask;
      }
    }
    else
    {
      /* Left partial word */
      unsigned short lmask = (unsigned short)(0xFFFFu >> bit_l);
      unsigned short *lptr = base + grp_l * PLANES;
      for (p = 0; p < PLANES; ++p)
      {
        if (pw[p])
          lptr[p] |= lmask;
        else
          lptr[p] &= (unsigned short)~lmask;
      }
      /* Full middle words */
      {
        int grp;
        for (grp = grp_l + 1; grp < grp_r; ++grp)
        {
          unsigned short *mptr = base + grp * PLANES;
          for (p = 0; p < PLANES; ++p)
            mptr[p] = pw[p];
        }
      }
      /* Right partial word */
      unsigned short rmask = (unsigned short)(~(0x7FFFu >> bit_r));
      unsigned short *rptr = base + grp_r * PLANES;
      for (p = 0; p < PLANES; ++p)
      {
        if (pw[p])
          rptr[p] |= rmask;
        else
          rptr[p] &= (unsigned short)~rmask;
      }
    }
  }
}

/* --- Board -------------------------------------------------------- */

static void init_board(void)
{
  int x, y;
  dayScore = nightScore = 0;
  for (y = 0; y < GRID_SIZE; ++y)
    for (x = 0; x < GRID_SIZE; ++x)
    {
      int owner = (x < GRID_SIZE / 2) ? OWNER_DAY : OWNER_NIGHT;
      squares[y][x] = owner;
      if (owner == OWNER_DAY)
        dayScore++;
      else
        nightScore++;
    }
  scores_changed = 1;
}

static void draw_cell(int gx, int gy)
{
  int color;
  if ((unsigned)gx >= GRID_SIZE || (unsigned)gy >= GRID_SIZE)
    return;
  switch (squares[gy][gx])
  {
  case OWNER_DAY:
    color = DAY_COLOR;
    break;
  case OWNER_NIGHT:
    color = NIGHT_COLOR;
    break;
  default:
    color = COLOR_BG;
    break;
  }
  fast_fill_rect(GAME_LEFT + gx * SQUARE_SIZE,
                 GAME_TOP + gy * SQUARE_SIZE,
                 SQUARE_SIZE, SQUARE_SIZE, color);
}

static void drawSquares(void)
{
  int x, y;
  for (y = 0; y < GRID_SIZE; ++y)
    for (x = 0; x < GRID_SIZE; ++x)
      draw_cell(x, y);
}

static void paint_cell(int gx, int gy, int owner)
{
  if ((unsigned)gx >= GRID_SIZE || (unsigned)gy >= GRID_SIZE)
    return;
  if (squares[gy][gx] == owner)
    return;
  if (squares[gy][gx] == OWNER_DAY)
    dayScore--;
  else
    nightScore--;
  squares[gy][gx] = owner;
  if (owner == OWNER_DAY)
    dayScore++;
  else
    nightScore++;
  if (dirty_count < DIRTY_MAX)
  {
    dirty_gx[dirty_count] = (unsigned char)gx;
    dirty_gy[dirty_count] = (unsigned char)gy;
    dirty_count++;
  }
  scores_changed = 1;
}

/* --- Balls -------------------------------------------------------- */

static void init_balls(void)
{
  balls[0].x = GAME_PIXELS / 4;
  balls[0].y = GAME_PIXELS / 2;
  balls[0].vx = BALL_VELOCITY / 2;
  balls[0].vy = BALL_VELOCITY;
  balls[0].owner = OWNER_DAY;
  balls[0].color = NIGHT_COLOR;

  balls[1].x = (GAME_PIXELS * 3) / 4;
  balls[1].y = GAME_PIXELS / 2;
  balls[1].vx = -BALL_VELOCITY;
  balls[1].vy = -(BALL_VELOCITY / 2);
  balls[1].owner = OWNER_NIGHT;
  balls[1].color = DAY_COLOR;
}

/* Repaint the background cells under a ball at position (bx, by).
 * Takes explicit coords so it can be called with the *old* position
 * after the ball has already moved. */
static void eraseBallAt(int bx, int by)
{
  int min_gx = bx / SQUARE_SIZE;
  int max_gx = (bx + BALL_SIZE - 1) / SQUARE_SIZE;
  int min_gy = by / SQUARE_SIZE;
  int max_gy = (by + BALL_SIZE - 1) / SQUARE_SIZE;
  int gy, gx;

  for (gy = min_gy; gy <= max_gy; ++gy)
    for (gx = min_gx; gx <= max_gx; ++gx)
    {
      int cell_color;
      int cell_owner = ((unsigned)gx < GRID_SIZE && (unsigned)gy < GRID_SIZE)
                           ? squares[gy][gx]
                           : 0;
      if (cell_owner == OWNER_DAY)
        cell_color = DAY_COLOR;
      else if (cell_owner == OWNER_NIGHT)
        cell_color = NIGHT_COLOR;
      else
        cell_color = COLOR_BG;

      {
        int cl = gx * SQUARE_SIZE, ct = gy * SQUARE_SIZE;
        int ol = MAX(cl, bx);
        int ot = MAX(ct, by);
        int or2 = MIN(cl + SQUARE_SIZE - 1, bx + BALL_SIZE - 1);
        int ob = MIN(ct + SQUARE_SIZE - 1, by + BALL_SIZE - 1);
        if (ol <= or2 && ot <= ob)
          fast_fill_rect(GAME_LEFT + ol, GAME_TOP + ot,
                         or2 - ol + 1, ob - ot + 1, cell_color);
      }
    }
}

static void drawBall(const Ball *ball)
{
  fast_fill_rect(GAME_LEFT + ball->x, GAME_TOP + ball->y,
                 BALL_SIZE, BALL_SIZE, ball->color);
}

static void perturb_velocity(Ball *ball)
{
  if (Random() & 1)
  {
    int ax = (ball->vx < 0) ? -ball->vx : ball->vx;
    int ay = (ball->vy < 0) ? -ball->vy : ball->vy;
    if (Random() & 1)
    {
      ax++;
      ay--;
    }
    else
    {
      ax--;
      ay++;
    }
    if (ax < 4)
    {
      ax = 4;
      ay = 11;
    }
    if (ay < 4)
    {
      ay = 4;
      ax = 11;
    }
    if (ax > 11)
    {
      ax = 11;
      ay = 4;
    }
    if (ay > 11)
    {
      ay = 11;
      ax = 4;
    }
    ball->vx = (ball->vx < 0) ? -ax : ax;
    ball->vy = (ball->vy < 0) ? -ay : ay;
  }
}

static void checkBoundaryCollision(Ball *ball)
{
  if (ball->x + ball->vx < 0 || ball->x + BALL_SIZE + ball->vx > GAME_PIXELS)
  {
    ball->vx = -ball->vx;
    perturb_velocity(ball);
  }
  if (ball->y + ball->vy < 0 || ball->y + BALL_SIZE + ball->vy > GAME_PIXELS)
  {
    ball->vy = -ball->vy;
    perturb_velocity(ball);
  }
}

static void checkSquareCollision(Ball *ball, int old_gx, int old_gy)
{
  int gx = (ball->x + BALL_SIZE / 2) / SQUARE_SIZE;
  int gy = (ball->y + BALL_SIZE / 2) / SQUARE_SIZE;
  if ((unsigned)gx >= GRID_SIZE || (unsigned)gy >= GRID_SIZE)
    return;

  if (squares[gy][gx] != ball->owner)
  {
    int dxg = gx - old_gx, dyg = gy - old_gy;
    paint_cell(gx, gy, ball->owner);
    if (dxg != 0 && dyg == 0)
      ball->vx = -ball->vx;
    else if (dxg == 0 && dyg != 0)
      ball->vy = -ball->vy;
    else
    {
      ball->vx = -ball->vx;
      ball->vy = -ball->vy;
    }
    perturb_velocity(ball);
  }
  else
  {
    paint_cell(gx, gy, ball->owner);
  }
}

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
    int tvx = balls[0].vx, tvy = balls[0].vy;
    balls[0].vx = balls[1].vx;
    balls[0].vy = balls[1].vy;
    balls[1].vx = tvx;
    balls[1].vy = tvy;
  }
}

/* --- Text --------------------------------------------------------- */

static void long_to_string(long value, char *buf)
{
  char tmp[16];
  int i = 0, j = 0;
  if (value <= 0)
  {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  while (value > 0 && i < 15)
  {
    tmp[i++] = (char)('0' + (int)(value % 10L));
    value /= 10L;
  }
  while (i > 0)
    buf[j++] = tmp[--i];
  buf[j] = '\0';
}

static void draw_digit(int x, int y, char ch, int color)
{
  int d, dx, dy;
  if (ch < '0' || ch > '9')
    return;
  d = ch - '0';
  for (dy = 0; dy < DIGIT_HEIGHT; ++dy)
  {
    unsigned char row = digit_font[d][dy];
    for (dx = 0; dx < DIGIT_WIDTH; ++dx)
      if (row & (unsigned char)(1u << (DIGIT_WIDTH - 1 - dx)))
        put_pixel(x + dx, y + dy, color);
  }
}

static void draw_number(int field_x, int center_y, long value, int align)
{
  char buf[16];
  int len = 0, width, top_y, cx, i;
  long_to_string(value, buf);
  while (buf[len])
    len++;
  if (!len)
    return;
  width = len * (DIGIT_WIDTH + 1) - 1;
  top_y = center_y - DIGIT_HEIGHT / 2;
  /* Always clear the full fixed-width field to prevent ghost digits */
  fast_fill_rect(field_x, top_y, MAX_SCORE_WIDTH, DIGIT_HEIGHT, COLOR_BG);
  /* Position digits: left-justified or right-justified within field */
  cx = (align == ALIGN_RIGHT) ? field_x + MAX_SCORE_WIDTH - width : field_x;
  for (i = 0; i < len; ++i, cx += DIGIT_WIDTH + 1)
    draw_digit(cx, top_y, buf[i], COLOR_TEXT);
}

static void draw_counters(void)
{
  draw_number(LEFT_SCORE_X, SCORE_CENTER_Y, dayScore, ALIGN_LEFT);
  draw_number(RIGHT_SCORE_X, SCORE_CENTER_Y, nightScore, ALIGN_RIGHT);
}

/* Saved pre-physics ball positions for the render pass */
static int render_old_x[2], render_old_y[2];

/* step_physics: advance simulation by one frame.
 * Called AFTER render() so it runs during the active display period
 * (Timer A blocking) and its results are ready for the next render. */
static void step_physics(void)
{
  int i;
  for (i = 0; i < 2; ++i)
  {
    int old_gx = (balls[i].x + BALL_SIZE / 2) / SQUARE_SIZE;
    int old_gy = (balls[i].y + BALL_SIZE / 2) / SQUARE_SIZE;

    render_old_x[i] = balls[i].x;
    render_old_y[i] = balls[i].y;

    checkBoundaryCollision(&balls[i]);
    balls[i].x += balls[i].vx;
    balls[i].y += balls[i].vy;

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
  }

  handle_ball_collision();
  iteration++;
}

/* render: tight post-VBL ball update only — must complete before Timer A.
 * Dirty cells flushed first so balls always render on top.
 * Score rendering is deferred to after step_physics() in the game loop. */
static void render(void)
{
  int i;

  /* Flush cells queued by step_physics() — before ball draw so balls win */
  for (i = 0; i < dirty_count; ++i)
    draw_cell(dirty_gx[i], dirty_gy[i]);
  dirty_count = 0;

  /* Erase all old ball positions, then draw all new positions */
  for (i = 0; i < 2; ++i)
    eraseBallAt(render_old_x[i], render_old_y[i]);
  for (i = 0; i < 2; ++i)
    drawBall(&balls[i]);
}

/* --- IKBD command helper ----------------------------------------- */

static void ikbd_send(unsigned char cmd)
{
  /* Wait for ACIA TX ready (bit 1 of status register) */
  while (!(*(volatile unsigned char *)0xFFFC00 & 2))
    ;
  *(volatile unsigned char *)0xFFFC02 = cmd;
}

/* --- Main game loop (runs in supervisor via Supexec) -------------- */

static long run_game(void)
{
  /* 256-byte align both buffers */
  display_buf = (unsigned char *)(((unsigned long)disp_raw + 255UL) & ~255UL);
  board_buf = (unsigned char *)(((unsigned long)board_raw + 255UL) & ~255UL);

  /* Both scraddr slots point at display_buf — VBL swap is a no-op */
  scraddr1 = (unsigned long)display_buf;
  scraddr2 = (unsigned long)display_buf;

  save_palette();
  Cconws("\033f");

  /* Pre-render the initial game board into board_buf.
     Hardware still points at TOS screen — nothing visible yet. */
  memset(board_buf, 0, SCREEN_BYTES);
  display_buf = board_buf; /* redirect drawing to off-screen buffer */
  init_board();
  drawSquares();
  init_balls();
  drawBall(&balls[0]);
  drawBall(&balls[1]);
  draw_counters();
  display_buf = (unsigned char *)(((unsigned long)disp_raw + 255UL) & ~255UL);

  /* Start overscan (shows black display_buf), then show splash */
  memset(display_buf, 0, SCREEN_BYTES);
  overscan_ste_setup();

  /* Disable mouse and joystick so only keyboard scancodes arrive on ACIA */
  ikbd_send(0x12); /* disable mouse */
  ikbd_send(0x1A); /* disable joystick */

  load_and_display_pi1("PONGWRS2.PI1");

  /* Hold splash for ~2 seconds (100 VBL ticks at 50Hz) */
  {
    int n;
    for (n = 0; n < 100; ++n)
      wait_vbl();
  }

  /* Clear splash, switch to game palette, blit pre-rendered board */
  memset(display_buf, 0, SCREEN_BYTES);
  set_game_palette();
  fastcpy(display_buf, board_buf, SCREEN_BYTES);

  /* Prime render_old_x/y so the first render() erases from the correct spot */
  render_old_x[0] = balls[0].x;
  render_old_y[0] = balls[0].y;
  render_old_x[1] = balls[1].x;
  render_old_y[1] = balls[1].y;
  step_physics();

  {
    int ikbd_skip = 0; /* bytes remaining in current IKBD packet */
    for (;;)
    {
      /* render() runs immediately after fresh VBL — balls only, no scores */
      wait_vbl();
      render();
      /* step_physics() + scores run during active display / Timer A period */
      step_physics();
      if (scores_changed)
      {
        draw_counters();
        scores_changed = 0;
      }

      /* Poll IKBD ACIA with packet-aware parsing.
       * ikbd_skip: bytes remaining in current mouse/joystick packet.
       * When 0, the next byte is a header: keyboard scancode (0x00-0xF7)
       * or packet header (0xF8-0xFF). Mouse packets (0xF8-0xFB) have 2
       * continuation bytes; joystick (0xFE/0xFF) have 1. */
      while (*(volatile unsigned char *)0xFFFC00 & 1)
      {
        unsigned char b = *(volatile unsigned char *)0xFFFC02;
        if (ikbd_skip > 0)
        {
          ikbd_skip--;
        }
        else if (b >= 0xF8)
        {
          ikbd_skip = (b <= 0xFB) ? 2 : (b >= 0xFE) ? 1
                                                    : 0;
        }
        else if (b == 0x01)
        {
          goto exit_game; /* ESC make scancode */
        }
      }
    }
  } /* end ikbd_skip scope */

exit_game:
  /* Clean exit:
   * 1. Restore overscan (stops Timer A, restores VBL/MFP vectors) first
   *    so interrupts work normally again.
   * 2. Drain + reset the IKBD ACIA — we read its byte directly which
   *    bypassed the normal interrupt handler; flush any queued bytes and
   *    reset the ACIA control register so the desktop gets a clean state.
   * 3. Clear screen, wait for a clean VBL, then restore palette. */
  overscan_ste_restore();

  /* Re-enable mouse reporting so the desktop works normally */
  ikbd_send(0x08); /* relative mouse mode */

  memset(display_buf, 0, SCREEN_BYTES);
  /* Wait for a system VBL using the OS vblank counter at 0x462 */
  {
    volatile unsigned long *sys_vbl = (volatile unsigned long *)0x462;
    unsigned long v = *sys_vbl;
    while (*sys_vbl == v)
      ;
  }

  restore_palette();
  Cconws("\033e"); /* show cursor */

  return 0;
}

/* --- Entry point -------------------------------------------------- */

int main(void)
{
  int prev_rez = Getrez();

  if (prev_rez != 0)
  {
    Setscreen(-1L, -1L, 0);
    if (Getrez() != 0)
    {
      Cconws("Pong Wars requires ST low res.\r\n");
      return 0;
    }
  }

  Supexec(run_game);

  if (prev_rez != 0)
    Setscreen(-1L, -1L, prev_rez);

  return 0;
}
