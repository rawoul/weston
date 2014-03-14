#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <wayland-server.h>
#include <ela/ela.h>
#include <ela/backend.h>

#include "ela-wayland.h"

#define ELA_EVENT_DELAY_FREE  0x20000000
#define ELA_EVENT_NEED_FREE   0x40000000
#define ELA_EVENT_ENABLE      0x80000000

struct ela_wayland {
	struct ela_el base;
	struct wl_event_loop *loop;
	int run;
};

struct ela_event_source {
	struct ela_wayland *elw;
	struct wl_event_source *timer_source;
	struct wl_event_source *fd_source;
	uint32_t flags;
	int fd;
	int timeout;
	ela_handler_func *callback;
	void *user_data;
};

static struct ela_el *elw_create(void);
static ela_error_t elw_source_update_timer(struct ela_wayland *elw,
					   struct ela_event_source *source);

static void elw_source_free(struct ela_el *el, struct ela_event_source *source);
static ela_error_t elw_source_remove(struct ela_el *el,
				     struct ela_event_source *source);

static inline struct ela_wayland *
ela_wayland(struct ela_el *el)
{
	return (struct ela_wayland *)el;
}

static int
dispatch_event(struct ela_event_source *source, int fd, int flags)
{
	source->flags |= ELA_EVENT_DELAY_FREE;
	source->callback(source, fd, flags, source->user_data);
	source->flags &= ~ELA_EVENT_DELAY_FREE;

	if (source->flags & ELA_EVENT_NEED_FREE)
		elw_source_free(&source->elw->base, source);
	else if (source->flags & ELA_EVENT_ONCE)
		elw_source_remove(&source->elw->base, source);
	else
		elw_source_update_timer(source->elw, source);

	return 0;
}

static int
handle_fd_event(int fd, uint32_t mask, void *data)
{
	struct ela_event_source *source = data;
	uint32_t flags;

	flags = 0;
	if (mask & WL_EVENT_WRITABLE)
		flags |= ELA_EVENT_WRITABLE;
	if (mask & WL_EVENT_READABLE)
		flags |= ELA_EVENT_READABLE;
	if (source->flags & ELA_EVENT_ONCE)
		flags |= ELA_EVENT_ONCE;

	return dispatch_event(source, fd, flags);
}

static int
handle_timer_event(void *data)
{
	struct ela_event_source *source = data;
	uint32_t flags;

	flags = ELA_EVENT_TIMEOUT;

	if (source->flags & ELA_EVENT_ONCE)
		flags |= ELA_EVENT_ONCE;

	return dispatch_event(source, -1, flags);
}

static ela_error_t
elw_source_update_fd(struct ela_wayland *elw,
		     struct ela_event_source *source)
{
	/* check fd source */
	if ((source->flags & ELA_EVENT_ENABLE) &&
	    (source->flags & (ELA_EVENT_READABLE | ELA_EVENT_WRITABLE))) {
		uint32_t mask;

		mask = 0;
		if (source->flags & ELA_EVENT_WRITABLE)
			mask |= WL_EVENT_WRITABLE;
		if (source->flags & ELA_EVENT_READABLE)
			mask |= WL_EVENT_READABLE;

		if (!source->fd_source) {
			source->fd_source =
				wl_event_loop_add_fd(elw->loop, source->fd,
						     mask, handle_fd_event,
						     source);
			if (!source->fd_source)
				return EINVAL;
		} else
			wl_event_source_fd_update(source->fd_source, mask);

	} else if (source->fd_source) {
		wl_event_source_remove(source->fd_source);
		source->fd_source = NULL;
	}

	return 0;
}

static ela_error_t
elw_source_update_timer(struct ela_wayland *elw,
			struct ela_event_source *source)
{
	/* check timer source */
	if ((source->flags & ELA_EVENT_ENABLE) &&
	    (source->flags & ELA_EVENT_TIMEOUT)) {
		if (!source->timer_source) {
			source->timer_source =
				wl_event_loop_add_timer(elw->loop,
							handle_timer_event,
							source);
			if (!source->timer_source)
				return ENOMEM;
		}

		wl_event_source_timer_update(source->timer_source,
					     source->timeout);

	} else if (source->timer_source) {
		wl_event_source_remove(source->timer_source);
		source->timer_source = NULL;
	}

	return 0;
}

static ela_error_t
elw_source_update(struct ela_wayland *elw, struct ela_event_source *source)
{
	ela_error_t err;

	if ((err = elw_source_update_fd(elw, source)))
		return err;

	if ((err = elw_source_update_timer(elw, source)))
		return err;

	return 0;
}

static ela_error_t
elw_source_set_fd(struct ela_el *el, struct ela_event_source *source,
		  int fd, uint32_t flags)
{
	struct ela_wayland *elw = ela_wayland(el);

	if (source->fd_source) {
		wl_event_source_remove(source->fd_source);
		source->fd_source = NULL;
	}

	if (flags & ELA_EVENT_ONCE)
		source->flags |= ELA_EVENT_ONCE;
	else
		source->flags &= ~ELA_EVENT_ONCE;

	if (fd >= 0 && (flags & ELA_EVENT_READABLE))
		source->flags |= ELA_EVENT_READABLE;
	else
		source->flags &= ~ELA_EVENT_READABLE;

	if (fd >= 0 && (flags & ELA_EVENT_WRITABLE))
		source->flags |= ELA_EVENT_WRITABLE;
	else
		source->flags &= ~ELA_EVENT_WRITABLE;

	source->fd = fd;

	return elw_source_update(elw, source);
}

static ela_error_t
elw_source_set_timeout(struct ela_el *el, struct ela_event_source *source,
		       const struct timeval *tv, uint32_t flags)
{
	struct ela_wayland *elw = ela_wayland(el);

	if (tv != NULL) {
		source->flags |= ELA_EVENT_TIMEOUT;
		source->timeout = tv->tv_sec * 1000 + tv->tv_usec / 1000;
	} else {
		source->flags &= ~ELA_EVENT_TIMEOUT;
		source->timeout = 0;
	}

	if (flags & ELA_EVENT_ONCE)
		source->flags |= ELA_EVENT_ONCE;
	else
		source->flags &= ~ELA_EVENT_ONCE;

	return elw_source_update(elw, source);
}

static ela_error_t
elw_source_add(struct ela_el *el, struct ela_event_source *source)
{
	struct ela_wayland *elw = ela_wayland(el);

	source->flags |= ELA_EVENT_ENABLE;

	return elw_source_update(elw, source);
}

static ela_error_t
elw_source_remove(struct ela_el *el, struct ela_event_source *source)
{
	struct ela_wayland *elw = ela_wayland(el);
	int ret;

	source->flags &= ~ELA_EVENT_ENABLE;
	ret = elw_source_update(elw, source);

	assert(source->fd_source == NULL);
	assert(source->timer_source == NULL);

	return ret;
}

static ela_error_t
elw_source_alloc(struct ela_el *el, ela_handler_func *callback,
		 void *user_data, struct ela_event_source **ret)
{
	struct ela_wayland *elw = ela_wayland(el);
	struct ela_event_source *source;

	source = malloc(sizeof *source);
	if (!source)
		return ENOMEM;

	source->elw = elw;
	source->timer_source = NULL;
	source->fd_source = NULL;
	source->flags = 0;
	source->fd = -1;
	source->timeout = 0;
	source->user_data = user_data;
	source->callback = callback;

	*ret = source;

	return 0;
}

static void
elw_source_free(struct ela_el *el, struct ela_event_source *source)
{
	struct ela_wayland *elw = ela_wayland(el);

	if (source->flags & ELA_EVENT_DELAY_FREE) {
		source->flags |= ELA_EVENT_NEED_FREE;
		return;
	}

	elw_source_remove(&elw->base, source);
	free(source);
}

static void elw_exit(struct ela_el *el)
{
	struct ela_wayland *elw = ela_wayland(el);

	elw->run = 0;
}

static void elw_run(struct ela_el *el)
{
	struct ela_wayland *elw = ela_wayland(el);

	while (elw->run)
		wl_event_loop_dispatch(elw->loop, -1);
}

static void elw_close(struct ela_el *el)
{
	struct ela_wayland *elw = ela_wayland(el);

	free(elw);
}

static const struct ela_el_backend ela_wayland_funcs =
{
	elw_source_alloc,
	elw_source_free,
	elw_source_set_fd,
	elw_source_set_timeout,
	elw_source_add,
	elw_source_remove,
	elw_exit,
	elw_run,
	elw_close,
	"wayland",
	elw_create,
};

struct ela_el *
ela_wayland_create(struct wl_event_loop *loop)
{
	struct ela_wayland *elw;

	elw = malloc(sizeof *elw);
	if (!elw)
		return NULL;

	elw->base.backend = &ela_wayland_funcs;
	elw->loop = loop;
	elw->run = 1;

	return &elw->base;
}

static struct ela_el *
elw_create(void)
{
	struct wl_event_loop *loop;

	loop = wl_event_loop_create();
	if (!loop)
		return NULL;

	return ela_wayland_create(loop);
}
