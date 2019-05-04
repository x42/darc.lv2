/* common definitions UI and DSP */

#define DARC_URI "http://gareus.org/oss/lv2/darc#"

typedef enum {
	DARC_ENABLE,
	DARC_HOLD,
	DARC_INPUTGAIN,
	DARC_THRESHOLD,
	DARC_RATIO,
	DARC_ATTACK,
	DARC_RELEASE,

	DARC_GMIN,
	DARC_GMAX,
	DARC_RMS,

	DARC_INPUT0,
	DARC_OUTPUT0,
	DARC_INPUT1,
	DARC_OUTPUT1,
	DARC_LAST
} PortIndex;
