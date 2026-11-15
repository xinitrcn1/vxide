# vxide - BSD make. `make`, `make test`, `make clean`.

CC?=		cc
PKG_CONFIG?=	pkg-config
GLSLANG?=	glslangValidator

PKGS=		sdl3 freetype2 libgit2 vulkan
PKG_CFLAGS!=	${PKG_CONFIG} --cflags ${PKGS}
PKG_LIBS!=	${PKG_CONFIG} --libs ${PKGS}
GIT_CFLAGS!=	${PKG_CONFIG} --cflags libgit2
GIT_LIBS!=	${PKG_CONFIG} --libs libgit2

OPT?=		-O2 -g
CFLAGS+=	-std=c11 -pedantic ${OPT} -Wall -Wextra -Wno-unused-parameter \
		-Wno-missing-field-initializers -Ibuild -Isrc ${PKG_CFLAGS}
LDLIBS+=	${PKG_LIBS} -lm -lpthread

B=		build

# Core modules: no window or GPU, unit-tested on their own.
CORE=		base text syntax theme fstree fswatch json lsp git gitops gitgraph githunk lgblame blame regex search png
# Front end: window, Vulkan, fonts, UI.
APP=		main gpu font icons draw ui editor panes menu actions gitui bgwork diffview

HDRS!=		ls src/*.h
SHADERS=	${B}/ui_vert.h ${B}/ui_frag.h

CORE_OBJS=	${CORE:S/^/${B}\//:S/$/.o/}
APP_OBJS=	${APP:S/^/${B}\//:S/$/.o/}

TESTS=		t_text t_syntax t_json t_lsp t_git t_gitops t_gitgraph t_githunk t_blame t_fstree t_regex t_search
TEST_BINS=	${TESTS:S/^/${B}\//}

all: ${B}/vxide

${B}:
	mkdir -p ${B}

${B}/ui_vert.h: shaders/ui.vert ${B}
	${GLSLANG} -V --quiet --vn ui_vert_spv -o ${.TARGET} shaders/ui.vert
${B}/ui_frag.h: shaders/ui.frag ${B}
	${GLSLANG} -V --quiet --vn ui_frag_spv -o ${.TARGET} shaders/ui.frag

.for m in ${CORE} ${APP}
${B}/${m}.o: src/${m}.c ${HDRS} ${SHADERS}
	${CC} ${CFLAGS} -c src/${m}.c -o ${.TARGET}
.endfor

${B}/vxide: ${CORE_OBJS} ${APP_OBJS}
	${CC} ${LDFLAGS} -o ${.TARGET} ${CORE_OBJS} ${APP_OBJS} ${LDLIBS}

.for t in ${TESTS}
${B}/${t}: tests/${t}.c tests/tap.h ${CORE_OBJS}
	${CC} ${CFLAGS} -Itests -o ${.TARGET} tests/${t}.c ${CORE_OBJS} ${GIT_LIBS} -lm -lpthread
.endfor

test: ${TEST_BINS} ${B}/vxide
	@fail=0; for t in ${TEST_BINS}; do \
		if $$t >$$t.log 2>&1; then echo "ok   $${t##*/}"; \
		else echo "FAIL $${t##*/} (see $$t.log)"; fail=1; fi; \
	done; \
	if sh tests/t_ui.sh >${B}/t_ui.log 2>&1; then echo "ok   t_ui"; \
	else echo "FAIL t_ui (see ${B}/t_ui.log)"; fail=1; fi; \
	exit $$fail

clean:
	rm -rf ${B}

.PHONY: all test clean
