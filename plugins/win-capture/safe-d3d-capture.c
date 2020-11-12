#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <windows.h>
#include "window-helpers.h"
#include "cursor-capture.h"
#include "nt-stuff.h"
#include "StcClient.h"

#define do_log(level, format, ...)                  \
	blog(level, "[game-capture: '%s'] " format, \
	     obs_source_get_name(gc->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

/* clang-format off */

#define SETTING_MODE             "capture_mode"
#define SETTING_CAPTURE_WINDOW   "window"
#define SETTING_ACTIVE_WINDOW    "active_window"
#define SETTING_WINDOW_PRIORITY  "priority"
#define SETTING_CURSOR           "capture_cursor"
#define SETTING_TRANSPARENCY     "allow_transparency"

/* deprecated */
#define SETTING_ANY_FULLSCREEN   "capture_any_fullscreen"

#define SETTING_MODE_ANY         "any_fullscreen"
#define SETTING_MODE_WINDOW      "window"
#define SETTING_MODE_HOTKEY      "hotkey"

#define HOTKEY_START             "hotkey_start"
#define HOTKEY_STOP              "hotkey_stop"

#define TEXT_MODE                obs_module_text("Mode")
#define TEXT_SAFE_D3D_CAPTURE    obs_module_text("SafeD3DCapture")
#define TEXT_ANY_FULLSCREEN      obs_module_text("GameCapture.AnyFullscreen")
#define TEXT_ALLOW_TRANSPARENCY  obs_module_text("AllowTransparency")
#define TEXT_WINDOW              obs_module_text("WindowCapture.Window")
#define TEXT_MATCH_PRIORITY      obs_module_text("WindowCapture.Priority")
#define TEXT_MATCH_TITLE         obs_module_text("WindowCapture.Priority.Title")
#define TEXT_MATCH_CLASS         obs_module_text("WindowCapture.Priority.Class")
#define TEXT_MATCH_EXE           obs_module_text("WindowCapture.Priority.Exe")
#define TEXT_CAPTURE_CURSOR      obs_module_text("CaptureCursor")

#define TEXT_MODE_ANY            TEXT_ANY_FULLSCREEN
#define TEXT_MODE_WINDOW         obs_module_text("GameCapture.CaptureWindow")
#define TEXT_MODE_HOTKEY         obs_module_text("GameCapture.UseHotkey")

#define TEXT_HOTKEY_START        obs_module_text("GameCapture.HotkeyStart")
#define TEXT_HOTKEY_STOP         obs_module_text("GameCapture.HotkeyStop")

/* clang-format on */

#define DEFAULT_RETRY_INTERVAL 2.0f
#define ERROR_RETRY_INTERVAL 4.0f

enum capture_mode {
	CAPTURE_MODE_ANY,
	CAPTURE_MODE_WINDOW,
	CAPTURE_MODE_HOTKEY
};

struct safe_d3d_capture_config {
	char *title;
	char *class;
	char *executable;
	enum window_priority priority;
	enum capture_mode mode;
	bool cursor;
	bool allow_transparency;
};

struct safe_d3d_capture {
	obs_source_t *source;

	struct cursor_data cursor_data;
	uint32_t cx;
	uint32_t cy;
	DWORD process_id;
	DWORD thread_id;
	HWND next_window;
	HWND window;
	float retry_time;
	float fps_reset_time;
	float retry_interval;
	struct dstr title;
	struct dstr class;
	struct dstr executable;
	enum window_priority priority;
	obs_hotkey_pair_id hotkey_pair;
	StcClientD3D11 client;
	volatile long hotkey_window;
	volatile bool deactivate_hook;
	volatile bool activate_hook_now;
	bool wait_for_target_startup;
	bool showing;
	bool active;
	bool capturing;
	bool activate_hook;
	bool error_acquiring;
	bool initial_config;
	bool convert_16bit;
	bool cursor_hidden;

	struct safe_d3d_capture_config config;

	gs_texture_t *texture;
	gs_texture_t *textures[STC_TEXTURE_COUNT];
	float cursor_check_time;

	struct shtex_data *shtex_data;
};

static void stop_capture(struct safe_d3d_capture *gc)
{
	if (gc->active)
		info("capture stopped");

	gc->wait_for_target_startup = false;
	gc->active = false;
	gc->capturing = false;
}

static inline void free_config(struct safe_d3d_capture_config *config)
{
	bfree(config->title);
	bfree(config->class);
	bfree(config->executable);
	memset(config, 0, sizeof(*config));
}

static void safe_d3d_capture_destroy(void *data)
{
	struct safe_d3d_capture *gc = data;
	stop_capture(gc);

	if (gc->hotkey_pair)
		obs_hotkey_pair_unregister(gc->hotkey_pair);

	obs_enter_graphics();
	gs_unregister_loss_callbacks(gc);
	StcClientD3D11Destroy(&gc->client);
	cursor_data_free(&gc->cursor_data);
	obs_leave_graphics();

	dstr_free(&gc->title);
	dstr_free(&gc->class);
	dstr_free(&gc->executable);
	free_config(&gc->config);
	bfree(gc);
}

static inline bool using_older_non_mode_format(obs_data_t *settings)
{
	return obs_data_has_user_value(settings, SETTING_ANY_FULLSCREEN) &&
	       !obs_data_has_user_value(settings, SETTING_MODE);
}

static inline void get_config(struct safe_d3d_capture_config *cfg,
			      obs_data_t *settings, const char *window)
{
	const char *mode_str = NULL;

	build_window_strings(window, &cfg->class, &cfg->title,
			     &cfg->executable);

	if (using_older_non_mode_format(settings)) {
		bool any = obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
		mode_str = any ? SETTING_MODE_ANY : SETTING_MODE_WINDOW;
	} else {
		mode_str = obs_data_get_string(settings, SETTING_MODE);
	}

	if (mode_str && strcmp(mode_str, SETTING_MODE_WINDOW) == 0)
		cfg->mode = CAPTURE_MODE_WINDOW;
	else if (mode_str && strcmp(mode_str, SETTING_MODE_HOTKEY) == 0)
		cfg->mode = CAPTURE_MODE_HOTKEY;
	else
		cfg->mode = CAPTURE_MODE_ANY;

	cfg->priority = (enum window_priority)obs_data_get_int(
		settings, SETTING_WINDOW_PRIORITY);
	cfg->cursor = obs_data_get_bool(settings, SETTING_CURSOR);
	cfg->allow_transparency =
		obs_data_get_bool(settings, SETTING_TRANSPARENCY);
}

static inline int s_cmp(const char *str1, const char *str2)
{
	if (!str1 || !str2)
		return -1;

	return strcmp(str1, str2);
}

static inline bool capture_needs_reset(struct safe_d3d_capture_config *cfg1,
				       struct safe_d3d_capture_config *cfg2)
{
	if (cfg1->mode != cfg2->mode) {
		return true;

	} else if (cfg1->mode == CAPTURE_MODE_WINDOW &&
		   (s_cmp(cfg1->class, cfg2->class) != 0 ||
		    s_cmp(cfg1->title, cfg2->title) != 0 ||
		    s_cmp(cfg1->executable, cfg2->executable) != 0 ||
		    cfg1->priority != cfg2->priority)) {
		return true;
	}

	return false;
}

static bool hotkey_start(void *data, obs_hotkey_pair_id id,
			 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct safe_d3d_capture *gc = data;

	if (pressed && gc->config.mode == CAPTURE_MODE_HOTKEY) {
		info("Activate hotkey pressed");
		os_atomic_set_long(&gc->hotkey_window,
				   (long)(uintptr_t)GetForegroundWindow());
		os_atomic_set_bool(&gc->deactivate_hook, true);
		os_atomic_set_bool(&gc->activate_hook_now, true);
	}

	return true;
}

static bool hotkey_stop(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey,
			bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct safe_d3d_capture *gc = data;

	if (pressed && gc->config.mode == CAPTURE_MODE_HOTKEY) {
		info("Deactivate hotkey pressed");
		os_atomic_set_bool(&gc->deactivate_hook, true);
	}

	return true;
}

static void safe_d3d_capture_update(void *data, obs_data_t *settings)
{
	struct safe_d3d_capture *gc = data;
	struct safe_d3d_capture_config cfg;
	bool reset_capture = false;
	const char *window =
		obs_data_get_string(settings, SETTING_CAPTURE_WINDOW);

	get_config(&cfg, settings, window);
	reset_capture = capture_needs_reset(&cfg, &gc->config);

	gc->error_acquiring = false;

	if (cfg.mode == CAPTURE_MODE_HOTKEY &&
	    gc->config.mode != CAPTURE_MODE_HOTKEY) {
		gc->activate_hook = false;
	} else {
		gc->activate_hook = !!window && !!*window;
	}

	free_config(&gc->config);
	gc->config = cfg;
	gc->retry_interval = DEFAULT_RETRY_INTERVAL;
	gc->wait_for_target_startup = false;

	dstr_free(&gc->title);
	dstr_free(&gc->class);
	dstr_free(&gc->executable);

	if (cfg.mode == CAPTURE_MODE_WINDOW) {
		dstr_copy(&gc->title, gc->config.title);
		dstr_copy(&gc->class, gc->config.class);
		dstr_copy(&gc->executable, gc->config.executable);
		gc->priority = gc->config.priority;
	}

	if (!gc->initial_config) {
		if (reset_capture) {
			stop_capture(gc);
		}
	} else {
		gc->initial_config = false;
	}
}

static bool OnCreateFunctionD3D11(void *pUserData, size_t index,
				  ID3D11Texture2D *pTexture)
{
	obs_enter_graphics();

	struct safe_d3d_capture *const gc = pUserData;
	gc->texture = NULL;
	gc->textures[index] = gs_texture_wrap_obj(pTexture);
	const bool success = gc->textures[index] != NULL;
	if (success) {
		gc->cx = gs_texture_get_width(gc->textures[index]);
		gc->cy = gs_texture_get_height(gc->textures[index]);
	}

	obs_leave_graphics();

	return success;
}

static void OnDestroyFunctionD3D11(void *pUserData, size_t index)
{
	obs_enter_graphics();

	struct safe_d3d_capture *const gc = pUserData;
	gc->texture = NULL;
	gs_texture_destroy(gc->textures[index]);
	gc->cx = 0;
	gc->cy = 0;

	obs_leave_graphics();
}

static void safe_d3d_capture_device_loss_release(void *data)
{
	struct safe_d3d_capture *gc = data;

	stop_capture(gc);
	StcClientD3D11Destroy(&gc->client);
}

static void HandleMessage(StcMessageCategory category,
			  StcMessageSeverity severity, StcMessageId id,
			  const char *description, void *context)
{
	(void)category;
	(void)id;

	int level;
	switch (severity) {
	case STC_MESSAGE_SEVERITY_ERROR:
		level = LOG_ERROR;
		break;
	case STC_MESSAGE_SEVERITY_WARNING:
		level = LOG_WARNING;
		break;
	case STC_MESSAGE_SEVERITY_INFO:
	default:
		level = LOG_INFO;
	}

	blog(level, "%s", description);
}

static void safe_d3d_capture_device_loss_rebuild(void *device_void, void *data)
{
	ID3D11Device *device = device_void;
	struct safe_d3d_capture *gc = data;

	StcMessageCallbacks messenger;
	messenger.pUserData = NULL;
	messenger.pfnMessage = &HandleMessage;

	StcD3D11AllocationCallbacks allocator;
	allocator.pUserData = gc;
	allocator.pfnCreate = &OnCreateFunctionD3D11;
	allocator.pfnDestroy = &OnDestroyFunctionD3D11;
	StcClientD3D11Create(&gc->client, device, &allocator, &messenger);
}

static void *safe_d3d_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct safe_d3d_capture *gc = bzalloc(sizeof(*gc));

	obs_enter_graphics();

	StcMessageCallbacks messenger;
	messenger.pUserData = NULL;
	messenger.pfnMessage = &HandleMessage;

	StcD3D11AllocationCallbacks allocator;
	allocator.pUserData = gc;
	allocator.pfnCreate = &OnCreateFunctionD3D11;
	allocator.pfnDestroy = &OnDestroyFunctionD3D11;

	const StcClientStatus status = StcClientD3D11Create(
		&gc->client, gs_get_device_obj(), &allocator, &messenger);
	const bool success = status == STC_CLIENT_STATUS_SUCCESS;
	if (success) {
		struct gs_device_loss loss_callbacks;
		loss_callbacks.device_loss_release =
			&safe_d3d_capture_device_loss_release;
		loss_callbacks.device_loss_rebuild =
			&safe_d3d_capture_device_loss_rebuild;
		loss_callbacks.data = gc;
		gs_register_loss_callbacks(&loss_callbacks);
	}

	obs_leave_graphics();

	if (success) {
		gc->source = source;
		gc->initial_config = true;
		gc->retry_interval = DEFAULT_RETRY_INTERVAL;
		gc->hotkey_pair = obs_hotkey_pair_register_source(
			gc->source, HOTKEY_START, TEXT_HOTKEY_START,
			HOTKEY_STOP, TEXT_HOTKEY_STOP, hotkey_start,
			hotkey_stop, gc, gc);

		safe_d3d_capture_update(gc, settings);
	} else {
		bfree(gc);
		gc = NULL;
	}

	return gc;
}

static const char *blacklisted_exes[] = {
	"explorer",
	"steam",
	"battle.net",
	"galaxyclient",
	"skype",
	"uplay",
	"origin",
	"devenv",
	"taskmgr",
	"chrome",
	"discord",
	"firefox",
	"systemsettings",
	"applicationframehost",
	"cmd",
	"shellexperiencehost",
	"winstore.app",
	"searchui",
	"lockapp",
	"windowsinternal.composableshell.experiences.textinput.inputapp",
	NULL,
};

static bool is_blacklisted_exe(const char *exe)
{
	char cur_exe[MAX_PATH];

	if (!exe)
		return false;

	for (const char **vals = blacklisted_exes; *vals; vals++) {
		strcpy(cur_exe, *vals);
		strcat(cur_exe, ".exe");

		if (strcmpi(cur_exe, exe) == 0)
			return true;
	}

	return false;
}

static bool target_suspended(struct safe_d3d_capture *gc)
{
	return thread_is_suspended(gc->process_id, gc->thread_id);
}

static bool init_hook(struct safe_d3d_capture *gc)
{
	struct dstr exe = {0};
	bool blacklisted_process = false;

	if (gc->config.mode == CAPTURE_MODE_ANY) {
		if (get_window_exe(&exe, gc->next_window)) {
			info("attempting to hook fullscreen process: %s",
			     exe.array);
		}
	} else {
		if (get_window_exe(&exe, gc->next_window)) {
			info("attempting to hook process: %s", exe.array);
		}
	}

	blacklisted_process = is_blacklisted_exe(exe.array);
	if (blacklisted_process)
		info("cannot capture %s due to being blacklisted", exe.array);
	dstr_free(&exe);

	if (blacklisted_process) {
		return false;
	}
	if (target_suspended(gc)) {
		return false;
	}

	gc->window = gc->next_window;
	gc->next_window = NULL;
	gc->active = true;

	gc->capturing = StcClientD3D11Connect(&gc->client, STC_DEFAULT_PREFIX,
					      gc->process_id,
					      STC_BIND_FLAG_SHADER_RESOURCE,
					      STC_SRGB_CHANNEL_TYPE_TYPELESS) ==
			STC_CLIENT_STATUS_SUCCESS;
	return gc->capturing;
}

static void setup_window(struct safe_d3d_capture *gc, HWND window)
{
	if (gc->wait_for_target_startup) {
		gc->retry_interval = 3.0f;
		gc->wait_for_target_startup = false;
	} else {
		gc->next_window = window;
	}
}

static void get_fullscreen_window(struct safe_d3d_capture *gc)
{
	HWND window = GetForegroundWindow();
	MONITORINFO mi = {0};
	HMONITOR monitor;
	DWORD styles;
	RECT rect;

	gc->next_window = NULL;

	if (!window) {
		return;
	}
	if (!GetWindowRect(window, &rect)) {
		return;
	}

	/* ignore regular maximized windows */
	styles = (DWORD)GetWindowLongPtr(window, GWL_STYLE);
	if ((styles & WS_MAXIMIZE) != 0 && (styles & WS_BORDER) != 0) {
		return;
	}

	monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
	if (!monitor) {
		return;
	}

	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfo(monitor, &mi)) {
		return;
	}

	if (rect.left == mi.rcMonitor.left &&
	    rect.right == mi.rcMonitor.right &&
	    rect.bottom == mi.rcMonitor.bottom &&
	    rect.top == mi.rcMonitor.top) {
		setup_window(gc, window);
	} else {
		gc->wait_for_target_startup = true;
	}
}

static void get_selected_window(struct safe_d3d_capture *gc)
{
	HWND window;

	if (dstr_cmpi(&gc->class, "dwm") == 0) {
		wchar_t class_w[512];
		os_utf8_to_wcs(gc->class.array, 0, class_w, 512);
		window = FindWindowW(class_w, NULL);
	} else {
		window = find_window(INCLUDE_MINIMIZED, gc->priority,
				     gc->class.array, gc->title.array,
				     gc->executable.array);
	}

	if (window) {
		setup_window(gc, window);
	} else {
		gc->wait_for_target_startup = true;
	}
}

static void try_hook(struct safe_d3d_capture *gc)
{
	if (gc->config.mode == CAPTURE_MODE_ANY) {
		get_fullscreen_window(gc);
	} else {
		get_selected_window(gc);
	}

	if (gc->next_window) {
		gc->thread_id = GetWindowThreadProcessId(gc->next_window,
							 &gc->process_id);

		// Make sure we never try to hook ourselves (projector)
		if (gc->process_id == GetCurrentProcessId())
			return;

		if (!gc->thread_id && gc->process_id)
			return;
		if (!gc->process_id) {
			warn("error acquiring, failed to get window "
			     "thread/process ids: %lu",
			     GetLastError());
			gc->error_acquiring = true;
			return;
		}

		if (!init_hook(gc)) {
			stop_capture(gc);
		}
	} else {
		gc->active = false;
	}
}

enum capture_result { CAPTURE_FAIL, CAPTURE_RETRY, CAPTURE_SUCCESS };

static void check_foreground_window(struct safe_d3d_capture *gc, float seconds)
{
	// Hides the cursor if the user isn't actively in the game
	gc->cursor_check_time += seconds;
	if (gc->cursor_check_time >= 0.1f) {
		DWORD foreground_process_id;
		GetWindowThreadProcessId(GetForegroundWindow(),
					 &foreground_process_id);
		if (gc->process_id != foreground_process_id)
			gc->cursor_hidden = true;
		else
			gc->cursor_hidden = false;
		gc->cursor_check_time = 0.0f;
	}
}

static void safe_d3d_capture_tick(void *data, float seconds)
{
	struct safe_d3d_capture *gc = data;
	bool deactivate = os_atomic_set_bool(&gc->deactivate_hook, false);
	bool activate_now = os_atomic_set_bool(&gc->activate_hook_now, false);

	if (activate_now) {
		HWND hwnd = (HWND)(uintptr_t)os_atomic_load_long(
			&gc->hotkey_window);

		if (is_uwp_window(hwnd))
			hwnd = get_uwp_actual_window(hwnd);

		if (get_window_exe(&gc->executable, hwnd)) {
			get_window_title(&gc->title, hwnd);
			get_window_class(&gc->class, hwnd);

			gc->priority = WINDOW_PRIORITY_CLASS;
			gc->retry_time = 10.0f;
			gc->activate_hook = true;
		} else {
			deactivate = false;
			activate_now = false;
		}
	} else if (deactivate) {
		gc->activate_hook = false;
	}

	if (!obs_source_showing(gc->source)) {
		if (gc->showing) {
			if (gc->active)
				stop_capture(gc);
			gc->showing = false;
		}
		return;

	} else if (!gc->showing) {
		gc->retry_time = 10.0f;
	}

	if (gc->active && deactivate) {
		stop_capture(gc);
	}

	StcClientD3D11NextInfo nextInfo;
	if (StcClientD3D11Tick(&gc->client, &nextInfo) ==
	    STC_CLIENT_STATUS_SUCCESS) {
		if (nextInfo.pTexture) {
			gc->texture = gc->textures[nextInfo.index];
		}
	} else {
		stop_capture(gc);
	}

	if (!gc->capturing) {
		gc->retry_interval = ERROR_RETRY_INTERVAL;
		stop_capture(gc);
	}

	gc->retry_time += seconds;

	if (!gc->active) {
		if (!gc->error_acquiring &&
		    gc->retry_time > gc->retry_interval) {
			if (gc->config.mode == CAPTURE_MODE_ANY ||
			    gc->activate_hook) {
				try_hook(gc);
				gc->retry_time = 0.0f;
			}
		}
	} else {
		if (gc->config.cursor) {
			check_foreground_window(gc, seconds);
			obs_enter_graphics();
			cursor_capture(&gc->cursor_data);
			obs_leave_graphics();
		}

		gc->fps_reset_time += seconds;
		if (gc->fps_reset_time >= gc->retry_interval) {
			gc->fps_reset_time = 0.0f;
		}
	}

	if (!gc->showing)
		gc->showing = true;
}

static inline void safe_d3d_capture_render_cursor(struct safe_d3d_capture *gc)
{
	POINT p = {0};

	if (!gc->cx || !gc->cy)
		return;

	ClientToScreen(gc->window, &p);

	cursor_draw(&gc->cursor_data, -p.x, -p.y, gc->cx, gc->cy);
}

static void safe_d3d_capture_render(void *data, gs_effect_t *effect)
{
	struct safe_d3d_capture *gc = data;
	if (!gc->texture || !gc->active)
		return;

	effect = obs_get_base_effect(gc->config.allow_transparency
					     ? OBS_EFFECT_DEFAULT
					     : OBS_EFFECT_OPAQUE);

	while (gs_effect_loop(effect, "Draw")) {
		StcClientD3D11WaitForServerWrite(&gc->client);
		if (!gc->texture)
			return;

		obs_source_draw(gc->texture, 0, 0, 0, 0, false);

		StcClientD3D11SignalRead(&gc->client);

		if (gc->config.allow_transparency && gc->config.cursor &&
		    !gc->cursor_hidden) {
			safe_d3d_capture_render_cursor(gc);
		}
	}

	if (!gc->config.allow_transparency && gc->config.cursor &&
	    !gc->cursor_hidden) {
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

		while (gs_effect_loop(effect, "Draw")) {
			safe_d3d_capture_render_cursor(gc);
		}
	}
}

static uint32_t safe_d3d_capture_width(void *data)
{
	struct safe_d3d_capture *gc = data;
	return gc->active ? gc->cx : 0;
}

static uint32_t safe_d3d_capture_height(void *data)
{
	struct safe_d3d_capture *gc = data;
	return gc->active ? gc->cy : 0;
}

static const char *safe_d3d_capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_SAFE_D3D_CAPTURE;
}

static void safe_d3d_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_MODE, SETTING_MODE_ANY);
	obs_data_set_default_int(settings, SETTING_WINDOW_PRIORITY,
				 (int)WINDOW_PRIORITY_EXE);
	obs_data_set_default_bool(settings, SETTING_CURSOR, true);
	obs_data_set_default_bool(settings, SETTING_TRANSPARENCY, false);
}

static bool mode_callback(obs_properties_t *ppts, obs_property_t *p,
			  obs_data_t *settings)
{
	bool capture_window;

	if (using_older_non_mode_format(settings)) {
		capture_window =
			!obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
	} else {
		const char *mode = obs_data_get_string(settings, SETTING_MODE);
		capture_window = strcmp(mode, SETTING_MODE_WINDOW) == 0;
	}

	p = obs_properties_get(ppts, SETTING_CAPTURE_WINDOW);
	obs_property_set_visible(p, capture_window);

	p = obs_properties_get(ppts, SETTING_WINDOW_PRIORITY);
	obs_property_set_visible(p, capture_window);

	return true;
}

static void insert_preserved_val(obs_property_t *p, const char *val, size_t idx)
{
	char *class = NULL;
	char *title = NULL;
	char *executable = NULL;
	struct dstr desc = {0};

	build_window_strings(val, &class, &title, &executable);

	dstr_printf(&desc, "[%s]: %s", executable, title);
	obs_property_list_insert_string(p, idx, desc.array, val);
	obs_property_list_item_disable(p, idx, true);

	dstr_free(&desc);
	bfree(class);
	bfree(title);
	bfree(executable);
}

extern bool check_window_property_setting(obs_properties_t *ppts,
					  obs_property_t *p,
					  obs_data_t *settings, const char *val,
					  size_t idx);

static bool window_changed_callback(obs_properties_t *ppts, obs_property_t *p,
				    obs_data_t *settings)
{
	return check_window_property_setting(ppts, p, settings,
					     SETTING_CAPTURE_WINDOW, 1);
}

static BOOL CALLBACK EnumFirstMonitor(HMONITOR monitor, HDC hdc, LPRECT rc,
				      LPARAM data)
{
	*(HMONITOR *)data = monitor;

	UNUSED_PARAMETER(hdc);
	UNUSED_PARAMETER(rc);
	return false;
}

static bool window_not_blacklisted(const char *title, const char *class,
				   const char *exe)
{
	UNUSED_PARAMETER(title);
	UNUSED_PARAMETER(class);

	return !is_blacklisted_exe(exe);
}

static obs_properties_t *safe_d3d_capture_properties(void *data)
{
	HMONITOR monitor;
	uint32_t cx = 1920;
	uint32_t cy = 1080;

	/* scaling is free form, this is mostly just to provide some common
	 * values */
	bool success = !!EnumDisplayMonitors(NULL, NULL, EnumFirstMonitor,
					     (LPARAM)&monitor);
	if (success) {
		MONITORINFO mi = {0};
		mi.cbSize = sizeof(mi);

		if (!!GetMonitorInfo(monitor, &mi)) {
			cx = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
			cy = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
		}
	}

	/* update from deprecated settings */
	if (data) {
		struct safe_d3d_capture *gc = data;
		obs_data_t *settings = obs_source_get_settings(gc->source);
		if (using_older_non_mode_format(settings)) {
			bool any = obs_data_get_bool(settings,
						     SETTING_ANY_FULLSCREEN);
			const char *mode = any ? SETTING_MODE_ANY
					       : SETTING_MODE_WINDOW;

			obs_data_set_string(settings, SETTING_MODE, mode);
		}
		obs_data_release(settings);
	}

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(ppts, SETTING_MODE, TEXT_MODE,
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, TEXT_MODE_ANY, SETTING_MODE_ANY);
	obs_property_list_add_string(p, TEXT_MODE_WINDOW, SETTING_MODE_WINDOW);
	obs_property_list_add_string(p, TEXT_MODE_HOTKEY, SETTING_MODE_HOTKEY);

	obs_property_set_modified_callback(p, mode_callback);

	p = obs_properties_add_list(ppts, SETTING_CAPTURE_WINDOW, TEXT_WINDOW,
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "", "");
	fill_window_list(p, INCLUDE_MINIMIZED, window_not_blacklisted);

	obs_property_set_modified_callback(p, window_changed_callback);

	p = obs_properties_add_list(ppts, SETTING_WINDOW_PRIORITY,
				    TEXT_MATCH_PRIORITY, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_MATCH_TITLE, WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(p, TEXT_MATCH_CLASS, WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(p, TEXT_MATCH_EXE, WINDOW_PRIORITY_EXE);

	obs_properties_add_bool(ppts, SETTING_TRANSPARENCY,
				TEXT_ALLOW_TRANSPARENCY);

	obs_properties_add_bool(ppts, SETTING_CURSOR, TEXT_CAPTURE_CURSOR);

	UNUSED_PARAMETER(data);
	return ppts;
}

struct obs_source_info safe_d3d_capture_info = {
	.id = "safe_d3d_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = safe_d3d_capture_name,
	.create = safe_d3d_capture_create,
	.destroy = safe_d3d_capture_destroy,
	.get_width = safe_d3d_capture_width,
	.get_height = safe_d3d_capture_height,
	.get_defaults = safe_d3d_capture_defaults,
	.get_properties = safe_d3d_capture_properties,
	.update = safe_d3d_capture_update,
	.video_tick = safe_d3d_capture_tick,
	.video_render = safe_d3d_capture_render,
	.icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
};
