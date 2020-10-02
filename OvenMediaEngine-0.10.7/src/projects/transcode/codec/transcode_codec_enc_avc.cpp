//==============================================================================
//
//  Transcode
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "transcode_codec_enc_avc.h"

#include <unistd.h>

#define OV_LOG_TAG "TranscodeCodec"


OvenCodecImplAvcodecEncAVC::~OvenCodecImplAvcodecEncAVC()
{
	Stop();
}

// Notes.
//
// - B-frame must be disabled. because, WEBRTC does not support B-Frame.
//
bool OvenCodecImplAvcodecEncAVC::Configure(std::shared_ptr<TranscodeContext> context)
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

	_context->framerate = ::av_d2q(_output_context->GetFrameRate(), AV_TIME_BASE);

	_context->bit_rate = _output_context->GetBitrate();
	_context->rc_min_rate = _context->bit_rate;
	_context->rc_max_rate = _context->bit_rate;
	_context->rc_buffer_size = static_cast<int>(_context->bit_rate / 2);
	_context->sample_aspect_ratio = (AVRational){1, 1};

	// From avcodec.h:
	// For some codecs, the time base is closer to the field rate than the frame rate.
	// Most notably, H.264 and MPEG-2 specify time_base as half of frame duration
	// if no telecine is used ...
	// Set to time_base ticks per frame. Default 1, e.g., H.264/MPEG-2 set it to 2.
	_context->ticks_per_frame = 2;
	// From avcodec.h:
	// For fixed-fps content, timebase should be 1/framerate and timestamp increments should be identically 1.
	// This often, but not always is the inverse of the frame rate or field rate for video. 1/time_base is not the average frame rate if the frame rate is not constant.

	AVRational codec_timebase = ::av_inv_q(::av_mul_q(::av_d2q(_output_context->GetFrameRate(), AV_TIME_BASE), (AVRational){_context->ticks_per_frame, 1}));
	_context->time_base = codec_timebase;

	_context->gop_size = _context->framerate.num / _context->framerate.den;
	_context->max_b_frames = 0;
	_context->pix_fmt = AV_PIX_FMT_YUV420P;
	_context->width = _output_context->GetVideoWidth();
	_context->height = _output_context->GetVideoHeight();
	_context->thread_count = 2;
	AVRational output_timebase = TimebaseToAVRational(_output_context->GetTimeBase());
	_scale = ::av_q2d(::av_div_q(output_timebase, codec_timebase));
	_scale_inv = ::av_q2d(::av_div_q(codec_timebase, output_timebase));

	// 인코딩 품질 및 브라우저 호환성
	// For browser compatibility
	// _context->profile = FF_PROFILE_H264_MAIN;
	_context->profile = FF_PROFILE_H264_BASELINE;

	// 인코딩 성능
	::av_opt_set(_context->priv_data, "preset", "ultrafast", 0);

	// 인코딩 딜레이
	::av_opt_set(_context->priv_data, "tune", "zerolatency", 0);

	// 인코딩 딜레이에서 sliced-thread 옵션 제거. MAC 환경에서 브라우저 호환성
	::av_opt_set(_context->priv_data, "x264opts", "bframes=0:sliced-threads=0:b-adapt=1:no-scenecut:keyint=30:min-keyint=30", 0);
	// ::av_opt_set(_context->priv_data, "x264opts", "bframes=0:sliced-threads=0:b-adapt=1", 0);

	// CBR 옵션 / bitrate는 kbps 단위 / *문제는 MAC 크롬에서 재생이 안된다. 그래서 maxrate 값만 지정해줌.
	// x264opts.AppendFormat(":nal-hrd=cbr:force-cfr=1:bitrate=%d:vbv-maxrate=%d:vbv-bufsize=%d:", _context->bit_rate/1000,  _context->bit_rate/1000,  _context->bit_rate/1000);

	if (::avcodec_open2(_context, codec, nullptr) < 0)
	{
		logte("Could not open codec: %s (%d)", ::avcodec_get_name(codec_id), codec_id);
		return false;
	}

	// Generates a thread that reads and encodes frames in the input_buffer queue and places them in the output queue.
	try
	{
		_kill_flag = false;

		_thread_work = std::thread(&OvenCodecImplAvcodecEncAVC::ThreadEncode, this);
	}
	catch (const std::system_error &e)
	{
		_kill_flag = true;

		logte("Failed to start transcode stream thread.");
	}

	return true;
}

void OvenCodecImplAvcodecEncAVC::Stop()
{
	_kill_flag = true;

	_queue_event.Notify();

	if (_thread_work.joinable())
	{
		_thread_work.join();
		logtd("AVC encoder thread has ended.");
	}
}

void OvenCodecImplAvcodecEncAVC::ThreadEncode()
{
	while(!_kill_flag)
	{
		_queue_event.Wait();

		std::unique_lock<std::mutex> mlock(_mutex);

		// 스레드 종료와 같이 큐에 데이터가 없는 경우에는 다시 대기를 한다
		if (_input_buffer.empty())
		{
			continue;
		}

		auto frame = std::move(_input_buffer.front());
		_input_buffer.pop_front();

		mlock.unlock();

		///////////////////////////////////////////////////
		// Request frame encoding to codec
		///////////////////////////////////////////////////


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


std::shared_ptr<MediaPacket> OvenCodecImplAvcodecEncAVC::RecvBuffer(TranscodeResult *result)
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

std::shared_ptr<MediaPacket> OvenCodecImplAvcodecEncAVC::MakePacket() const
{
	auto flag = (_packet->flags & AV_PKT_FLAG_KEY) ? MediaPacketFlag::Key : MediaPacketFlag::NoFlag;
	// This is workaround: avcodec_receive_packet() does not give the duration that sent to avcodec_send_frame()
	int den = _output_context->GetTimeBase().GetDen();
	int64_t duration = (den == 0) ? 0LL : (float)den / _output_context->GetFrameRate();
	auto packet = std::make_shared<MediaPacket>(common::MediaType::Video, 0, _packet->data, _packet->size, _packet->pts * _scale_inv, _packet->dts * _scale_inv, duration, flag);
	FragmentationHeader fragment_header;

	int nal_pattern_size = 4;
	int sps_start_index = -1;
	int sps_end_index = -1;
	int pps_start_index = -1;
	int pps_end_index = -1;
	int fragment_count = 0;
	int current_index = 0;

	while ((current_index + nal_pattern_size) < _packet->size)
	{
		if (_packet->data[current_index] == 0 && _packet->data[current_index + 1] == 0)
		{
			if (_packet->data[current_index + 2] == 0 && _packet->data[current_index + 3] == 1)
			{
				// Pattern 0x00 0x00 0x00 0x01
				nal_pattern_size = 4;
			}
			else if (_packet->data[current_index + 2] == 1)
			{
				// Pattern 0x00 0x00 0x01
				nal_pattern_size = 3;
			}
			else
			{
				current_index++;
				continue;
			}
		}
		else
		{
			current_index++;
			continue;
		}

		fragment_count++;

		if (sps_start_index == -1)
		{
			sps_start_index = current_index + nal_pattern_size;
			current_index += nal_pattern_size;

			if (_packet->flags != AV_PKT_FLAG_KEY)
			{
				break;
			}
		}
		else if (sps_end_index == -1)
		{
			sps_end_index = current_index - 1;
			pps_start_index = current_index + nal_pattern_size;
			current_index += nal_pattern_size;
		}
		else  // (pps_end_index == -1)
		{
			pps_end_index = current_index - 1;
			break;
		}
	}

	fragment_header.Clear();

	if (_packet->flags == AV_PKT_FLAG_KEY)  // KeyFrame
	{
		// SPS + PPS + IDR
		fragment_header.fragmentation_offset.emplace_back(sps_start_index);
		fragment_header.fragmentation_offset.emplace_back(pps_start_index);
		fragment_header.fragmentation_offset.emplace_back((pps_end_index + 1) + nal_pattern_size);

		fragment_header.fragmentation_length.emplace_back(sps_end_index - (sps_start_index - 1));
		fragment_header.fragmentation_length.emplace_back(pps_end_index - (pps_start_index - 1));
		fragment_header.fragmentation_length.emplace_back(_packet->size - (pps_end_index + nal_pattern_size));
	}
	else
	{
		// NON-IDR
		fragment_header.fragmentation_offset.emplace_back(sps_start_index);
		fragment_header.fragmentation_length.emplace_back(_packet->size - (sps_start_index - 1));
	}

	packet->SetFragHeader(&fragment_header);

	return std::move(packet);
}
