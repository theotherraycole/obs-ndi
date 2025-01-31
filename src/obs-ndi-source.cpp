/*
obs-ndi
Copyright (C) 2016-2023 Stéphane Lepin <stephane.lepin@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#ifdef _WIN32
#include <Windows.h>
#endif

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <QString>

#include "plugin-main.h"
#include "Config.h"

#define PROP_SOURCE "ndi_source_name"
#define PROP_BANDWIDTH "ndi_bw_mode"
#define PROP_SYNC "ndi_sync"
#define PROP_FRAMESYNC "ndi_framesync"
#define PROP_HW_ACCEL "ndi_recv_hw_accel"
#define PROP_FIX_ALPHA "ndi_fix_alpha_blending"
#define PROP_YUV_RANGE "yuv_range"
#define PROP_YUV_COLORSPACE "yuv_colorspace"
#define PROP_LATENCY "latency"
#define PROP_AUDIO "ndi_audio"
#define PROP_PTZ "ndi_ptz"
#define PROP_PAN "ndi_pan"
#define PROP_TILT "ndi_tilt"
#define PROP_ZOOM "ndi_zoom"

#define PROP_BW_UNDEFINED -1
#define PROP_BW_HIGHEST 0
#define PROP_BW_LOWEST 1
#define PROP_BW_AUDIO_ONLY 2

// sync mode "Internal" got removed
#define PROP_SYNC_INTERNAL 0
#define PROP_SYNC_NDI_TIMESTAMP 1
#define PROP_SYNC_NDI_SOURCE_TIMECODE 2

#define PROP_YUV_RANGE_PARTIAL 1
#define PROP_YUV_RANGE_FULL 2

#define PROP_YUV_SPACE_BT601 1
#define PROP_YUV_SPACE_BT709 2

#define PROP_LATENCY_UNDEFINED -1
#define PROP_LATENCY_NORMAL 0
#define PROP_LATENCY_LOW 1
#define PROP_LATENCY_LOWEST 2

extern NDIlib_find_instance_t ndi_finder;

typedef struct {
	bool enabled;
	float pan;
	float tilt;
	float zoom;
} ptz_t;

typedef struct {
	QByteArray ndi_receiver_name;
	QByteArray ndi_source_name;
	int bandwidth;
	int sync_mode;
	bool framesync_enabled;
	bool hw_accel_enabled;
	video_range_type yuv_range;
	video_colorspace yuv_colorspace;
	int latency;
	bool audio_enabled;
	ptz_t ptz;
	NDIlib_tally_t tally;
} ndi_source_config_t;

typedef struct {
	obs_source_t *obs_source;
	ndi_source_config_t config;
	bool pulseFlag;
	bool running;
	pthread_t av_thread;
	float   pulse;
	int64_t frameCnt;
	char	runState;
        char    capType;
	char    locked;

	NDIlib_recv_instance_t ndi_receiver;
	NDIlib_framesync_instance_t ndi_fsync;
	NDIlib_video_frame_v2_t videoFrame2;
        os_sem_t *syncSem;
} ndi_source_t;

static obs_source_t *find_filter_by_id(obs_source_t *context, const char *id)
{
	if (!context)
		return nullptr;

	typedef struct {
		const char *query;
		obs_source_t *result;
	} search_context_t;

	search_context_t filter_search = {};
	filter_search.query = id;
	filter_search.result = nullptr;

	obs_source_enum_filters(
		context,
		[](obs_source_t *, obs_source_t *filter, void *param) {
#if defined(__linux__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
			search_context_t *filter_search =
				static_cast<search_context_t *>(param);
			const char *id = obs_source_get_id(filter);
#if defined(__linux__)
#pragma GCC diagnostic pop
#endif
			if (strcmp(id, filter_search->query) == 0) {
				obs_source_get_ref(filter);
				filter_search->result = filter;
			}
		},
		&filter_search);

	return filter_search.result;
}

static speaker_layout channel_count_to_layout(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(21, 0, 0)
		return SPEAKERS_4POINT0;
#else
		return SPEAKERS_QUAD;
#endif
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

static video_colorspace prop_to_colorspace(int index)
{
	switch (index) {
	case PROP_YUV_SPACE_BT601:
		return VIDEO_CS_601;
	default:
	case PROP_YUV_SPACE_BT709:
		return VIDEO_CS_709;
	}
}

static video_range_type prop_to_range_type(int index)
{
	switch (index) {
	case PROP_YUV_RANGE_FULL:
		return VIDEO_RANGE_FULL;
	default:
	case PROP_YUV_RANGE_PARTIAL:
		return VIDEO_RANGE_PARTIAL;
	}
}

static obs_source_frame *blank_video_frame()
{
	obs_source_frame *frame =
		obs_source_frame_create(VIDEO_FORMAT_NONE, 0, 0);
	frame->timestamp = os_gettime_ns();
	return frame;
}

const char *ndi_source_getname(void *)
{
	return obs_module_text("NDIPlugin.NDISourceName");
}

obs_properties_t *ndi_source_getproperties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *source_list = obs_properties_add_list(
		props, PROP_SOURCE,
		obs_module_text("NDIPlugin.SourceProps.SourceName"),
		OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	uint32_t nbSources = 0;
	const NDIlib_source_t *sources =
		ndiLib->find_get_current_sources(ndi_finder, &nbSources);
	for (uint32_t i = 0; i < nbSources; ++i) {
		obs_property_list_add_string(source_list, sources[i].p_ndi_name,
					     sources[i].p_ndi_name);
	}

	obs_property_t *bw_modes = obs_properties_add_list(
		props, PROP_BANDWIDTH,
		obs_module_text("NDIPlugin.SourceProps.Bandwidth"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(bw_modes,
				  obs_module_text("NDIPlugin.BWMode.Highest"),
				  PROP_BW_HIGHEST);
	obs_property_list_add_int(bw_modes,
				  obs_module_text("NDIPlugin.BWMode.Lowest"),
				  PROP_BW_LOWEST);
	obs_property_list_add_int(bw_modes,
				  obs_module_text("NDIPlugin.BWMode.AudioOnly"),
				  PROP_BW_AUDIO_ONLY);
#if defined(__linux__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
	obs_property_set_modified_callback(bw_modes, [](obs_properties_t *props,
							obs_property_t *,
							obs_data_t *settings) {
#if defined(__linux__)
#pragma GCC diagnostic pop
#endif
		bool is_audio_only =
			(obs_data_get_int(settings, PROP_BANDWIDTH) ==
			 PROP_BW_AUDIO_ONLY);

		obs_property_t *yuv_range =
			obs_properties_get(props, PROP_YUV_RANGE);
		obs_property_t *yuv_colorspace =
			obs_properties_get(props, PROP_YUV_COLORSPACE);

		obs_property_set_visible(yuv_range, !is_audio_only);
		obs_property_set_visible(yuv_colorspace, !is_audio_only);

		return true;
	});

	obs_property_t *sync_modes = obs_properties_add_list(
		props, PROP_SYNC, obs_module_text("NDIPlugin.SourceProps.Sync"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(
		sync_modes, obs_module_text("NDIPlugin.SyncMode.NDITimestamp"),
		PROP_SYNC_NDI_TIMESTAMP);
	obs_property_list_add_int(
		sync_modes,
		obs_module_text("NDIPlugin.SyncMode.NDISourceTimecode"),
		PROP_SYNC_NDI_SOURCE_TIMECODE);

	obs_properties_add_bool(props, PROP_FRAMESYNC,
				obs_module_text("NDIPlugin.NDIFrameSync"));

	obs_properties_add_bool(
		props, PROP_HW_ACCEL,
		obs_module_text("NDIPlugin.SourceProps.HWAccel"));

	obs_properties_add_bool(
		props, PROP_FIX_ALPHA,
		obs_module_text("NDIPlugin.SourceProps.AlphaBlendingFix"));

	obs_property_t *yuv_ranges = obs_properties_add_list(
		props, PROP_YUV_RANGE,
		obs_module_text("NDIPlugin.SourceProps.ColorRange"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(
		yuv_ranges,
		obs_module_text("NDIPlugin.SourceProps.ColorRange.Partial"),
		PROP_YUV_RANGE_PARTIAL);
	obs_property_list_add_int(
		yuv_ranges,
		obs_module_text("NDIPlugin.SourceProps.ColorRange.Full"),
		PROP_YUV_RANGE_FULL);

	obs_property_t *yuv_spaces = obs_properties_add_list(
		props, PROP_YUV_COLORSPACE,
		obs_module_text("NDIPlugin.SourceProps.ColorSpace"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(yuv_spaces, "BT.709", PROP_YUV_SPACE_BT709);
	obs_property_list_add_int(yuv_spaces, "BT.601", PROP_YUV_SPACE_BT601);

	obs_property_t *latency_modes = obs_properties_add_list(
		props, PROP_LATENCY,
		obs_module_text("NDIPlugin.SourceProps.Latency"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(
		latency_modes,
		obs_module_text("NDIPlugin.SourceProps.Latency.Normal"),
		PROP_LATENCY_NORMAL);
	obs_property_list_add_int(
		latency_modes,
		obs_module_text("NDIPlugin.SourceProps.Latency.Low"),
		PROP_LATENCY_LOW);
	obs_property_list_add_int(
		latency_modes,
		obs_module_text("NDIPlugin.SourceProps.Latency.Lowest"),
		PROP_LATENCY_LOWEST);

	obs_properties_add_bool(props, PROP_AUDIO,
				obs_module_text("NDIPlugin.SourceProps.Audio"));

	obs_properties_t *group_ptz = obs_properties_create();
	obs_properties_add_float_slider(
		group_ptz, PROP_PAN,
		obs_module_text("NDIPlugin.SourceProps.Pan"), -1.0, 1.0, 0.001);
	obs_properties_add_float_slider(
		group_ptz, PROP_TILT,
		obs_module_text("NDIPlugin.SourceProps.Tilt"), -1.0, 1.0,
		0.001);
	obs_properties_add_float_slider(
		group_ptz, PROP_ZOOM,
		obs_module_text("NDIPlugin.SourceProps.Zoom"), 0.0, 1.0, 0.001);
	obs_properties_add_group(props, PROP_PTZ,
				 obs_module_text("NDIPlugin.SourceProps.PTZ"),
				 OBS_GROUP_CHECKABLE, group_ptz);

	auto ndi_website = obs_module_text("NDIPlugin.NDIWebsite");
	obs_properties_add_button2(
		props, "ndi_website", ndi_website,
		[](obs_properties_t *, obs_property_t *, void *data) {
#if defined(__linux__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
			QString ndi_website = (const char *)data;
#if defined(__linux__)
#pragma GCC diagnostic pop
#endif

#if defined(_WIN32)
			ShellExecute(NULL, L"open",
				     (const wchar_t *)ndi_website.utf16(), NULL,
				     NULL, SW_SHOWNORMAL);
#elif defined(__linux__) || defined(__APPLE__)
			(void)!system(QString("open %1")
					      .arg(ndi_website)
					      .toUtf8()
					      .constData());
#endif
			return true;
		},
		(void *)ndi_website);

	return props;
}

void ndi_source_getdefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, PROP_BANDWIDTH, PROP_BW_HIGHEST);
	obs_data_set_default_int(settings, PROP_SYNC,
				 PROP_SYNC_NDI_SOURCE_TIMECODE);
	obs_data_set_default_int(settings, PROP_YUV_RANGE,
				 PROP_YUV_RANGE_PARTIAL);
	obs_data_set_default_int(settings, PROP_YUV_COLORSPACE,
				 PROP_YUV_SPACE_BT709);
	obs_data_set_default_int(settings, PROP_LATENCY, PROP_LATENCY_NORMAL);
	obs_data_set_default_bool(settings, PROP_AUDIO, true);
}

void ndi_source_thread_process_audio2(ndi_source_config_t *config,
				      NDIlib_audio_frame_v2_t *ndi_audio_frame2,
				      obs_source_t *obs_source,
				      obs_source_audio *obs_audio_frame);

void ndi_source_thread_process_audio3(ndi_source_config_t *config,
				      NDIlib_audio_frame_v3_t *ndi_audio_frame3,
				      obs_source_t *obs_source,
				      obs_source_audio *obs_audio_frame);

void ndi_source_thread_process_video2(ndi_source_config_t *config,
				      NDIlib_video_frame_v2_t *ndi_video_frame2,
				      obs_source *obs_source,
				      obs_source_frame *obs_video_frame);

void ndi_source_tick(void *data, float aSecs)
{

auto s = (ndi_source_t *)data;

if (s->pulse != aSecs)
{
   s->pulse = aSecs;
   s->frameCnt = 0;
   s->locked = 'N';
   blog(LOG_INFO,
        "[obs-ndi] ndi_source_tick: Pulse changed to %f",
        aSecs);        
}
else
{
	s->frameCnt ++;
	if (s->frameCnt > 30)
		s->locked = 'Y';
};
	
if (!s->pulseFlag)
	return;

os_sem_post(s->syncSem);	

ndiLib->framesync_capture_video(s->ndi_fsync,
			   	&s->videoFrame2,
				NDIlib_frame_format_type_progressive);
	
if (s->videoFrame2.p_data != NULL)
{
	
	obs_source_frame obs_video_frame = {};

	ndi_source_thread_process_video2
		(&s->config, &s->videoFrame2,
		 s->obs_source, &obs_video_frame);

	ndiLib->framesync_free_video(s->ndi_fsync,
  			   	     &s->videoFrame2);

}
}

void *ndi_source_thread(void *data)
{
	auto s = (ndi_source_t *)data;
	auto obs_source = s->obs_source;
	QByteArray obs_source_ndi_receiver_name_utf8 =
		QString(obs_source_get_name(obs_source)).toUtf8();
	const char *obs_source_ndi_receiver_name =
		obs_source_ndi_receiver_name_utf8.constData();
	blog(LOG_INFO, "[obs-ndi] +ndi_source_thread('%s'...)",
	     obs_source_ndi_receiver_name);

	ndi_source_config_t config_most_recent = {};
	ndi_source_config_t config_last_used = {};
	config_last_used.bandwidth = PROP_BW_UNDEFINED;
	config_last_used.latency = PROP_LATENCY_UNDEFINED;

	obs_source_audio obs_audio_frame = {};
	obs_source_frame obs_video_frame = {};

	NDIlib_recv_create_v3_t recv_desc;
	recv_desc.allow_video_fields = true;

	NDIlib_metadata_frame_t metadata_frame;
	NDIlib_audio_frame_v2_t audio_frame2;
	NDIlib_audio_frame_v3_t audio_frame3;
	int64_t timestamp_audio = 0;
	int64_t timestamp_video = 0;
	int	iAudioSamples = 1600;

	NDIlib_frame_type_e frame_received = NDIlib_frame_type_none;

	NDIlib_recv_create_v3_t *reset_recv_desc = &recv_desc;

	s->runState = 'S'; // sleeping
        s->capType = 0;
	s->pulse = 1;
	s->ndi_fsync = nullptr;
	s->ndi_receiver = nullptr;

	std::this_thread::sleep_for(std::chrono::milliseconds(5000));

	os_inhibit_t *pInhibit = os_inhibit_sleep_create("Idle is bad for NDI");
	os_inhibit_sleep_set_active(pInhibit, true);
	
	s->runState = 'I'; // init

	while (s->running) {
		//
		// Main NDI receiver loop
		//

		s->runState = 'C'; // checking

		
		// semi-atomic not *TOO* heavy snapshot
		config_most_recent = s->config;

		if (config_most_recent.ndi_receiver_name !=
		    config_last_used.ndi_receiver_name) {
			config_last_used.ndi_receiver_name =
				config_most_recent.ndi_receiver_name;

			reset_recv_desc = &recv_desc;
			obs_source_ndi_receiver_name_utf8 =
				config_most_recent.ndi_receiver_name;
			obs_source_ndi_receiver_name =
				obs_source_ndi_receiver_name_utf8.constData();
			recv_desc.p_ndi_recv_name =
				obs_source_ndi_receiver_name;
			blog(LOG_INFO,
			     "[obs-ndi] ndi_source_thread: '%s' ndi_receiver_name changed; Setting recv_desc.p_ndi_recv_name='%s'",
			     obs_source_ndi_receiver_name,
			     recv_desc.p_ndi_recv_name);
		}

		s->runState = 'n'; // checking name
		
		if (config_most_recent.ndi_source_name !=
		    config_last_used.ndi_source_name) {
			config_last_used.ndi_source_name =
				config_most_recent.ndi_source_name;

			reset_recv_desc = &recv_desc;
			recv_desc.source_to_connect_to.p_ndi_name =
				config_most_recent.ndi_source_name;
			blog(LOG_INFO,
			     "[obs-ndi] ndi_source_thread: '%s' ndi_source_name changed; Setting recv_desc.source_to_connect_to.p_ndi_name='%s'",
			     obs_source_ndi_receiver_name,
			     recv_desc.source_to_connect_to.p_ndi_name);
		}

		s->runState = 'b'; // checkin bw

		if (config_most_recent.bandwidth !=
		    config_last_used.bandwidth) {
			config_last_used.bandwidth =
				config_most_recent.bandwidth;

			reset_recv_desc = &recv_desc;
			switch (config_most_recent.bandwidth) {
			case PROP_BW_HIGHEST:
			default:
				recv_desc.bandwidth =
					NDIlib_recv_bandwidth_highest;
				break;
			case PROP_BW_LOWEST:
				recv_desc.bandwidth =
					NDIlib_recv_bandwidth_lowest;
				break;
			case PROP_BW_AUDIO_ONLY:
				recv_desc.bandwidth =
					NDIlib_recv_bandwidth_audio_only;
				obs_source_output_video(obs_source,
							blank_video_frame());
				break;
			}
			blog(LOG_INFO,
			     "[obs-ndi] ndi_source_thread: '%s' bandwidth changed; Setting recv_desc.bandwidth='%d'",
			     obs_source_ndi_receiver_name, recv_desc.bandwidth);
		}

		s->runState = 'l'; // checking latency
		
		if (config_most_recent.latency != config_last_used.latency) {
			config_last_used.latency = config_most_recent.latency;

			reset_recv_desc = &recv_desc;
			//if (config_most_recent.latency == PROP_LATENCY_NORMAL)
				recv_desc.color_format =
					NDIlib_recv_color_format_UYVY_BGRA;
			//else
				//recv_desc.color_format =
					//NDIlib_recv_color_format_fastest;
			blog(LOG_INFO,
			     "[obs-ndi] ndi_source_thread: '%s' latency changed; Setting recv_desc.color_format='%d'",
			     obs_source_ndi_receiver_name,
			     recv_desc.color_format);
		}
		
		s->runState = 'f'; // checking framesync
		
		if (config_most_recent.framesync_enabled !=
		    config_last_used.framesync_enabled) {
			config_last_used.framesync_enabled =
				config_most_recent.framesync_enabled;

			reset_recv_desc = &recv_desc;
			blog(LOG_INFO,
			     "[obs-ndi] ndi_source_thread: '%s' framesync changed to %s",
			     obs_source_ndi_receiver_name,
			     config_most_recent.framesync_enabled ? "enabled"
								  : "disabled");
		}
		if (reset_recv_desc) {

			s->runState = 'R'; // resetting
			s->frameCnt = 0;   // reset frame counter
			s->locked = 'N';

			reset_recv_desc = nullptr;
			s->pulseFlag = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

			blog(LOG_INFO,
			     "[obs-ndi] ndi_source_thread: '%s' Resetting NDI receiver...",
			     obs_source_ndi_receiver_name);

			if (s->ndi_fsync) {
				ndiLib->framesync_destroy(s->ndi_fsync);
				s->ndi_fsync = nullptr;
			}

			if (s->ndi_receiver) {
				ndiLib->recv_destroy(s->ndi_receiver);
				s->ndi_receiver = nullptr;
			}

			s->ndi_receiver = ndiLib->recv_create_v3(&recv_desc);

			if (!s->ndi_receiver) {
				blog(LOG_ERROR,
				     "[obs-ndi] ndi_source_thread: '%s' Cannot create ndi_receiver for NDI source '%s'",
				     obs_source_ndi_receiver_name,
				     recv_desc.source_to_connect_to.p_ndi_name);
				break;
			}

			s->capType = 0;
		
                        if (!config_most_recent.framesync_enabled)
				s->pulseFlag = true;
			
			if (config_most_recent.framesync_enabled) {
				timestamp_audio = 0;
				timestamp_video = 0;

				// Start with NO frames in buffer before creating framesync. Might not be necessary.
				blog(LOG_INFO,
			     	     "[obs-ndi] ndi_source_thread: '%s' draining queue for framesync",
			     	     obs_source_ndi_receiver_name);
				
				while (s->running) {

					frame_received = ndiLib->recv_capture_v3
								(s->ndi_receiver,
								 &s->videoFrame2,
								 &audio_frame3,
								 nullptr,
								 0);	
					
					if (frame_received == NDIlib_frame_type_video) {
				
						ndiLib->recv_free_video_v2(s->ndi_receiver,
									   &s->videoFrame2);
	
					}

					else if (frame_received == NDIlib_frame_type_audio) {

						ndiLib->recv_free_audio_v3(s->ndi_receiver,
									   &audio_frame3);
					}

					else if (s->locked == 'Y')

						break;

					else
						
					   std::this_thread::sleep_for(std::chrono::milliseconds(2));
					
				}			
				
			        s->capType = 'f';

				s->ndi_fsync =
					ndiLib->framesync_create(s->ndi_receiver);

				s->pulseFlag = true;

				blog(LOG_INFO,
			     	     "[obs-ndi] ndi_source_thread: '%s' framesync is ready",
			     	     obs_source_ndi_receiver_name);
			
			}
		}
		
		s->runState = 'h'; // checking hw accel
		
		if (config_most_recent.hw_accel_enabled !=
		    config_last_used.hw_accel_enabled) {
			config_last_used.hw_accel_enabled =
				config_most_recent.hw_accel_enabled;

			NDIlib_metadata_frame_t hwAccelMetadata;
			hwAccelMetadata.p_data =
				config_most_recent.hw_accel_enabled
					? (char *)"<ndi_hwaccel enabled=\"true\"/>"
					: (char *)"<ndi_hwaccel enabled=\"false\"/>";
			blog(LOG_INFO,
			     "[obs-ndi] ndi_source_thread: '%s' hw_accel_enabled changed; Sending NDI metadata '%s'",
			     obs_source_ndi_receiver_name,
			     hwAccelMetadata.p_data);
			ndiLib->recv_send_metadata(s->ndi_receiver,
						   &hwAccelMetadata);
		}

		s->runState = 'z'; // checking PTZ

		if (config_most_recent.ptz.enabled) {
			const static float tollerance = 0.001f;
			if (fabs(config_most_recent.ptz.pan -
				 config_last_used.ptz.pan) > tollerance ||
			    fabs(config_most_recent.ptz.tilt -
				 config_last_used.ptz.tilt) > tollerance ||
			    fabs(config_most_recent.ptz.zoom -
				 config_last_used.ptz.zoom) > tollerance) {
				if (ndiLib->recv_ptz_is_supported(
					    s->ndi_receiver)) {
					config_last_used.ptz =
						config_most_recent.ptz;

					blog(LOG_INFO,
					     "[obs-ndi] ndi_source_thread: '%s' ptz changed; Sending PTZ pan=%f, tilt=%f, zoom=%f",
					     obs_source_ndi_receiver_name,
					     config_most_recent.ptz.pan,
					     config_most_recent.ptz.tilt,
					     config_most_recent.ptz.zoom);
					ndiLib->recv_ptz_pan_tilt(
						s->ndi_receiver,
						config_most_recent.ptz.pan,
						config_most_recent.ptz.tilt);
					ndiLib->recv_ptz_zoom(
						s->ndi_receiver,
						config_most_recent.ptz.zoom);
				}
			}
		}

		s->runState = 't'; // checking tally
		
		if (config_most_recent.tally.on_preview !=
			    config_last_used.tally.on_preview ||
		    config_most_recent.tally.on_program !=
			    config_last_used.tally.on_program) {
			config_last_used.tally = config_most_recent.tally;

			blog(LOG_INFO,
			     "[obs-ndi] ndi_source_thread: '%s' tally changed; Sending tally on_preview=%d, on_program=%d",
			     obs_source_ndi_receiver_name,
			     config_most_recent.tally.on_preview,
			     config_most_recent.tally.on_program);
			ndiLib->recv_set_tally(s->ndi_receiver,
					       &config_most_recent.tally);
		}

		s->runState = 'c'; // capturing
	
		if (!config_most_recent.framesync_enabled) {
		
			frame_received = ndiLib->recv_capture_v3(s->ndi_receiver,
								 &s->videoFrame2,
								 &audio_frame3,
								 nullptr,
								 100);

			if (frame_received == NDIlib_frame_type_audio) {
				ndi_source_thread_process_audio3(
					&config_most_recent, &audio_frame3,
					obs_source, &obs_audio_frame);
				ndiLib->recv_free_audio_v3(s->ndi_receiver,
							   &audio_frame3);
				continue;
			}

			if (frame_received == NDIlib_frame_type_video) {
				
				ndi_source_thread_process_video2(
					&config_most_recent, &s->videoFrame2,
					obs_source, &obs_video_frame);

				ndiLib->recv_free_video_v2(s->ndi_receiver,
							   &s->videoFrame2);
	
				continue;
			}

			if (frame_received == NDIlib_frame_type_none)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));

			//else {
			//	blog(LOG_INFO, "[obs-ndi] ndi_source_thread('%s'...) did not receive a video frame",
	     		//		obs_source_ndi_receiver_name);
			//}
			s->runState = 'f'; // we are freeing

			
		} else {

			os_sem_wait(s->syncSem);

			iAudioSamples = (int)(48000 / (1.0 / s->pulse));

			// Fix up for 30 and 60 fps pulse...pulse is not accurate enough to nail the calculation
			if (iAudioSamples > 1598 && iAudioSamples < 1601)
				iAudioSamples = 1600;
			else
			if (iAudioSamples > 3197 && iAudioSamples < 3201)
				iAudioSamples = 3200;	
			
			ndiLib->framesync_capture_audio(s->ndi_fsync,
			   				&audio_frame2,48000,2,
							iAudioSamples);
					
			ndi_source_thread_process_audio2(&config_most_recent, &audio_frame2,
			 				obs_source, &obs_audio_frame);
			
			ndiLib->framesync_free_audio(s->ndi_fsync,
						     &audio_frame2);

		}
	}

	os_inhibit_sleep_destroy(pInhibit);
	
	s->pulseFlag = false;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	
	if (s->ndi_fsync) {
			
		ndiLib->framesync_destroy(s->ndi_fsync);
		s->ndi_fsync = nullptr;
					
	}
	
	if (s->ndi_receiver) {
		
		ndiLib->recv_destroy(s->ndi_receiver);
		s->ndi_receiver = nullptr;
	}

	blog(LOG_INFO, "[obs-ndi] -ndi_source_thread('%s'...)",
	     obs_source_ndi_receiver_name);

	return nullptr;
}

void ndi_source_thread_process_audio2(ndi_source_config_t *config,
				      NDIlib_audio_frame_v2_t *ndi_audio_frame2,
				      obs_source_t *obs_source,
				      obs_source_audio *obs_audio_frame)
{
	if (!config->audio_enabled) {
		return;
	}

	const int channelCount = ndi_audio_frame2->no_channels > 8
					 ? 8
					 : ndi_audio_frame2->no_channels;

	obs_audio_frame->speakers = channel_count_to_layout(channelCount);

	switch (config->sync_mode) {
	case PROP_SYNC_NDI_TIMESTAMP:
		obs_audio_frame->timestamp =
			(uint64_t)(ndi_audio_frame2->timestamp * 100);
		break;

	case PROP_SYNC_NDI_SOURCE_TIMECODE:
		obs_audio_frame->timestamp =
			(uint64_t)(ndi_audio_frame2->timecode * 100);
		break;
	}

	obs_audio_frame->samples_per_sec = ndi_audio_frame2->sample_rate;
	obs_audio_frame->format = AUDIO_FORMAT_FLOAT_PLANAR;
	obs_audio_frame->frames = ndi_audio_frame2->no_samples;
	for (int i = 0; i < channelCount; ++i) {
		obs_audio_frame->data[i] =
			(uint8_t *)ndi_audio_frame2->p_data +
			(i * ndi_audio_frame2->channel_stride_in_bytes);
	}

	obs_source_output_audio(obs_source, obs_audio_frame);
}

void ndi_source_thread_process_audio3(ndi_source_config_t *config,
				      NDIlib_audio_frame_v3_t *ndi_audio_frame3,
				      obs_source_t *obs_source,
				      obs_source_audio *obs_audio_frame)
{
	if (!config->audio_enabled) {
		return;
	}

	const int channelCount = ndi_audio_frame3->no_channels > 8
					 ? 8
					 : ndi_audio_frame3->no_channels;

	obs_audio_frame->speakers = channel_count_to_layout(channelCount);

	switch (config->sync_mode) {
	case PROP_SYNC_NDI_TIMESTAMP:
		obs_audio_frame->timestamp =
			(uint64_t)(ndi_audio_frame3->timestamp * 100);
		break;

	case PROP_SYNC_NDI_SOURCE_TIMECODE:
		obs_audio_frame->timestamp =
			(uint64_t)(ndi_audio_frame3->timecode * 100);
		break;
	}

	obs_audio_frame->samples_per_sec = ndi_audio_frame3->sample_rate;
	obs_audio_frame->format = AUDIO_FORMAT_FLOAT_PLANAR;
	obs_audio_frame->frames = ndi_audio_frame3->no_samples;
	for (int i = 0; i < channelCount; ++i) {
		obs_audio_frame->data[i] =
			(uint8_t *)ndi_audio_frame3->p_data +
			(i * ndi_audio_frame3->channel_stride_in_bytes);
	}

	obs_source_output_audio(obs_source, obs_audio_frame);
}

void ndi_source_thread_process_video2(ndi_source_config_t *config,
				      NDIlib_video_frame_v2_t *ndi_video_frame,
				      obs_source *obs_source,
				      obs_source_frame *obs_video_frame)
{
	switch (ndi_video_frame->FourCC) {
	case NDIlib_FourCC_type_BGRA:
		obs_video_frame->format = VIDEO_FORMAT_BGRA;
		break;

	case NDIlib_FourCC_type_BGRX:
		obs_video_frame->format = VIDEO_FORMAT_BGRX;
		break;

	case NDIlib_FourCC_type_RGBA:
	case NDIlib_FourCC_type_RGBX:
		obs_video_frame->format = VIDEO_FORMAT_RGBA;
		break;

	case NDIlib_FourCC_type_UYVY:
	case NDIlib_FourCC_type_UYVA:
		obs_video_frame->format = VIDEO_FORMAT_UYVY;
		break;

	case NDIlib_FourCC_type_I420:
		obs_video_frame->format = VIDEO_FORMAT_I420;
		break;

	case NDIlib_FourCC_type_NV12:
		obs_video_frame->format = VIDEO_FORMAT_NV12;
		break;

	default:
		blog(LOG_INFO,
		     "[obs-ndi] warning: unsupported video pixel format: %d",
		     ndi_video_frame->FourCC);
		break;
	}

	switch (config->sync_mode) {
	case PROP_SYNC_NDI_TIMESTAMP:
		obs_video_frame->timestamp =
			(uint64_t)(ndi_video_frame->timestamp * 100);
		break;

	case PROP_SYNC_NDI_SOURCE_TIMECODE:
		obs_video_frame->timestamp =
			(uint64_t)(ndi_video_frame->timecode * 100);
		break;
	}

	obs_video_frame->width = ndi_video_frame->xres;
	obs_video_frame->height = ndi_video_frame->yres;
	obs_video_frame->linesize[0] = ndi_video_frame->line_stride_in_bytes;
	obs_video_frame->data[0] = ndi_video_frame->p_data;

	video_format_get_parameters(config->yuv_colorspace, config->yuv_range,
				    obs_video_frame->color_matrix,
				    obs_video_frame->color_range_min,
				    obs_video_frame->color_range_max);

	obs_source_output_video(obs_source, obs_video_frame);
}

void ndi_source_thread_start(ndi_source_t *s)
{
	s->pulseFlag = false;
	s->frameCnt = 0;
	s->running = true;
	
	pthread_attr_t threadAttr;
	struct sched_param threadSched;
	os_sem_init(&s->syncSem, 0);

	pthread_attr_init(&threadAttr);
	threadSched.sched_priority = sched_get_priority_max(SCHED_OTHER);
	pthread_attr_setschedpolicy(&threadAttr, SCHED_OTHER);
	pthread_attr_setschedparam(&threadAttr, &threadSched);
	pthread_attr_setinheritsched(&threadAttr, PTHREAD_EXPLICIT_SCHED);
	pthread_create(&s->av_thread, &threadAttr, ndi_source_thread, s);
	pthread_attr_destroy(&threadAttr);
	blog(LOG_INFO,
	     "[obs-ndi] ndi_source_thread_start: '%s' Started A/V ndi_source_thread for NDI source '%s'",
	     s->config.ndi_receiver_name.constData(),
	     s->config.ndi_source_name.constData());
}

void ndi_source_thread_stop(ndi_source_t *s)
{
	if (s->running) {
		s->running = false;
		os_sem_post(s->syncSem);
		pthread_join(s->av_thread, NULL);
		os_sem_destroy(s->syncSem);
	}
}

void ndi_source_update(void *data, obs_data_t *settings)
{
	auto s = (ndi_source_t *)data;
	auto obs_source = s->obs_source;
	auto name = obs_source_get_name(obs_source);
	blog(LOG_INFO, "[obs-ndi] +ndi_source_update('%s'...)", name);

	ndi_source_config_t config = s->config;
		
	config.ndi_source_name = obs_data_get_string(settings, PROP_SOURCE);
	config.bandwidth = (int)obs_data_get_int(settings, PROP_BANDWIDTH);

	config.sync_mode = (int)obs_data_get_int(settings, PROP_SYNC);
	// if sync mode is set to the unsupported "Internal" mode, set it
	// to "Source Timing" mode and apply that change to the settings data
	if (config.sync_mode == PROP_SYNC_INTERNAL) {
		config.sync_mode = PROP_SYNC_NDI_SOURCE_TIMECODE;
		obs_data_set_int(settings, PROP_SYNC,
				 PROP_SYNC_NDI_SOURCE_TIMECODE);
	}

	config.framesync_enabled = obs_data_get_bool(settings, PROP_FRAMESYNC);
	config.hw_accel_enabled = obs_data_get_bool(settings, PROP_HW_ACCEL);

	bool alpha_filter_enabled = obs_data_get_bool(settings, PROP_FIX_ALPHA);
	// Prevent duplicate filters by not persisting this value in settings
	obs_data_set_bool(settings, PROP_FIX_ALPHA, false);
	if (alpha_filter_enabled) {
		obs_source_t *existing_filter =
			find_filter_by_id(obs_source, OBS_NDI_ALPHA_FILTER_ID);
		if (!existing_filter) {
			obs_source_t *new_filter = obs_source_create(
				OBS_NDI_ALPHA_FILTER_ID,
				obs_module_text(
					"NDIPlugin.PremultipliedAlphaFilterName"),
				nullptr, nullptr);
			obs_source_filter_add(obs_source, new_filter);
			obs_source_release(new_filter);
		}
	}

	config.yuv_range = prop_to_range_type(
		(int)obs_data_get_int(settings, PROP_YUV_RANGE));
	config.yuv_colorspace = prop_to_colorspace(
		(int)obs_data_get_int(settings, PROP_YUV_COLORSPACE));

	config.latency = (int)obs_data_get_int(settings, PROP_LATENCY);
	// Disable OBS buffering only for "Lowest" latency mode
	const bool is_unbuffered = (config.latency == PROP_LATENCY_LOWEST);
	obs_source_set_async_unbuffered(obs_source, is_unbuffered);

	config.audio_enabled = obs_data_get_bool(settings, PROP_AUDIO);
	obs_source_set_audio_active(obs_source, config.audio_enabled);

	bool ptz_enabled = obs_data_get_bool(settings, PROP_PTZ);
	float pan = (float)obs_data_get_double(settings, PROP_PAN);
	float tilt = (float)obs_data_get_double(settings, PROP_TILT);
	float zoom = (float)obs_data_get_double(settings, PROP_ZOOM);
	config.ptz = {ptz_enabled, pan, tilt, zoom};

	// Update tally status
	Config *conf = Config::Current();
	config.tally.on_preview = conf->TallyPreviewEnabled &&
				  obs_source_showing(obs_source);
	config.tally.on_program = conf->TallyProgramEnabled &&
				  obs_source_active(obs_source);

	s->config = config;

	if (!config.ndi_source_name.isEmpty()) {
		if (!s->running) {
			ndi_source_thread_start(s);
		}
	} else {
		ndi_source_thread_stop(s);
	}

	blog(LOG_INFO, "[obs-ndi] -ndi_source_update('%s'...)", name);
}

void ndi_source_shown(void *data)
{
	auto s = (ndi_source_t *)data;
	auto name = obs_source_get_name(s->obs_source);
	blog(LOG_INFO, "[obs-ndi] ndi_source_shown('%s'...)", name);
	s->config.tally.on_preview = (Config::Current())->TallyPreviewEnabled;
}

void ndi_source_hidden(void *data)
{
	auto s = (ndi_source_t *)data;
	auto name = obs_source_get_name(s->obs_source);
	blog(LOG_INFO, "[obs-ndi] ndi_source_hidden('%s'...)", name);
	s->config.tally.on_preview = false;
}

void ndi_source_activated(void *data)
{
	auto s = (ndi_source_t *)data;
	auto name = obs_source_get_name(s->obs_source);
	blog(LOG_INFO, "[obs-ndi] ndi_source_activated('%s'...)", name);
	s->config.tally.on_program = (Config::Current())->TallyProgramEnabled;
}

void ndi_source_deactivated(void *data)
{
	auto s = (ndi_source_t *)data;
	auto name = obs_source_get_name(s->obs_source);
	blog(LOG_INFO, "[obs-ndi] ndi_source_deactivated('%s'...)", name);
	s->config.tally.on_program = false;
}

void ndi_source_renamed(void *data, calldata_t *)
{
	auto s = (ndi_source_t *)data;
	auto name = obs_source_get_name(s->obs_source);
	blog(LOG_INFO, "[obs-ndi] ndi_source_renamed: name='%s'", name);
	s->config.ndi_receiver_name =
		QString("OBS-NDI '%1'").arg(name).toUtf8();
}

void *ndi_source_create(obs_data_t *settings, obs_source_t *obs_source)
{
	auto name = obs_source_get_name(obs_source);
	blog(LOG_INFO, "[obs-ndi] +ndi_source_create('%s'...)", name);

	auto s = (ndi_source_t *)bzalloc(sizeof(ndi_source_t));
	s->obs_source = obs_source;
	s->config.ndi_receiver_name =
		QString("OBS-NDI '%1'").arg(name).toUtf8();

	auto sh = obs_source_get_signal_handler(s->obs_source);
	signal_handler_connect(sh, "rename", ndi_source_renamed, s);

	ndi_source_update(s, settings);

	blog(LOG_INFO, "[obs-ndi] -ndi_source_create('%s'...)", name);

	return s;
}

void ndi_source_destroy(void *data)
{
	auto s = (ndi_source_t *)data;
	auto name = obs_source_get_name(s->obs_source);
	blog(LOG_INFO, "[obs-ndi] +ndi_source_destroy('%s'...)", name);

	signal_handler_disconnect(obs_source_get_signal_handler(s->obs_source),
				  "rename", ndi_source_renamed, s);

	ndi_source_thread_stop(s);

	bfree(s);

	blog(LOG_INFO, "[obs-ndi] -ndi_source_destroy('%s'...)", name);
}

obs_source_info create_ndi_source_info()
{
	obs_source_info ndi_source_info = {};
	ndi_source_info.id = "ndi_source";
	ndi_source_info.type = OBS_SOURCE_TYPE_INPUT;
	ndi_source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO |
				       OBS_SOURCE_AUDIO |
				       OBS_SOURCE_DO_NOT_DUPLICATE;

	ndi_source_info.get_name = ndi_source_getname;
	ndi_source_info.get_properties = ndi_source_getproperties;
	ndi_source_info.get_defaults = ndi_source_getdefaults;

	ndi_source_info.create = ndi_source_create;
	ndi_source_info.activate = ndi_source_activated;
	ndi_source_info.show = ndi_source_shown;
	ndi_source_info.update = ndi_source_update;
	ndi_source_info.hide = ndi_source_hidden;
	ndi_source_info.deactivate = ndi_source_deactivated;
	ndi_source_info.destroy = ndi_source_destroy;
        ndi_source_info.video_tick = ndi_source_tick; 


	return ndi_source_info;
}
