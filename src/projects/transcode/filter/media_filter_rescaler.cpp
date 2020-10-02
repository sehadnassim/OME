//==============================================================================
//
//  Transcode
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#include "media_filter_rescaler.h"

#include <base/ovlibrary/ovlibrary.h>

#define OV_LOG_TAG "MediaFilter.Rescaler"

MediaFilterRescaler::MediaFilterRescaler()
{
	::avfilter_register_all();

	_frame = ::av_frame_alloc();

	_outputs = ::avfilter_inout_alloc();
	_inputs = ::avfilter_inout_alloc();

	OV_ASSERT2(_frame != nullptr);
	OV_ASSERT2(_inputs != nullptr);
	OV_ASSERT2(_outputs != nullptr);
}

MediaFilterRescaler::~MediaFilterRescaler()
{
	Stop();

	OV_SAFE_FUNC(_frame, nullptr, ::av_frame_free, &);

	OV_SAFE_FUNC(_inputs, nullptr, ::avfilter_inout_free, &);
	OV_SAFE_FUNC(_outputs, nullptr, ::avfilter_inout_free, &);

	OV_SAFE_FUNC(_filter_graph, nullptr, ::avfilter_graph_free, &);
}

bool MediaFilterRescaler::Configure(const std::shared_ptr<MediaTrack> &input_media_track, const std::shared_ptr<TranscodeContext> &input_context, const std::shared_ptr<TranscodeContext> &output_context)
{
	int ret;

	const AVFilter *buffersrc = ::avfilter_get_by_name("buffer");
	const AVFilter *buffersink = ::avfilter_get_by_name("buffersink");

	_filter_graph = ::avfilter_graph_alloc();

	if ((_filter_graph == nullptr) || (_inputs == nullptr) || (_outputs == nullptr))
	{
		logte("Could not allocate variables for filter graph: %p, %p, %p", _filter_graph, _inputs, _outputs);
		return false;
	}

	AVRational input_timebase = TimebaseToAVRational(input_context->GetTimeBase());
	AVRational output_timebase = TimebaseToAVRational(output_context->GetTimeBase());

	_scale = ::av_q2d(::av_div_q(input_timebase, output_timebase));

	if (::isnan(_scale))
	{
		logte("Invalid timebase: input: %d/%d, output: %d/%d",
			  input_timebase.num, input_timebase.den,
			  output_timebase.num, output_timebase.den);

		return false;
	}

	// Prepare filters
	//
	// Filter graph:
	//     [buffer] -> [fps] -> [scale] -> [settb] -> [buffersink]

	// Prepare the input filter

	// "buffer" filter
/*	
	AVRational framerate = ::av_d2q(input_context->GetFrameRate(), AV_TIME_BASE);
	logte("framerate=%0.2f, num=%d, den=%d", input_context->GetFrameRate(), framerate.num, framerate.den);

	ov::String input_args = ov::String::FormatString(
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d:sws_param=flags=bicubic",
		input_media_track->GetWidth(), input_media_track->GetHeight(),
		input_media_track->GetFormat(),
		input_media_track->GetTimeBase().GetNum(), input_media_track->GetTimeBase().GetDen(),
		1, 1,
		framerate.num, framerate.den);
*/
	// Removed framerate filter. because, Timestamp of frame is shifted. In case of not constant framerate as is VFR.
	ov::String input_args = ov::String::FormatString(
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:sws_param=flags=bicubic",
		input_media_track->GetWidth(), input_media_track->GetHeight(),
		input_media_track->GetFormat(),
		input_media_track->GetTimeBase().GetNum(), input_media_track->GetTimeBase().GetDen(),
		1, 1);



	ret = ::avfilter_graph_create_filter(&_buffersrc_ctx, buffersrc, "in", input_args, nullptr, _filter_graph);
	if (ret < 0)
	{
		logte("Could not create video buffer source filter for rescaling: %d", ret);
		return false;
	}

	// Prepare output filters
	std::vector<ov::String> filters = {
		// "fps" filter options
		ov::String::FormatString("fps=fps=%.2f:0:round=near", output_context->GetFrameRate()),
		// "scale" filter options
		ov::String::FormatString("scale=%dx%d:flags=bicubic", output_context->GetVideoWidth(), output_context->GetVideoHeight()),
		// "settb" filter options
		ov::String::FormatString("settb=%s", output_context->GetTimeBase().GetStringExpr().CStr()),
	};

	ov::String output_filters = ov::String::Join(filters, ",");

	ret = ::avfilter_graph_create_filter(&_buffersink_ctx, buffersink, "out", nullptr, nullptr, _filter_graph);
	if (ret < 0)
	{
		logte("Could not create video buffer sink filter for rescaling: %d", ret);
		return false;
	}

	enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};

	ret = av_opt_set_int_list(_buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

	if (ret < 0)
	{
		logte("Could not set output pixel format for rescaling: %d", ret);
		return false;
	}

	_outputs->name = ::av_strdup("in");
	_outputs->filter_ctx = _buffersrc_ctx;
	_outputs->pad_idx = 0;
	_outputs->next = nullptr;

	_inputs->name = ::av_strdup("out");
	_inputs->filter_ctx = _buffersink_ctx;
	_inputs->pad_idx = 0;
	_inputs->next = nullptr;

	if ((ret = ::avfilter_graph_parse_ptr(_filter_graph, output_filters, &_inputs, &_outputs, nullptr)) < 0)
	{
		logte("Could not parse filter string for rescaling: %d (%s)", ret, output_filters.CStr());
		return false;
	}

	if ((ret = ::avfilter_graph_config(_filter_graph, nullptr)) < 0)
	{
		logte("Could not validate filter graph for rescaling: %d", ret);
		return false;
	}

	logtd("Rescaler is enabled for track #%u using parameters: input: %s, outputs: %s", input_media_track->GetId(), input_args.CStr(), output_filters.CStr());

	_input_context = input_context;
	_output_context = output_context;

	// Generates a thread that reads and encodes frames in the input_buffer queue and places them in the output queue.
	try
	{
		_kill_flag = false;

		_thread_work = std::thread(&MediaFilterRescaler::ThreadFilter, this);
	}
	catch (const std::system_error &e)
	{
		_kill_flag = true;

		logte("Failed to start transcode rescale filter thread.");
	}

	return true;
}

int32_t MediaFilterRescaler::SendBuffer(std::shared_ptr<MediaFrame> buffer)
{
	//logtp("Data before rescaling: %lld (%.0f)\n%s", buffer->GetPts(), buffer->GetPts() * _output_context->GetTimeBase().GetExpr() * 1000.0f, ov::Dump(buffer->GetBuffer(0), buffer->GetBufferSize(0), 32).CStr());
	std::unique_lock<std::mutex> mlock(_mutex);

	_input_buffer.push_back(std::move(buffer));
	
	mlock.unlock();
	
	_queue_event.Notify();;

	return 0;
}


void MediaFilterRescaler::Stop()
{
	_kill_flag = true;

	_queue_event.Notify();

	if (_thread_work.joinable())
	{
		_thread_work.join();
		logtd("Terminated transcode rescale filter thread.");
	}
}

void MediaFilterRescaler::ThreadFilter()
{
	logtd("Start transcode rescaler filter thread.");

	while(!_kill_flag)
	{
		_queue_event.Wait();

		std::unique_lock<std::mutex> mlock(_mutex);

		if(_input_buffer.empty())
		{
			continue;
		}

		// Dequeue a packet
		auto frame = std::move(_input_buffer.front());
		_input_buffer.pop_front();

		mlock.unlock();

		//logtp("Dequeued data for rescaling: %lld (%.0f)\n%s", frame->GetPts(), frame->GetPts() * _output_context->GetTimeBase().GetExpr() * 1000.0f, ov::Dump(frame->GetBuffer(0), frame->GetBufferSize(0), 32).CStr());

		_frame->format = frame->GetFormat();
		_frame->width = frame->GetWidth();
		_frame->height = frame->GetHeight();
		_frame->pts = frame->GetPts() * _scale;
		_frame->pkt_duration = frame->GetDuration();

		_frame->linesize[0] = frame->GetStride(0);
		_frame->linesize[1] = frame->GetStride(1);
		_frame->linesize[2] = frame->GetStride(2);

		int ret = ::av_frame_get_buffer(_frame, 32);
		if (ret < 0)
		{
			logte("Could not allocate the video frame data\n");

			// *result = TranscodeResult::DataError;
			break;
		}

		ret = ::av_frame_make_writable(_frame);
		if (ret < 0)
		{
			logte("Could not make writable frame: %d", ret);

			// *result = TranscodeResult::DataError;
			break;
		}

		// Copy data of frame to _frame
		::memcpy(_frame->data[0], frame->GetBuffer(0), frame->GetBufferSize(0));
		::memcpy(_frame->data[1], frame->GetBuffer(1), frame->GetBufferSize(1));
		::memcpy(_frame->data[2], frame->GetBuffer(2), frame->GetBufferSize(2));

		if (::av_buffersrc_add_frame_flags(_buffersrc_ctx, _frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
		{
			logte("An error occurred while feeding the audio filtergraph: format: %d, pts: %lld, linesize: %d, size: %d", _frame->format, _frame->pts, _frame->linesize[0], _input_buffer.size());
		
			std::unique_lock<std::mutex> mlock(_mutex);
			_input_buffer.push_front(std::move(frame));
			mlock.unlock();
			_queue_event.Notify();
		}

		::av_frame_unref(_frame);
	

		while (true)
		{
			// 출력될 프레임이 있는지 확인함
			int ret = ::av_buffersink_get_frame(_buffersink_ctx, _frame);

			if (ret == AVERROR(EAGAIN))
			{
				// Need more data
				break;
			}
			else if (ret == AVERROR_EOF)
			{
				logte("End of file: %d", ret);
				// *result = TranscodeResult::EndOfFile;
				// return nullptr;
				break;
			}
			else if (ret < 0)
			{
				logte("Unknown error is occurred while get frame: %d", ret);
				// *result = TranscodeResult::DataError;
				// return nullptr;
				break;
			}
			else
			{
				auto output_frame = std::make_shared<MediaFrame>();

				output_frame->SetFormat(_frame->format);
				output_frame->SetWidth(_frame->width);
				output_frame->SetHeight(_frame->height);
				output_frame->SetPts((_frame->pts == AV_NOPTS_VALUE) ? -1LL : _frame->pts);
				output_frame->SetDuration(_frame->pkt_duration * _scale);

				output_frame->SetStride(_frame->linesize[0], 0);
				output_frame->SetStride(_frame->linesize[1], 1);
				output_frame->SetStride(_frame->linesize[2], 2);

				output_frame->SetBuffer(_frame->data[0], output_frame->GetStride(0) * output_frame->GetHeight(), 0);	  // Y-Plane
				output_frame->SetBuffer(_frame->data[1], output_frame->GetStride(1) * output_frame->GetHeight() / 2, 1);  // Cb Plane
				output_frame->SetBuffer(_frame->data[2], output_frame->GetStride(2) * output_frame->GetHeight() / 2, 2);  // Cr Plane

				//logtp("Rescaled data: %lld (%.0f)\n%s", output_frame->GetPts(), output_frame->GetPts() * _output_context->GetTimeBase().GetExpr() * 1000.0f, ov::Dump(_frame->data[0], _frame->linesize[0], 32).CStr());

				::av_frame_unref(_frame);

				std::unique_lock<std::mutex> mlock(_mutex);

				_output_buffer.push_back(std::move(output_frame));
			}
		}


	}
}
std::shared_ptr<MediaFrame> MediaFilterRescaler::RecvBuffer(TranscodeResult *result)
{
	std::unique_lock<std::mutex> mlock(_mutex);
	if(!_output_buffer.empty())
	{
		*result = TranscodeResult::DataReady;

		auto frame = std::move(_output_buffer.front());
		_output_buffer.pop_front();

		return std::move(frame);
	}

	*result = TranscodeResult::NoData;

	return nullptr;
}
