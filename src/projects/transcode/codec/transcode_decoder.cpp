//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "transcode_decoder.h"

#include "transcode_codec_dec_aac.h"
#include "transcode_codec_dec_avc.h"
#include "transcode_codec_dec_hevc.h"

#define OV_LOG_TAG "TranscodeCodec"

TranscodeDecoder::TranscodeDecoder(info::Stream stream_info)
	: _stream_info(stream_info)
{
	::avcodec_register_all();

	_pkt = ::av_packet_alloc();
	_frame = ::av_frame_alloc();
}

TranscodeDecoder::~TranscodeDecoder()
{
	::avcodec_free_context(&_context);
	::avcodec_parameters_free(&_codec_par);

	::av_frame_free(&_frame);
	::av_packet_free(&_pkt);

	::av_parser_close(_parser);

	_input_buffer.clear();
	_output_buffer.clear();
}

std::shared_ptr<TranscodeContext>& TranscodeDecoder::GetContext()
{
	return _input_context;
}

common::Timebase TranscodeDecoder::GetTimebase() const
{
	return _input_context->GetTimeBase();
}

std::shared_ptr<TranscodeDecoder> TranscodeDecoder::CreateDecoder(const info::Stream &info, common::MediaCodecId codec_id, std::shared_ptr<TranscodeContext> input_context)
{
	std::shared_ptr<TranscodeDecoder> decoder = nullptr;

	switch (codec_id)
	{
		case common::MediaCodecId::H264:
			decoder = std::make_shared<OvenCodecImplAvcodecDecAVC>(info);
			break;
		case common::MediaCodecId::H265:
			decoder = std::make_shared<OvenCodecImplAvcodecDecHEVC>(info);
			break;
		case common::MediaCodecId::Aac:
			decoder = std::make_shared<OvenCodecImplAvcodecDecAAC>(info);
			break;

		default:
			OV_ASSERT(false, "Not supported codec: %d", codec_id);
			break;
	}

	if (decoder != nullptr)
	{
		if (decoder->Configure(input_context) == false)
		{
			return nullptr;
		}
	}

	return std::move(decoder);
}

bool TranscodeDecoder::Configure(std::shared_ptr<TranscodeContext> context)
{
	if (context == nullptr)
	{
		return false;
	}

	_input_context = context;

	_codec = ::avcodec_find_decoder(GetCodecID());

	if (_codec == nullptr)
	{
		logte("Codec not found: %s (%d)", ::avcodec_get_name(GetCodecID()), GetCodecID());
		return false;
	}

	// create codec context
	_context = ::avcodec_alloc_context3(_codec);

	if (_context == nullptr)
	{
		logte("Could not allocate codec context for %s (%d)", ::avcodec_get_name(GetCodecID()), GetCodecID());
		return false;
	}

	if (::avcodec_open2(_context, _codec, nullptr) < 0)
	{
		logte("Could not open codec: %s (%d)", ::avcodec_get_name(GetCodecID()), GetCodecID());
		return false;
	}

	_parser = ::av_parser_init(_codec->id);

	if (_parser == nullptr)
	{
		logte("Parser not found");
		return false;
	}
	
	_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

	_context->time_base = TimebaseToAVRational(_input_context->GetTimeBase());

	return true;
}

void TranscodeDecoder::SendBuffer(std::shared_ptr<const MediaPacket> packet)
{
	_input_buffer.push_back(std::move(packet));
}

const ov::String TranscodeDecoder::ShowCodecParameters(const AVCodecContext *context, const AVCodecParameters *parameters)
{
	ov::String message;

	message.AppendFormat("[%s] ", ::av_get_media_type_string(parameters->codec_type));

	switch (parameters->codec_type)
	{
		case AVMEDIA_TYPE_UNKNOWN:
			message = "Unknown media type";
			break;

		case AVMEDIA_TYPE_VIDEO:
		case AVMEDIA_TYPE_AUDIO:
			// H.264 (Baseline
			message.AppendFormat("%s (%s", ::avcodec_get_name(parameters->codec_id), ::avcodec_profile_name(parameters->codec_id, parameters->profile));

			if (parameters->level >= 0)
			{
				// lv: 5.2
				message.AppendFormat(" %.1f", parameters->level / 10.0f);
			}
			message.Append(')');

			if (parameters->codec_tag != 0)
			{
				char tag[AV_FOURCC_MAX_STRING_SIZE]{};
				::av_fourcc_make_string(tag, parameters->codec_tag);

				// (avc1 / 0x31637661
				message.AppendFormat(", (%s / 0x%08X", tag, parameters->codec_tag);

				if (parameters->extradata_size != 0)
				{
					// extra: 1234
					message.AppendFormat(", extra: %d", parameters->extradata_size);
				}

				message.Append(')');
			}

			message.Append(", ");

			if (parameters->codec_type == AVMEDIA_TYPE_VIDEO)
			{
				int gcd = ::av_gcd(parameters->width, parameters->height);

				if (gcd == 0)
				{
					OV_ASSERT2(false);
					gcd = 1;
				}

				int digit = 0;

				if(context->framerate.den > 1)
				{
					digit = 3;
				}

				// yuv420p, 1920x1080 [SAR 1:1 DAR 16:9], 24 fps
				message.AppendFormat("%s, %dx%d [SAR %d:%d DAR %d:%d], %.*f fps, ",
									 ::av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format)),
									 parameters->width, parameters->height,
									 parameters->sample_aspect_ratio.num, parameters->sample_aspect_ratio.den,
									 parameters->width / gcd, parameters->height / gcd,
									 digit, ::av_q2d(context->framerate)
									 );
			}
			else
			{
				char channel_layout[16]{};
				::av_get_channel_layout_string(channel_layout, OV_COUNTOF(channel_layout), parameters->channels, parameters->channel_layout);

				// 48000 Hz, stereo, fltp,
				message.AppendFormat("%d Hz, %s, %s, ", parameters->sample_rate, channel_layout, ::av_get_sample_fmt_name(static_cast<AVSampleFormat>(parameters->format)));
			}

			// xxx kbps
			message.AppendFormat("%d kbps, ", (parameters->bit_rate / 1024));
			// timebase: 1/48000
			message.AppendFormat("timebase: %d/%d, ", context->time_base.num, context->time_base.den);
			// frame_size: 1234
			message.AppendFormat("frame_size: %d", parameters->frame_size);
			if (parameters->block_align != 0)
			{
				// align: 32
				message.AppendFormat(", align: %d", parameters->block_align);
			}

			break;
		
		case AVMEDIA_TYPE_DATA:
			message = "Data";
			break;

		case AVMEDIA_TYPE_SUBTITLE:
			message = "Subtitle";
			break;

		case AVMEDIA_TYPE_ATTACHMENT:
			message = "Attachment";
			break;

		case AVMEDIA_TYPE_NB:
			message = "NB";
			break;
	}

	return message;
}