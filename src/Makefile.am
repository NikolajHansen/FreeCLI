AM_CPPFLAGS = -I$(srcdir)/core -I$(srcdir)/ui -I$(srcdir)/providers \
              -I$(srcdir)/workers $(ZMQ_CFLAGS)

bin_PROGRAMS = freecli freecli-backend

freecli_SOURCES = \
	ui/main.c                        \
	ui/input.c                       \
	ui/session.c                     \
	ui/persist.c                     \
	ui/commands.c                    \
	ui/overlay.c                     \
	core/chat.c                      \
	core/utf8.c                      \
	core/ipc.c                       \
	core/cJSON.c                     \
	providers/xai/xai_models.c       \
	providers/anthropic/anthropic_models.c \
	providers/google/google_models.c \
	providers/ibm/ibm_models.c       \
	providers/registry.c
freecli_LDADD = -lncurses -lmenu -lform -lpanel

freecli_backend_SOURCES = \
	backend/backend.c                     \
	providers/xai/provider_xai.c          \
	providers/xai/xai_models.c            \
	providers/anthropic/provider_anthropic.c \
	providers/anthropic/anthropic_models.c   \
	providers/google/provider_google.c    \
	providers/google/google_models.c      \
	providers/ibm/provider_ibm.c          \
	providers/ibm/ibm_models.c            \
	providers/registry.c                  \
	core/chat.c                           \
	core/utf8.c                           \
	core/ipc.c                            \
	core/cJSON.c                          \
	core/provider.c                       \
	core/backend_worker.c                 \
	workers/fs_worker.c
freecli_backend_LDADD = -lcurl -lpthread $(ZMQ_LIBS)
