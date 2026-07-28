// Groovalizer for OBS — module entry point.

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-groovalizer", "en-US")

void register_groove_source();

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Groovalizer — GPU music visualizer source. Reacts to any OBS audio "
	       "source with MilkDrop-derived beat detection.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Groovalizer";
}

bool obs_module_load(void)
{
	register_groove_source();
	blog(LOG_INFO, "[groovalizer] loaded version %s", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[groovalizer] unloaded");
}
