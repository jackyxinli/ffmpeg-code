#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

#define WAVE_FORMAT_PCM 1

// https://www3.nd.edu/~dthain/courses/cse20211/fall2013/wavfile/wavfile.c
typedef struct wavfile_header {
	char riff_tag[4];
	int	riff_length;
	char wave_tag[4];
	char fmt_tag[4];
	int	fmt_length;
	short audio_format;
	short num_channels;
	int	sample_rate;
	int	byte_rate;
	short block_align;
	short bits_per_sample;
	char data_tag[4];
	int	data_length;
} wavfile_header;

static struct SwrContext *swr_ctx = NULL;
static uint8_t *audio_buf = NULL;
static unsigned int audio_buf_size;
static uint8_t *audio_buf1 = NULL;
static unsigned int audio_buf1_size;

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph, AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph)
	{
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs)
		{
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    }
	else
	{
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int decode(AVCodecContext *dec_ctx, AVPacket *pkt, AVFrame *frame, FILE *outfile)
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

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0)
	{
        printf("Error submitting the packet to the decoder\n");
        return decode_size;
    }

    while (ret >= 0)
	{
		audio_buf_size = 0;
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return decode_size;
        else if (ret < 0)
		{
            printf("Error during decoding\n");
            return decode_size;
        }

		if (frame->format != AV_SAMPLE_FMT_S16)
		{
			if (swr_ctx != NULL)
			{
				swr_free(&swr_ctx);
				swr_ctx = NULL;
			}

			if (!swr_ctx)
			{
				swr_ctx = swr_alloc_set_opts(NULL,
					frame->channel_layout, AV_SAMPLE_FMT_S16, frame->sample_rate,
					frame->channel_layout, frame->format, frame->sample_rate,
					0, NULL);
				if (swr_init(swr_ctx) < 0)
				{
					swr_free(&swr_ctx);
					break;
				}

				printf("need resample -> swr_ctx: %p\n", swr_ctx);
			}
		}

		data_size = av_samples_get_buffer_size(NULL, frame->channels, frame->nb_samples, frame->format, 1);

		if (swr_ctx)
		{
			in = (const uint8_t **)frame->extended_data;
			out = &audio_buf1;
			out_count = (int64_t)frame->nb_samples * frame->sample_rate / frame->sample_rate + 256;
			channels = av_get_channel_layout_nb_channels(frame->channel_layout);
			out_size  = av_samples_get_buffer_size(NULL, channels, out_count, AV_SAMPLE_FMT_S16, 0);

			av_fast_malloc(&audio_buf1, &audio_buf1_size, out_size);
			len2 = swr_convert(swr_ctx, out, out_count, in, frame->nb_samples);

			audio_buf = audio_buf1;
			audio_buf_size = len2 * channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
		}
		else
		{
			audio_buf = frame->data[0];
			audio_buf_size = data_size;
		}

        if (audio_buf_size < 0) {
            printf("Failed to calculate data size\n");
            return 0;
        }

		fwrite(audio_buf, sizeof(uint8_t), audio_buf_size, outfile);
		decode_size += audio_buf_size;
    }

	return decode_size;
}

int main(int argc, char **argv)
{
    const char *outfilename, *filename;
    const AVCodec *codec;
	AVFormatContext *ic = NULL;
    AVCodecContext *c = NULL;
    int len, ret, err, i, idx = -1;
    FILE *outfile;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
	int64_t duration = 0;
	int hours, mins, secs, us;

	wavfile_header header = {0};
	int32_t chunk_size = 0;
	int32_t data_len = 0;

    if (argc <= 2)
	{
        printf("Usage: %s <input file> <output file>\n", argv[0]);
        exit(0);
    }

    filename = argv[1];
    outfilename = argv[2];

	if (avformat_open_input(&ic, filename, NULL, NULL) < 0)
	{
		printf("Could not open source file %s\n", filename);
		return -1;
	}

	if (avformat_find_stream_info(ic, NULL) < 0)
	{
		printf("Could not find stream information\n");
		return -1;
	}

	for (i = 0; i < ic->nb_streams; i++)
	{
		if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			idx = i;
			break;
		}
	}

	if (idx < 0)
	{
		avformat_close_input(&ic);
		return -1;
	}

	err = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (err < 0)
	{
		printf("Could not find %s stream in input file '%s'\n", av_get_media_type_string(AVMEDIA_TYPE_AUDIO), filename);
		avformat_close_input(&ic);
		return -1;
	}

	av_dump_format(ic, 0, filename, 0);
	printf("sample_format: %d(%s)\n", ic->streams[idx]->codecpar->format, av_get_sample_fmt_name(ic->streams[idx]->codecpar->format));
	printf("sample_rate: %d\n", ic->streams[idx]->codecpar->sample_rate);
	printf("channels: %d\n", ic->streams[idx]->codecpar->channels);
	printf("channel_layout: %lld\n", ic->streams[idx]->codecpar->channel_layout);

	duration = ic->duration + (ic->duration <= INT64_MAX - 5000 ? 5000 : 0);
	secs = duration / AV_TIME_BASE;
    us = duration % AV_TIME_BASE;
    mins = secs / 60;
    secs %= 60;
    hours = mins / 60;
    mins %= 60;
	printf("[1] duration: %lld\n", ic->duration);
	printf("[2] duration: %d\n",
		(int)(ic->streams[idx]->duration * av_q2d(ic->streams[idx]->time_base)));
	printf("Duration: %02d:%02d:%02d.%02d\n", hours, mins, secs, (100 * us) / AV_TIME_BASE);

	codec = avcodec_find_decoder(ic->streams[idx]->codecpar->codec_id);
	if (!codec)
	{
        printf("Codec not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c)
	{
        printf("Could not allocate audio codec context.\n");
        exit(1);
    }

	err = avcodec_parameters_to_context(c, ic->streams[idx]->codecpar);
	if (err < 0)
	{
		printf("avcodec_parameters_to_context failed.\n");
		exit(1);
	}

	/*
	printf("sample_format: %d(%s)\n", c->sample_fmt, av_get_sample_fmt_name(c->sample_fmt));
	printf("sample_rate: %d\n", c->sample_rate);
	printf("channels: %d\n", c->channels);
	printf("channel_layout: %lld\n", c->channel_layout);
	*/

	c->codec_id = codec->id;
    if (avcodec_open2(c, codec, NULL) < 0)
	{
        printf("Could not open codec\n");
        exit(1);
    }

    outfile = fopen(outfilename, "wb");
    if (!outfile)
	{
        av_free(c);
        exit(1);
    }

	strncpy(header.riff_tag,"RIFF", 4);
	strncpy(header.wave_tag,"WAVE", 4);
	strncpy(header.fmt_tag,"fmt ", 4);
	strncpy(header.data_tag,"data", 4);

	header.riff_length = 0;
	header.fmt_length = 16;
	header.audio_format = WAVE_FORMAT_PCM;
	header.num_channels = ic->streams[idx]->codecpar->channels;
	header.sample_rate = ic->streams[idx]->codecpar->sample_rate;
	header.block_align = av_get_bytes_per_sample(ic->streams[idx]->codecpar->format);
	header.byte_rate = header.sample_rate * header.block_align;
	header.bits_per_sample = header.block_align << header.num_channels;
	header.data_length = 0;

	fwrite(&header, sizeof(wavfile_header), 1, outfile);

	pkt = av_packet_alloc();
	frame = av_frame_alloc();

    while (1)
	{
		if (av_read_frame(ic, pkt) < 0)
			break;

        if (pkt->size)
            data_len += decode(c, pkt, frame, outfile);

		av_packet_unref(pkt);
    }

	chunk_size = 36 + data_len;

	printf("chunk_size:%d\n", chunk_size);
	printf("data_len:%d\n", data_len);
	
	fseek(outfile, 4, SEEK_SET);
	fwrite(&chunk_size, 1, sizeof(int32_t), outfile);

	fseek(outfile, 40, SEEK_SET);
	fwrite(&data_len, 1, sizeof(int32_t), outfile);

    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);
	avformat_close_input(&ic);
	fclose(outfile);

	if (swr_ctx != NULL)
		swr_free(&swr_ctx);

	if (audio_buf1 != NULL)
		 av_freep(&audio_buf1);

    return 0;
}
