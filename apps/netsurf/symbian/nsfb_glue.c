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

/* ---- settings (the app's Settings page) ---- */

static char user_choices[96];

void nsfb_sym_set_user_choices(const char *path)
{
	strncpy(user_choices, path, sizeof(user_choices) - 1);
	user_choices[sizeof(user_choices) - 1] = '\0';
}

const char *nsfb_sym_user_choices(void)
{
	return user_choices[0] != '\0' ? user_choices : NULL;
}

int nsfb_sym_option(int opt)
{
	switch (opt) {
	case NSFB_SYM_OPT_SCALE:     return nsoption_int(scale);
	case NSFB_SYM_OPT_FONT_SIZE: return nsoption_int(font_size);
	case NSFB_SYM_OPT_IMAGES:    return nsoption_bool(foreground_images);
	case NSFB_SYM_OPT_BLOCK_ADS: return nsoption_bool(block_advertisements);
	case NSFB_SYM_OPT_DNT:       return nsoption_bool(do_not_track);
	}
	return 0;
}

void nsfb_sym_set_option(int opt, int value)
{
	switch (opt) {
	case NSFB_SYM_OPT_SCALE:
		nsoption_set_int(scale, value);
		if (bw() != NULL)
			browser_window_set_scale(bw(), (float)value / 100.0f, true);
		break;
	case NSFB_SYM_OPT_FONT_SIZE:
		nsoption_set_int(font_size, value);
		break;
	case NSFB_SYM_OPT_IMAGES:
		nsoption_set_bool(foreground_images, value != 0);
		nsoption_set_bool(background_images, value != 0);
		break;
	case NSFB_SYM_OPT_BLOCK_ADS:
		nsoption_set_bool(block_advertisements, value != 0);
		break;
	case NSFB_SYM_OPT_DNT:
		nsoption_set_bool(do_not_track, value != 0);
		break;
	}
}

char *nsfb_sym_homepage(char *buf, int buflen)
{
	const char *home = nsoption_charp(homepage_url);
	strncpy(buf, home != NULL ? home : "", (size_t)buflen - 1);
	buf[buflen - 1] = '\0';
	return buf;
}

void nsfb_sym_set_homepage(const char *url)
{
	nsoption_set_charp(homepage_url,
			   url != NULL && url[0] != '\0' ? strdup(url) : NULL);
}

/* Only the options the Settings page changes are written, so the defaults
 * in res/Choices still apply to everything else after an upgrade. */
int nsfb_sym_save_options(void)
{
	FILE *fp;
	const char *home = nsoption_charp(homepage_url);
	if (user_choices[0] == '\0' || (fp = fopen(user_choices, "w")) == NULL)
		return -1;
	if (home != NULL && home[0] != '\0')
		fprintf(fp, "homepage_url:%s\n", home);
	fprintf(fp, "scale:%d\n", nsoption_int(scale));
	fprintf(fp, "font_size:%d\n", nsoption_int(font_size));
	fprintf(fp, "foreground_images:%d\n", nsoption_bool(foreground_images) ? 1 : 0);
	fprintf(fp, "background_images:%d\n", nsoption_bool(background_images) ? 1 : 0);
	fprintf(fp, "block_advertisements:%d\n", nsoption_bool(block_advertisements) ? 1 : 0);
	fprintf(fp, "do_not_track:%d\n", nsoption_bool(do_not_track) ? 1 : 0);
	fclose(fp);
	return 0;
}
