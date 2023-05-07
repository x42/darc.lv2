/* robtk dyncomp gui
 *
 * Copyright 2019 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LV2_1_18_6
#include <lv2/atom/atom.h>
#include <lv2/options/options.h>
#else
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/options/options.h>
#endif

#include "../src/darc.h"

#define RTK_USE_HOST_COLORS
#define RTK_URI DARC_URI
#define RTK_GUI "ui"

#ifndef MAX
#define MAX(A, B) ((A) > (B)) ? (A) : (B)
#endif

#ifndef MIN
#define MIN(A, B) ((A) < (B)) ? (A) : (B)
#endif

typedef struct {
	LV2UI_Write_Function write;
	LV2UI_Controller     controller;
	LV2UI_Touch*         touch;

	PangoFontDescription* font[2];

	RobWidget* rw;   // top-level container
	RobWidget* ctbl; // control element table

	/* Level + reduction drawing area */
	RobWidget* m0;
	int        m0_width;
	int        m0_height;

	/* Gain Mapping */
	RobWidget* m1;

	/* current gain */
	float _gmin;
	float _gmax;
	float _rms;

	/* control knobs */
	RobTkDial* spn_ctrl[5];
	RobTkLbl*  lbl_ctrl[5];
	RobTkCBtn* btn_hold;

	cairo_surface_t* dial_bg[5];

	/* gain meter */
	cairo_pattern_t* m_fg;
	cairo_pattern_t* m_bg;
	cairo_surface_t* m0bg;

	/* gain curve/mapping */
	cairo_surface_t* m1_grid;
	cairo_surface_t* m1_ctrl;
	cairo_surface_t* m1_mask;

	bool ctrl_dirty; // update m1_ctrl, m1_mask

	/* tooltips */
	int                tt_id;
	int                tt_timeout;
	cairo_rectangle_t* tt_pos;
	cairo_rectangle_t* tt_box;

	bool disable_signals;

	RobWidget*  m2;
	const char* nfo;
} darcUI;

/* ****************************************************************************
 * Control knob ranges and value mapping
 */

struct CtrlRange {
	float       min;
	float       max;
	float       dflt;
	float       step;
	float       mult;
	bool        log;
	const char* name;
};

const struct CtrlRange ctrl_range[] = {
	/* clang-format off */
	{  -10,  30,   0, 0.2, 5, false, "Input Gain" },
	{  -50, -10, -30, 0.1, 5, false, "Threshold" },
	{    0,   1,   0,  72, 2, true,  "Ratio" },
	{ .001, .1, 0.01, 100, 5, true,  "Attack" },
	{  .03,  3,  0.3, 100, 5, true,  "Release" },
	/* clang-format on */
};

static const char* tooltips[] = {
	"<markup><b>Input Gain.</b> Gain applied before level detection\nor any other processing.\n(not visualized as x-axis offset in curve)</markup>",
	"<markup><b>Threshold.</b> Signal level (RMS) at which\nthe compression effect is engaged.</markup>",
	"<markup><b>Ratio.</b> The amount of gain or attenuation to be\napplied (dB/dB above threshold).\nUnity is retained at -10dBFS/RMS (auto makeup-gain).</markup>",
	"<markup><b>Attack time.</b> Time it takes for the signal\nto become fully compressed after\nexceeding the threshold.</markup>",
	"<markup><b>Release time.</b> Minimum recovery time\nto uncompressed signal-level\nafter falling below threshold.</markup>",
	"<markup><b>Hold.</b> Retain current attenuation when the signal\nsubceeds the threshold.\nThis prevents modulation of the noise-floor\nand can counter-act 'pumping'.</markup>",
};

static float
ctrl_to_gui (const uint32_t c, const float v)
{
	if (!ctrl_range[c].log) {
		return v;
	}
	if (ctrl_range[c].min == 0) {
		return v * v * ctrl_range[c].step;
	}
	const float r = logf (ctrl_range[c].max / ctrl_range[c].min);
	return rintf (ctrl_range[c].step / r * (logf (v / ctrl_range[c].min)));
}

static float
gui_to_ctrl (const uint32_t c, const float v)
{
	if (!ctrl_range[c].log) {
		return v;
	}
	if (ctrl_range[c].min == 0) {
		return sqrt (v / ctrl_range[c].step);
	}
	const float r = log (ctrl_range[c].max / ctrl_range[c].min);
	return expf (logf (ctrl_range[c].min) + v * r / ctrl_range[c].step);
}

static float
k_min (const uint32_t c)
{
	if (!ctrl_range[c].log) {
		return ctrl_range[c].min;
	}
	return 0;
}

static float
k_max (const uint32_t c)
{
	if (!ctrl_range[c].log) {
		return ctrl_range[c].max;
	}
	return ctrl_range[c].step;
}

static float
k_step (const uint32_t c)
{
	if (!ctrl_range[c].log) {
		return ctrl_range[c].step;
	}
	return 1;
}

/* ****************************************************************************/

static float c_dlf[4] = { 0.8, 0.8, 0.8, 1.0 };

/* *****************************************************************************
 * Knob faceplates
 */

static void
prepare_faceplates (darcUI* ui)
{
	cairo_t* cr;
	float    xlp, ylp;

/* clang-format off */
#define INIT_DIAL_SF(VAR, W, H)                                             \
  VAR = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 2 * (W), 2 * (H)); \
  cr  = cairo_create (VAR);                                                 \
  cairo_scale (cr, 2.0, 2.0);                                               \
  CairoSetSouerceRGBA (c_trs);                                              \
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);                           \
  cairo_rectangle (cr, 0, 0, W, H);                                         \
  cairo_fill (cr);                                                          \
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

#define DIALDOTS(V, XADD, YADD)                                \
  float ang = (-.75 * M_PI) + (1.5 * M_PI) * (V);              \
  xlp       = GED_CX + XADD + sinf (ang) * (GED_RADIUS + 3.0); \
  ylp       = GED_CY + YADD - cosf (ang) * (GED_RADIUS + 3.0); \
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);               \
  CairoSetSouerceRGBA (c_dlf);                                 \
  cairo_set_line_width (cr, 2.5);                              \
  cairo_move_to (cr, rint (xlp) - .5, rint (ylp) - .5);        \
  cairo_close_path (cr);                                       \
  cairo_stroke (cr);

#define RESPLABLEL(V)                                             \
  {                                                               \
    DIALDOTS (V, 4.5, 15.5)                                       \
    xlp = rint (GED_CX + 4.5 + sinf (ang) * (GED_RADIUS + 9.5));  \
    ylp = rint (GED_CY + 15.5 - cosf (ang) * (GED_RADIUS + 9.5)); \
  }
	/* clang-format on */

	INIT_DIAL_SF (ui->dial_bg[0], GED_WIDTH + 8, GED_HEIGHT + 20);
	RESPLABLEL (0.00);
	write_text_full (cr, "-10", ui->font[0], xlp + 6, ylp, 0, 1, c_dlf);
	RESPLABLEL (0.25);
	RESPLABLEL (0.5);
	write_text_full (cr, "+10", ui->font[0], xlp - 2, ylp, 0, 2, c_dlf);
	RESPLABLEL (.75);
	RESPLABLEL (1.0);
	write_text_full (cr, "+30", ui->font[0], xlp - 6, ylp, 0, 3, c_dlf);
	cairo_destroy (cr);

	INIT_DIAL_SF (ui->dial_bg[1], GED_WIDTH + 8, GED_HEIGHT + 20);
	RESPLABLEL (0.00);
	write_text_full (cr, "-50", ui->font[0], xlp + 6, ylp, 0, 1, c_dlf);
	RESPLABLEL (0.25);
	RESPLABLEL (0.5);
	write_text_full (cr, "-30", ui->font[0], xlp - 2, ylp, 0, 2, c_dlf);
	RESPLABLEL (.75);
	RESPLABLEL (1.0);
	write_text_full (cr, "-10", ui->font[0], xlp - 6, ylp, 0, 3, c_dlf);
	cairo_destroy (cr);

	INIT_DIAL_SF (ui->dial_bg[2], GED_WIDTH + 8, GED_HEIGHT + 20);
	RESPLABLEL (0.00);
	write_text_full (cr, "1", ui->font[0], xlp + 4, ylp, 0, 1, c_dlf);
	RESPLABLEL (.25);
	write_text_full (cr, "2", ui->font[0], xlp + 3, ylp, 0, 1, c_dlf);
	RESPLABLEL (.44);
	write_text_full (cr, "3", ui->font[0], xlp + 1, ylp, 0, 1, c_dlf);
	RESPLABLEL (.64)
	write_text_full (cr, "5", ui->font[0], xlp + 4, ylp, 0, 1, c_dlf);
	RESPLABLEL (.81)
	write_text_full (cr, "10", ui->font[0], xlp + 6, ylp, 0, 1, c_dlf);
	RESPLABLEL (1.0);
	write_text_full (cr, "Lim", ui->font[0], xlp - 9, ylp, 0, 3, c_dlf);
	cairo_destroy (cr);

	INIT_DIAL_SF (ui->dial_bg[3], GED_WIDTH + 8, GED_HEIGHT + 20);
	RESPLABLEL (0.00);
	write_text_full (cr, "1ms", ui->font[0], xlp + 9, ylp, 0, 1, c_dlf);
	RESPLABLEL (.16);
	RESPLABLEL (.33);
	write_text_full (cr, "5", ui->font[0], xlp - 1, ylp, 0, 2, c_dlf);
	RESPLABLEL (0.5);
	RESPLABLEL (.66);
	write_text_full (cr, "20", ui->font[0], xlp + 3, ylp, 0, 2, c_dlf);
	RESPLABLEL (.83);
	RESPLABLEL (1.0);
	write_text_full (cr, "100", ui->font[0], xlp - 9, ylp, 0, 3, c_dlf);
	cairo_destroy (cr);

	INIT_DIAL_SF (ui->dial_bg[4], GED_WIDTH + 8, GED_HEIGHT + 20);
	RESPLABLEL (0.00);
	write_text_full (cr, "30ms", ui->font[0], xlp + 9, ylp, 0, 1, c_dlf);
	RESPLABLEL (.16);
	RESPLABLEL (.33);
	write_text_full (cr, "150", ui->font[0], xlp - 5, ylp, 0, 2, c_dlf);
	RESPLABLEL (0.5);
	RESPLABLEL (.66);
	write_text_full (cr, "600", ui->font[0], xlp + 5, ylp, 0, 2, c_dlf);
	RESPLABLEL (.83);
	RESPLABLEL (1.0);
	write_text_full (cr, "3s", ui->font[0], xlp - 6, ylp, 0, 3, c_dlf);
	cairo_destroy (cr);

#undef DIALDOTS
#undef INIT_DIAL_SF
#undef RESPLABLEL
}

/* *****************************************************************************
 * Numeric value display - knob tooltips
 */

static void
display_annotation (darcUI* ui, RobTkDial* d, cairo_t* cr, const char* txt)
{
	int tw, th;
	cairo_save (cr);
	PangoLayout* pl = pango_cairo_create_layout (cr);
	pango_layout_set_font_description (pl, ui->font[0]);
	pango_layout_set_text (pl, txt, -1);
	pango_layout_get_pixel_size (pl, &tw, &th);
	cairo_translate (cr, d->w_width / 2, d->w_height - 2);
	cairo_translate (cr, -tw / 2.0, -th);
	cairo_set_source_rgba (cr, .0, .0, .0, .7);
	rounded_rectangle (cr, -1, -1, tw + 3, th + 1, 3);
	cairo_fill (cr);
	CairoSetSouerceRGBA (c_wht);
	pango_cairo_show_layout (cr, pl);
	g_object_unref (pl);
	cairo_restore (cr);
	cairo_new_path (cr);
}

static void
dial_annotation_db (RobTkDial* d, cairo_t* cr, void* data)
{
	darcUI* ui = (darcUI*)(data);
	char    txt[16];
	snprintf (txt, 16, "%5.1f dB", d->cur);
	display_annotation (ui, d, cr, txt);
}

static void
format_msec (char* txt, const float val)
{
	if (val < 0.03) {
		snprintf (txt, 16, "%.1f ms", val * 1000.f);
	} else if (val < 0.3) {
		snprintf (txt, 16, "%.0f ms", val * 1000.f);
	} else {
		snprintf (txt, 16, "%.2f s", val);
	}
}

static void
dial_annotation_tm (RobTkDial* d, cairo_t* cr, void* data)
{
	darcUI* ui = (darcUI*)(data);
	char    txt[16];
	assert (d == ui->spn_ctrl[3] || d == ui->spn_ctrl[4]);
	const float val = gui_to_ctrl ((d == ui->spn_ctrl[3]) ? 3 : 4, d->cur);
	format_msec (txt, val);
	display_annotation (ui, d, cr, txt);
}

static void
dial_annotation_rr (RobTkDial* d, cairo_t* cr, void* data)
{
	darcUI*     ui = (darcUI*)(data);
	char        txt[16];
	const float val = gui_to_ctrl (2, d->cur);
	if (val >= 1) {
		snprintf (txt, 16, "\u221E : 1");
	} else if (val >= .9) {
		snprintf (txt, 16, "%.0f : 1", 1 / (1.f - val));
	} else {
		snprintf (txt, 16, "%.1f : 1", 1 / (1.f - val));
	}
	display_annotation (ui, d, cr, txt);
}

/* *****************************************************************************
 * knob & button callbacks
 */

static bool
cb_spn_ctrl (RobWidget* w, void* handle)
{
	darcUI* ui = (darcUI*)handle;
	if (w == ui->spn_ctrl[1]->rw || w == ui->spn_ctrl[2]->rw) {
		ui->ctrl_dirty = true;
		queue_draw (ui->m1);
	}

	if (ui->disable_signals) {
		return TRUE;
	}

	for (uint32_t i = 0; i < 5; ++i) {
		if (w != ui->spn_ctrl[i]->rw) {
			continue;
		}
		const float val = gui_to_ctrl (i, robtk_dial_get_value (ui->spn_ctrl[i]));
		ui->write (ui->controller, DARC_INPUTGAIN + i, sizeof (float), 0, (const void*)&val);
		break;
	}
	return TRUE;
}

static bool
cb_btn_hold (RobWidget* w, void* handle)
{
	darcUI* ui = (darcUI*)handle;

	ui->ctrl_dirty = true;
	queue_draw (ui->m1);

	if (ui->disable_signals) {
		return TRUE;
	}

	const float val = robtk_cbtn_get_active (ui->btn_hold) ? 1.f : 0.f;
	ui->write (ui->controller, DARC_HOLD, sizeof (float), 0, (const void*)&val);
	return TRUE;
}

/* *****************************************************************************
 * Tooltip & Help Overlay
 */

static bool
tooltip_overlay (RobWidget* rw, cairo_t* cr, cairo_rectangle_t* ev)
{
	darcUI* ui = (darcUI*)rw->top;
	assert (ui->tt_id >= 0 && ui->tt_id < 6);

	cairo_save (cr);
	cairo_rectangle_t event = { 0, 0, rw->area.width, rw->area.height };
	rcontainer_clear_bg (rw, cr, &event);
	rcontainer_expose_event (rw, cr, &event);
	cairo_restore (cr);

	const float top = ui->tt_box->y;
	rounded_rectangle (cr, 0, top, rw->area.width, ui->tt_pos->y - top, 3);
	cairo_set_source_rgba (cr, 0, 0, 0, .7);
	cairo_fill (cr);

	if (ui->tt_id < 5) {
		rounded_rectangle (cr, ui->tt_pos->x, ui->tt_pos->y,
		                   ui->tt_pos->width + 2, ui->tt_pos->height + 1, 3);
		cairo_set_source_rgba (cr, 1, 1, 1, .5);
		cairo_fill (cr);
	}

	const float*          color = c_wht;
	PangoFontDescription* font  = pango_font_description_from_string ("Sans 11px");

	const float xp = .5 * rw->area.width;
	const float yp = .5 * (ui->tt_pos->y - top);

	cairo_save (cr);
	cairo_scale (cr, rw->widget_scale, rw->widget_scale);
	write_text_full (cr, tooltips[ui->tt_id], font,
	                 xp / rw->widget_scale, yp / rw->widget_scale,
	                 0, 2, color);
	cairo_restore (cr);

	pango_font_description_free (font);

	return TRUE;
}

static bool
tooltip_cnt (RobWidget* rw, cairo_t* cr, cairo_rectangle_t* ev)
{
	darcUI* ui = (darcUI*)rw->top;
	if (++ui->tt_timeout < 12) {
		rcontainer_expose_event (rw, cr, ev);
		queue_draw (rw);
	} else {
		rw->expose_event = tooltip_overlay;
		tooltip_overlay (rw, cr, ev);
	}
	return TRUE;
}

static void
ttip_handler (RobWidget* rw, bool on, void* handle)
{
	darcUI* ui     = (darcUI*)handle;
	ui->tt_id      = -1;
	ui->tt_timeout = 0;

	for (int i = 0; i < 5; ++i) {
		if (rw == ui->lbl_ctrl[i]->rw) {
			ui->tt_id = i;
			break;
		}
	}
	if (rw == ui->btn_hold->rw) {
		ui->tt_id = 5;
	}

	if (on && ui->tt_id >= 0) {
		ui->tt_pos             = &rw->area;
		ui->tt_box             = &ui->spn_ctrl[0]->rw->area;
		ui->ctbl->expose_event = tooltip_cnt;
		queue_draw (ui->ctbl);
	} else {
		ui->ctbl->expose_event    = rcontainer_expose_event;
		ui->ctbl->parent->resized = TRUE; // full re-expose
		queue_draw (ui->rw);
	}
}

static void
top_leave_notify (RobWidget* rw)
{
	darcUI* ui = (darcUI*)rw->children[1]->top;
	if (ui->ctbl->expose_event != rcontainer_expose_event) {
		ui->ctbl->expose_event    = rcontainer_expose_event;
		ui->ctbl->parent->resized = TRUE; //full re-expose
		queue_draw (ui->rw);
	}
}

/* *****************************************************************************
 * Gain Meter Display
 */

#define M0HEIGHT 36

static void
m0_size_request (RobWidget* handle, int* w, int* h)
{
	darcUI* ui = (darcUI*)GET_HANDLE (handle);

	*w = 300;
	*h = M0HEIGHT * ui->rw->widget_scale;
}

static void
m0_size_allocate (RobWidget* handle, int w, int h)
{
	darcUI* ui = (darcUI*)GET_HANDLE (handle);

	h = M0HEIGHT * ui->rw->widget_scale;

	ui->m0_width  = w;
	ui->m0_height = h;

	robwidget_set_size (ui->m0, w, h);

	if (ui->m_fg) {
		cairo_pattern_destroy (ui->m_fg);
	}
	if (ui->m_bg) {
		cairo_pattern_destroy (ui->m_bg);
	}
	if (ui->m0bg) {
		cairo_surface_destroy (ui->m0bg);
	}
	ui->m_fg = NULL;
	ui->m_bg = NULL;
	ui->m0bg = NULL;

	if (1) {
		pango_font_description_free (ui->font[1]);
		char fnt[32];
		snprintf (fnt, 32, "Mono %.0fpx\n", 10 * sqrtf (h / (float)M0HEIGHT));
		ui->font[1] = pango_font_description_from_string (fnt);
	}

	queue_draw (ui->m0);
}

static void
m0_render_faceplate (darcUI* ui, cairo_t* cr)
{
	const uint32_t yscale = ui->m0_height / M0HEIGHT;
	const uint32_t top    = (ui->m0_height - M0HEIGHT * yscale) * .5;
	const uint32_t disp_w = ui->m0_width - 20; // deafult: 300

#define YPOS(y) (top + yscale * (y))
#define HGHT(y) (yscale * (y))

#define DEF(x) MAX (0, MIN (1., ((20. + (x)) / 60.)))
#define DEFLECT(x) (rint (disp_w * DEF (x)) - .5)

	cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

	if (is_light_theme ()) {
		CairoSetSouerceRGBA (c_g80);
	} else {
		CairoSetSouerceRGBA (c_blk);
	}
	rounded_rectangle (cr, 0, top, ui->m0_width, HGHT (M0HEIGHT), 6);
	cairo_fill_preserve (cr);
	cairo_clip (cr);

	/* meter background */
	cairo_set_source (cr, ui->m_bg);
	cairo_rectangle (cr, 5, YPOS (4), disp_w + 10, HGHT (12));
	cairo_fill (cr);

	/* meter ticks */
	cairo_set_line_width (cr, yscale);
	if (is_light_theme ()) {
		CairoSetSouerceRGBA (c_blk);
	} else {
		CairoSetSouerceRGBA (c_wht);
	}
	for (int i = 0; i < 7; ++i) {
		float dbx = DEFLECT (-20 + i * 10);
		cairo_move_to (cr, 10 + dbx, YPOS (2));
		cairo_line_to (cr, 10 + dbx, YPOS (18));
		cairo_stroke (cr);

		int          tw, th;
		PangoLayout* pl = pango_cairo_create_layout (cr);
		pango_layout_set_font_description (pl, ui->font[1]);

		if (i == 0) {
			pango_layout_set_text (pl, "Gain:", -1);
			pango_layout_get_pixel_size (pl, &tw, &th);
			cairo_move_to (cr, 5 + dbx, YPOS (20));
			pango_cairo_show_layout (cr, pl);
			g_object_unref (pl);
			continue;
		}
		char txt[16];
		snprintf (txt, 16, "%+2d ", (i - 2) * 10);
		pango_layout_set_text (pl, txt, -1);
		pango_layout_get_pixel_size (pl, &tw, &th);
		cairo_move_to (cr, 10 + dbx - tw * .5, YPOS (20));
		pango_cairo_show_layout (cr, pl);
		g_object_unref (pl);
	}
}

static bool
m0_expose_event (RobWidget* handle, cairo_t* cr, cairo_rectangle_t* ev)
{
	darcUI* ui = (darcUI*)GET_HANDLE (handle);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
	cairo_rectangle (cr, ev->x, ev->y, ev->width, ev->height);
	cairo_clip_preserve (cr);

	float c[4];
	get_color_from_theme (1, c);
	cairo_set_source_rgb (cr, c[0], c[1], c[2]);
	cairo_fill (cr);

	const uint32_t yscale = ui->m0_height / M0HEIGHT;
	const uint32_t top    = (ui->m0_height - M0HEIGHT * yscale) * .5;
	const uint32_t disp_w = ui->m0_width - 20; // deafult: 300

#define YPOS(y) (top + yscale * (y))
#define HGHT(y) (yscale * (y))

#define DEF(x) MAX (0, MIN (1., ((20. + (x)) / 60.)))
#define DEFLECT(x) (rint (disp_w * DEF (x)) - .5)

	if (!ui->m_fg) {
		cairo_pattern_t* pat = cairo_pattern_create_linear (10, 0.0, disp_w, 0.0);
		/* clang-format off */
		cairo_pattern_add_color_stop_rgb (pat, DEF (40),  .1, .9, .1);
		cairo_pattern_add_color_stop_rgb (pat, DEF (5),   .1, .9, .1);
		cairo_pattern_add_color_stop_rgb (pat, DEF (-5),  .9, .9, .1);
		cairo_pattern_add_color_stop_rgb (pat, DEF (-20), .9, .9, .1);
		/* clang-format on */
		ui->m_fg = pat;
	}

	if (!ui->m_bg) {
		const float      alpha = 0.5;
		cairo_pattern_t* pat   = cairo_pattern_create_linear (10, 0.0, disp_w, 0.0);
		/* clang-format off */
		cairo_pattern_add_color_stop_rgba (pat, DEF (40),  .0, .5, .0, alpha);
		cairo_pattern_add_color_stop_rgba (pat, DEF (5),   .0, .5, .0, alpha);
		cairo_pattern_add_color_stop_rgba (pat, DEF (-5),  .5, .0, .0, alpha);
		cairo_pattern_add_color_stop_rgba (pat, DEF (-20), .5, .0, .0, alpha);
		/* clang-format on */
		ui->m_bg = pat;
	}

	if (!ui->m0bg) {
		ui->m0bg     = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, ui->m0_width, ui->m0_height);
		cairo_t* icr = cairo_create (ui->m0bg);
		m0_render_faceplate (ui, icr);
		cairo_destroy (icr);
	}

	cairo_set_source_surface (cr, ui->m0bg, 0, 0);
	cairo_paint (cr);

	/* current reduction */
	float v0 = DEFLECT (ui->_gmin);
	float v1 = DEFLECT (ui->_gmax);
	cairo_rectangle (cr, 7.5 + v0, YPOS (4), 5 + v1 - v0, HGHT (12));
	cairo_set_source (cr, ui->m_fg);
	cairo_fill (cr);

	return TRUE;
}

/* ****************************************************************************/

static float
comp_curve (float in, float threshold, float ratio, bool hold)
{
	float key = in;
	if (hold && in < threshold) {
		key = threshold;
	}
#ifdef __USE_GNU
	float g = logf (exp10f (1.f + .1f * threshold) + exp10f (1.f + .1f * key));
#else
	float g = logf (powf (10.f, 1.f + .1f * threshold) + powf (10.f, 1.f + .1f * key));
#endif
	return /*-10/log(10)*/ -4.342944819f * ratio * g + in;
}

/* ****************************************************************************/

#define M1RECT 350

static void
m1_size_request (RobWidget* handle, int* w, int* h)
{
	darcUI* ui = (darcUI*)GET_HANDLE (handle);

	*w = M1RECT * ui->rw->widget_scale;
	*h = M1RECT * ui->rw->widget_scale;
}

static void
m1_size_allocate (RobWidget* handle, int w, int h)
{
	darcUI* ui = (darcUI*)GET_HANDLE (handle);

	if (ui->m1_grid) {
		cairo_surface_destroy (ui->m1_grid);
	}
	if (ui->m1_ctrl) {
		cairo_surface_destroy (ui->m1_ctrl);
	}
	if (ui->m1_mask) {
		cairo_surface_destroy (ui->m1_mask);
	}
	ui->m1_grid = NULL;
	ui->m1_ctrl = NULL;
	ui->m1_mask = NULL;

	queue_draw (ui->m1);
}

static void
m1_render_grid (darcUI* ui, cairo_t* cr)
{
	cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

	cairo_scale (cr, ui->rw->widget_scale, ui->rw->widget_scale);

	if (is_light_theme ()) {
		CairoSetSouerceRGBA (c_g80);
	} else {
		CairoSetSouerceRGBA (c_blk);
	}
	rounded_rectangle (cr, 0, 0, M1RECT, M1RECT, 8);
	cairo_fill_preserve (cr);
	cairo_clip (cr);

	/* draw grid 10dB steps */
	cairo_set_line_width (cr, 1.0);

	const double dash1[] = { 1, 2 };
	const double dash2[] = { 1, 3 };

	cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_dash (cr, dash2, 2, 2);

	for (uint32_t d = 1; d < 7; ++d) {
		const float x = -.5 + floor (M1RECT * d * 10.f / 70.f);
		const float y = -.5 + floor (M1RECT * (70 - d * 10.f) / 70.f);

		cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.5);

		cairo_move_to (cr, x, 0);
		cairo_line_to (cr, x, M1RECT);
		cairo_stroke (cr);

		cairo_move_to (cr, 0, y);
		cairo_line_to (cr, M1RECT, y);
		cairo_stroke (cr);

		char txt[16];
		snprintf (txt, 16, "%+2d", -60 + d * 10);
		write_text_full (cr, txt, ui->font[1], x, M1RECT * (10.f / 70.f) - 2, 0, 5, c_dlf);
		if (d != 6) {
			write_text_full (cr, txt, ui->font[1], M1RECT * (60.f / 70.f) + 2, y, M_PI * .5, 5, c_dlf);
		}
	}

	/* diagonal unity */
	cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 1.0);
	cairo_set_dash (cr, dash1, 2, 2);
	cairo_move_to (cr, 0, M1RECT);
	cairo_line_to (cr, M1RECT, 0);
	cairo_stroke (cr);

	cairo_set_dash (cr, 0, 0, 0);

	write_text_full (cr, "Output", ui->font[0], M1RECT * (65.f / 70.f), M1RECT * .5, M_PI * .5, 5, c_dlf);
	write_text_full (cr, "Input [dBFS/RMS]", ui->font[0], M1RECT * .5, M1RECT * (5.f / 70.f), 0, 5, c_dlf);

	/* 0dBFS limit indicator */
	cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.5);
	const float x = -.5 + floor (M1RECT * 60.f / 70.f);
	const float y = -.5 + floor (M1RECT * 10.f / 70.f);
	cairo_move_to (cr, x, 0);
	cairo_line_to (cr, x, M1RECT);
	cairo_stroke (cr);
	cairo_move_to (cr, 0, y);
	cairo_line_to (cr, M1RECT, y);
	cairo_stroke (cr);
}

static void
m1_render_mask (darcUI* ui)
{
	if (ui->m1_ctrl) {
		cairo_surface_destroy (ui->m1_ctrl);
	}
	if (ui->m1_mask) {
		cairo_surface_destroy (ui->m1_mask);
	}

	int sq      = M1RECT * ui->rw->widget_scale;
	ui->m1_ctrl = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, sq, sq);
	ui->m1_mask = cairo_image_surface_create (CAIRO_FORMAT_A8, M1RECT, M1RECT);

	cairo_t* cr = cairo_create (ui->m1_ctrl);
	cairo_t* cm = cairo_create (ui->m1_mask);

	cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
	cairo_set_operator (cm, CAIRO_OPERATOR_CLEAR);
	cairo_paint (cm);
	cairo_set_operator (cm, CAIRO_OPERATOR_OVER);

	cairo_scale (cr, ui->rw->widget_scale, ui->rw->widget_scale);

	rounded_rectangle (cr, 0, 0, M1RECT, M1RECT, 8);
	cairo_clip (cr);

	rounded_rectangle (cm, 0, 0, M1RECT, M1RECT, 8);
	cairo_clip (cm);

	const float thrsh = gui_to_ctrl (1, robtk_dial_get_value (ui->spn_ctrl[1]));
	const float ratio = gui_to_ctrl (2, robtk_dial_get_value (ui->spn_ctrl[2]));
	const bool  hold  = robtk_cbtn_get_active (ui->btn_hold);

	if (is_light_theme ()) {
		cairo_set_source_rgba (cr, .2, .2, .2, 1.0);
	} else {
		cairo_set_source_rgba (cr, .8, .8, .8, 1.0);
	}
	cairo_set_line_width (cr, 1.0);

	if (hold) {
		cairo_move_to (cr, 0, M1RECT * ((comp_curve (-60, thrsh, ratio, true) - 10) / -70.f));

		uint32_t x = 1;
		for (; x <= M1RECT; ++x) {
			const float x_db = 70.f * (-1.f + x / (float)M1RECT) + 10.f;
			const float y_db = comp_curve (x_db, thrsh, ratio, true) - 10;
			const float y    = M1RECT * (y_db / -70.f);
			cairo_line_to (cr, x, y);
			if (x_db > thrsh) {
				break;
			}
		}

		const double dash1[] = { 1, 2, 4, 2 };
		cairo_set_dash (cr, dash1, 4, 0);
		cairo_stroke_preserve (cr);
		cairo_set_dash (cr, NULL, 0, 0);

		for (; x > 0; --x) {
			const float x_db = 70.f * (-1.f + x / (float)M1RECT) + 10.f;
			const float y_db = comp_curve (x_db, thrsh, ratio, false) - 10;
			const float y    = M1RECT * (y_db / -70.f);
			cairo_line_to (cr, x, y);
		}

		cairo_close_path (cr);

		cairo_set_source_rgba (cr, 0, 0, .5, .5);
		cairo_fill (cr);
	}

	cairo_move_to (cr, 0, M1RECT * ((comp_curve (-60, thrsh, ratio, false) - 10) / -70.f));
	cairo_move_to (cm, 0, M1RECT * ((comp_curve (-60, thrsh, ratio, hold) - 10) / -70.f));

	if (is_light_theme ()) {
		cairo_set_source_rgba (cr, .2, .2, .2, 1.0);
	} else {
		cairo_set_source_rgba (cr, .8, .8, .8, 1.0);
	}

	for (uint32_t x = 1; x <= M1RECT; ++x) {
		const float x_db = 70.f * (-1.f + x / (float)M1RECT) + 10.f;
		const float y_db = comp_curve (x_db, thrsh, ratio, false) - 10;
		const float h_db = comp_curve (x_db, thrsh, ratio, hold) - 10;
		cairo_line_to (cr, x, M1RECT * (y_db / -70.f));
		cairo_line_to (cm, x, M1RECT * (h_db / -70.f));
	}
	cairo_stroke_preserve (cr);

	cairo_line_to (cr, M1RECT, M1RECT);
	cairo_line_to (cr, 0, M1RECT);
	cairo_close_path (cr);

	cairo_line_to (cm, M1RECT, M1RECT);
	cairo_line_to (cm, 0, M1RECT);
	cairo_close_path (cm);

	cairo_set_source_rgba (cr, 1, 1, 1, .1);
	cairo_fill (cr);

	cairo_set_source_rgba (cm, 1, 1, 1, 1);
	cairo_fill (cm);

	cairo_destroy (cr);
	cairo_destroy (cm);
}

static bool
m1_expose_event (RobWidget* handle, cairo_t* cr, cairo_rectangle_t* ev)
{
	darcUI* ui = (darcUI*)GET_HANDLE (handle);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
	cairo_rectangle (cr, ev->x, ev->y, ev->width, ev->height);
	cairo_clip_preserve (cr);

	float c[4];
	get_color_from_theme (1, c);
	cairo_set_source_rgb (cr, c[0], c[1], c[2]);
	cairo_fill (cr);

	if (!ui->m1_grid) {
		int sq       = M1RECT * ui->rw->widget_scale;
		ui->m1_grid  = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, sq, sq);
		cairo_t* icr = cairo_create (ui->m1_grid);
		m1_render_grid (ui, icr);
		cairo_destroy (icr);
	}

	if (!ui->m1_ctrl || !ui->m1_mask || ui->ctrl_dirty) {
		ui->ctrl_dirty = false;
		m1_render_mask (ui);
	}

	cairo_set_source_surface (cr, ui->m1_grid, 0, 0);
	cairo_paint (cr);

	cairo_set_source_surface (cr, ui->m1_ctrl, 0, 0);
	cairo_paint (cr);

	cairo_scale (cr, ui->rw->widget_scale, ui->rw->widget_scale);

	const float thrsh = gui_to_ctrl (1, robtk_dial_get_value (ui->spn_ctrl[1]));
	const bool  hold  = robtk_cbtn_get_active (ui->btn_hold);

	float thx = (thrsh + 60.f) * M1RECT / 70.f;
	if (hold) {
		/* shade area where hold is active */
		cairo_save (cr);
		cairo_rectangle (cr, 0, 0, thx, M1RECT);
		cairo_clip (cr);
		cairo_set_source_rgba (cr, 0.0, 0.0, .7, .1);
		cairo_mask_surface (cr, ui->m1_mask, 0, 0);
		cairo_fill (cr);
		cairo_restore (cr);
	}

	cairo_set_line_width (cr, 1);
	cairo_move_to (cr, floor (thx) - .5, M1RECT * 9.f / 70.f);
	cairo_line_to (cr, floor (thx) - .5, M1RECT);
	cairo_set_source_rgba (cr, .8, .7, .1, .9);
	const double dash1[] = { 1, 1 };
	cairo_set_dash (cr, dash1, 2, 0);
	cairo_stroke (cr);
	cairo_set_dash (cr, NULL, 0, 0);

	float pkx = (ui->_rms + 60.f) * M1RECT / 70.f;
	if (pkx > 0) {
		cairo_save (cr);
		cairo_rectangle (cr, 0, 0, MIN (M1RECT, pkx), M1RECT);
		cairo_clip (cr);
		if (is_light_theme ()) {
			cairo_set_source_rgba (cr, 0.4, 0.4, 0.4, 0.5);
		} else {
			cairo_set_source_rgba (cr, 0.6, 0.6, 0.6, 0.5);
		}
		cairo_mask_surface (cr, ui->m1_mask, 0, 0);
		cairo_fill (cr);
		cairo_restore (cr);

		cairo_save (cr);
		cairo_rectangle (cr, 0, 0, MIN (M1RECT, pkx + 6), M1RECT);
		cairo_clip (cr);
		cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

		float pky0 = (ui->_rms + ui->_gmax - 10.f) * M1RECT / -70.f;
		float pky1 = (ui->_rms + ui->_gmin - 10.f) * M1RECT / -70.f;
		cairo_move_to (cr, pkx, pky0);
		cairo_line_to (cr, pkx, pky1);
		cairo_set_line_width (cr, 5);
		cairo_set_source_rgba (cr, 1, 1, 1, 1);
		cairo_stroke (cr);
		cairo_restore (cr);
	}

	return TRUE;
}

/* ****************************************************************************/

static void
m2_size_request (RobWidget* handle, int* w, int* h)
{
	*w = 12;
	*h = 10;
}

static void
m2_size_allocate (RobWidget* rw, int w, int h)
{
	robwidget_set_size (rw, w, h);
}

static bool
m2_expose_event (RobWidget* rw, cairo_t* cr, cairo_rectangle_t* ev)
{
	darcUI* ui = (darcUI*)GET_HANDLE (rw);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
	cairo_rectangle (cr, ev->x, ev->y, ev->width, ev->height);
	cairo_clip (cr);
	cairo_rectangle (cr, 0, 0, rw->area.width, rw->area.height);
	cairo_clip_preserve (cr);

	float c[4];
	get_color_from_theme (1, c);
	cairo_set_source_rgb (cr, c[0], c[1], c[2]);
	cairo_fill (cr);

	cairo_scale (cr, ui->rw->widget_scale, ui->rw->widget_scale);
	if (ui->nfo) {
		write_text_full (cr, ui->nfo, ui->font[0], 0, .5 * rw->area.height / ui->rw->widget_scale, 0, 3, c_gry);
	}
	return TRUE;
}

/* ****************************************************************************/

static RobWidget*
toplevel (darcUI* ui, void* const top)
{
	/* main widget: layout */
	ui->rw = rob_vbox_new (FALSE, 2);
	robwidget_make_toplevel (ui->rw, top);
	robwidget_toplevel_enable_scaling (ui->rw, NULL, NULL);

	ui->font[0] = pango_font_description_from_string ("Mono 9px");
	ui->font[1] = pango_font_description_from_string ("Mono 10px");

	prepare_faceplates (ui);

	/* level display */
	ui->m0 = robwidget_new (ui);
	robwidget_set_alignment (ui->m0, .5, .5);
	robwidget_set_expose_event (ui->m0, m0_expose_event);
	robwidget_set_size_request (ui->m0, m0_size_request);
	robwidget_set_size_allocate (ui->m0, m0_size_allocate);

	/* graph display */
	ui->m1 = robwidget_new (ui);
	robwidget_set_alignment (ui->m1, .5, .5);
	robwidget_set_expose_event (ui->m1, m1_expose_event);
	robwidget_set_size_request (ui->m1, m1_size_request);
	robwidget_set_size_allocate (ui->m1, m1_size_allocate);

	/* control knob table */
	ui->ctbl      = rob_table_new (/*rows*/ 3, /*cols*/ 5, FALSE);
	ui->ctbl->top = (void*)ui;

#define GSP_W(PTR) robtk_dial_widget (PTR)
#define GLB_W(PTR) robtk_lbl_widget (PTR)
#define GBT_W(PTR) robtk_cbtn_widget (PTR)

	for (uint32_t i = 0; i < 5; ++i) {
		ui->lbl_ctrl[i] = robtk_lbl_new (ctrl_range[i].name);
		ui->spn_ctrl[i] = robtk_dial_new_with_size (
		    k_min (i), k_max (i), k_step (i),
		    GED_WIDTH + 8, GED_HEIGHT + 20, GED_CX + 4, GED_CY + 15, GED_RADIUS);
		ui->spn_ctrl[i]->with_scroll_accel = false;

		robtk_dial_set_value (ui->spn_ctrl[i], ctrl_to_gui (i, ctrl_range[i].dflt));
		robtk_dial_set_callback (ui->spn_ctrl[i], cb_spn_ctrl, ui);
		robtk_dial_set_default (ui->spn_ctrl[i], ctrl_to_gui (i, ctrl_range[i].dflt));
		robtk_dial_set_scroll_mult (ui->spn_ctrl[i], ctrl_range[i].mult);

		if (ui->touch) {
			robtk_dial_set_touch (ui->spn_ctrl[i], ui->touch->touch, ui->touch->handle, DARC_INPUTGAIN + i);
		}

		robtk_dial_set_scaled_surface_scale (ui->spn_ctrl[i], ui->dial_bg[i], 2.0);
		robtk_lbl_annotation_callback (ui->lbl_ctrl[i], ttip_handler, ui);

		rob_table_attach (ui->ctbl, GSP_W (ui->spn_ctrl[i]), i, i + 1, 0, 1, 4, 0, RTK_EXANDF, RTK_SHRINK);
		rob_table_attach (ui->ctbl, GLB_W (ui->lbl_ctrl[i]), i, i + 1, 1, 2, 4, 0, RTK_EXANDF, RTK_SHRINK);
	}

	/* snap at 0dB gain */
	robtk_dial_set_detent_default (ui->spn_ctrl[0], true);

	/* use 'dot' for time knobs */
	ui->spn_ctrl[3]->displaymode = 3;
	ui->spn_ctrl[4]->displaymode = 3; // use dot

	/* these numerics are meaningful */
	robtk_dial_annotation_callback (ui->spn_ctrl[0], dial_annotation_db, ui);
	robtk_dial_annotation_callback (ui->spn_ctrl[1], dial_annotation_db, ui);
	robtk_dial_annotation_callback (ui->spn_ctrl[2], dial_annotation_rr, ui);
	robtk_dial_annotation_callback (ui->spn_ctrl[3], dial_annotation_tm, ui);
	robtk_dial_annotation_callback (ui->spn_ctrl[4], dial_annotation_tm, ui);

	/* custom knob colors */
	{
		const float c_bg[4] = { .7, .7, .1, 1.0 };
		create_dial_pattern (ui->spn_ctrl[0], c_bg);
		ui->spn_ctrl[0]->dcol[0][0] =
		    ui->spn_ctrl[0]->dcol[0][1] =
		        ui->spn_ctrl[0]->dcol[0][2] = .05;
	}
	{
		const float c_bg[4] = { .8, .3, .0, 1.0 };
		create_dial_pattern (ui->spn_ctrl[1], c_bg);
		ui->spn_ctrl[1]->dcol[0][0] =
		    ui->spn_ctrl[1]->dcol[0][1] =
		        ui->spn_ctrl[1]->dcol[0][2] = .05;
	}
	{
		const float c_bg[4] = { .9, .2, .2, 1.0 };
		create_dial_pattern (ui->spn_ctrl[2], c_bg);
		ui->spn_ctrl[2]->dcol[0][0] =
		    ui->spn_ctrl[2]->dcol[0][1] =
		        ui->spn_ctrl[2]->dcol[0][2] = .05;
	}
	{
		const float c_bg[4] = { .3, .3, .7, 1.0 };
		create_dial_pattern (ui->spn_ctrl[3], c_bg);
		ui->spn_ctrl[3]->dcol[0][0] =
		    ui->spn_ctrl[3]->dcol[0][1] =
		        ui->spn_ctrl[3]->dcol[0][2] = .05;
		create_dial_pattern (ui->spn_ctrl[4], c_bg);
		ui->spn_ctrl[4]->dcol[0][0] =
		    ui->spn_ctrl[4]->dcol[0][1] =
		        ui->spn_ctrl[4]->dcol[0][2] = .05;
	}

	/* explicit hold button */
	ui->btn_hold = robtk_cbtn_new ("Hold", GBT_LED_RIGHT, false);
	robtk_cbtn_set_callback (ui->btn_hold, cb_btn_hold, ui);
	rob_table_attach (ui->ctbl, GBT_W (ui->btn_hold), 4, 5, 3, 4, 8, 2, RTK_EXANDF, RTK_SHRINK);

	robtk_cbtn_set_temporary_mode (ui->btn_hold, 1);
	robtk_cbtn_set_color_on (ui->btn_hold, 0.1, 0.3, 0.8);
	robtk_cbtn_set_color_off (ui->btn_hold, .1, .1, .3);
	robtk_cbtn_annotation_callback (ui->btn_hold, ttip_handler, ui);

	/* version info */

	ui->m2 = robwidget_new (ui);
	robwidget_set_alignment (ui->m2, 0, 0);
	robwidget_set_expose_event (ui->m2, m2_expose_event);
	robwidget_set_size_request (ui->m2, m2_size_request);
	robwidget_set_size_allocate (ui->m2, m2_size_allocate);

	rob_table_attach (ui->ctbl, ui->m2, 0, 2, 3, 4, 8, 2, RTK_FILL, RTK_FILL);

	/* top-level packing */
	rob_vbox_child_pack (ui->rw, ui->m1, FALSE, TRUE);
	rob_vbox_child_pack (ui->rw, ui->ctbl, FALSE, TRUE);
	rob_vbox_child_pack (ui->rw, ui->m0, TRUE, TRUE);
	robwidget_set_leave_notify(ui->rw, top_leave_notify);
	return ui->rw;
}

static void
gui_cleanup (darcUI* ui)
{
	for (int i = 0; i < 5; ++i) {
		robtk_dial_destroy (ui->spn_ctrl[i]);
		robtk_lbl_destroy (ui->lbl_ctrl[i]);
		cairo_surface_destroy (ui->dial_bg[i]);
	}

	pango_font_description_free (ui->font[0]);
	pango_font_description_free (ui->font[1]);

	if (ui->m_fg) {
		cairo_pattern_destroy (ui->m_fg);
	}
	if (ui->m_bg) {
		cairo_pattern_destroy (ui->m_bg);
	}
	if (ui->m0bg) {
		cairo_surface_destroy (ui->m0bg);
	}
	if (ui->m1_grid) {
		cairo_surface_destroy (ui->m1_grid);
	}
	if (ui->m1_ctrl) {
		cairo_surface_destroy (ui->m1_ctrl);
	}
	if (ui->m1_mask) {
		cairo_surface_destroy (ui->m1_mask);
	}

	robtk_cbtn_destroy (ui->btn_hold);
	robwidget_destroy (ui->m0);
	robwidget_destroy (ui->m1);
	robwidget_destroy (ui->m2);
	rob_table_destroy (ui->ctbl);
	rob_box_destroy (ui->rw);
}

/* *****************************************************************************
 * RobTk + LV2
 */

#define LVGL_RESIZEABLE

static void
ui_enable (LV2UI_Handle handle)
{
}

static void
ui_disable (LV2UI_Handle handle)
{
}

static enum LVGLResize
plugin_scale_mode (LV2UI_Handle handle)
{
	return LVGL_LAYOUT_TO_FIT;
}

static LV2UI_Handle
instantiate (
    void* const               ui_toplevel,
    const LV2UI_Descriptor*   descriptor,
    const char*               plugin_uri,
    const char*               bundle_path,
    LV2UI_Write_Function      write_function,
    LV2UI_Controller          controller,
    RobWidget**               widget,
    const LV2_Feature* const* features)
{
	darcUI* ui = (darcUI*)calloc (1, sizeof (darcUI));
	if (!ui) {
		return NULL;
	}

	if (strcmp (plugin_uri, RTK_URI "mono") && strcmp (plugin_uri, RTK_URI "stereo")) {
		free (ui);
		return NULL;
	}

	const LV2_Options_Option* options = NULL;
	const LV2_URID_Map*       map     = NULL;

	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_UI__touch)) {
			ui->touch = (LV2UI_Touch*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_URID__map)) {
			map = (LV2_URID_Map*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_OPTIONS__options)) {
			options = (LV2_Options_Option*)features[i]->data;
		}
	}

	interpolate_fg_bg (c_dlf, .2);

	ui->nfo             = robtk_info (ui_toplevel);
	ui->write           = write_function;
	ui->controller      = controller;
	ui->ctrl_dirty      = false;
	ui->disable_signals = true;
	*widget             = toplevel (ui, ui_toplevel);
	ui->disable_signals = false;

	if (options && map) {
		LV2_URID atom_Float = map->map (map->handle, LV2_ATOM__Float);
		LV2_URID ui_scale   = map->map (map->handle, "http://lv2plug.in/ns/extensions/ui#scaleFactor");
		for (const LV2_Options_Option* o = options; o->key; ++o) {
			if (o->context == LV2_OPTIONS_INSTANCE && o->key == ui_scale && o->type == atom_Float) {
				float ui_scale = *(const float*)o->value;
				if (ui_scale < 1.0) {
					ui_scale = 1.0;
				}
				if (ui_scale > 2.0) {
					ui_scale = 2.0;
				}
				robtk_queue_scale_change (ui->rw, ui_scale);
			}
		}
	}
	return ui;
}

static void
cleanup (LV2UI_Handle handle)
{
	darcUI* ui = (darcUI*)handle;
	gui_cleanup (ui);
	free (ui);
}

/* receive information from DSP */
static void
port_event (LV2UI_Handle handle,
            uint32_t     port_index,
            uint32_t     buffer_size,
            uint32_t     format,
            const void*  buffer)
{
	darcUI* ui = (darcUI*)handle;

	if (format != 0) {
		return;
	}

	if (port_index == DARC_GMIN) {
		ui->_gmin = *(float*)buffer;
		/* TODO: partial exposure, only redraw changed gain area */
		queue_draw (ui->m0);
		queue_draw (ui->m1);
	} else if (port_index == DARC_GMAX) {
		ui->_gmax = *(float*)buffer;
		queue_draw (ui->m0);
		queue_draw (ui->m1);
	} else if (port_index == DARC_RMS) {
		ui->_rms = *(float*)buffer;
		queue_draw (ui->m1);
	} else if (port_index == DARC_HOLD) {
		ui->disable_signals = true;
		robtk_cbtn_set_active (ui->btn_hold, (*(float*)buffer) > 0);
		ui->disable_signals = false;
	} else if (port_index >= DARC_INPUTGAIN && port_index <= DARC_RELEASE) {
		const float v       = *(float*)buffer;
		ui->disable_signals = true;
		uint32_t ctrl       = port_index - DARC_INPUTGAIN;
		robtk_dial_set_value (ui->spn_ctrl[ctrl], ctrl_to_gui (ctrl, v));
		ui->disable_signals = false;
	}
}

static const void*
extension_data (const char* uri)
{
	return NULL;
}
