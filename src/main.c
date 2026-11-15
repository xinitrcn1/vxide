/* vxide entry point: SDL3 window + event loop, or a headless scripted run
 * (offscreen Vulkan target, PNG screenshots) for tests. */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "png.h"

#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <SDL3/SDL.h>

static void sleep_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

static int map_mods(SDL_Keymod m)
{
	int r = 0;
	if (m & SDL_KMOD_CTRL)
		r |= MOD_CTRL;
	if (m & SDL_KMOD_SHIFT)
		r |= MOD_SHIFT;
	if (m & SDL_KMOD_ALT)
		r |= MOD_ALT;
	if (m & SDL_KMOD_GUI)
		r |= MOD_SUPER;
	return r;
}

static float density = 1.0f;

/* System cursors for the UI's cursor shapes (CURSOR_*). */
static void apply_cursor(int shape)
{
	static SDL_Cursor *cur[5];
	static int last = -1;
	static const SDL_SystemCursor ids[5] = {SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_TEXT,
	                                        SDL_SYSTEM_CURSOR_POINTER, SDL_SYSTEM_CURSOR_EW_RESIZE,
	                                        SDL_SYSTEM_CURSOR_NS_RESIZE};
	if (shape == last || shape < 0 || shape >= 5)
		return;
	if (!cur[shape])
		cur[shape] = SDL_CreateSystemCursor(ids[shape]);
	if (cur[shape])
		SDL_SetCursor(cur[shape]);
	last = shape;
}

static bool translate(const SDL_Event *s, Event *e)
{
	memset(e, 0, sizeof *e);
	switch (s->type) {
	case SDL_EVENT_QUIT:
		e->type = EV_QUIT;
		return true;
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		e->type = EV_RESIZE;
		e->x = s->window.data1;
		e->y = s->window.data2;
		return true;
	case SDL_EVENT_KEY_DOWN:
		e->type = EV_KEY;
		e->key = (u32)s->key.key;
		e->mods = map_mods(s->key.mod);
		return true;
	case SDL_EVENT_TEXT_INPUT:
		e->type = EV_TEXT;
		snprintf(e->text, sizeof e->text, "%s", s->text.text);
		return true;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		e->type = s->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? EV_MDOWN : EV_MUP;
		e->x = (int)(s->button.x * density);
		e->y = (int)(s->button.y * density);
		e->button = s->button.button == SDL_BUTTON_LEFT ? 1 : s->button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
		e->clicks = s->button.clicks;
		e->mods = map_mods(SDL_GetModState());
		return true;
	case SDL_EVENT_MOUSE_MOTION:
		e->type = EV_MMOVE;
		e->x = (int)(s->motion.x * density);
		e->y = (int)(s->motion.y * density);
		e->mods = map_mods(SDL_GetModState());
		return true;
	case SDL_EVENT_MOUSE_WHEEL: {
		e->type = EV_WHEEL;
		float mx, my;
		SDL_GetMouseState(&mx, &my);
		e->x = (int)(mx * density);
		e->y = (int)(my * density);
		e->wx = s->wheel.x;
		e->wy = s->wheel.y;
		e->mods = map_mods(SDL_GetModState());
		return true;
	}
	case SDL_EVENT_WINDOW_FOCUS_GAINED:
		e->type = EV_FOCUS;
		return true;
	default:
		return false;
	}
}

/* ---- headless script ------------------------------------------------------- */

static void send(const Event *e) { ui_event(e); }

static void pump_for(u64 ms)
{
	u64 end = vx_now_ns() + ms * 1000000ull;
	do {
		ui_tick();
		sleep_ms(5);
	} while (vx_now_ns() < end);
}

static bool parse_key(const char *spec, Event *e)
{
	memset(e, 0, sizeof *e);
	e->type = EV_KEY;
	char buf[64];
	snprintf(buf, sizeof buf, "%s", spec);
	char *p = buf;
	for (;;) {
		char *plus = strchr(p, '+');
		if (!plus || plus[1] == 0)
			break;
		*plus = 0;
		if (!strcasecmp(p, "ctrl"))
			e->mods |= MOD_CTRL;
		else if (!strcasecmp(p, "shift"))
			e->mods |= MOD_SHIFT;
		else if (!strcasecmp(p, "alt"))
			e->mods |= MOD_ALT;
		p = plus + 1;
	}
	SDL_Keycode k = SDL_GetKeyFromName(p);
	if (k == SDLK_UNKNOWN) {
		if (strlen(p) == 1)
			k = (SDL_Keycode)(unsigned char)(p[0] >= 'A' && p[0] <= 'Z' ? p[0] + 32 : p[0]);
		else {
			fprintf(stderr, "vxide: unknown key '%s'\n", p);
			return false;
		}
	}
	e->key = (u32)k;
	return true;
}

static void unescape(char *s)
{
	char *d = s;
	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			*d++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
		} else {
			*d++ = *s;
		}
	}
	*d = 0;
}

static int frame_shot(const char *path)
{
	ui_draw();
	u8 *px = vx_alloc((size_t)app.w * app.h * 4);
	int r = -1;
	if (gpu_readback(px, app.w, app.h))
		r = png_write(path, px, app.w, app.h);
	free(px);
	if (r == 0)
		fprintf(stderr, "vxide: wrote %s\n", path);
	return r;
}

static int run_script(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "vxide: cannot open script %s\n", path);
		return 1;
	}
	char line[4096];
	int rc = 0;
	while (fgets(line, sizeof line, f) && app.running) {
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s || *s == '#')
			continue;
		char *arg = strchr(s, ' ');
		if (arg)
			*arg++ = 0;
		else
			arg = s + strlen(s);
		Event e;
		if (!strcmp(s, "key")) {
			/* several keys separated by spaces */
			char *tok = strtok(arg, " ");
			while (tok) {
				if (parse_key(tok, &e))
					send(&e);
				ui_tick();
				tok = strtok(NULL, " ");
			}
		} else if (!strcmp(s, "type")) {
			unescape(arg);
			/* one event per character, newlines become Enter */
			for (char *p = arg; *p;) {
				if (*p == '\n') {
					parse_key("Return", &e);
					send(&e);
					p++;
					continue;
				}
				if (*p == '\t') {
					parse_key("Tab", &e);
					send(&e);
					p++;
					continue;
				}
				u32 cp;
				u32 k = utf8_decode(p, strlen(p), &cp);
				memset(&e, 0, sizeof e);
				e.type = EV_TEXT;
				memcpy(e.text, p, k);
				send(&e);
				p += k;
			}
		} else if (!strcmp(s, "rclick")) {
			int x = 0, y = 0;
			sscanf(arg, "%d %d", &x, &y);
			memset(&e, 0, sizeof e);
			e.x = x;
			e.y = y;
			e.type = EV_MMOVE;
			send(&e);
			e.type = EV_MDOWN;
			e.button = 3;
			e.clicks = 1;
			send(&e);
			e.type = EV_MUP;
			send(&e);
		} else if (!strcmp(s, "click") || !strcmp(s, "dclick") || !strcmp(s, "move") ||
		           !strcmp(s, "drag")) {
			int x = 0, y = 0, x2 = 0, y2 = 0;
			char mods[32] = "";
			sscanf(arg, "%d %d %d %d", &x, &y, &x2, &y2);
			if (strstr(arg, "ctrl"))
				snprintf(mods, sizeof mods, "ctrl");
			memset(&e, 0, sizeof e);
			e.x = x;
			e.y = y;
			e.type = EV_MMOVE;
			send(&e);
			if (strcmp(s, "move") != 0) {
				e.type = EV_MDOWN;
				e.button = 1;
				e.clicks = 1;
				e.mods = strstr(arg, "ctrl") ? MOD_CTRL : strstr(arg, "alt") ? MOD_ALT : strstr(arg, "shift") ? MOD_SHIFT : 0;
				send(&e);
				if (!strcmp(s, "drag")) {
					e.type = EV_MMOVE;
					e.x = x2;
					e.y = y2;
					send(&e);
				}
				e.type = EV_MUP;
				send(&e);
				if (!strcmp(s, "dclick")) {
					e.type = EV_MDOWN;
					e.clicks = 2;
					send(&e);
					e.type = EV_MUP;
					send(&e);
				}
			}
		} else if (!strcmp(s, "wheel")) {
			int x = 0, y = 0;
			float dy = 0;
			sscanf(arg, "%f %d %d", &dy, &x, &y);
			memset(&e, 0, sizeof e);
			e.type = EV_WHEEL;
			e.x = x;
			e.y = y;
			e.wy = dy;
			send(&e);
		} else if (!strcmp(s, "wait")) {
			pump_for((u64)atoi(arg));
		} else if (!strcmp(s, "waitlsp")) {
			/* until the server is ready and has published something */
			u64 end = vx_now_ns() + (u64)(*arg ? atoi(arg) : 20000) * 1000000ull;
			while (vx_now_ns() < end) {
				ui_tick();
				if ((app.lsps_ready[LSV_CLANGD] || app.lsps_ready[LSV_RUST]) && problems_count(0) + app.docs.sn[0] > 0)
					break;
				sleep_ms(10);
			}
			pump_for(300);
		} else if (!strcmp(s, "open")) {
			open_rel(arg, false, UINT32_MAX, 0);
		} else if (!strcmp(s, "shot")) {
			ui_tick();
			if (frame_shot(arg) != 0)
				rc = 1;
		} else if (!strcmp(s, "resize")) {
			int w = 0, h = 0;
			sscanf(arg, "%d %d", &w, &h);
			gpu_resize(w, h);
			memset(&e, 0, sizeof e);
			e.type = EV_RESIZE;
			e.x = w;
			e.y = h;
			send(&e);
		} else if (!strcmp(s, "theme")) {
			app.theme = !strcmp(arg, "light") ? THEME_LIGHT : THEME_DARK;
		} else if (!strcmp(s, "expect")) {
			/* expect status|cursor|text substrings for scripted checks */
			View *v = active_view();
			if (!strncmp(arg, "text ", 5) && v) {
				const char *want = arg + 5;
				char w2[1024];
				snprintf(w2, sizeof w2, "%s", want);
				unescape(w2);
				if (!strstr(text_flat(&app.docs.text[v->doc]), w2)) {
					fprintf(stderr, "vxide: expect text failed: %s\n", want);
					rc = 1;
				}
			} else if (!strncmp(arg, "line ", 5) && v) {
				u32 want = (u32)atoi(arg + 5);
				u32 got = text_line_of(&app.docs.text[v->doc], v->head[0]) + 1;
				if (got != want) {
					fprintf(stderr, "vxide: expect line %u, got %u\n", want, got);
					rc = 1;
				}
			} else if (!strncmp(arg, "problems ", 9)) {
				u32 want = (u32)atoi(arg + 9);
				if (problems_count(0) < want) {
					fprintf(stderr, "vxide: expect >= %u problems, got %u\n", want, problems_count(0));
					rc = 1;
				}
			} else if (!strncmp(arg, "file ", 5) && v) {
				if (!strstr(app.docs.rel[v->doc], arg + 5)) {
					fprintf(stderr, "vxide: expect file %s, got %s\n", arg + 5, app.docs.rel[v->doc]);
					rc = 1;
				}
			}
		} else if (!strcmp(s, "print")) {
			View *v = active_view();
			if (v) {
				Text *t = &app.docs.text[v->doc];
				fprintf(stderr, "--- %s (cursor %u:%u, %u cursors)\n%s\n---\n", app.docs.rel[v->doc],
				        text_line_of(t, v->head[0]) + 1, v->head[0] - text_line_start(t, text_line_of(t, v->head[0])) + 1,
				        v->nc, text_flat(t));
			}
		} else {
			fprintf(stderr, "vxide: unknown script command '%s'\n", s);
			rc = 1;
		}
	}
	fclose(f);
	return rc;
}

static void usage(void)
{
	fprintf(stderr, "usage: vxide [--scale N] [--light] [--headless WxH --script FILE] [DIR]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	/* towupper/towlower for the case transforms need a UTF-8 ctype */
	if (!setlocale(LC_CTYPE, "C.UTF-8"))
		setlocale(LC_CTYPE, "");
	const char *dir = ".";
	const char *script = NULL;
	int hw = 0, hh = 0;
	int scale = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--headless") && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &hw, &hh) != 2)
				usage();
		} else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
			script = argv[++i];
		} else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
			scale = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--light")) {
			setenv("VXIDE_THEME", "light", 1);
		} else if (argv[i][0] == '-') {
			usage();
		} else {
			dir = argv[i];
		}
	}
	char root[PATH_MAX];
	if (!realpath(dir, root)) {
		fprintf(stderr, "vxide: %s: no such directory\n", dir);
		return 1;
	}
	bool headless = hw > 0;
	SDL_Window *win = NULL;
	int w = hw, h = hh;
	if (!headless) {
		if (!SDL_Init(SDL_INIT_VIDEO)) {
			fprintf(stderr, "vxide: SDL_Init: %s\n", SDL_GetError());
			return 1;
		}
		char title[PATH_MAX + 16];
		const char *base = strrchr(root, '/');
		snprintf(title, sizeof title, "%s - vxide", base && base[1] ? base + 1 : root);
		win = SDL_CreateWindow(title, 1280, 800,
		                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
		                           SDL_WINDOW_MAXIMIZED);
		if (!win) {
			fprintf(stderr, "vxide: SDL_CreateWindow: %s\n", SDL_GetError());
			return 1;
		}
		SDL_GetWindowSizeInPixels(win, &w, &h);
		density = SDL_GetWindowPixelDensity(win);
		if (!scale) {
			float ds = SDL_GetWindowDisplayScale(win);
			scale = ds >= 1.75f ? 2 : 1;
		}
		SDL_StartTextInput(win);
	}
	if (!scale)
		scale = 1;
	if (!gpu_init(win, w, h)) {
		fprintf(stderr, "vxide: Vulkan initialisation failed\n");
		return 1;
	}
	if (!ui_init(root, w, h, scale, headless)) {
		fprintf(stderr, "vxide: UI initialisation failed (fonts?)\n");
		return 1;
	}
	ui_log("vxide: %s on %s", root, gpu_device_name());
	int rc = 0;
	if (headless) {
		if (script)
			rc = run_script(script);
		else
			rc = frame_shot("vxide.png");
	} else {
		ui_draw();
		u64 last_blink = 0;
		while (app.running) {
			SDL_Event se;
			u64 wait = ui_wait_hint_ms();
			if (app.focus == FOCUS_EDITOR || app.focus == FOCUS_FIND || app.focus == FOCUS_QUICK)
				wait = VX_MIN(wait, 530ull);
			if (SDL_WaitEventTimeout(&se, (Sint32)wait)) {
				do {
					Event e;
					if (translate(&se, &e)) {
						if (e.type == EV_RESIZE)
							gpu_resize(e.x, e.y);
						ui_event(&e);
					}
				} while (SDL_PollEvent(&se));
			}
			ui_tick();
			apply_cursor(app.cursor);
			u64 blink = (vx_now_ns() - app.blink_t0) / 530000000ull;
			if (blink != last_blink) {
				last_blink = blink;
				app.dirty = true;
			}
			if (app.dirty)
				ui_draw();
		}
	}
	ui_shutdown();
	gpu_shutdown();
	if (win)
		SDL_DestroyWindow(win);
	if (!headless)
		SDL_Quit();
	return rc;
}
