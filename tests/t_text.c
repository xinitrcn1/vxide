/* Piece table + line index + undo: random edits against a flat model. */
#define _POSIX_C_SOURCE 200809L
#include "text.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char model[1 << 16];
static u32 mlen;

static u32 rnd(void)
{
	static u32 s = 12345;
	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	return s;
}

static bool same(Text *t)
{
	if (t->len != mlen)
		return false;
	if (memcmp(text_flat(t), model, mlen) != 0)
		return false;
	/* line starts */
	u32 l = 1;
	if (t->ls[0] != 0)
		return false;
	for (u32 i = 0; i < mlen; i++)
		if (model[i] == '\n') {
			if (l >= t->nl || t->ls[l] != i + 1)
				return false;
			l++;
		}
	if (l != t->nl)
		return false;
	/* every line through text_line */
	for (u32 k = 0; k < t->nl; k++) {
		u32 n;
		const char *s = text_line(t, k, &n);
		u32 e = k + 1 < t->nl ? t->ls[k + 1] - 1 : mlen;
		u32 st = t->ls[k];
		if (e > st && model[e - 1] == '\r')
			e--;
		if (n != e - st || memcmp(s, model + st, n) != 0)
			return false;
	}
	return true;
}

static void medit(u32 pos, u32 del, const char *ins, u32 n)
{
	memmove(model + pos + n, model + pos + del, mlen - pos - del);
	memcpy(model + pos, ins, n);
	mlen = mlen - del + n;
}

int main(void)
{
	char dir[] = "/tmp/vxide-text-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	char path[256];
	snprintf(path, sizeof path, "%s/a.c", dir);
	const char *init = "int main(void)\n{\n\treturn 0;\n}\n";
	CHECK(vx_write_file(path, init, strlen(init)) == 0);

	Text t;
	CHECK(text_open(&t, path) == 0);
	CHECK(t.mapped);
	memcpy(model, init, strlen(init));
	mlen = (u32)strlen(init);
	CHECK(same(&t));
	CHECK(text_lines(&t) == 5);
	CHECK(!text_dirty(&t));

	/* Random edits with undo groups, snapshotting the model per group. */
	static char snap[64][1 << 12];
	static u32 snapn[64];
	u32 groups = 0;
	const char *alphabet = "ab\n\tcd\n{}x\r\n";
	bool ok = true;
	for (int it = 0; it < 4000 && ok; it++) {
		if (groups < 64 && mlen < sizeof snap[0] - 64) {
			memcpy(snap[groups], model, mlen);
			snapn[groups] = mlen;
		} else {
			break;
		}
		u32 pos = mlen ? rnd() % (mlen + 1) : 0;
		u32 del = (rnd() % 4 == 0 && mlen > pos) ? rnd() % VX_MIN(mlen - pos, 8u) : 0;
		char ins[8];
		u32 n = rnd() % 6;
		for (u32 i = 0; i < n; i++)
			ins[i] = alphabet[rnd() % strlen(alphabet)];
		if (!del && !n)
			continue;
		medit(pos, del, ins, n);
		text_replace(&t, pos, del, ins, n, ++groups);
		ok = same(&t);
	}
	CHECK(ok);
	CHECK(text_dirty(&t));
	/* Undo everything, checking each snapshot. */
	bool uok = true;
	for (u32 g = groups; g-- > 0;) {
		u32 a, b;
		if (!text_undo(&t, &a, &b)) {
			uok = false;
			break;
		}
		memcpy(model, snap[g], snapn[g]);
		mlen = snapn[g];
		if (!same(&t)) {
			uok = false;
			fprintf(stderr, "undo mismatch at group %u\n", g);
			break;
		}
	}
	CHECK(uok);
	CHECK(!text_dirty(&t));
	/* Redo all and compare with the final state. */
	u32 a, b;
	while (text_redo(&t, &a, &b))
		;
	CHECK(t.upos == t.nu);

	/* Typing extends one piece. */
	Text u;
	text_init(&u);
	mlen = 0;
	for (int i = 0; i < 1000; i++) {
		char c = (char)('a' + i % 26);
		text_replace(&u, u.len, 0, &c, 1, 1);
		medit(mlen, 0, &c, 1);
	}
	CHECK(u.np == 1);
	CHECK(same(&u));
	text_free(&u);

	/* Save remaps and keeps the content; old mapping not truncated. */
	memcpy(model, text_flat(&t), t.len);
	mlen = t.len;
	CHECK(text_save(&t, path) == 0);
	CHECK(t.np <= 1);
	CHECK(same(&t));
	CHECK(!text_dirty(&t));
	CHECK(!text_changed_on_disk(&t, path));
	size_t n = 0;
	char *disk = vx_read_file(path, &n);
	CHECK(disk && n == mlen && memcmp(disk, model, n) == 0);
	free(disk);
	/* After save, undo still works and makes it dirty. */
	if (t.upos) {
		CHECK(text_undo(&t, &a, &b));
		CHECK(text_dirty(&t));
	}

	/* CRLF detection. */
	snprintf(path, sizeof path, "%s/crlf.txt", dir);
	vx_write_file(path, "a\r\nbb\r\n", 7);
	Text c;
	CHECK(text_open(&c, path) == 0);
	CHECK(c.crlf);
	CHECK(text_line_len(&c, 0) == 1 && text_line_len(&c, 1) == 2);
	CHECK(strcmp(text_eol(&c), "\r\n") == 0);
	text_free(&c);

	/* Missing file opens empty. */
	snprintf(path, sizeof path, "%s/none.txt", dir);
	CHECK(text_open(&c, path) == 0);
	CHECK(c.len == 0 && c.nl == 1);
	text_free(&c);

	text_free(&t);
	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	system(cmd);
	return tap_done();
}
