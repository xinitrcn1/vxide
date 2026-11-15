/* Icons are drawn once at start-up from distance-field primitives into the
 * glyph atlas (coverage only; the colour comes from the quad). */
#include "draw.h"
#include "font.h"

#include <math.h>

static u16 icon_u[ICON_COUNT], icon_v[ICON_COUNT];
static u8 icon_px[ICON_COUNT];

static float cov[32 * 32];
static int S; /* canvas size */
static float G = 1.0f; /* coordinate scale: the 24 px art drawn at other sizes */

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
	float dx = bx - ax, dy = by - ay;
	float l2 = dx * dx + dy * dy;
	float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
	return sqrtf(ex * ex + ey * ey);
}

static void put(int x, int y, float d)
{
	/* d = signed distance to the shape edge (negative inside). */
	float c = 0.5f - d;
	c = c < 0 ? 0 : c > 1 ? 1 : c;
	float *p = &cov[y * S + x];
	if (c > *p)
		*p = c;
}

/* Line of width w between points (pixel-centre coordinates). */
static void line(float ax, float ay, float bx, float by, float w)
{
	ax *= G;
	ay *= G;
	bx *= G;
	by *= G;
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++)
			put(x, y, seg_dist(x + 0.5f, y + 0.5f, ax, ay, bx, by) - w * 0.5f);
}

static void ring(float cx, float cy, float r, float w)
{
	cx *= G;
	cy *= G;
	r *= G;
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			put(x, y, fabsf(sqrtf(dx * dx + dy * dy) - r) - w * 0.5f);
		}
}

static void disc(float cx, float cy, float r)
{
	cx *= G;
	cy *= G;
	r *= G;
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			put(x, y, sqrtf(dx * dx + dy * dy) - r);
		}
}

/* Arc from angle a0 to a1 (radians, y down, clockwise positive). */
static void arc(float cx, float cy, float r, float a0, float a1, float w)
{
	int steps = 24;
	for (int i = 0; i < steps; i++) {
		float t0 = a0 + (a1 - a0) * i / steps, t1 = a0 + (a1 - a0) * (i + 1) / steps;
		line(cx + r * cosf(t0), cy + r * sinf(t0), cx + r * cosf(t1), cy + r * sinf(t1), w);
	}
}

static void box(float x0, float y0, float x1, float y1, float w)
{
	line(x0, y0, x1, y0, w);
	line(x1, y0, x1, y1, w);
	line(x1, y1, x0, y1, w);
	line(x0, y1, x0, y0, w);
}

static void store(int icon)
{
	u16 u, v;
	if (!font_atlas_alloc(S, S, &u, &v))
		return;
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++)
			font_atlas[(size_t)(v + y) * ATLAS_SIZE + u + x] = (u8)lrintf(cov[y * S + x] * 255.0f);
	gpu_atlas_dirty(u, v, S, S);
	icon_u[icon] = u;
	icon_v[icon] = v;
	icon_px[icon] = (u8)S;
}

static void begin(int size)
{
	S = size;
	memset(cov, 0, sizeof cov);
}

static void art_files(float W)
{
	const float PI = 3.14159265f;
	(void)PI;
	begin((int)(24.0f * G + 0.5f));
	line(8.5f, 3.5f, 15.5f, 3.5f, W);
	line(15.5f, 3.5f, 19.5f, 7.5f, W);
	line(19.5f, 7.5f, 19.5f, 17.5f, W);
	line(19.5f, 17.5f, 8.5f, 17.5f, W);
	line(8.5f, 17.5f, 8.5f, 3.5f, W);
	line(15.5f, 3.5f, 15.5f, 7.5f, W);
	line(15.5f, 7.5f, 19.5f, 7.5f, W);
	line(5.5f, 7.5f, 5.5f, 20.5f, W);
	line(5.5f, 20.5f, 15.5f, 20.5f, W);
}

static void art_search(float W)
{
	const float PI = 3.14159265f;
	(void)PI;
	begin((int)(24.0f * G + 0.5f));
	ring(14.0f, 10.0f, 5.5f, W);
	line(10.0f, 14.0f, 4.5f, 19.5f, 2.0f);
}

static void art_scm(float W)
{
	const float PI = 3.14159265f;
	(void)PI;
	begin((int)(24.0f * G + 0.5f));
	ring(7.5f, 5.5f, 2.2f, W);
	ring(7.5f, 18.5f, 2.2f, W);
	ring(16.5f, 8.5f, 2.2f, W);
	line(7.5f, 7.8f, 7.5f, 16.2f, W);
	arc(11.0f, 13.5f, 5.5f, -0.05f * PI, 0.55f * PI, W);
	line(16.5f, 10.8f, 16.5f, 13.0f, W);
}

/* an open book, square like everything else */
static void art_book(float W)
{
	begin((int)(24.0f * G + 0.5f));
	line(3.5f, 5.5f, 10.5f, 5.5f, W);
	line(10.5f, 5.5f, 12.0f, 7.0f, W);
	line(12.0f, 7.0f, 13.5f, 5.5f, W);
	line(13.5f, 5.5f, 20.5f, 5.5f, W);
	line(20.5f, 5.5f, 20.5f, 18.5f, W);
	line(20.5f, 18.5f, 13.5f, 18.5f, W);
	line(13.5f, 18.5f, 12.0f, 20.0f, W);
	line(12.0f, 20.0f, 10.5f, 18.5f, W);
	line(10.5f, 18.5f, 3.5f, 18.5f, W);
	line(3.5f, 18.5f, 3.5f, 5.5f, W);
	line(12.0f, 7.0f, 12.0f, 20.0f, W);
}

static void art_pulse(float W)
{
	begin((int)(24.0f * G + 0.5f));
	line(2.5f, 12.5f, 7.5f, 12.5f, W);
	line(7.5f, 12.5f, 10.0f, 5.0f, W);
	line(10.0f, 5.0f, 14.0f, 19.5f, W);
	line(14.0f, 19.5f, 16.5f, 12.5f, W);
	line(16.5f, 12.5f, 21.5f, 12.5f, W);
}

static void art_play(float W)
{
	begin((int)(24.0f * G + 0.5f));
	line(7.0f, 5.0f, 7.0f, 19.0f, W);
	line(7.0f, 5.0f, 18.5f, 12.0f, W);
	line(7.0f, 19.0f, 18.5f, 12.0f, W);
}

/* ucas: a sigma */
static void art_sigma(float W)
{
	begin((int)(24.0f * G + 0.5f));
	line(18.0f, 5.0f, 6.0f, 5.0f, W);
	line(6.0f, 5.0f, 12.5f, 12.0f, W);
	line(12.5f, 12.0f, 6.0f, 19.0f, W);
	line(6.0f, 19.0f, 18.0f, 19.0f, W);
}

/* the LLM agent: a speech bubble with two lines of text */
static void art_chat(float W)
{
	begin((int)(24.0f * G + 0.5f));
	line(3.5f, 4.5f, 20.5f, 4.5f, W);
	line(20.5f, 4.5f, 20.5f, 16.0f, W);
	line(20.5f, 16.0f, 11.0f, 16.0f, W);
	line(11.0f, 16.0f, 6.5f, 20.5f, W);
	line(6.5f, 20.5f, 6.5f, 16.0f, W);
	line(6.5f, 16.0f, 3.5f, 16.0f, W);
	line(3.5f, 16.0f, 3.5f, 4.5f, W);
	line(7.5f, 8.5f, 16.5f, 8.5f, W);
	line(7.5f, 12.0f, 13.5f, 12.0f, W);
}

static void art_gear(float W)
{
	const float PI = 3.14159265f;
	(void)PI;
	begin((int)(24.0f * G + 0.5f));
	ring(12, 12, 5.5f, W);
	ring(12, 12, 2.0f, W);
	for (int i = 0; i < 8; i++) {
		float a = i * PI / 4;
		line(12 + 5.5f * cosf(a), 12 + 5.5f * sinf(a), 12 + 8.5f * cosf(a), 12 + 8.5f * sinf(a), 2.4f);
	}
}

void icons_init(void)
{
	const float PI = 3.14159265f;
	const float w = 1.0f; /* stroke for 16 px icons */

	/* the activity icons at 24 px, and at 16 px for the top-level tabs */
	for (int pass = 0; pass < 2; pass++) {
		int sz = pass ? 16 : 24;
		G = (float)sz / 24.0f;
		float W = pass ? 1.6f : 1.5f; /* strokes in 24 px units */
		art_files(W);
		store(pass ? ICON_FILES_S : ICON_FILES);
		art_search(W);
		store(pass ? ICON_SEARCH_S : ICON_SEARCH);
		art_scm(W);
		store(pass ? ICON_SCM_S : ICON_SCM);
		art_gear(W);
		store(pass ? ICON_GEAR_S : ICON_GEAR);
		if (pass) {
			art_book(W);
			store(ICON_BOOK_S);
			art_pulse(W);
			store(ICON_PULSE_S);
			art_play(W);
			store(ICON_PLAY_S);
			art_sigma(W);
			store(ICON_SIGMA_S);
			art_chat(W);
			store(ICON_CHAT_S);
		}
	}
	G = 1.0f;

	begin(16);
	line(6.0f, 4.0f, 10.0f, 8.0f, w + 0.2f);
	line(10.0f, 8.0f, 6.0f, 12.0f, w + 0.2f);
	store(ICON_CHEVRON_RIGHT);

	/* debugger toolbar */
	begin(16); /* continue: a filled triangle */
	for (float k = 0; k < 4.5f; k += 0.5f)
		line(4.5f + k, 3.5f + k * 0.78f, 4.5f + k, 12.5f - k * 0.78f, 1.0f);
	line(4.5f, 3.5f, 12.5f, 8.0f, 1.2f);
	line(4.5f, 12.5f, 12.5f, 8.0f, 1.2f);
	store(ICON_DBG_CONTINUE);
	begin(16); /* pause: two bars */
	line(5.0f, 3.5f, 5.0f, 12.5f, 2.2f);
	line(11.0f, 3.5f, 11.0f, 12.5f, 2.2f);
	store(ICON_DBG_PAUSE);
	begin(16); /* step over: an arc over a dot */
	ring(8.0f, 8.5f, 5.0f, 1.2f);
	for (int yy = 0; yy < S; yy++) /* keep the top half of the ring */
		for (int xx = 0; xx < S; xx++)
			if (yy > 8)
				cov[yy * S + xx] = 0;
	line(13.0f, 8.5f, 13.0f, 5.0f, 1.2f);
	line(13.0f, 8.5f, 10.0f, 8.0f, 1.2f);
	disc(8.0f, 13.0f, 1.6f);
	store(ICON_DBG_STEP_OVER);
	begin(16); /* step into: an arrow down to a dot */
	line(8.0f, 2.5f, 8.0f, 9.5f, 1.2f);
	line(5.0f, 7.0f, 8.0f, 10.0f, 1.2f);
	line(11.0f, 7.0f, 8.0f, 10.0f, 1.2f);
	disc(8.0f, 13.5f, 1.6f);
	store(ICON_DBG_STEP_INTO);
	begin(16); /* step out: an arrow up from a dot */
	line(8.0f, 3.0f, 8.0f, 10.0f, 1.2f);
	line(5.0f, 6.0f, 8.0f, 3.0f, 1.2f);
	line(11.0f, 6.0f, 8.0f, 3.0f, 1.2f);
	disc(8.0f, 13.5f, 1.6f);
	store(ICON_DBG_STEP_OUT);
	begin(16); /* stop: a square */
	for (float k = 4.0f; k <= 12.0f; k += 1.0f)
		line(4.0f, k, 12.0f, k, 1.2f);
	store(ICON_DBG_STOP);
	begin(16); /* restart: an open ring with an arrow head */
	ring(8.0f, 8.0f, 5.0f, 1.2f);
	for (int yy = 0; yy < 7; yy++)
		for (int xx = 9; xx < S; xx++)
			cov[yy * S + xx] = 0;
	line(9.0f, 3.0f, 12.5f, 3.0f, 1.2f);
	line(12.5f, 3.0f, 12.5f, 6.5f, 1.2f);
	store(ICON_DBG_RESTART);

	begin(16);
	line(4.0f, 6.0f, 8.0f, 10.0f, w + 0.2f);
	line(8.0f, 10.0f, 12.0f, 6.0f, w + 0.2f);
	store(ICON_CHEVRON_DOWN);

	begin(16);
	line(4.0f, 4.0f, 12.0f, 12.0f, w + 0.2f);
	line(12.0f, 4.0f, 4.0f, 12.0f, w + 0.2f);
	store(ICON_CLOSE);

	begin(16);
	disc(8, 8, 4.0f);
	store(ICON_DOT);

	begin(16);
	ring(8, 8, 6.0f, w);
	line(5.5f, 5.5f, 10.5f, 10.5f, w);
	line(10.5f, 5.5f, 5.5f, 10.5f, w);
	store(ICON_ERROR);

	begin(16);
	line(8.0f, 2.0f, 14.5f, 14.0f, w);
	line(14.5f, 14.0f, 1.5f, 14.0f, w);
	line(1.5f, 14.0f, 8.0f, 2.0f, w);
	line(8.0f, 6.0f, 8.0f, 10.0f, w + 0.3f);
	disc(8.0f, 12.0f, 0.8f);
	store(ICON_WARNING);

	begin(16);
	ring(8, 8, 6.0f, w);
	line(8.0f, 7.0f, 8.0f, 11.5f, w + 0.3f);
	disc(8.0f, 4.8f, 0.8f);
	store(ICON_INFO);

	begin(16);
	line(3.0f, 8.5f, 6.5f, 12.0f, w + 0.2f);
	line(6.5f, 12.0f, 13.0f, 4.5f, w + 0.2f);
	store(ICON_CHECK);

	begin(16);
	line(8.0f, 3.0f, 8.0f, 13.0f, w + 0.2f);
	line(3.0f, 8.0f, 13.0f, 8.0f, w + 0.2f);
	store(ICON_PLUS);

	begin(16);
	line(3.0f, 8.0f, 13.0f, 8.0f, w + 0.2f);
	store(ICON_MINUS);

	/* Discard: arrow curling back to the left. */
	begin(16);
	arc(9.0f, 9.0f, 4.5f, -0.5f * PI, 0.75f * PI, w);
	line(9.0f, 4.5f, 3.5f, 4.5f, w);
	line(3.5f, 4.5f, 6.0f, 2.0f, w);
	line(3.5f, 4.5f, 6.0f, 7.0f, w);
	store(ICON_DISCARD);

	begin(16);
	arc(8.0f, 8.5f, 5.0f, -0.35f * PI, 1.5f * PI, w);
	line(11.0f, 4.5f, 12.5f, 1.8f, w);
	line(11.0f, 4.5f, 14.0f, 5.2f, w);
	store(ICON_REFRESH);

	begin(16);
	ring(5.0f, 3.5f, 1.6f, w);
	ring(5.0f, 12.5f, 1.6f, w);
	ring(11.5f, 5.5f, 1.6f, w);
	line(5.0f, 5.1f, 5.0f, 10.9f, w);
	arc(7.5f, 9.0f, 4.0f, -0.05f * PI, 0.55f * PI, w);
	store(ICON_BRANCH);

	begin(16);
	box(5.5f, 2.5f, 13.5f, 10.5f, w);
	line(2.5f, 5.5f, 2.5f, 13.5f, w);
	line(2.5f, 13.5f, 10.5f, 13.5f, w);
	line(7.5f, 6.5f, 11.5f, 6.5f, w);
	store(ICON_COLLAPSE);

	begin(16);
	line(3.5f, 1.5f, 9.5f, 1.5f, w);
	line(9.5f, 1.5f, 12.5f, 4.5f, w);
	line(12.5f, 4.5f, 12.5f, 8.0f, w);
	line(3.5f, 1.5f, 3.5f, 14.5f, w);
	line(3.5f, 14.5f, 8.0f, 14.5f, w);
	line(12.0f, 10.0f, 12.0f, 15.0f, w);
	line(9.5f, 12.5f, 14.5f, 12.5f, w);
	store(ICON_NEW_FILE);

	begin(16);
	line(1.5f, 3.5f, 6.0f, 3.5f, w);
	line(6.0f, 3.5f, 7.5f, 5.0f, w);
	line(7.5f, 5.0f, 14.5f, 5.0f, w);
	line(14.5f, 5.0f, 14.5f, 8.0f, w);
	line(1.5f, 3.5f, 1.5f, 13.5f, w);
	line(1.5f, 13.5f, 8.0f, 13.5f, w);
	line(12.0f, 9.5f, 12.0f, 14.5f, w);
	line(9.5f, 12.0f, 14.5f, 12.0f, w);
	store(ICON_NEW_FOLDER);

	begin(16);
	line(8.0f, 3.0f, 8.0f, 13.0f, w);
	line(8.0f, 3.0f, 4.0f, 7.0f, w);
	line(8.0f, 3.0f, 12.0f, 7.0f, w);
	store(ICON_ARROW_UP);

	begin(16);
	line(8.0f, 3.0f, 8.0f, 13.0f, w);
	line(8.0f, 13.0f, 4.0f, 9.0f, w);
	line(8.0f, 13.0f, 12.0f, 9.0f, w);
	store(ICON_ARROW_DOWN);

	begin(16);
	box(2.5f, 2.5f, 13.5f, 13.5f, w);
	line(8.0f, 2.5f, 8.0f, 13.5f, w);
	store(ICON_SPLIT);
}

int icon_size(int icon) { return icon_px[icon]; }

void draw_icon(int icon, int x, int y, u32 c)
{
	int s = icon_px[icon];
	if (!s)
		return;
	draw_atlas_rect(x, y, s, s, icon_u[icon], icon_v[icon], c);
}
