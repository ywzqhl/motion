/**********************************************************************
 *
 * ffmpeg.c
 *
 * This software is distributed under the GNU Public License version 2
 * See also the file 'COPYING'.
 *
 * The contents of this file has been derived from output_example.c 
 * and apiexample.c from the FFmpeg distribution.
 *
 **********************************************************************/

#ifdef HAVE_FFMPEG

#include "ffmpeg.h"
#include "motion.h"



#if LIBAVCODEC_BUILD > 4680
/* FFmpeg after build 4680 doesn't have support for mpeg1 videos with 
 * non-standard framerates. Previous builds contained a broken hack 
 * that padded with B frames to obtain the correct framerate.
 */
#	define FFMPEG_NO_NONSTD_MPEG1
#	ifdef __GNUC__
/* #warning is a non-standard gcc extension */
#		warning **************************************************
#		warning Your version of FFmpeg is newer than version 0.4.8
#		warning Newer versions of ffmpeg do not support MPEG1 with
#		warning non-standard framerate. MPEG1 will be disabled for
#		warning normal video output. You can still use mpeg4 and
#		warning and mpeg4ms which are both better in terms of size
#		warning and quality. MPEG1 is always used for timelapse.
#		warning Please read the Motion Guide for more information.
#		warning Note that this is not an error message!
#		warning **************************************************
#	endif /* __GNUC__ */
#endif /* LIBAVCODEC_BUILD > 4680 */

#if FFMPEG_VERSION_INT >= 0x000409
/* The API for av_write_frame changed with FFmpeg version 0.4.9pre1.
 * It now uses an AVPacket struct instead of direct parameters to the
 * function.
 */
#	define FFMPEG_AVWRITEFRAME_NEWAPI
#endif /* FFMPEG_VERSION_INT >= 0x000409 */

#if LIBAVFORMAT_BUILD >= 4629
/* In this build/header version, the codec member of struct AVStream
 * was changed to a pointer so changes to AVCodecContext shouldn't
 * break binary compatibility with AVStream.
 */
#	define AVSTREAM_CODEC_PTR(avs_ptr) (avs_ptr->codec)
#else
#	define AVSTREAM_CODEC_PTR(avs_ptr) (&avs_ptr->codec)
#endif /* LIBAVFORMAT_BUILD >= 4629 */

/* Name of custom file protocol for appending to existing files instead
 * of truncating.
 */
#define APPEND_PROTO "appfile"

/* Some forward-declarations. */
void ffmpeg_put_frame(struct ffmpeg *, AVFrame *);
void ffmpeg_cleanups(struct ffmpeg *);
AVFrame *ffmpeg_prepare_frame(struct ffmpeg *, unsigned char *, 
                              unsigned char *, unsigned char *);

/* This is the trailer used to end mpeg1 videos. */
static unsigned char mpeg1_trailer[] = {0x00, 0x00, 0x01, 0xb7};

/* Append version of the file open function used in libavformat when opening
 * an ordinary file. The original file open function truncates an existing
 * file, but this version appends to it instead.
 */
static int file_open_append(URLContext *h, const char *filename, int flags)
{
	const char *colon;
	int access_flags, fd;

	/* Skip past the protocol part of filename. */
	colon = strchr(filename, ':');
	if (colon) {
		filename = colon + 1;
	}

	if (flags & URL_RDWR) {
		access_flags = O_CREAT | O_APPEND | O_RDWR;
	} else if (flags & URL_WRONLY) {
		access_flags = O_CREAT | O_APPEND | O_WRONLY;
	} else {
		access_flags = O_RDONLY;
	}

	fd = open(filename, access_flags, 0666);
	if (fd < 0) {
		return -ENOENT;
	}
	h->priv_data = (void *)(size_t)fd;
	return 0;
}

/* URLProtocol entry for the append file protocol, which we use for mpeg1 videos
 * in order to get append behavior with url_fopen.
 *
 * Libavformat uses protocols for achieving flexibility when handling files
 * and other resources. A call to url_fopen will eventually be redirected to
 * a protocol-specific open function.
 *
 * The remaining functions (for writing, seeking etc.) are set in ffmpeg_init.
 */
URLProtocol mpeg1_file_protocol = {
	.name     = APPEND_PROTO,
	.url_open = file_open_append
};

/* We set AVOutputFormat->write_trailer to this function for mpeg1. That way,
 * the mpeg1 video gets a proper trailer when it is closed.
 */
static int mpeg1_write_trailer(AVFormatContext *s)
{
	put_buffer(&s->pb, mpeg1_trailer, 4);
	put_flush_packet(&s->pb);
	return 0; /* success */
}

/* ffmpeg_init initializes for libavformat. */
void ffmpeg_init()
{
	av_register_all();

	/* Copy the functions to use for the append file protocol from the standard
	 * file protocol.
	 */
	mpeg1_file_protocol.url_read  = file_protocol.url_read;
	mpeg1_file_protocol.url_write = file_protocol.url_write;
	mpeg1_file_protocol.url_seek  = file_protocol.url_seek;
	mpeg1_file_protocol.url_close = file_protocol.url_close;

	/* Register the append file protocol. */
	register_protocol(&mpeg1_file_protocol);
}

/* Obtains the output format used for the specified codec. For mpeg4 codecs,
 * the format is avi; for mpeg1 codec, the format is mpeg. The filename has
 * to be passed, because it gets the appropriate extension appended onto it.
 */
static AVOutputFormat *get_oformat(const char *codec, char *filename)
{
	const char *ext;
	AVOutputFormat *of = NULL;

	/* Here, we use guess_format to automatically setup the codec information.
	 * If we are using msmpeg4, manually set that codec here.
	 * We also dynamically add the file extension to the filename here. This was
	 * done to support both mpeg1 and mpeg4 codecs since they have different extensions.
	 */
	if ((strcmp(codec, TIMELAPSE_CODEC) == 0)
#ifndef FFMPEG_NO_NONSTD_MPEG1
		|| (strcmp(codec, "mpeg1") == 0)
#endif 
	) {
		ext = "mpg";
		/* We use "mpeg1video" for raw mpeg1 format. Using "mpeg" would
		 * result in a muxed output file, which isn't appropriate here.
		 */
		of = guess_format("mpeg1video", NULL, NULL);
		if (of) {
			/* But we want the trailer to be correctly written. */
			of->write_trailer = mpeg1_write_trailer;
		}
#ifdef FFMPEG_NO_NONSTD_MPEG1
	} else if (strcmp(codec, "mpeg1") == 0) {
		motion_log(LOG_ERR, 0, "*** mpeg1 support for normal videos has been disabled ***");
		return NULL;
#endif
	} else if (strcmp(codec, "mpeg4") == 0) {
		ext = "avi";
		of = guess_format("avi", NULL, NULL);
	} else if (strcmp(codec, "msmpeg4") == 0) {
		ext = "avi";
		of = guess_format("avi", NULL, NULL);
		if (of) {
			/* Manually override the codec id. */
			of->video_codec = CODEC_ID_MSMPEG4V2;
		}
	} else {
		motion_log(LOG_ERR, 0, "ffmpeg_video_codec option value %s is not supported", codec);
		return NULL;
	}

	if (!of) {
		motion_log(LOG_ERR, 0, "Could not guess format for %s", codec);
		return NULL;
	}

	/* WARNING: potential buffer overflow */
	sprintf(filename, "%s.%s", filename, ext);
	return of;
}

/* This function opens an mpeg file using the new libavformat method. Both mpeg1
 * and mpeg4 are supported. However, if the current ffmpeg version doesn't allow
 * mpeg1 with non-standard framerate, the open will fail. Timelapse is a special
 * case and is tested separately.
 */
struct ffmpeg *ffmpeg_open(char *ffmpeg_video_codec, char *filename,
                           unsigned char *y, unsigned char *u, unsigned char *v,
                           int width, int height, int rate, int bps, int vbr)
{
	AVCodecContext *c;
	AVCodec *codec;
	struct ffmpeg *ffmpeg;
	int is_mpeg1;

	/* Allocate space for our ffmpeg structure. This structure contains all the 
	 * codec and image information we need to generate movies.
	 * FIXME when motion exits we should close the movie to ensure that
	 * ffmpeg is freed.
	 */
	ffmpeg = mymalloc(sizeof(struct ffmpeg));
	memset(ffmpeg, 0, sizeof(struct ffmpeg));

	ffmpeg->vbr = vbr;
	
	/* store codec name in ffmpeg->codec, with buffer overflow check */
	snprintf(ffmpeg->codec, sizeof(ffmpeg->codec), "%s", ffmpeg_video_codec);

	/* allocation the output media context */
	ffmpeg->oc = av_mallocz(sizeof(AVFormatContext));
	if (!ffmpeg->oc) {
		motion_log(LOG_ERR, 1, "Memory error while allocating output media context");
		ffmpeg_cleanups(ffmpeg);
		return (NULL);
	}

	/* Setup output format */
	ffmpeg->oc->oformat = get_oformat(ffmpeg_video_codec, filename);
	if (!ffmpeg->oc->oformat) {
		ffmpeg_cleanups(ffmpeg);
		return NULL;
	}

	snprintf(ffmpeg->oc->filename, sizeof(ffmpeg->oc->filename), "%s", filename);

	/* Create a new video stream and initialize the codecs */
	ffmpeg->video_st = NULL;
	if (ffmpeg->oc->oformat->video_codec != CODEC_ID_NONE) {
		ffmpeg->video_st = av_new_stream(ffmpeg->oc, 0);
		if (!ffmpeg->video_st) {
			motion_log(LOG_ERR, 1, "av_new_stream - could not alloc stream");
			ffmpeg_cleanups(ffmpeg);
			return (NULL);
		}
	} else {
		/* We did not get a proper video codec. */
		motion_log(LOG_ERR, 0, "Failed to obtain a proper video codec");
		ffmpeg_cleanups(ffmpeg);
		return (NULL);
	}

	ffmpeg->c     = c = AVSTREAM_CODEC_PTR(ffmpeg->video_st);
	c->codec_id   = ffmpeg->oc->oformat->video_codec;
	c->codec_type = CODEC_TYPE_VIDEO;
	is_mpeg1      = c->codec_id == CODEC_ID_MPEG1VIDEO;

	/* Uncomment to allow non-standard framerates. */
	//c->strict_std_compliance = -1;

	/* Set default parameters */
	c->bit_rate = bps;
	c->width    = width;
	c->height   = height;
#if LIBAVCODEC_BUILD >= 4754
	/* frame rate = 1/time_base, so we set 1/rate, not rate/1 */
	c->time_base.num = 1;
	c->time_base.den = rate;
#else
	c->frame_rate      = rate;
	c->frame_rate_base = 1;
#endif /* LIBAVCODEC_BUILD >= 4754 */
	
	if (vbr)
		c->flags |= CODEC_FLAG_QSCALE;

	/* Set codec specific parameters. */
	/* set intra frame distance in frames depending on codec */
	c->gop_size = is_mpeg1 ? 10 : 12;
	
	/* some formats want stream headers to be separate */
	if(!strcmp(ffmpeg->oc->oformat->name, "mp4") || 
	   !strcmp(ffmpeg->oc->oformat->name, "mov") ||
	   !strcmp(ffmpeg->oc->oformat->name, "3gp")) {
		c->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}

	/* set the output parameters (must be done even if no parameters). */
	if (av_set_parameters(ffmpeg->oc, NULL) < 0) {
		motion_log(LOG_ERR, 0, "ffmpeg av_set_parameters error: Invalid output format parameters");
		ffmpeg_cleanups(ffmpeg);
		return (NULL);
	}

	/* Dump the format settings.  This shows how the various streams relate to each other */
	//dump_format(ffmpeg->oc, 0, filename, 1);

	/* Now that all the parameters are set, we can open the video
		codec and allocate the necessary encode buffers */
	codec = avcodec_find_encoder(c->codec_id);
	if (!codec) {
		motion_log(LOG_ERR, 1, "Codec not found");
		ffmpeg_cleanups(ffmpeg);
		return (NULL);
	}
	
	/* Set the picture format - need in ffmpeg starting round April-May 2005 */
	c->pix_fmt = PIX_FMT_YUV420P;

	/* open the codec */
	if (avcodec_open(c, codec) < 0) {
		motion_log(LOG_ERR, 1, "avcodec_open - could not open codec");
		ffmpeg_cleanups(ffmpeg);
		return (NULL);
	}

	ffmpeg->video_outbuf = NULL;
	if (!(ffmpeg->oc->oformat->flags & AVFMT_RAWPICTURE)) {
		/* allocate output buffer */
		/* XXX: API change will be done */
		ffmpeg->video_outbuf_size = 200000;
		ffmpeg->video_outbuf = mymalloc(ffmpeg->video_outbuf_size);
	}

	/* allocate the encoded raw picture */
	ffmpeg->picture = avcodec_alloc_frame();
	if (!ffmpeg->picture) {
		motion_log(LOG_ERR, 1, "avcodec_alloc_frame - could not alloc frame");
		ffmpeg_cleanups(ffmpeg);
		return (NULL);
	}

	/* set variable bitrate if requested */
	if (ffmpeg->vbr) {
		ffmpeg->picture->quality = ffmpeg->vbr;
	}

	/* set the frame data */
	ffmpeg->picture->data[0] = y;
	ffmpeg->picture->data[1] = u;
	ffmpeg->picture->data[2] = v;
	ffmpeg->picture->linesize[0] = ffmpeg->c->width;
	ffmpeg->picture->linesize[1] = ffmpeg->c->width / 2;
	ffmpeg->picture->linesize[2] = ffmpeg->c->width / 2;

	/* open the output file, if needed */
	if (!(ffmpeg->oc->oformat->flags & AVFMT_NOFILE)) {
		char file_proto[256];

		/* Use append file protocol for mpeg1, to get the append behavior from 
		 * url_fopen, but no protocol (=> default) for other codecs.
		 */
		if(is_mpeg1) {
			snprintf(file_proto, sizeof(file_proto), APPEND_PROTO ":%s", filename);
		} else {
			snprintf(file_proto, sizeof(file_proto), "%s", filename);
		}

		if (url_fopen(&ffmpeg->oc->pb, file_proto, URL_WRONLY) < 0) {
			/* path did not exist? */
			if (errno == ENOENT) {
				/* create path for file (don't use file_proto)... */
				if (create_path(filename) == -1) {
					ffmpeg_cleanups(ffmpeg);
					return (NULL);
				}

				/* and retry opening the file (use file_proto) */
				if (url_fopen(&ffmpeg->oc->pb, file_proto, URL_WRONLY) < 0) {
					motion_log(LOG_ERR, 1, "url_fopen - error opening file %s",filename);
					ffmpeg_cleanups(ffmpeg);
					return (NULL);
				}
				/* Permission denied */
			} else if (errno ==  EACCES) {
				motion_log(LOG_ERR, 1,
				           "url_fopen - error opening file %s"
				           " ... check access rights to target directory", filename);
				/* create path for file... */
				ffmpeg_cleanups(ffmpeg);
				return (NULL);
			} else {
				motion_log(LOG_ERR, 1, "Error opening file %s", filename);
				ffmpeg_cleanups(ffmpeg);
				return (NULL);
			}
		}
	}

	/* write the stream header, if any */
	av_write_header(ffmpeg->oc);
	
	return ffmpeg;
}

/*
  Clean up ffmpeg struct if something was wrong
*/
void ffmpeg_cleanups(struct ffmpeg *ffmpeg)
{
	int i;

	/* close each codec */
	if (ffmpeg->video_st) {
		avcodec_close(AVSTREAM_CODEC_PTR(ffmpeg->video_st));
		av_freep(&ffmpeg->picture);
		av_freep(&ffmpeg->video_outbuf);
	}

	/* free the streams */
	for (i = 0; i < ffmpeg->oc->nb_streams; i++) {
		av_freep(&ffmpeg->oc->streams[i]);
	}
/*
		if (!(ffmpeg->oc->oformat->flags & AVFMT_NOFILE)) {
			// close the output file 
			if (ffmpeg->oc->pb) url_fclose(&ffmpeg->oc->pb);
		}
*/
	/* free the stream */
	av_free(ffmpeg->oc);
#if LIBAVFORMAT_BUILD >= 4629
	free(ffmpeg->c);
#endif
	free(ffmpeg);
}

/* Closes a video file. */
void ffmpeg_close(struct ffmpeg *ffmpeg)
{
	int i;

	/* close each codec */
	if (ffmpeg->video_st) {
		avcodec_close(AVSTREAM_CODEC_PTR(ffmpeg->video_st));
		av_freep(&ffmpeg->picture);
		av_freep(&ffmpeg->video_outbuf);
	}

	/* write the trailer, if any */
	av_write_trailer(ffmpeg->oc);

	/* free the streams */
	for (i = 0; i < ffmpeg->oc->nb_streams; i++) {
		av_freep(&ffmpeg->oc->streams[i]);
	}

	if (!(ffmpeg->oc->oformat->flags & AVFMT_NOFILE)) {
		/* close the output file */
		url_fclose(&ffmpeg->oc->pb);
	}

	/* free the stream */
	av_free(ffmpeg->oc);
#if LIBAVFORMAT_BUILD >= 4629
	free(ffmpeg->c);
#endif
	free(ffmpeg);
}

/* Puts the image pointed to by ffmpeg->picture. */
void ffmpeg_put_image(struct ffmpeg *ffmpeg) 
{
	ffmpeg_put_frame(ffmpeg, ffmpeg->picture);
}

/* Puts an arbitrary picture defined by y, u and v. */
void ffmpeg_put_other_image(struct ffmpeg *ffmpeg, unsigned char *y,
                            unsigned char *u, unsigned char *v)
{
	AVFrame *picture;
	/* allocate the encoded raw picture */
	picture = ffmpeg_prepare_frame(ffmpeg, y, u, v);

	if (picture) {
		ffmpeg_put_frame(ffmpeg, picture);
		free(picture);
	}
}

/* Encodes and writes a video frame using the av_write_frame API. This is
 * a helper function for ffmpeg_put_image and ffmpeg_put_other_image. 
 */
void ffmpeg_put_frame(struct ffmpeg *ffmpeg, AVFrame *pic)
{
	int out_size, ret;
#ifdef FFMPEG_AVWRITEFRAME_NEWAPI
	AVPacket pkt;

	av_init_packet(&pkt); /* init static structure */
	pkt.stream_index = ffmpeg->video_st->index;
#endif /* FFMPEG_AVWRITEFRAME_NEWAPI */

	if (ffmpeg->oc->oformat->flags & AVFMT_RAWPICTURE) {
		/* raw video case. The API will change slightly in the near future for that */
#ifdef FFMPEG_AVWRITEFRAME_NEWAPI
		pkt.flags |= PKT_FLAG_KEY;
		pkt.data = (uint8_t *)pic;
		pkt.size = sizeof(AVPicture);
		ret = av_write_frame(ffmpeg->oc, &pkt);
#else
		ret = av_write_frame(ffmpeg->oc, ffmpeg->video_st->index,
			(uint8_t *)pic, sizeof(AVPicture));
#endif /* FFMPEG_AVWRITEFRAME_NEWAPI */
	} else {
		/* encode the image */
		out_size = avcodec_encode_video(AVSTREAM_CODEC_PTR(ffmpeg->video_st),
		                                ffmpeg->video_outbuf, 
		                                ffmpeg->video_outbuf_size, pic);

		/* if zero size, it means the image was buffered */
		if (out_size != 0) {
			/* write the compressed frame in the media file */
			/* XXX: in case of B frames, the pts is not yet valid */
#ifdef FFMPEG_AVWRITEFRAME_NEWAPI
			pkt.pts = AVSTREAM_CODEC_PTR(ffmpeg->video_st)->coded_frame->pts;
			if (AVSTREAM_CODEC_PTR(ffmpeg->video_st)->coded_frame->key_frame) {
				pkt.flags |= PKT_FLAG_KEY;
			}
			pkt.data = ffmpeg->video_outbuf;
			pkt.size = out_size;
			ret = av_write_frame(ffmpeg->oc, &pkt);
#else
			ret = av_write_frame(ffmpeg->oc, ffmpeg->video_st->index, 
			                     ffmpeg->video_outbuf, out_size);
#endif /* FFMPEG_AVWRITEFRAME_NEWAPI */
		} else {
			ret = 0;
		}
	}
	
	if (ret != 0) {
		motion_log(LOG_ERR, 1, "Error while writing video frame");
		return;
	}
}

/* Allocates and prepares a picture frame by setting up the U, Y and V pointers in
 * the frame according to the passed pointers.
 *
 * Returns NULL If the allocation fails.
 *
 * The returned AVFrame pointer must be freed after use.
 */
AVFrame *ffmpeg_prepare_frame(struct ffmpeg *ffmpeg, unsigned char *y,
                              unsigned char *u, unsigned char *v)
{
	AVFrame *picture;

	picture = avcodec_alloc_frame();
	if (!picture) {
		motion_log(LOG_ERR, 1, "Could not alloc frame");
		return NULL;
	}

	/* take care of variable bitrate setting */
	if (ffmpeg->vbr) {
		picture->quality = ffmpeg->vbr;
	}
	
	/* setup pointers and line widths */
	picture->data[0] = y;
	picture->data[1] = u;
	picture->data[2] = v;
	picture->linesize[0] = ffmpeg->c->width;
	picture->linesize[1] = ffmpeg->c->width / 2;
	picture->linesize[2] = ffmpeg->c->width / 2;

	return picture;
}


/** ffmpeg_deinterlace
 *      Make the image suitable for deinterlacing using ffmpeg, then deinterlace the picture.
 * 
 * Parameters
 *      img     image in YUV420P format
 *      width   image width in pixels
 *      height  image height in pixels
 *
 * Returns
 *      Function returns nothing.
 *      img     contains deinterlaced image
 */
void ffmpeg_deinterlace(unsigned char *img, int width, int height)
{
	AVFrame *picture;
	int width2 = width / 2;
	
	picture = avcodec_alloc_frame();
	if (!picture) {
		motion_log(LOG_ERR, 1, "Could not alloc frame");
		return;
	}
	
	picture->data[0] = img;
	picture->data[1] = img+width*height;
	picture->data[2] = picture->data[1]+(width*height)/4;
	picture->linesize[0] = width;
	picture->linesize[1] = width2;
	picture->linesize[2] = width2;
	
	/* We assume using 'PIX_FMT_YUV420P' always */
	avpicture_deinterlace((AVPicture *)picture, (AVPicture *)picture, PIX_FMT_YUV420P, width, height);
	
	av_free(picture);
	
	return;
}

#endif /* HAVE_FFMPEG */