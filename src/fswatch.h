/* File-system change notifications for a few hundred paths (open explorer
 * folders, the .git directory, open documents): kqueue on the BSDs and
 * macOS, inotify on Linux, else a cheap mtime poll. Instead of rescanning
 * on a timer (which keeps a slow disk seeking), the UI reacts to events.
 * Events are coalesced per tag; only directory entry changes and file
 * writes are reported. */
#ifndef VX_FSWATCH_H
#define VX_FSWATCH_H

#include "base.h"

typedef struct FsWatch FsWatch;

FsWatch *fsw_open(void);
void fsw_close(FsWatch *w);
/* Watch path (file or directory) under tag; re-adding a tag replaces it.
 * Returns 0 on success. */
int fsw_add(FsWatch *w, const char *path, u32 tag);
void fsw_remove(FsWatch *w, u32 tag);
bool fsw_has(const FsWatch *w, u32 tag);
/* Tags that saw changes since the last call (non-blocking). */
u32 fsw_poll(FsWatch *w, u32 *tags, u32 max);
/* Descriptor to poll() for readiness, or -1 (poll backend). */
int fsw_fd(const FsWatch *w);
const char *fsw_backend(void);

#endif
