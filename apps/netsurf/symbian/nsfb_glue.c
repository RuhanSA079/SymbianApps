/*
 * nsfb_glue.c: browser actions for the Symbian app's menu, on the
 * framebuffer frontend's (only) window. See nsfb_glue.h.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <libnsfb.h>
#include <libnsfb_event.h>

#include "utils/nsurl.h"
#include "utils/nsoption.h"
#include "netsurf/browser_window.h"
#include "desktop/browser_history.h"
#include "desktop/searchweb.h"
#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"

#include "nsfb_glue.h"

static struct browser_window *bw(void)
{
	return window_list != NULL ? window_list->bw : NULL;
}

int nsfb_sym_in_content(int x, int y)
{
	struct fbtk_widget_s *w;
	int ax, ay;

	if (window_list == NULL || (w = window_list->browser) == NULL)
		return 0;
	ax = fbtk_get_absx(w);
	ay = fbtk_get_absy(w);
	return x >= ax && y >= ay &&
		x < ax + fbtk_get_width(w) && y < ay + fbtk_get_height(w);
}

void nsfb_sym_open_url(const char *text)
{
	nsurl *url;
	if (bw() == NULL || text == NULL || text[0] == '\0')
		return;
	/* "example.com" -> http://example.com/, other text -> web search */
	if (search_web_omni(text, SEARCH_WEB_OMNI_NONE, &url) == NSERROR_OK) {
		browser_window_navigate(bw(), url, NULL, BW_NAVIGATE_HISTORY,
					NULL, NULL, NULL);
		nsurl_unref(url);
	}
}

int nsfb_sym_can_back(void)
{
	return bw() != NULL && browser_window_back_available(bw());
}

void nsfb_sym_back(void)
{
	if (nsfb_sym_can_back())
		browser_window_history_back(bw(), false);
}

void nsfb_sym_forward(void)
{
	if (bw() != NULL && browser_window_forward_available(bw()))
		browser_window_history_forward(bw(), false);
}

void nsfb_sym_reload(void)
{
	if (bw() != NULL)
		browser_window_reload(bw(), true);
}

void nsfb_sym_stop(void)
{
	if (bw() != NULL)
		browser_window_stop(bw());
}

void nsfb_sym_home(void)
{
	const char *home = nsoption_charp(homepage_url);
	nsfb_sym_open_url(home != NULL && home[0] != '\0' ? home : NETSURF_HOMEPAGE);
}

char *nsfb_sym_current_url(char *buf, int buflen)
{
	nsurl *url;
	buf[0] = '\0';
	if (bw() != NULL && (url = browser_window_access_url(bw())) != NULL) {
		strncpy(buf, nsurl_access(url), (size_t)buflen - 1);
		buf[buflen - 1] = '\0';
	}
	return buf;
}
