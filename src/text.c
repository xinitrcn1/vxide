#define _POSIX_C_SOURCE 200809L
#include "text.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *src_ptr(const Text *t, u32 p)
{
	return (t->psrc[p] == SRC_FILE ? t->file : t->add) + t->poff[p];
}

static void pieces_reserve(Text *t, u32 n)
{
	if (n <= t->pcap)
		return;
	u32 c = t->pcap ? t->pcap : 16;
	while (c < n)
		c *= 2;
	t->psrc = vx_realloc(t->psrc, c);
	t->poff = vx_realloc(t->poff, c * sizeof(u32));
	t->plen = vx_realloc(t->plen, c * sizeof(u32));
	t->pstart = vx_realloc(t->pstart, c * sizeof(u32));
	t->pcap = c;
}

static void undo_reserve(Text *t, u32 n)
{
	if (n <= t->ucap)
		return;
	u32 c = t->ucap ? t->ucap * 2 : 64;
	while (c < n)
		c *= 2;
	t->u_pos = vx_realloc(t->u_pos, c * sizeof(u32));
	t->u_del = vx_realloc(t->u_del, c * sizeof(u32));
	t->u_delo = vx_realloc(t->u_delo, c * sizeof(u32));
	t->u_ins = vx_realloc(t->u_ins, c * sizeof(u32));
	t->u_inso = vx_realloc(t->u_inso, c * sizeof(u32));
	t->u_group = vx_realloc(t->u_group, c * sizeof(u32));
	t->ucap = c;
}

void text_init(Text *t)
{
	memset(t, 0, sizeof *t);
	VX_GROW(t->ls, t->lcap, 1);
	t->ls[0] = 0;
	t->nl = 1;
	t->saved_upos = 0;
}

static void unmap(Text *t)
{
	if (t->file) {
		if (t->mapped)
			munmap((void *)t->file, t->file_len);
		else
			free((void *)t->file);
	}
	t->file = NULL;
	t->file_len = 0;
	t->mapped = false;
}

void text_free(Text *t)
{
	unmap(t);
	free(t->add);
	free(t->psrc);
	free(t->poff);
	free(t->plen);
	free(t->pstart);
	free(t->ls);
	free(t->u_pos);
	free(t->u_del);
	free(t->u_delo);
	free(t->u_ins);
	free(t->u_inso);
	free(t->u_group);
	pool_free(&t->upool);
	free(t->scratch);
	free(t->flat);
	memset(t, 0, sizeof *t);
}

static void index_lines(Text *t)
{
	t->nl = 1;
	t->ls[0] = 0;
	const char *b = t->file;
	size_t n = t->file_len;
	const char *p = b;
	while ((p = memchr(p, '\n', n - (size_t)(p - b))) != NULL) {
		p++;
		VX_GROW(t->ls, t->lcap, t->nl + 1);
		t->ls[t->nl++] = (u32)(p - b);
	}
	t->crlf = t->nl > 1 && t->ls[1] >= 2 && b[t->ls[1] - 2] == '\r';
}

static u64 stat_ns(const struct stat *st)
{
	return (u64)st->st_mtim.tv_sec * 1000000000ull + (u64)st->st_mtim.tv_nsec;
}

/* Map path as the file source and reset the table to one piece. */
static int load(Text *t, const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno == ENOENT ? 1 : -1;
	struct stat st;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return -1;
	}
	if ((u64)st.st_size >= 0xFFFFFFF0ull) {
		close(fd);
		errno = EFBIG;
		return -1;
	}
	/* Map the new source before dropping the old one. */
	const char *nf = NULL;
	size_t nlen = 0;
	bool nmapped = false;
	if (S_ISREG(st.st_mode) && st.st_size > 0) {
		void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (m == MAP_FAILED) {
			close(fd);
			return -1;
		}
		nf = m;
		nlen = (size_t)st.st_size;
		nmapped = true;
		/* start reading the whole file now, asynchronously: on a slow disk
		 * one sequential read beats a page fault per 4 KiB while scrolling */
		if (nlen <= (256u << 20))
			posix_madvise(m, nlen, POSIX_MADV_WILLNEED);
	} else if (!S_ISREG(st.st_mode)) {
		close(fd);
		fd = -1;
		char *b = vx_read_file(path, &nlen);
		if (!b)
			return -1;
		nf = b;
	}
	unmap(t);
	t->file = nf;
	t->file_len = nlen;
	t->mapped = nmapped;
	if (fd >= 0)
		close(fd);
	t->mtime_ns = stat_ns(&st);
	t->inode = (u64)st.st_ino;
	t->np = 0;
	if (t->file_len) {
		pieces_reserve(t, 1);
		t->psrc[0] = SRC_FILE;
		t->poff[0] = 0;
		t->plen[0] = (u32)t->file_len;
		t->pstart[0] = 0;
		t->np = 1;
	}
	t->len = (u32)t->file_len;
	t->add_n = 0;
	index_lines(t);
	t->version++;
	t->flat_ok = false;
	return 0;
}

int text_open(Text *t, const char *path)
{
	text_init(t);
	int r = load(t, path);
	if (r < 0)
		return -1;
	return 0;
}

bool text_changed_on_disk(const Text *t, const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return false;
	return stat_ns(&st) != t->mtime_ns || (u64)st.st_ino != t->inode;
}

void text_restat(Text *t, const char *path)
{
	struct stat st;
	if (stat(path, &st) == 0) {
		t->mtime_ns = stat_ns(&st);
		t->inode = (u64)st.st_ino;
	}
}

int text_save(Text *t, const char *path)
{
	char tmp[4096];
	if (snprintf(tmp, sizeof tmp, "%s.vxide~XXXXXX", path) >= (int)sizeof tmp)
		return -1;
	int fd = mkstemp(tmp);
	if (fd < 0)
		return -1;
	struct stat st;
	if (stat(path, &st) == 0)
		fchmod(fd, st.st_mode & 07777);
	else
		fchmod(fd, 0644);
	for (u32 p = 0; p < t->np; p++) {
		const char *s = src_ptr(t, p);
		u32 left = t->plen[p];
		while (left) {
			ssize_t w = write(fd, s, left);
			if (w < 0) {
				if (errno == EINTR)
					continue;
				close(fd);
				unlink(tmp);
				return -1;
			}
			s += w;
			left -= (u32)w;
		}
	}
	if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	/* Remap: the new file is exactly the buffer. Undo bytes live in the
	 * undo pool, so the add buffer can be dropped. */
	if (load(t, path) != 0)
		return -1;
	t->saved_upos = t->upos;
	return 0;
}

u32 text_line_of(const Text *t, u32 off)
{
	u32 lo = 0, hi = t->nl;
	while (hi - lo > 1) {
		u32 mid = (lo + hi) / 2;
		if (t->ls[mid] <= off)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

static u32 piece_of(const Text *t, u32 off)
{
	u32 lo = 0, hi = t->np;
	while (hi - lo > 1) {
		u32 mid = (lo + hi) / 2;
		if (t->pstart[mid] <= off)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

u8 text_byte(const Text *t, u32 off)
{
	if (off >= t->len)
		return 0;
	u32 p = piece_of(t, off);
	return (u8)src_ptr(t, p)[off - t->pstart[p]];
}

void text_copy(const Text *t, u32 off, u32 n, char *dst)
{
	if (!n)
		return;
	u32 p = piece_of(t, off);
	u32 in = off - t->pstart[p];
	while (n && p < t->np) {
		u32 k = VX_MIN(n, t->plen[p] - in);
		memcpy(dst, src_ptr(t, p) + in, k);
		dst += k;
		n -= k;
		p++;
		in = 0;
	}
}

u32 text_line_len(const Text *t, u32 line)
{
	u32 s = t->ls[line];
	u32 e = line + 1 < t->nl ? t->ls[line + 1] - 1 : t->len;
	if (e > s && text_byte(t, e - 1) == '\r')
		e--;
	return e - s;
}

const char *text_line(Text *t, u32 line, u32 *n)
{
	u32 s = t->ls[line];
	u32 len = text_line_len(t, line);
	*n = len;
	if (!len)
		return "";
	u32 p = piece_of(t, s);
	if (s + len <= t->pstart[p] + t->plen[p])
		return src_ptr(t, p) + (s - t->pstart[p]);
	VX_GROW(t->scratch, t->scap, len + 1);
	text_copy(t, s, len, t->scratch);
	return t->scratch;
}

const char *text_flat(Text *t)
{
	if (t->flat_ok && t->flat_version == t->version)
		return t->flat;
	t->flat = vx_realloc(t->flat, t->len + 1);
	text_copy(t, 0, t->len, t->flat);
	t->flat[t->len] = 0;
	t->flat_ok = true;
	t->flat_version = t->version;
	return t->flat;
}

const char *text_eol(const Text *t) { return t->crlf ? "\r\n" : "\n"; }

/* Split so that a piece boundary sits at off; returns the index of the
 * piece starting at off (np when off == len). */
static u32 split_at(Text *t, u32 off)
{
	if (off >= t->len)
		return t->np;
	u32 p = piece_of(t, off);
	u32 in = off - t->pstart[p];
	if (in == 0)
		return p;
	pieces_reserve(t, t->np + 1);
	u32 tail = t->np - p - 1;
	memmove(t->psrc + p + 2, t->psrc + p + 1, tail);
	memmove(t->poff + p + 2, t->poff + p + 1, tail * sizeof(u32));
	memmove(t->plen + p + 2, t->plen + p + 1, tail * sizeof(u32));
	memmove(t->pstart + p + 2, t->pstart + p + 1, tail * sizeof(u32));
	t->psrc[p + 1] = t->psrc[p];
	t->poff[p + 1] = t->poff[p] + in;
	t->plen[p + 1] = t->plen[p] - in;
	t->pstart[p + 1] = off;
	t->plen[p] = in;
	t->np++;
	return p + 1;
}

static void edit_pieces(Text *t, u32 pos, u32 del, const char *ins, u32 nins)
{
	u32 a = split_at(t, pos);
	u32 b = split_at(t, pos + del);
	/* split_at(pos + del) may have shifted nothing before a. Pieces [a, b) go. */
	u32 add_piece = 0;
	if (nins) {
		/* Extend the previous piece when it ends where the add buffer ends
		 * (plain typing): no new piece per keystroke. */
		if (a > 0 && t->psrc[a - 1] == SRC_ADD && t->poff[a - 1] + t->plen[a - 1] == t->add_n) {
			VX_GROW(t->add, t->add_cap, t->add_n + nins);
			memcpy(t->add + t->add_n, ins, nins);
			t->add_n += nins;
			t->plen[a - 1] += nins;
		} else {
			add_piece = 1;
		}
	}
	u32 removed = b - a;
	i64 delta = (i64)add_piece - (i64)removed;
	if (delta > 0)
		pieces_reserve(t, t->np + (u32)delta);
	u32 tail = t->np - b;
	u32 dst = a + add_piece;
	memmove(t->psrc + dst, t->psrc + b, tail);
	memmove(t->poff + dst, t->poff + b, tail * sizeof(u32));
	memmove(t->plen + dst, t->plen + b, tail * sizeof(u32));
	t->np = dst + tail;
	if (add_piece) {
		VX_GROW(t->add, t->add_cap, t->add_n + nins);
		memcpy(t->add + t->add_n, ins, nins);
		t->psrc[a] = SRC_ADD;
		t->poff[a] = t->add_n;
		t->plen[a] = nins;
		t->add_n += nins;
	}
	/* Recompute prefix sums from the first touched piece. */
	u32 from = a > 0 ? a - 1 : 0;
	u32 s = from > 0 ? t->pstart[from - 1] + t->plen[from - 1] : 0;
	for (u32 p = from; p < t->np; p++) {
		t->pstart[p] = s;
		s += t->plen[p];
	}
}

static void edit_lines(Text *t, u32 pos, u32 del, const char *ins, u32 nins)
{
	/* Line starts strictly inside (pos, pos + del] vanish. */
	u32 first = text_line_of(t, pos) + 1; /* first start > pos */
	u32 last = first;
	while (last < t->nl && t->ls[last] <= pos + del)
		last++;
	u32 add = 0;
	for (u32 i = 0; i < nins; i++)
		add += ins[i] == '\n';
	u32 new_nl = t->nl - (last - first) + add;
	VX_GROW(t->ls, t->lcap, new_nl);
	memmove(t->ls + first + add, t->ls + last, (t->nl - last) * sizeof(u32));
	u32 k = first;
	for (u32 i = 0; i < nins; i++)
		if (ins[i] == '\n')
			t->ls[k++] = pos + i + 1;
	i64 shift = (i64)nins - (i64)del;
	for (u32 i = first + add; i < new_nl; i++)
		t->ls[i] = (u32)((i64)t->ls[i] + shift);
	t->nl = new_nl;
}

static void apply(Text *t, u32 pos, u32 del, const char *ins, u32 nins)
{
	if (pos > t->len)
		pos = t->len;
	if (pos + del > t->len)
		del = t->len - pos;
	edit_pieces(t, pos, del, ins, nins);
	edit_lines(t, pos, del, ins, nins);
	t->len = t->len - del + nins;
	t->version++;
}

void text_replace(Text *t, u32 pos, u32 del, const char *ins, u32 nins, u32 group)
{
	if (pos > t->len)
		pos = t->len;
	if (pos + del > t->len)
		del = t->len - pos;
	if (!del && !nins)
		return;
	if (group) {
		/* Drop the redo tail. */
		if (t->saved_upos > t->upos && t->saved_upos != UINT32_MAX)
			t->saved_upos = UINT32_MAX;
		t->nu = t->upos;
		if (t->nu == 0)
			pool_clear(&t->upool);
		undo_reserve(t, t->nu + 1);
		u32 i = t->nu++;
		t->u_pos[i] = pos;
		t->u_del[i] = del;
		t->u_ins[i] = nins;
		t->u_group[i] = group;
		char *tmp = del ? vx_alloc(del) : NULL;
		text_copy(t, pos, del, tmp);
		t->u_delo[i] = pool_add(&t->upool, tmp ? tmp : "", del);
		free(tmp);
		t->u_inso[i] = pool_add(&t->upool, ins, nins);
		t->upos = t->nu;
	}
	apply(t, pos, del, ins, nins);
}

bool text_undo(Text *t, u32 *pos, u32 *end)
{
	if (t->upos == 0)
		return false;
	u32 g = t->u_group[t->upos - 1];
	while (t->upos > 0 && t->u_group[t->upos - 1] == g) {
		u32 i = --t->upos;
		apply(t, t->u_pos[i], t->u_ins[i], t->upool.b + t->u_delo[i], t->u_del[i]);
		*pos = t->u_pos[i];
		*end = t->u_pos[i] + t->u_del[i];
	}
	return true;
}

bool text_redo(Text *t, u32 *pos, u32 *end)
{
	if (t->upos >= t->nu)
		return false;
	u32 g = t->u_group[t->upos];
	while (t->upos < t->nu && t->u_group[t->upos] == g) {
		u32 i = t->upos++;
		apply(t, t->u_pos[i], t->u_del[i], t->upool.b + t->u_inso[i], t->u_ins[i]);
		*pos = t->u_pos[i];
		*end = t->u_pos[i] + t->u_ins[i];
	}
	return true;
}

u32 text_last_group(const Text *t) { return t->upos ? t->u_group[t->upos - 1] : 0; }
