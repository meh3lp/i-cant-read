#include <obs-module.h>
#include <obs-properties.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "settings-dialog.hpp"

static obs_data_t *cant_read_settings = NULL;

static void cant_read_set_defaults(obs_data_t *settings_data)
{
	obs_data_set_default_int(settings_data, "stability_frames", 7);
	obs_data_set_default_double(settings_data, "stability_threshold", 0.2);
	obs_data_set_default_int(settings_data, "hash_threshold", 15);
	obs_data_set_default_double(settings_data, "text_lap_threshold", 30.0);
	obs_data_set_default_int(settings_data, "black_threshold", 0);
	obs_data_set_default_double(settings_data, "luma_noise_floor", 30.0);
	obs_data_set_default_bool(settings_data, "use_lap_carve", true);
	obs_data_set_default_double(settings_data, "lap_carve_edge_threshold", 12.0);
	obs_data_set_default_int(settings_data, "lap_dilate_kernel", 9);
}

static char *cant_read_settings_path(void)
{
	return obs_module_config_path("settings.json");
	// resolves to ~/.config/obs-studio/plugin_config/i-cant-read-ocr-gates/settings.json
}

static void cant_read_save_settings(obs_data_t *settings_data)
{
	char *path = cant_read_settings_path();
	obs_data_save_json_safe(settings_data, path, "tmp", "bak");
	bfree(path);
}

static void cant_read_load_settings(obs_data_t *settings_data)
{
	char *path = cant_read_settings_path();
	obs_data_t *loaded = obs_data_create_from_json_file(path);
	if (loaded) {
		obs_data_apply(settings_data, loaded); // overlay loaded values onto defaults
		obs_data_release(loaded);
	}
	bfree(path);
}

static void cant_read_show_settings(void *private_data)
{
	(void)private_data;
	show_settings_dialog(cant_read_settings); // calls into C++
	cant_read_save_settings(cant_read_settings);
}

obs_data_t *cant_read_get_settings(void)
{
	return cant_read_settings;
}

void cant_read_settings_init(void)
{
	// Ensure the obs config path
	char *config_dir = obs_module_config_path("");
	os_mkdirs(config_dir);
	bfree(config_dir);

	// Init the settings
	cant_read_settings = obs_data_create();
	cant_read_set_defaults(cant_read_settings);
	cant_read_load_settings(cant_read_settings);
	obs_frontend_add_tools_menu_item("I Can't Read Settings", cant_read_show_settings, NULL);
}

void cant_read_settings_deinit(void)
{
	obs_data_release(cant_read_settings);
}
