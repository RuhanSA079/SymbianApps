/*
 * nsfb_symbian.c: a libnsfb surface for Symbian. NetSurf draws into an
 * ordinary 32 bpp buffer (XRGB8888: 0x00RRGGBB, the layout of an EColor16MU
 * bitmap); update() hands changed areas to the app, which copies them into
 * its bitmap. Input is queued by the app and handed out without blocking:
 * the app's active object does the waiting.
 */
#include <stdbool.h>
#include <stdlib.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"

#include "nsfb_glue.h"

#define QUEUE_LEN 256

static nsfb_event_t queue[QUEUE_LEN];
static int q_head, q_tail;             /* pop at head, push at tail */

static void push(const nsfb_event_t *ev)
{
	int next = (q_tail + 1) % QUEUE_LEN;
	if (next == q_head)
		return;                 /* full: drop it */
	queue[q_tail] = *ev;
	q_tail = next;
}

int nsfb_symbian_input_pending(void)
{
	return q_head != q_tail;
}

void nsfb_sym_key(int down, int code)
{
	nsfb_event_t ev;
	ev.type = down ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
	ev.value.keycode = (enum nsfb_key_code_e)code;
	push(&ev);
}

void nsfb_sym_pointer_move(int x, int y)
{
	nsfb_event_t ev;
	ev.type = NSFB_EVENT_MOVE_ABSOLUTE;
	ev.value.vector.x = x;
	ev.value.vector.y = y;
	ev.value.vector.z = 0;
	push(&ev);
}

void nsfb_sym_pointer_button(int down)
{
	nsfb_sym_key(down, NSFB_KEY_MOUSE_1);
}

void nsfb_sym_resize(int width, int height)
{
	nsfb_event_t ev;
	ev.type = NSFB_EVENT_RESIZE;
	ev.value.resize.w = width;
	ev.value.resize.h = height;
	push(&ev);
}

/* ---- surface routines ---- */

static int sym_defaults(nsfb_t *nsfb)
{
	nsfb->width = 640;
	nsfb->height = 360;
	nsfb->format = NSFB_FMT_XRGB8888;
	select_plotters(nsfb);
	return 0;
}

static int sym_initialise(nsfb_t *nsfb)
{
	size_t size = (size_t)nsfb->width * nsfb->height * nsfb->bpp / 8;
	uint8_t *p = realloc(nsfb->ptr, size);
	if (p == NULL)
		return -1;
	nsfb->ptr = p;
	nsfb->linelen = nsfb->width * nsfb->bpp / 8;
	return 0;
}

static int sym_geometry(nsfb_t *nsfb, int width, int height,
			enum nsfb_format_e format)
{
	int pw = nsfb->width, ph = nsfb->height;
	enum nsfb_format_e pf = nsfb->format;

	if (width > 0)
		nsfb->width = width;
	if (height > 0)
		nsfb->height = height;
	if (format != NSFB_FMT_ANY)
		nsfb->format = format;
	select_plotters(nsfb);

	if (nsfb->ptr != NULL) {
		size_t size = (size_t)nsfb->width * nsfb->height * nsfb->bpp / 8;
		uint8_t *p = realloc(nsfb->ptr, size);
		if (p == NULL) {
			nsfb->width = pw;
			nsfb->height = ph;
			nsfb->format = pf;
			select_plotters(nsfb);
			return -1;
		}
		nsfb->ptr = p;
	}
	nsfb->linelen = nsfb->width * nsfb->bpp / 8;
	return 0;
}

static int sym_finalise(nsfb_t *nsfb)
{
	free(nsfb->ptr);
	nsfb->ptr = NULL;
	return 0;
}

static bool sym_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
	(void)nsfb;
	(void)timeout;
	if (q_head == q_tail)
		return false;
	*event = queue[q_head];
	q_head = (q_head + 1) % QUEUE_LEN;
	return true;
}

static int sym_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
	int x0 = box->x0 < 0 ? 0 : box->x0;
	int y0 = box->y0 < 0 ? 0 : box->y0;
	int x1 = box->x1 > nsfb->width ? nsfb->width : box->x1;
	int y1 = box->y1 > nsfb->height ? nsfb->height : box->y1;

	if (nsfb->ptr == NULL || x1 <= x0 || y1 <= y0)
		return 0;
	if (nsfb->format != NSFB_FMT_XRGB8888)
		return -1;
	nsfb_symbian_update((const unsigned int *)nsfb->ptr, nsfb->linelen,
			    nsfb->width, nsfb->height, x0, y0, x1, y1);
	return 0;
}

static const nsfb_surface_rtns_t symbian_rtns = {
	.defaults = sym_defaults,
	.initialise = sym_initialise,
	.finalise = sym_finalise,
	.input = sym_input,
	.geometry = sym_geometry,
	.update = sym_update,
};

/* libnsfb's own surfaces register from __attribute__((constructor))
 * functions, which Symbian executables never run; so register ours and the
 * RAM surface explicitly. The ABLE slot is free: that surface is not built
 * for Symbian. */
void nsfb_ram_register(void);	/* libnsfb/src/surface/ram.c (patched) */

void nsfb_symbian_register(void)
{
	static bool done = false;
	if (!done) {
		_nsfb_register_surface(NSFB_SURFACE_ABLE, &symbian_rtns, "symbian");
		/* NetSurf keeps every image in a RAM surface */
		nsfb_ram_register();
		done = true;
	}
}
