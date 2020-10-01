//==============================================================================
//
//  Transcode
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "transcode_codec_enc_vp8.h"

#define OV_LOG_TAG "TranscodeCodec"

OvenCodecImplAvcodecEncVP8::~OvenCodecImplAvcodecEncVP8()
{
	Stop();
}

bool OvenCodecImplAvcodecEncVP8::Configure(std::shared_ptr<TranscodeContext> context)
{
	if (TranscodeEncoder::Configure(context) == false)
	{
		return false;
	}

	auto codec_id = GetCodecID();

	AVCodec *codec = ::avcodec_find_encoder(codec_id);

	if (codec == nullptr)
	{
		logte("Could not find encoder: %d (%s)", codec_id, ::avcodec_get_name(codec_id));
		return false;
	}

	_context = ::avcodec_alloc_context3(codec);

	if (_context == nullptr)
	{
		logte("Could not allocate codec context for %s (%d)", ::avcodec_get_name(codec_id), codec_id);
		return false;
	}

	// 인코딩 옵션 설정
	AVRational codec_timebase = TimebaseToAVRational(_output_context->GetTimeBase());

	_context->bit_rate = _output_context->GetBitrate();
	_context->rc_max_rate = _context->bit_rate;
	_context->rc_min_rate = _context->bit_rate;
	_context->sample_aspect_ratio = (AVRational){1, 1};
	_context->time_base = TimebaseToAVRational(_output_context->GetTimeBase());
	_context->framerate = ::av_d2q(_output_context->GetFrameRate(), AV_TIME_BASE);
	_context->gop_size = _output_context->GetFrameRate() * 1;  // create keyframes every second
	_context->max_b_frames = 0;
	_context->pix_fmt = AV_PIX_FMT_YUV420P;
	_context->width = _output_context->GetVideoWidth();
	_context->height = _output_context->GetVideoHeight();
	_context->thread_count = 2;

	AVRational output_timebase = TimebaseToAVRational(_output_context->GetTimeBase());
	_scale = ::av_q2d(::av_div_q(output_timebase, codec_timebase));
	_scale_inv = ::av_q2d(::av_div_q(codec_timebase, output_timebase));

	AVDictionary *opts = nullptr;
	// ::av_dict_set_int(&opts, "cpu-used", _context->thread_count, 0);
	::av_dict_set(&opts, "quality", "realtime", 0);

	if (::avcodec_open2(_context, codec, &opts) < 0)
	{
		logte("Could not open codec");
		return false;
	}

	try
	{
		_kill_flag = false;

		_thread_work = std::thread(&OvenCodecImplAvcodecEncVP8::ThreadEncode, this);
	}
	catch (const std::system_error &e)
	{
		_kill_flag = true;

		logte("Failed to start transcode stream thread.");
	}


	return true;
}


void OvenCodecImplAvcodecEncVP8::Stop()
{
	_kill_flag = true;

	_queue_event.Notify();

	if (_thread_work.joinable())
	{
		_thread_work.join();
		logtd("VP8 encoder thread has ended.");
	}
}

void OvenCodecImplAvcodecEncVP8::ThreadEncode()
{
	while(!_kill_flag)
	{
		_queue_event.Wait();

		std::unique_lock<std::mutex> mlock(_mutex);

		if (_input_buffer.empty())
		{
			continue;
		}

		///////////////////////////////////////////////////
		// Request frame encoding to codec
		///////////////////////////////////////////////////

		auto frame = std::move(_input_buffer.front());
		_input_buffer.pop_front();

		mlock.unlock();


		_frame->format = frame->GetFormat();
		_frame->nb_samples = 1;
		_frame->pts = frame->GetPts() * _scale;
		// The encoder will not pass this duration
		_frame->pkt_duration = frame->GetDuration();

		_frame->width = frame->GetWidth();
		_frame->height = frame->GetHeight();
		_frame->linesize[0] = frame->GetStride(0);
		_frame->linesize[1] = frame->GetStride(1);
		_frame->linesize[2] = frame->GetStride(2);

		if (::av_frame_get_buffer(_frame, 32) < 0)
		{
			logte("Could not allocate the video frame data");
			// *result = TranscodeResult::DataError;
			break;
		}

		if (::av_frame_make_writable(_frame) < 0)
		{
			logte("Could not make sure the frame data is writable");
			// *result = TranscodeResult::DataError;
			break;
		}

		::memcpy(_frame->data[0], frame->GetBuffer(0), frame->GetBufferSize(0));
		::memcpy(_frame->data[1], frame->GetBuffer(1), frame->GetBufferSize(1));
		::memcpy(_frame->data[2], frame->GetBuffer(2), frame->GetBufferSize(2));

		int ret = ::avcodec_send_frame(_context, _frame);
		// int ret = 0;
		::av_frame_unref(_frame);

		if (ret < 0)
		{
			logte("Error sending a frame for encoding : %d", ret);

			// Failure to send frame to encoder. Wait and put it back in. But it doesn't happen as often as possible.
			std::unique_lock<std::mutex> mlock(_mutex);
			_input_buffer.push_front(std::move(frame));
			mlock.unlock();
			_queue_event.Notify();
		}

		///////////////////////////////////////////////////
		// The encoded packet is taken from the codec.
		///////////////////////////////////////////////////
		while(true)
		{
			// Check frame is availble
			int ret = ::avcodec_receive_packet(_context, _packet);

			if (ret == AVERROR(EAGAIN))
			{
				// More packets are needed for encoding.

				// logte("Error receiving a packet for decoding : EAGAIN");

				break;
			}
			else if (ret == AVERROR_EOF)
			{
				logte("Error receiving a packet for decoding : AVERROR_EOF");
				break;
			}
			else if (ret < 0)
			{
				logte("Error receiving a packet for decoding : %d", ret);
				break;
			}
			else
			{
				// Encoded packet is ready
				auto packet_buffer = MakePacket();
				::av_packet_unref(_packet);

				SendOutputBuffer(std::move(packet_buffer));
			}			
		}
	}
}

std::shared_ptr<MediaPacket> OvenCodecImplAvcodecEncVP8::RecvBuffer(TranscodeResult *result)
{
	std::unique_lock<std::mutex> mlock(_mutex);
	if(!_output_buffer.empty())
	{
		*result = TranscodeResult::DataReady;

		auto packet = std::move(_output_buffer.front());
		_output_buffer.pop_front();
		
		return std::move(packet);
	}

	*result = TranscodeResult::NoData;

	return nullptr;
}

std::shared_ptr<MediaPacket> OvenCodecImplAvcodecEncVP8::MakePacket() const
{
	auto flag = (_packet->flags & AV_PKT_FLAG_KEY) ? MediaPacketFlag::Key : MediaPacketFlag::NoFlag;
	// This is workaround: avcodec_receive_packet() does not give the duration that sent to avcodec_send_frame()
	int den = _output_context->GetTimeBase().GetDen();
	int64_t duration = (den == 0) ? 0LL : (float)den / _output_context->GetFrameRate();
	auto packet = std::make_shared<MediaPacket>(common::MediaType::Video, 0, _packet->data, _packet->size, _packet->pts, _packet->dts, duration, flag);

	return std::move(packet);
}
