/* gork.r -- the resources of gork for the Mac: Retro68's, and a bigger
   partition than its 1 MB (Rez, with Retro68's RIncludes) */
#include "Processes.r"
#include "Retro68.r"

resource 'SIZE' (-1) {
	reserved,
	ignoreSuspendResumeEvents,
	reserved,
	canBackground,
	needsActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreChildDiedEvents,
	is32BitCompatible,
	notHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	4096 * 1024,
	2048 * 1024
};
