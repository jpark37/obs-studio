#include <obs-module.h>

#include <process.h>
#include <Windows.h>

struct color_source {
	struct vec4 color;
	struct vec4 color_srgb;

	uint32_t width;
	uint32_t height;

	obs_source_t *src;

	HANDLE handle;
	volatile bool finished;
};

static const char *color_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ColorSource");
}

static void color_source_update(void *data, obs_data_t *settings)
{
	struct color_source *context = data;
	uint32_t color = (uint32_t)obs_data_get_int(settings, "color");
	uint32_t width = (uint32_t)obs_data_get_int(settings, "width");
	uint32_t height = (uint32_t)obs_data_get_int(settings, "height");

	vec4_from_rgba(&context->color, color);
	vec4_from_rgba_srgb(&context->color_srgb, color);
	context->width = width;
	context->height = height;
}

static unsigned __stdcall color_source_async_render(void *data)
{
	struct color_source *context = data;

	uint8_t *const cool0 = malloc(1048576);
	uint8_t *const cool1 = malloc(1048576);
	uint8_t *const cool2 = malloc(1048576);
	uint8_t *const cool3 = malloc(1048576);
	for (int i = 0; i < 1048576 / 4; ++i) {
		cool0[4 * i] = cool1[4 * i] = cool2[4 * i] = cool3[4 * i] =
			0x90;
		cool0[4 * i + 1] = cool1[4 * i + 1] = cool2[4 * i + 1] =
			cool3[4 * i + 1] = 0x60;
		cool0[4 * i + 2] = cool1[4 * i + 2] = cool2[4 * i + 2] =
			cool3[4 * i + 2] = 0x90;
		cool0[4 * i + 3] = cool1[4 * i + 3] = cool2[4 * i + 3] =
			cool3[4 * i + 3] = 0x60;
	}

	while (!context->finished) {
		_ReadWriteBarrier();

		struct obs_source_frame2 frame;
		video_format_get_parameters(VIDEO_CS_SRGB, VIDEO_RANGE_PARTIAL,
					    frame.color_matrix,
					    frame.color_range_min,
					    frame.color_range_max);
		frame.data[0] = cool0;
		frame.data[1] = cool1;
		frame.data[2] = cool2;
		const uint32_t double_width =
			((context->width + 1) & (UINT32_MAX - 1)) * 2;
		frame.linesize[0] = double_width;
		frame.linesize[1] = 0;
		frame.linesize[2] = 0;
		frame.width = context->width;
		frame.height = context->height;
		frame.timestamp = 0;
		frame.format = VIDEO_FORMAT_UYVY;
		frame.range = VIDEO_RANGE_PARTIAL;
		frame.flip = false;
		obs_source_output_video2(context->src, &frame);

		Sleep(20);
	}

	return 0;
}

static void *color_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	struct color_source *context = bzalloc(sizeof(struct color_source));
	context->src = source;

	color_source_update(context, settings);
	unsigned addr;
	context->handle = (HANDLE)_beginthreadex(
		NULL, 0, &color_source_async_render, context, 0, &addr);

	return context;
}

static void color_source_destroy(void *data)
{
	struct color_source *const context = data;
	context->finished = true;
	WaitForSingleObject(context->handle, INFINITE);

	bfree(data);
}

static obs_properties_t *color_source_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_color_alpha(props, "color",
				       obs_module_text("ColorSource.Color"));

	obs_properties_add_int(props, "width",
			       obs_module_text("ColorSource.Width"), 0, 4096,
			       1);

	obs_properties_add_int(props, "height",
			       obs_module_text("ColorSource.Height"), 0, 4096,
			       1);

	return props;
}

static void color_source_render_helper(struct color_source *context,
				       struct vec4 *colorVal)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	gs_effect_set_vec4(color, colorVal);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	gs_draw_sprite(0, 0, context->width, context->height);

	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

static uint32_t color_source_getwidth(void *data)
{
	struct color_source *context = data;
	return context->width;
}

static uint32_t color_source_getheight(void *data)
{
	struct color_source *context = data;
	return context->height;
}

static void color_source_defaults_v1(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "width", 400);
	obs_data_set_default_int(settings, "height", 400);
}

static void color_source_defaults_v2(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
}

static void color_source_defaults_v3(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "color", 0xFFD1D1D1);
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
}

struct obs_source_info color_source_info_v1 = {
	.id = "color_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_CAP_OBSOLETE,
	.create = color_source_create,
	.destroy = color_source_destroy,
	.update = color_source_update,
	.get_name = color_source_get_name,
	.get_defaults = color_source_defaults_v1,
	.get_width = color_source_getwidth,
	.get_height = color_source_getheight,
	.get_properties = color_source_properties,
	.icon_type = OBS_ICON_TYPE_COLOR,
};

struct obs_source_info color_source_info_v2 = {
	.id = "color_source",
	.version = 2,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_CAP_OBSOLETE,
	.create = color_source_create,
	.destroy = color_source_destroy,
	.update = color_source_update,
	.get_name = color_source_get_name,
	.get_defaults = color_source_defaults_v2,
	.get_width = color_source_getwidth,
	.get_height = color_source_getheight,
	.get_properties = color_source_properties,
	.icon_type = OBS_ICON_TYPE_COLOR,
};

struct obs_source_info color_source_info_v3 = {
	.id = "color_source",
	.version = 3,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_SRGB,
	.create = color_source_create,
	.destroy = color_source_destroy,
	.update = color_source_update,
	.get_name = color_source_get_name,
	.get_defaults = color_source_defaults_v3,
	.get_width = color_source_getwidth,
	.get_height = color_source_getheight,
	.get_properties = color_source_properties,
	.icon_type = OBS_ICON_TYPE_COLOR,
};
