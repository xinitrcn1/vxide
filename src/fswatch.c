#define _DEFAULT_SOURCE
#include "fswatch.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || \
    defined(__APPLE__)
#define USE_KQUEUE 1
#include <sys/event.h>
#elif defined(__linux__)
#define USE_INOTIFY 1
#include <sys/inotify.h>
#endif

#define MAX_WATCH 2048

struct FsWatch {
	int q; /* kqueue / inotify descriptor */
	u32 n;
	u32 tag[MAX_WATCH];
	int fd[MAX_WATCH]; /* kqueue: open fd per path; inotify: wd */
	u64 mtime[MAX_WATCH]; /* poll backend */
	char *path[MAX_WATCH];
	u64 last_poll;
};

const char *fsw_backend(void)
{
#if USE_KQUEUE
	return "kqueue";
#elif USE_INOTIFY
	return "inotify";
#else
	return "poll";
#endif
}

FsWatch *fsw_open(void)
{
	FsWatch *w = vx_alloc(sizeof *w);
#if USE_KQUEUE
	w->q = kqueue();
#elif USE_INOTIFY
	w->q = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#else
	w->q = -1;
#endif
	return w;
}

static int find(const FsWatch *w, u32 tag)
{
	for (u32 i = 0; i < w->n; i++)
		if (w->tag[i] == tag)
			return (int)i;
	return -1;
}

bool fsw_has(const FsWatch *w, u32 tag) { return find(w, tag) >= 0; }

static u64 mtime_of(const char *p)
{
	struct stat st;
	if (stat(p, &st) != 0)
		return 0;
	return (u64)st.st_mtim.tv_sec * 1000000000ull + (u64)st.st_mtim.tv_nsec;
}

static void drop(FsWatch *w, u32 i)
{
#if USE_KQUEUE
	if (w->fd[i] >= 0)
		close(w->fd[i]); /* closing removes the kevent */
#elif USE_INOTIFY
	if (w->fd[i] >= 0)
		inotify_rm_watch(w->q, w->fd[i]);
#endif
	free(w->path[i]);
	u32 last = --w->n;
	w->tag[i] = w->tag[last];
	w->fd[i] = w->fd[last];
	w->mtime[i] = w->mtime[last];
	w->path[i] = w->path[last];
}

void fsw_remove(FsWatch *w, u32 tag)
{
	int i = find(w, tag);
	if (i >= 0)
		drop(w, (u32)i);
}

int fsw_add(FsWatch *w, const char *path, u32 tag)
{
	fsw_remove(w, tag);
	if (w->n == MAX_WATCH)
		return -1;
	u32 i = w->n;
	w->tag[i] = tag;
	w->fd[i] = -1;
	w->path[i] = vx_strdup(path);
	w->mtime[i] = 0;
#if USE_KQUEUE
	if (w->q >= 0) {
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			free(w->path[i]);
			return -1;
		}
		struct kevent ev;
		EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
		       NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB, 0, (void *)(uintptr_t)tag);
		if (kevent(w->q, &ev, 1, NULL, 0, NULL) != 0) {
			close(fd);
			free(w->path[i]);
			return -1;
		}
		w->fd[i] = fd;
	}
#elif USE_INOTIFY
	if (w->q >= 0) {
		int wd = inotify_add_watch(w->q, path,
		                           IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
		                               IN_DELETE_SELF | IN_MOVE_SELF);
		if (wd < 0) {
			free(w->path[i]);
			return -1;
		}
		w->fd[i] = wd;
	}
#endif
	if (w->q < 0)
		w->mtime[i] = mtime_of(path);
	w->n++;
	return 0;
}

static u32 push_tag(u32 *tags, u32 n, u32 max, u32 tag)
{
	for (u32 k = 0; k < n; k++)
		if (tags[k] == tag)
			return n;
	if (n < max)
		tags[n++] = tag;
	return n;
}

u32 fsw_poll(FsWatch *w, u32 *tags, u32 max)
{
	u32 n = 0;
#if USE_KQUEUE
	if (w->q >= 0) {
		struct kevent ev[64];
		struct timespec zero = {0, 0};
		int k;
		while ((k = kevent(w->q, NULL, 0, ev, 64, &zero)) > 0)
			for (int e = 0; e < k; e++)
				n = push_tag(tags, n, max, (u32)(uintptr_t)ev[e].udata);
		return n;
	}
#elif USE_INOTIFY
	if (w->q >= 0) {
		_Alignas(struct inotify_event) char buf[8192];
		ssize_t len;
		while ((len = read(w->q, buf, sizeof buf)) > 0) {
			for (char *p = buf; p < buf + len;) {
				struct inotify_event *ie = (struct inotify_event *)p;
				for (u32 i = 0; i < w->n; i++)
					if (w->fd[i] == ie->wd)
						n = push_tag(tags, n, max, w->tag[i]);
				p += sizeof *ie + ie->len;
			}
		}
		return n;
	}
#endif
	/* poll backend: one stat per watched path, at most every 2 s */
	u64 now = vx_now_ns();
	if (now - w->last_poll < 2000000000ull)
		return 0;
	w->last_poll = now;
	for (u32 i = 0; i < w->n; i++) {
		u64 m = mtime_of(w->path[i]);
		if (m != w->mtime[i]) {
			w->mtime[i] = m;
			n = push_tag(tags, n, max, w->tag[i]);
		}
	}
	return n;
}

int fsw_fd(const FsWatch *w) { return w->q; }

void fsw_close(FsWatch *w)
{
	if (!w)
		return;
	while (w->n)
		drop(w, w->n - 1);
	if (w->q >= 0)
		close(w->q);
	free(w);
}
