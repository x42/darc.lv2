#define MULTIPLUGIN 1
#define X42_MULTIPLUGIN_NAME "Dynamic Compressor"
#define X42_MULTIPLUGIN_URI "http://gareus.org/oss/lv2/darc"

#include "lv2ttl/darc_mono.h"
#include "lv2ttl/darc_stereo.h"

static const RtkLv2Description _plugins[] = {
	_plugin_mono,
	_plugin_stereo,
};
