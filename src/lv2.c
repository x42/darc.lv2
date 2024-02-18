/* darc.lv2
 *
 * Copyright (C) 2018,2019 Robin Gareus <robin@gareus.org>
 * inspired by Fons Adriaensen's zita-dc1
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
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "darc.h"

#ifdef HAVE_LV2_1_18_6
#include <lv2/core/lv2.h>
#else
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#endif

#ifdef DISPLAY_INTERFACE
#include "lv2_rgext.h"
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#endif

#ifndef MAX
#define MAX(A, B) ((A) > (B)) ? (A) : (B)
#endif

#ifndef MIN
#define MIN(A, B) ((A) < (B)) ? (A) : (B)
#endif

/* ****************************************************************************/

typedef struct {
	float sample_rate;

	uint32_t n_channels;
	float    norm_input;

	float ratio;
	float p_rat;

	bool hold;

	float igain;
	float p_ign;
	float l_ign;

	float p_thr;
	float l_thr;

	float w_att;
	float w_rel;
	float t_att;
	float t_rel;

	float za1;
	float zr1;
	float zr2;

	bool  newg;
	float gmax;
	float gmin;

	float rms;
	float w_rms;
	float w_lpf;

} Dyncomp;

static inline void
Dyncomp_reset (Dyncomp* self)
{
	self->za1  = 0.f;
	self->zr1  = 0.f;
	self->zr2  = 0.f;
	self->rms  = 0.f;
	self->gmin = 0.f;
	self->gmax = 0.f;
	self->newg = true;
}

static inline void
Dyncomp_set_ratio (Dyncomp* self, float r)
{
	self->p_rat = 0.5f * r;
}

static inline void
Dyncomp_set_inputgain (Dyncomp* self, float g)
{
	if (g == self->l_ign) {
		return;
	}
	self->l_ign = g;
#ifdef __USE_GNU
	self->p_ign = exp10f (0.05f * g);
#else
	self->p_ign = powf (10.0f, 0.05f * g);
#endif
}

static inline void
Dyncomp_set_threshold (Dyncomp* self, float t)
{
	if (t == self->l_thr) {
		return;
	}
	self->l_thr = t;
	/* Note that this is signal-power, hence .5 * 10^(x/10) */
#ifdef __USE_GNU
	self->p_thr = 0.5f * exp10f (0.1f * t);
#else
	self->p_thr = 0.5f * powf (10.0f, 0.1f * t);
#endif
}

static inline void
Dyncomp_set_hold (Dyncomp* self, bool hold)
{
	self->hold = hold;
}

static inline void
Dyncomp_set_attack (Dyncomp* self, float a)
{
	if (a == self->t_att) {
		return;
	}
	self->t_att = a;
	self->w_att = 0.5f / (self->sample_rate * a);
}

static inline void
Dyncomp_set_release (Dyncomp* self, float r)
{
	if (r == self->t_rel) {
		return;
	}
	self->t_rel = r;
	self->w_rel = 3.5f / (self->sample_rate * r);
}

static inline void
Dyncomp_get_gain (Dyncomp* self, float* gmin, float* gmax, float* rms)
{
	*gmin = self->gmin * 8.68589f; /* 20 / log(10) */
	*gmax = self->gmax * 8.68589f;
	if (self->rms > 1e-8f) {
		*rms = 10.f * log10f (2.f * self->rms);
	} else {
		*rms = -80;
	}
	self->newg = true;
}

static inline void
Dyncomp_init (Dyncomp* self, float sample_rate, uint32_t n_channels)
{
	self->sample_rate = sample_rate;
	self->n_channels  = n_channels;
	self->norm_input  = 1.f / n_channels;

	self->ratio = 0.f;
	self->p_rat = 0.f;

	self->igain = 1.f;
	self->p_ign = 1.f;
	self->l_ign = 0.f;

	self->p_thr = 0.05f;
	self->l_thr = -10.f;

	self->hold = false;

	self->t_att = 0.f;
	self->t_rel = 0.f;

	self->w_rms = 5.f / sample_rate;
	self->w_lpf = 160.f / sample_rate;

	Dyncomp_set_attack (self, 0.01f);
	Dyncomp_set_release (self, 0.03f);
	Dyncomp_reset (self);
}

static inline void
Dyncomp_process (Dyncomp* self, uint32_t n_samples, float* io[])
{
	float gmin, gmax;

	/* reset min/max gain report */
	if (self->newg) {
		gmax       = -100.0f;
		gmin       = 100.0f;
		self->newg = false;
	} else {
		gmax = self->gmax;
		gmin = self->gmin;
	}

	/* interpolate input gain */
	float       g  = self->igain;
	const float g1 = self->p_ign;
	float       dg = g1 - g;
	if (fabsf (dg) < 1e-5f || (g > 1.f && fabsf (dg) < 1e-3f)) {
		g  = g1;
		dg = 0;
	}

	/* interpolate ratio */
	float       r  = self->ratio;
	const float r1 = self->p_rat;
	float       dr = r1 - r;
	if (fabsf (dr) < 1e-5f) {
		r  = r1;
		dr = 0;
	}

	/* localize variables */
	float za1 = self->za1;
	float zr1 = self->zr1;
	float zr2 = self->zr2;

	float rms = self->rms;

	const float w_rms = self->w_rms;
	const float w_lpf = self->w_lpf;
	const float w_att = self->w_att;
	const float w_rel = self->w_rel;
	const float p_thr = self->p_thr;

	const float p_hold = self->hold ? 2.f * p_thr : 0.f;

	const uint32_t nc  = self->n_channels;
	const float    n_1 = self->norm_input;

	for (uint32_t j = 0; j < n_samples; ++j) {
		/* update input gain */
		if (dg != 0) {
			g += w_lpf * (g1 - g);
		}

		/* Input/Key RMS */
		float v = 0;
		for (uint32_t i = 0; i < nc; ++i) {
			const float x = g * io[i][j];
			v += x * x;
		}

		v *= n_1; // normalize *= 1 / (number of channels)

		/* slow moving RMS, used for GUI level meter display */
		rms += w_rms * (v - rms); // TODO: consider reporting range; 5ms integrate, 50ms min/max readout

		/* calculate signal power relative to threshold, LPF using attack time constant */
		za1 += w_att * (p_thr + v - za1);

		/* hold release */
		const bool hold = 0 != isless (za1, p_hold);

		/* Note: za1 >= p_thr; so zr1, zr2 can't become denormal */
		if (isless (zr1, za1)) {
			zr1 = za1;
		} else if (!hold) {
			zr1 -= w_rel * zr1;
		}

		if (isless (zr2, za1)) {
			zr2 = za1;
		} else if (!hold) {
			zr2 += w_rel * (zr1 - zr2);
		}

		/* update ratio */
		if (dr != 0) {
			r += w_lpf * (r1 - r);
		}

		/* Note: expf (a * logf (b)) == powf (b, a);
		 * however powf() is significantly slower
		 *
		 * Effective gain is  (zr2) ^ (-ratio).
		 *
		 * with 0 <= ratio <= 0.5 and
		 * zr2 being low-pass (attack/release) filtered square of the key-signal.
		 */

		float pg = -r * logf (20.0f * zr2);

		/* store min/max gain in dB, report to UI */
		gmax = fmaxf (gmax, pg);
		gmin = fminf (gmin, pg);

		pg = g * expf (pg);

		/* apply gain factor to all channels */
		for (uint32_t i = 0; i < nc; ++i) {
			io[i][j] *= pg;
		}
	}

	/* copy back variables */
	self->igain = g;
	self->ratio = r;

	if (!isfinite (za1)) {
		self->za1  = 0.f;
		self->zr1  = 0.f;
		self->zr2  = 0.f;
		self->newg = true; /* reset gmin/gmax next cycle */
	} else {
		self->za1  = za1;
		self->zr1  = zr1;
		self->zr2  = zr2;
		self->gmax = gmax;
		self->gmin = gmin;
	}

	if (!isfinite (rms)) {
		self->rms = 0.f;
	} else if (rms > 10) {
		self->rms = 10; // 20dBFS
	} else {
		self->rms = rms + 1e-12; // + denormal protection
	}
}

/* ****************************************************************************/

typedef struct {
	float* _port[DARC_LAST];

	Dyncomp dyncomp;

	float _gmin;
	float _gmax;
	float _rms;

	uint32_t samplecnt;
	uint32_t sampletme; // 50ms

#ifdef DISPLAY_INTERFACE
	LV2_Inline_Display_Image_Surface surf;
	cairo_surface_t*                 display;
	LV2_Inline_Display*              queue_draw;
	cairo_pattern_t*                 mpat;
	cairo_pattern_t*                 cpat;
	uint32_t                         w, h;
	float                            ui_gmin;
	float                            ui_gmax;
#endif

} Darc;

static LV2_Handle
instantiate (const LV2_Descriptor*     descriptor,
             double                    rate,
             const char*               bundle_path,
             const LV2_Feature* const* features)
{
	Darc* self = (Darc*)calloc (1, sizeof (Darc));

	uint32_t n_channels;

	if (!strcmp (descriptor->URI, DARC_URI "mono")) {
		n_channels = 1;
	} else if (!strcmp (descriptor->URI, DARC_URI "stereo")) {
		n_channels = 2;
	} else {
		free (self);
		return NULL;
	}

#ifdef DISPLAY_INTERFACE
	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_INLINEDISPLAY__queue_draw)) {
			self->queue_draw = (LV2_Inline_Display*)features[i]->data;
		}
	}
#endif

	Dyncomp_init (&self->dyncomp, rate, n_channels);
	self->sampletme = ceilf (rate * 0.05); // 50ms
	self->samplecnt = self->sampletme;

	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance,
              uint32_t   port,
              void*      data)
{
	Darc* self = (Darc*)instance;
	if (port < DARC_LAST) {
		self->_port[port] = (float*)data;
	}
}

static void
activate (LV2_Handle instance)
{
	Darc* self = (Darc*)instance;
	Dyncomp_reset (&self->dyncomp);
	self->samplecnt = self->sampletme;
}

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	Darc* self = (Darc*)instance;

	/* bypass/enable */
	const bool enable = *self->_port[DARC_ENABLE] > 0;

	if (enable) {
		Dyncomp_set_inputgain (&self->dyncomp, *self->_port[DARC_INPUTGAIN]);
		Dyncomp_set_threshold (&self->dyncomp, *self->_port[DARC_THRESHOLD]);
		Dyncomp_set_ratio (&self->dyncomp, *self->_port[DARC_RATIO]);
		Dyncomp_set_hold (&self->dyncomp, *self->_port[DARC_HOLD] > 0);
	} else {
		Dyncomp_set_inputgain (&self->dyncomp, 0);
		Dyncomp_set_threshold (&self->dyncomp, -10.f);
		Dyncomp_set_ratio (&self->dyncomp, 0);
		Dyncomp_set_hold (&self->dyncomp, false);
	}

	Dyncomp_set_attack (&self->dyncomp, *self->_port[DARC_ATTACK]);
	Dyncomp_set_release (&self->dyncomp, *self->_port[DARC_RELEASE]);

	float* ins[2]  = { self->_port[DARC_INPUT0], self->_port[DARC_INPUT1] };
	float* outs[2] = { self->_port[DARC_OUTPUT0], self->_port[DARC_OUTPUT1] };

	for (uint32_t i = 0; i < self->dyncomp.n_channels; ++i) {
		if (ins[i] != outs[i]) {
			memcpy (outs[i], ins[i], sizeof (float) * n_samples);
		}
	}

	Dyncomp_process (&self->dyncomp, n_samples, outs);

	self->samplecnt += n_samples;
	while (self->samplecnt >= self->sampletme) {
		self->samplecnt -= self->sampletme;
		Dyncomp_get_gain (&self->dyncomp, &self->_gmin, &self->_gmax, &self->_rms);

		self->_gmin = fminf (40.f, fmaxf (-20.f, self->_gmin));
		self->_gmax = fminf (40.f, fmaxf (-20.f, self->_gmax));
		self->_rms  = fminf (10.f, fmaxf (-80.f, self->_rms));

#ifdef DISPLAY_INTERFACE
		if (self->queue_draw && (self->ui_gmin != self->_gmin || self->ui_gmax != self->_gmax)) {
			self->ui_gmin = self->_gmin;
			self->ui_gmax = self->_gmax;
			self->queue_draw->queue_draw (self->queue_draw->handle);
		}
#endif
	}

	*self->_port[DARC_GMIN] = self->_gmin;
	*self->_port[DARC_GMAX] = self->_gmax;
	*self->_port[DARC_RMS]  = self->_rms;
}

static void
cleanup (LV2_Handle instance)
{
	Darc* self = (Darc*)instance;
#ifdef DISPLAY_INTERFACE
	if (self->mpat) {
		cairo_pattern_destroy (self->mpat);
	}
	if (self->cpat) {
		cairo_pattern_destroy (self->cpat);
	}
	if (self->display) {
		cairo_surface_destroy (self->display);
	}
#endif
	free (instance);
}

/* ****************************************************************************/

#ifdef WITH_SIGNATURE
#define RTK_URI DARC_URI
#include "gpg_init.c"
#include WITH_SIGNATURE
struct license_info license_infos = {
	"x42-Compressor",
	"http://x42-plugins.com/x42/x42-compressor"
};
#include "gpg_lv2ext.c"
#endif

#ifdef DISPLAY_INTERFACE
static void
create_pattern (Darc* self, const double w)
{
	const int x0 = floor (w * 0.05);
	const int x1 = ceil (w * 0.95);
	const int wd = x1 - x0;

#define DEF(x) ((x0 + wd * ((x) + 20.) / 60.) / w)

	cairo_pattern_t* pat = cairo_pattern_create_linear (0.0, 0.0, w, 0);
	/* clang-format off */
	cairo_pattern_add_color_stop_rgba (pat, 1.0,       .0, .5, .0, 0);
	cairo_pattern_add_color_stop_rgba (pat, DEF (40),  .0, .5, .0, 0.5);
	cairo_pattern_add_color_stop_rgba (pat, DEF (5),   .0, .5, .0, 0.5);
	cairo_pattern_add_color_stop_rgba (pat, DEF (-5),  .5, .0, .0, 0.5);
	cairo_pattern_add_color_stop_rgba (pat, DEF (-20), .5, .0, .0, 0.5);
	cairo_pattern_add_color_stop_rgba (pat, 0.0,       .5, .0, .0, 0);
	/* clang-format on */
	self->mpat = pat;

	pat = cairo_pattern_create_linear (0.0, 0.0, w, 0);
	/* clang-format off */
	cairo_pattern_add_color_stop_rgba (pat, 1.0,       .1, .9, .1, 0);
	cairo_pattern_add_color_stop_rgba (pat, DEF (40),  .1, .9, .1, 1);
	cairo_pattern_add_color_stop_rgba (pat, DEF (5),   .1, .9, .1, 1);
	cairo_pattern_add_color_stop_rgba (pat, DEF (-5),  .9, .9, .1, 1);
	cairo_pattern_add_color_stop_rgba (pat, DEF (-20), .9, .9, .1, 1);
	cairo_pattern_add_color_stop_rgba (pat, 0.0,       .9, .9, .1, 0);
	/* clang-format on */
	self->cpat = pat;

#undef DEF
}

static LV2_Inline_Display_Image_Surface*
dpl_render (LV2_Handle handle, uint32_t w, uint32_t max_h)
{
#ifdef WITH_SIGNATURE
	if (!is_licensed (handle)) {
		return NULL;
	}
#endif
	uint32_t h = MAX (11, MIN (1 | (uint32_t)ceilf (w / 10.f), max_h));

	Darc* self = (Darc*)handle;

	if (!self->display || self->w != w || self->h != h) {
		if (self->display)
			cairo_surface_destroy (self->display);
		self->display = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
		self->w       = w;
		self->h       = h;
		if (self->mpat) {
			cairo_pattern_destroy (self->mpat);
			self->mpat = NULL;
		}
		if (self->cpat) {
			cairo_pattern_destroy (self->cpat);
			self->cpat = NULL;
		}
	}

	if (!self->mpat || !self->cpat) {
		create_pattern (self, w);
	}

	cairo_t* cr = cairo_create (self->display);
	cairo_rectangle (cr, 0, 0, w, h);
	cairo_set_source_rgba (cr, .2, .2, .2, 1.0);
	cairo_fill (cr);

	const int x0 = floor (w * 0.05);
	const int x1 = ceil (w * 0.95);
	const int wd = x1 - x0;

	cairo_set_line_width (cr, 1);
	cairo_set_source_rgba (cr, 0.8, 0.8, 0.8, 1.0);

#define DEF(x) (rint (x0 + wd * ((x) + 20.) / 60.) - .5)
#define GRID(x)                         \
	cairo_move_to (cr, DEF (x), 0); \
	cairo_rel_line_to (cr, 0, h);   \
	cairo_stroke (cr)

	GRID (-20);
	GRID (-10);
	GRID (0);
	GRID (10);
	GRID (20);
	GRID (30);
	GRID (40);

#undef GRID

	cairo_rectangle (cr, x0, 2, wd, h - 5);
	cairo_set_source (cr, self->mpat);
	cairo_fill (cr);

	if (1) {
		float v0 = DEF (self->ui_gmin);
		float v1 = DEF (self->ui_gmax);
		cairo_rectangle (cr, v0 - 1, 2, 2. + v1 - v0, h - 5);
		cairo_set_source (cr, self->cpat);
		cairo_fill (cr);
	} else { /* bypassed */
		cairo_rectangle (cr, 0, 0, w, h);
		cairo_set_source_rgba (cr, .2, .2, .2, 0.8);
		cairo_fill (cr);
	}

#undef DEF

	/* finish surface */
	cairo_destroy (cr);
	cairo_surface_flush (self->display);
	self->surf.width  = cairo_image_surface_get_width (self->display);
	self->surf.height = cairo_image_surface_get_height (self->display);
	self->surf.stride = cairo_image_surface_get_stride (self->display);
	self->surf.data   = cairo_image_surface_get_data (self->display);

	return &self->surf;
}
#endif

const void*
extension_data (const char* uri)
{
#ifdef DISPLAY_INTERFACE
	static const LV2_Inline_Display_Interface display = { dpl_render };
	if (!strcmp (uri, LV2_INLINEDISPLAY__interface)) {
#if (defined _WIN32 && defined RTK_STATIC_INIT)
		static int once = 0;
		if (!once) {
			once = 1;
			gobject_init_ctor ();
		}
#endif
		return &display;
	}
#endif
#ifdef WITH_SIGNATURE
	LV2_LICENSE_EXT_C
#endif
	return NULL;
}

static const LV2_Descriptor descriptor_mono = {
	DARC_URI "mono",
	instantiate,
	connect_port,
	activate,
	run,
	NULL,
	cleanup,
	extension_data
};

static const LV2_Descriptor descriptor_stereo = {
	DARC_URI "stereo",
	instantiate,
	connect_port,
	activate,
	run,
	NULL,
	cleanup,
	extension_data
};

/* clang-format off */
#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
# define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
# define LV2_SYMBOL_EXPORT __attribute__ ((visibility ("default")))
#endif
/* clang-format on */
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	switch (index) {
		case 0:
			return &descriptor_mono;
		case 1:
			return &descriptor_stereo;
		default:
			return NULL;
	}
}
