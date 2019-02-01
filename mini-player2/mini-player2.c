#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#ifdef _WIN32
#undef main
#include <windows.h>
#endif

#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

static AVFormatContext *ic = NULL;
static int st_idx = -1;
static AVStream *st = NULL;
static AVCodec *codec = NULL;
static AVCodecContext *c = NULL;
static AudioParams audio_tgt;
static SDL_AudioDeviceID audio_dev;
static int eof = 0;

static struct SwrContext *swr_ctx = NULL;
static uint8_t *audio_buf = NULL;
static unsigned int audio_buf_size;
static uint8_t *audio_buf1 = NULL;
static unsigned int audio_buf1_size;
static int audio_buf_index;
static int audio_volume = 100;

static AVPacket pkt;
static AVFrame frame;

static SDL_mutex *mutex = NULL;
static SDL_cond *cond = NULL;
static SDL_Thread *tid = NULL;

static int audio_decode_frame()
{
	int i, ch;
    int ret, data_size;
	const uint8_t **in = NULL;
	uint8_t **out = NULL;
	int out_count = 0;
	int channels = 0;
	int out_size = 0;
	int len2 = 0;
	int decode_size = 0;

	ret = av_read_frame(ic, &pkt);
	if (ret < 0)
	{
		eof = 1;
		return -1;
	}

	ret = avcodec_send_packet(c, &pkt);
    if (ret < 0)
	{
        printf("[ERROR] submitting the packet to the decoder\n");
        return -1;
    }

	while (ret >= 0)
	{
		audio_buf_size = 0;
        ret = avcodec_receive_frame(c, &frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		{
            return decode_size;
		}
        else if (ret < 0)
		{
            printf("[ERROR] during decoding\n");
            return decode_size;
        }

		if (frame.format != AV_SAMPLE_FMT_S16)
		{
			if (swr_ctx != NULL)
			{
				swr_free(&swr_ctx);
				swr_ctx = NULL;
			}

			if (!swr_ctx)
			{
				swr_ctx = swr_alloc_set_opts(NULL,
					audio_tgt.channel_layout, audio_tgt.fmt, audio_tgt.freq,
					frame.channel_layout, frame.format, frame.sample_rate,
					0, NULL);
				if (swr_init(swr_ctx) < 0)
				{
					swr_free(&swr_ctx);
					break;
				}

#ifdef DEBUG
				printf("[DEBUG] need resample -> swr_ctx: %p\n", swr_ctx);
#endif
			}
		}

		data_size = av_samples_get_buffer_size(NULL, frame.channels, frame.nb_samples, frame.format, 1);

		if (swr_ctx)
		{
			in = (const uint8_t **)frame.extended_data;
			out = &audio_buf1;
			out_count = (int64_t)frame.nb_samples * frame.sample_rate / frame.sample_rate + 256;
			channels = av_get_channel_layout_nb_channels(frame.channel_layout);
			out_size  = av_samples_get_buffer_size(NULL, channels, out_count, AV_SAMPLE_FMT_S16, 0);

			av_fast_malloc(&audio_buf1, &audio_buf1_size, out_size);
			len2 = swr_convert(swr_ctx, out, out_count, in, frame.nb_samples);

			audio_buf = audio_buf1;
			audio_buf_size = len2 * channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
		}
		else
		{
			audio_buf = frame.data[0];
			audio_buf_size = data_size;
		}

        if (audio_buf_size < 0) {
            printf("[ERROR] failed to calculate data size\n");
            return 0;
        }

		decode_size += audio_buf_size;
    }

	return decode_size;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
	int audio_size, len1;

#ifdef DEBUG
	printf("[DEBUG] opaque: %p, len: %d\n", opaque, len);
#endif

	while (len > 0)
	{
#ifdef DEBUG
		printf("len: %d, audio_buf_index: %d, audio_buf_size: %d\n", len, audio_buf_index, audio_buf_size);
#endif

		if (audio_buf_index >= audio_buf_size)
		{
			audio_size = audio_decode_frame();
			if (audio_size <= 0)
			{
				audio_buf = NULL;
				audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / audio_tgt.frame_size * audio_tgt.frame_size;
			}
			else
				audio_buf_size = audio_size;

			audio_buf_index = 0;
		}

		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len)
			len1 = len;

		if (audio_buf && audio_volume == SDL_MIX_MAXVOLUME)
			memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
		else
		{
			memset(stream, 0, len1);
			if (audio_buf)
				SDL_MixAudioFormat(stream, (uint8_t *)audio_buf + audio_buf_index, AUDIO_S16SYS, len1, audio_volume);
		}

		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}

static int init_context(const char *filename)
{
	int err, i;
	err = avformat_open_input(&ic, filename, NULL, NULL);
	if (err < 0)
	{
		printf("[ERROR] avformat_open_input(): can not open input file '%s'.\n", filename);
		return err;
	}

	err = avformat_find_stream_info(ic, NULL);
	if (err < 0)
	{
		printf("[ERROR] avformat_find_stream_info().\n");
		return err;
	}

	av_dump_format(ic, 0, filename, 0);
	for (i = 0; i < ic->nb_streams; i++)
	{
		st = ic->streams[i];
		if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			st_idx = i;
			break;
		}
	}

	if (st == NULL)
	{
		printf("[ERROR] can not find audio stream in input file '%s'.\n", filename);
		return err;
	}

	err = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (err < 0)
	{
		printf("[ERROR] av_find_best_stream() failed.\n");
		return err;
	}

	codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec)
	{
        printf("[ERROR] avcodec_find_decoder() failed, codec[%d] can not be found.\n", st->codecpar->codec_id);
        return -1;
    }

	c = avcodec_alloc_context3(codec);
	if (!c)
	{
        printf("[ERROR] avcodec_alloc_context3() failed, could not allocate audio codec context.\n");
        return -1;
    }

	err = avcodec_parameters_to_context(c, st->codecpar);
	if (err < 0)
	{
		printf("[ERROR] avcodec_parameters_to_context() failed.\n");
		return -1;
	}

	c->codec_id = codec->id;
	c->pkt_timebase = st->time_base;

	err = avcodec_open2(c, codec, NULL);
    if (err < 0)
	{
        printf("[ERROR] avcodec_open2() failed, could not open codec\n");
        return -1;
    }

	st->discard = AVDISCARD_DEFAULT;
	return 0;
}

static void show_audio_spec(const char *name, SDL_AudioSpec *spec)
{
	printf("%s.freq: %d\n", name, spec->freq);
	printf("%s.format: %d\n", name, spec->format);
	printf("%s.channels: %d\n", name, spec->channels);
	printf("%s.silence: %d\n", name, spec->silence);
	printf("%s.samples: %d\n", name, spec->samples);
	printf("%s.padding: %d\n", name, spec->padding);
	printf("%s.size: %d\n", name, spec->size);
	printf("%s.callback: 0x%p\n", name, spec->callback);
	printf("%s.userdata: 0x%p\n", name, spec->userdata);
}

static int audio_thread(void *arg)
{
	int err;
	char buf[256];
	int size = 256;
	int sample_rate, channels;
	int64_t duration;
	int64_t channel_layout;
	int seconds;
	enum AVSampleFormat sample_fmt;
	SDL_AudioSpec wanted_spec, spec;
	AVFormatContext *ic = arg;

	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("[ERROR] Could not initialize SDL - %s\n", SDL_GetError());
        printf("[ERROR] (Did you set the DISPLAY variable?)\n");
		return -1;
	}

	duration = ic->duration;
	seconds = (int)(duration / AV_TIME_BASE);
	sample_rate = st->codecpar->sample_rate;
	channels = st->codecpar->channels;
	channel_layout = st->codecpar->channel_layout;
	sample_fmt = (enum AVSampleFormat)st->codecpar->format;

	av_get_channel_layout_string(buf, size, channels, channel_layout);
	buf[size] = '\0';

#ifdef DEBUG
	printf("[DEBUG] duration: %lld, seconds: %d\n", duration, seconds);
	printf("[DEBUG] sample_format: %d(%s)\n", st->codecpar->format, av_get_sample_fmt_name(st->codecpar->format));
	printf("[DEBUG] samplerate: %d\n", st->codecpar->sample_rate);
	printf("[DEBUG] channels: %d\n", st->codecpar->channels);
	printf("[DEBUG] layout: %lld(%s)\n", st->codecpar->channel_layout, buf);
#endif

	wanted_spec.channels = av_get_channel_layout_nb_channels(channel_layout);;
    wanted_spec.freq = sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = NULL;

#ifdef DEBUG
	show_audio_spec("wanted_spec", &wanted_spec);
#endif

	audio_dev =  SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
	if (!audio_dev)
	{
		printf("[ERROR] SDL_OpenAudioDevice() failed.\n");
		return -1;
	}

#ifdef DEBUG
	show_audio_spec("wanted_spec", &wanted_spec);
	show_audio_spec("spec", &spec);
#endif

	audio_tgt.fmt = AV_SAMPLE_FMT_S16;
    audio_tgt.freq = spec.freq;
    audio_tgt.channel_layout = channel_layout;
    audio_tgt.channels =  spec.channels;
    audio_tgt.frame_size = av_samples_get_buffer_size(NULL, audio_tgt.channels, 1, audio_tgt.fmt, 1);
    audio_tgt.bytes_per_sec = av_samples_get_buffer_size(NULL, audio_tgt.channels, audio_tgt.freq, audio_tgt.fmt, 1);

#ifdef DEBUG
	printf("[DEBUG] audio_tgt.fmt: %d\n", audio_tgt.fmt);
	printf("[DEBUG] audio_tgt.freq: %d\n", audio_tgt.freq);
	printf("[DEBUG] audio_tgt.channel_layout: %lld\n", audio_tgt.channel_layout);
	printf("[DEBUG] audio_tgt.channels: %d\n", audio_tgt.channels);
	printf("[DEBUG] audio_tgt.frame_size: %d\n", audio_tgt.frame_size);
	printf("[DEBUG] audio_tgt.bytes_per_sec: %d\n", audio_tgt.bytes_per_sec);
#endif

	SDL_PauseAudioDevice(audio_dev, 0);
	while (1)
	{
		if (eof == 1)
			break;

		av_usleep(1000);
	}

	SDL_CondSignal(cond);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *filename = "D:/Music/Count On Me.mp3";
	int err, i;

#ifdef _WIN32
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif

	mutex = SDL_CreateMutex();
	cond = SDL_CreateCond();

	err = init_context(filename);
	if (err < 0)
		goto fail;

	tid = SDL_CreateThread(audio_thread, "audio_thread", ic);
	SDL_CondWait(cond, mutex);

fail:
	if (mutex != NULL)
		SDL_DestroyMutex(mutex);

	if (cond != NULL)
		SDL_DestroyCond(cond);

	if (c != NULL)
		avcodec_free_context(&c);

	if (ic != NULL)
		avformat_close_input(&ic);

	if (swr_ctx != NULL)
		swr_free(&swr_ctx);

	if (audio_buf1 != NULL)
		av_freep(&audio_buf1);

	return 0;
}