#include <obs-module.h>
#include <util/platform.h>

#include <Windows.h>

#pragma warning(push)
#pragma warning(disable : 4828)
#include "C:\Program Files\Pico Technology\SDK\inc\ps5000aApi.h"
#pragma warning(pop)

#pragma comment(lib, \
		"C:\\Program Files\\Pico Technology\\SDK\\lib\\ps5000a.lib")

#define PICOSCOPE_WIDTH 256
#define PICOSCOPE_HEIGHT 240
#define PICOSCOPE_SAMPLE_COUNT 312500

struct picoscope_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
};

struct picoscope_source {
	int line_thresholds[PICOSCOPE_WIDTH];
	int16_t buffers[PS5000A_MAX_CHANNELS][PICOSCOPE_SAMPLE_COUNT];
	int16_t sample_history[PICOSCOPE_HEIGHT][6000][3];
	struct picoscope_color pixels[PICOSCOPE_HEIGHT][PICOSCOPE_WIDTH];
	obs_source_t *src;
	bool open;
	int16_t handle;
	HANDLE thread;
	volatile int thread_active;

	int64_t total_samples;
	int16_t previous_sync_sample;
	uint64_t previous_rising_index;
	int line_number;
	size_t x;
	size_t overall_x;
	double base_red;
	double base_green;
	double base_blue;
};

static const char *picoscope_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "PicoScope";
}

static void *picoscope_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct picoscope_source *context =
		bzalloc(sizeof(struct picoscope_source));
	context->src = source;
	context->open = false;
	context->thread_active = 0;
	for (int i = 0; i < 256; ++i) {
		context->line_thresholds[i] =
			(int)(5.818897637795276 * (double)i + 373.5);
	}

	return context;
}

static void picoscope_source_destroy(void *data)
{
	struct picoscope_source *context = data;
	if (context->open) {
		context->thread_active = false;
		WaitForSingleObject(context->thread, INFINITE);

		ps5000aCloseUnit(context->handle);
		bfree(data);
	}
}

static uint32_t picoscope_source_getwidth(void *data)
{
	(void)data;
	return PICOSCOPE_WIDTH;
}

static uint32_t picoscope_source_getheight(void *data)
{
	(void)data;
	return PICOSCOPE_HEIGHT;
}

static void PREF4 stream_callback(int16_t handle, int32_t noOfSamples,
				  uint32_t startIndex, int16_t overflow,
				  uint32_t triggerAt, int16_t triggered,
				  int16_t autoStop, void *pParameter)
{
	static const int16_t sync_median = 0;
	static const int start_line_number = -15;

	struct picoscope_source *context = pParameter;
	int16_t(*buffers)[PICOSCOPE_SAMPLE_COUNT] = context->buffers;
	int16_t(*sample_history)[6000][3] = context->sample_history;
	int *line_thresholds = context->line_thresholds;
	struct picoscope_color(*pixels)[PICOSCOPE_WIDTH] = context->pixels;

	for (int32_t sample_index = 0; sample_index < noOfSamples;
	     ++sample_index) {
		const uint32_t buffer_index = startIndex + sample_index;
		int16_t sample = buffers[3][buffer_index];

		if ((context->previous_sync_sample >= sync_median) &&
		    (sample < sync_median)) {
			const uint64_t falling_index =
				context->total_samples + sample_index;
			if (falling_index - context->previous_rising_index <
			    400) {
				if (context->line_number >= 239 &&
				    context->line_number < 999) {
					struct obs_source_frame2 frame;
					frame.data[0] = &pixels[0][0].r;
					frame.linesize[0] = 1024;
					frame.width = PICOSCOPE_WIDTH;
					frame.height = PICOSCOPE_HEIGHT;
					frame.timestamp =
						(context->total_samples +
						 sample_index) >>
						6;
					frame.format = VIDEO_FORMAT_RGBA;
					frame.range = VIDEO_RANGE_FULL;
					frame.flip = false;
					assert(false); //601
					obs_source_output_video2(context->src,
								 &frame);
				}

				context->line_number = start_line_number;
			} else {
				++context->line_number;
			}

			context->x = 0;
			context->overall_x = 0;
		} else if (context->overall_x < 6000) {
			int red = buffers[1][buffer_index];
			int green = buffers[2][buffer_index];
			int blue = buffers[0][buffer_index];
			if (context->line_number >= 0 &&
			    context->line_number < 240) {
				sample_history[context->line_number]
					      [context->overall_x][0] = red;
				sample_history[context->line_number]
					      [context->overall_x][1] = green;
				sample_history[context->line_number]
					      [context->overall_x][2] = blue;
			}
			++context->overall_x;

			if ((context->previous_sync_sample < sync_median) &&
			    (sample >= sync_median)) {
				static const size_t interpolate_width = 10;
				if (context->line_number >= 0 &&
				    context->line_number < 240 &&
				    context->overall_x >= interpolate_width) {
					int total_red = 0;
					int total_green = 0;
					int total_blue = 0;
					for (size_t interpolate_x =
						     context->overall_x -
						     interpolate_width;
					     interpolate_x < context->overall_x;
					     ++interpolate_x) {
						total_red += sample_history
							[context->line_number]
							[interpolate_x][0];
						total_green += sample_history
							[context->line_number]
							[interpolate_x][1];
						total_blue += sample_history
							[context->line_number]
							[interpolate_x][2];
					}

					context->base_red =
						-(double)total_red /
							(double)interpolate_width -
						200.0;
					context->base_green =
						-(double)total_green /
							(double)interpolate_width -
						200.0;
					context->base_blue =
						-(double)total_blue /
							(double)interpolate_width -
						200.0;
				}
				context->previous_rising_index =
					context->total_samples + sample_index;
			} else if (context->line_number >= 0 &&
				   context->line_number < 240) {
				if (context->x < 256) {
					if (line_thresholds[context->x] ==
					    context->overall_x) {
						int totalRed =
							(red +
							 sample_history
								 [context->line_number]
								 [context->overall_x -
								  1][0] +
							 sample_history
								 [context->line_number]
								 [context->overall_x -
								  2][0]);
						int totalGreen =
							(green +
							 sample_history
								 [context->line_number]
								 [context->overall_x -
								  1][1] +
							 sample_history
								 [context->line_number]
								 [context->overall_x -
								  2][1]);
						int totalBlue =
							(blue +
							 sample_history
								 [context->line_number]
								 [context->overall_x -
								  1][2] +
							 sample_history
								 [context->line_number]
								 [context->overall_x -
								  2][2]);
						const uint8_t red = (uint8_t)min(
							max(0,
							    (int)(((totalRed *
									    (1.0 /
									     3.0) +
								    context->base_red) /
								   23400.0 *
								   255.0) +
								  0.5)),
							255);
						const uint8_t green = (uint8_t)min(
							max(0,
							    (int)(((totalGreen *
									    (1.0 /
									     3.0) +
								    context->base_green) /
								   22300.0 *
								   255.0) +
								  0.5)),
							255);
						const uint8_t blue = (uint8_t)min(
							max(0,
							    (int)(((totalBlue *
									    (1.0 /
									     3.0) +
								    context->base_blue) /
								   21400.0 *
								   255.0) +
								  0.5)),
							255);
						pixels[context->line_number]
						      [context->x]
							      .r = red;
						pixels[context->line_number]
						      [context->x]
							      .g = green;
						pixels[context->line_number]
						      [context->x]
							      .b = blue;
						pixels[context->line_number]
						      [context->x]
							      .a = 0xff;
						++context->x;
					}
				}
			}
		}

		context->previous_sync_sample = sample;
	}

	context->total_samples += noOfSamples;
}

static DWORD CALLBACK PicoScopeThread(LPVOID ptr)
{
	struct picoscope_source *context = ptr;
	context->total_samples = 0;
	context->previous_sync_sample = 0x7FFF;
	context->previous_rising_index = 0xf0000000u;
	context->line_number = 999;
	context->x = 0;
	context->overall_x = 0;
	context->base_red = 0.0;
	context->base_green = 0.0;
	context->base_blue = 0.0;

	uint32_t sample_interval = 32;
	const int16_t handle = context->handle;
	PICO_STATUS status = ps5000aRunStreaming(handle, &sample_interval,
						 PS5000A_NS, 0, 0, 0, 1,
						 PS5000A_RATIO_MODE_NONE,
						 PICOSCOPE_SAMPLE_COUNT);
	if (status == PICO_OK) {
		while (context->thread_active) {
			ps5000aGetStreamingLatestValues(
				handle, &stream_callback, context);
		}
	}

	return 0;
}

static void picoscope_source_show(void *data)
{
	struct picoscope_source *context = data;
	if (context->open)
		return;

	int16_t handle;
	PICO_STATUS status = ps5000aOpenUnit(&handle, NULL, PS5000A_DR_8BIT);
	if (status) {
		return;
	}

	status = ps5000aSetChannel(handle, PS5000A_CHANNEL_A, 1, PS5000A_DC,
				   PS5000A_1V, -0.2f);
	if (status) {
		ps5000aCloseUnit(handle);
		return;
	}

	status = ps5000aSetChannel(handle, PS5000A_CHANNEL_B, 1, PS5000A_DC,
				   PS5000A_1V, -0.2f);
	if (status) {
		ps5000aCloseUnit(handle);
		return;
	}

	status = ps5000aSetChannel(handle, PS5000A_CHANNEL_C, 1, PS5000A_DC,
				   PS5000A_1V, -0.2f);
	if (status) {
		ps5000aCloseUnit(handle);
		return;
	}

	status = ps5000aSetChannel(handle, PS5000A_CHANNEL_D, 1, PS5000A_DC,
				   PS5000A_200MV, -0.15f);
	if (status) {
		ps5000aCloseUnit(handle);
		return;
	}

	for (int i = 0; i < 4; ++i) {
		status = ps5000aSetDataBuffer(handle, (PS5000A_CHANNEL)i,
					      context->buffers[i],
					      PICOSCOPE_SAMPLE_COUNT, 0,
					      PS5000A_RATIO_MODE_NONE);
		if (status) {
			ps5000aCloseUnit(handle);
			return;
		}
	}

	context->thread_active = 1;
	const HANDLE thread =
		CreateThread(NULL, 0, &PicoScopeThread, context, 0, NULL);

	context->open = true;
	context->handle = handle;
	context->thread = thread;
}

static void picoscope_source_hide(void *data)
{
	struct picoscope_source *context = data;
	if (context->open) {
		context->thread_active = false;
		WaitForSingleObject(context->thread, INFINITE);

		ps5000aCloseUnit(context->handle);
		context->open = false;
	}
}

struct obs_source_info picoscope_source_info = {
	.id = "picoscope_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.get_name = picoscope_source_get_name,
	.create = picoscope_source_create,
	.destroy = picoscope_source_destroy,
	.get_width = picoscope_source_getwidth,
	.get_height = picoscope_source_getheight,
	.show = picoscope_source_show,
	.hide = picoscope_source_hide,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("picoscope-source", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "picoscope source";
}

bool obs_module_load(void)
{
	obs_register_source(&picoscope_source_info);
	return true;
}
