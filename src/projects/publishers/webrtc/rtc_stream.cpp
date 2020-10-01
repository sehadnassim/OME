#include "rtc_private.h"
#include "rtc_stream.h"
#include "rtc_application.h"
#include "rtc_session.h"
#include <base/info/media_extradata.h>

using namespace common;

std::shared_ptr<RtcStream> RtcStream::Create(const std::shared_ptr<pub::Application> application,
                                             const info::Stream &info,
                                             uint32_t worker_count)
{
	auto stream = std::make_shared<RtcStream>(application, info);
	if(!stream->Start(worker_count))
	{
		return nullptr;
	}
	return stream;
}

RtcStream::RtcStream(const std::shared_ptr<pub::Application> application,
                     const info::Stream &info)
	: Stream(application, info)
{
	_certificate = application->GetSharedPtrAs<RtcApplication>()->GetCertificate();
	_vp8_picture_id = 0x8000; // 1 {000 0000 0000 0000} 1 is marker for 15 bit length
}

RtcStream::~RtcStream()
{
	logtd("RtcStream(%d) has been terminated finally", GetId());
	Stop();
}

bool RtcStream::Start(uint32_t worker_count)
{
	// OFFER SDP 생성
	_offer_sdp = std::make_shared<SessionDescription>();
	_offer_sdp->SetOrigin("OvenMediaEngine", ov::Random::GenerateUInt32(), 2, "IN", 4, "127.0.0.1");
	_offer_sdp->SetTiming(0, 0);
	_offer_sdp->SetIceOption("trickle");
	_offer_sdp->SetIceUfrag(ov::Random::GenerateString(8));
	_offer_sdp->SetIcePwd(ov::Random::GenerateString(32));
	_offer_sdp->SetMsidSemantic("WMS", "*");
	_offer_sdp->SetFingerprint("sha-256", _certificate->GetFingerprint("sha-256"));

	std::shared_ptr<MediaDescription> video_media_desc = nullptr;
	std::shared_ptr<MediaDescription> audio_media_desc = nullptr;

	bool first_video_desc = true;
	bool first_audio_desc = true;
	uint8_t payload_type_num = PAYLOAD_TYPE_OFFSET;

	for(auto &track_item : _tracks)
	{
		ov::String codec = "";
		auto &track = track_item.second;

		switch(track->GetMediaType())
		{
			case MediaType::Video:
			{
				auto payload = std::make_shared<PayloadAttr>();

				switch(track->GetCodecId())
				{
					case MediaCodecId::Vp8:
						codec = "VP8";
						break;
					case MediaCodecId::H265:
						codec = "H265";
						// TODO(Getroot): Go!
						break;
					case MediaCodecId::H264:
						codec = "H264";

						{
							const auto &codec_extradata = track_item.second->GetCodecExtradata();
							H264Extradata h264_extradata;
							if (codec_extradata.empty() == false
								&& h264_extradata.Deserialize(codec_extradata)
								&& h264_extradata.GetSps().empty() == false
								&& h264_extradata.GetSps().front().size() >= 4
								&& h264_extradata.GetPps().empty() == false
							)
							{
								ov::String parameter_sets;
								for (const auto &sps : h264_extradata.GetSps())
								{
									parameter_sets.Append(ov::Base64::Encode(std::make_shared<ov::Data>(sps.data(), sps.size())));
									parameter_sets.Append(',');
								}
								const auto &pps = h264_extradata.GetPps();
								for (size_t pps_index = 0; pps_index < pps.size(); ++pps_index)
								{
									parameter_sets.Append(ov::Base64::Encode(std::make_shared<ov::Data>(pps[pps_index].data(), pps[pps_index].size())));
									if (pps_index != pps.size() - 1)
									{
										parameter_sets.Append(',');
									}
								}
								const auto &first_sps = h264_extradata.GetSps().front();
								payload->SetFmtp(ov::String::FormatString(
									// NonInterleaved => packetization-mode=1
									"packetization-mode=1;profile-level-id=%02x%02x%02x;sprop-parameter-sets=%s;level-asymmetry-allowed=1",
									first_sps[1], first_sps[2], first_sps[3], parameter_sets.CStr()
								));
							}
							else
							{
								payload->SetFmtp(ov::String::FormatString(
									// NonInterleaved => packetization-mode=1
									// baseline & lvl 3.1 => profile-level-id=42e01f
									"packetization-mode=1;profile-level-id=%x;level-asymmetry-allowed=1",
									0x42e01f
								));
							}
						}

						break;
					default:
						logti("Unsupported codec(%s/%s) is being input from media track", 
								ov::Converter::ToString(track->GetMediaType()).CStr(), 
								ov::Converter::ToString(track->GetCodecId()).CStr());
						continue;
				}

				if(first_video_desc)
				{
					video_media_desc = std::make_shared<MediaDescription>(_offer_sdp);
					video_media_desc->SetConnection(4, "0.0.0.0");
					// TODO(dimiden): Prevent duplication
					video_media_desc->SetMid(ov::Random::GenerateString(6));
					video_media_desc->SetSetup(MediaDescription::SetupType::ActPass);
					video_media_desc->UseDtls(true);
					video_media_desc->UseRtcpMux(true);
					video_media_desc->SetDirection(MediaDescription::Direction::SendOnly);
					video_media_desc->SetMediaType(MediaDescription::MediaType::Video);
					video_media_desc->SetCname(ov::Random::GenerateUInt32(), ov::Random::GenerateString(16));
					_offer_sdp->AddMedia(video_media_desc);
					first_video_desc = false;
				}

				payload->SetRtpmap(payload_type_num++, codec, 90000);
				payload->EnableRtcpFb(PayloadAttr::RtcpFbType::Nack, true);

				video_media_desc->AddPayload(payload);
				video_media_desc->Update();

				// RTP Packetizer를 추가한다.
				AddPacketizer(track->GetCodecId(), track->GetId(), payload->GetId(), video_media_desc->GetSsrc());

				break;
			}

			case MediaType::Audio:
			{
				auto payload = std::make_shared<PayloadAttr>();

				switch(track->GetCodecId())
				{
					case MediaCodecId::Opus:
						codec = "OPUS";

						// Enable inband-fec
						// a=fmtp:111 maxplaybackrate=16000; useinbandfec=1; maxaveragebitrate=20000
						if (track->GetChannel().GetLayout() == common::AudioChannel::Layout::LayoutStereo)
						{
							payload->SetFmtp("stereo=1;useinbandfec=1;");
						}
						else
						{
							payload->SetFmtp("useinbandfec=1;");
						}
						break;

					default:
						logti("Unsupported codec(%s/%s) is being input from media track", 
								ov::Converter::ToString(track->GetMediaType()).CStr(), 
								ov::Converter::ToString(track->GetCodecId()).CStr());
						continue;
				}

				if(first_audio_desc)
				{
					audio_media_desc = std::make_shared<MediaDescription>(_offer_sdp);
					audio_media_desc->SetConnection(4, "0.0.0.0");
					// TODO(dimiden): Need to prevent duplication
					audio_media_desc->SetMid(ov::Random::GenerateString(6));
					audio_media_desc->SetSetup(MediaDescription::SetupType::ActPass);
					audio_media_desc->UseDtls(true);
					audio_media_desc->UseRtcpMux(true);
					audio_media_desc->SetDirection(MediaDescription::Direction::SendOnly);
					audio_media_desc->SetMediaType(MediaDescription::MediaType::Audio);
					audio_media_desc->SetCname(ov::Random::GenerateUInt32(), ov::Random::GenerateString(16));
					_offer_sdp->AddMedia(audio_media_desc);
					first_audio_desc = false;
				}

				payload->SetRtpmap(payload_type_num++, codec, static_cast<uint32_t>(track->GetSample().GetRateNum()),
								   std::to_string(track->GetChannel().GetCounts()).c_str());

				audio_media_desc->AddPayload(payload);
				audio_media_desc->Update();

				// RTP Packetizer를 추가한다.
				AddPacketizer(track->GetCodecId(), track->GetId(), payload->GetId(), audio_media_desc->GetSsrc());

				break;
			}

			default:
				// not supported type
				logtw("Not supported media type: %d", (int)(track->GetMediaType()));
				break;
		}
	}

	if (video_media_desc)
	{
        // RED & ULPFEC
        auto red_payload = std::make_shared<PayloadAttr>();
        red_payload->SetRtpmap(RED_PAYLOAD_TYPE, "red", 90000);
        auto ulpfec_payload = std::make_shared<PayloadAttr>();
        ulpfec_payload->SetRtpmap(ULPFEC_PAYLOAD_TYPE, "ulpfec", 90000);

        video_media_desc->AddPayload(red_payload);
        video_media_desc->AddPayload(ulpfec_payload);
		video_media_desc->Update();
    }

	logtd("Stream is created : %s/%u", GetName().CStr(), GetId());

	_stream_metrics = StreamMetrics(*std::static_pointer_cast<info::Stream>(pub::Stream::GetSharedPtr()));

	_offer_sdp->Update();

	return Stream::Start(worker_count);
}

bool RtcStream::Stop()
{
	_offer_sdp->Release();
	_packetizers.clear();

	return Stream::Stop();
}

std::shared_ptr<SessionDescription> RtcStream::GetSessionDescription()
{
	return _offer_sdp;
}

bool RtcStream::OnRtpPacketized(std::shared_ptr<RtpPacket> packet)
{
	uint32_t rtp_payload_type = packet->PayloadType();
	uint32_t red_block_pt = 0;
	uint32_t origin_pt_of_fec = 0;

	if(rtp_payload_type == RED_PAYLOAD_TYPE)
	{
		red_block_pt = packet->Header()[packet->HeadersSize()-1];

		// RED includes FEC packet or Media packet.
		if(packet->IsUlpfec())
		{
			origin_pt_of_fec = packet->OriginPayloadType();
		}
	}

	// We make payload_type with the following structure:
	// 0               8                 16             24                 32
	//                 | origin_pt_of_fec | red block_pt | rtp_payload_type |
	uint32_t payload_type = rtp_payload_type | (red_block_pt << 8) | (origin_pt_of_fec << 16);

	BroadcastPacket(payload_type, packet->GetData());
	if(_stream_metrics != nullptr)
	{
		_stream_metrics->IncreaseBytesOut(PublisherType::Webrtc, packet->GetData()->GetLength() * GetSessionCount());
	}

	return true;
}

void RtcStream::SendVideoFrame(const std::shared_ptr<MediaPacket> &media_packet)
{
	auto media_track = GetTrack(media_packet->GetTrackId());

	// Create RTP Video Header
	CodecSpecificInfo codec_info;
	RTPVideoHeader rtp_video_header;

	codec_info.codec_type = media_track->GetCodecId();

	if(codec_info.codec_type == MediaCodecId::Vp8)
	{
		// Structure for future expansion.
		// In the future, when OME uses codec-specific features, certain information is obtained from media_packet.
		codec_info.codec_specific.vp8 = CodecSpecificInfoVp8();
	}
	else if(codec_info.codec_type == MediaCodecId::H264 || 
			codec_info.codec_type == MediaCodecId::H265)
	{
		codec_info.codec_specific.h26X = CodecSpecificInfoH26X();
	}

	memset(&rtp_video_header, 0, sizeof(RTPVideoHeader));

	MakeRtpVideoHeader(&codec_info, &rtp_video_header);

	// RTP Packetizing
	auto packetizer = GetPacketizer(media_track->GetId());
	if(packetizer == nullptr)
	{
		return;
	}

	auto frame_type = (media_packet->GetFlag() == MediaPacketFlag::Key) ? FrameType::VideoFrameKey : FrameType::VideoFrameDelta;
	auto timestamp = media_packet->GetPts();
	auto data = media_packet->GetData();
	auto fragmentation = media_packet->GetFragHeader();

	packetizer->Packetize(frame_type,
	                      timestamp,
	                      data->GetDataAs<uint8_t>(),
	                      data->GetLength(),
						  fragmentation,
	                      &rtp_video_header);
}

void RtcStream::SendAudioFrame(const std::shared_ptr<MediaPacket> &media_packet)
{
	auto media_track = GetTrack(media_packet->GetTrackId());

	// RTP Packetizing
	// Track의 GetId와 PayloadType은 같다. Track의 ID로 Payload Type을 만들기 때문이다.
	auto packetizer = GetPacketizer(media_track->GetId());
	if(packetizer == nullptr)
	{
		return;
	}

	auto frame_type = (media_packet->GetFlag() == MediaPacketFlag::Key) ? FrameType::AudioFrameKey : FrameType::AudioFrameDelta;
	auto timestamp = media_packet->GetPts();
	auto data = media_packet->GetData();
	auto fragmentation = media_packet->GetFragHeader();

	packetizer->Packetize(frame_type,
						  timestamp,
						  data->GetDataAs<uint8_t>(),
						  data->GetLength(),
						  fragmentation,
	                      nullptr);
}

uint16_t RtcStream::AllocateVP8PictureID()
{
	_vp8_picture_id++;

	// PictureID is 7 bit or 15 bit. We use only 15 bit.
	if(_vp8_picture_id == 0)
	{
		// 1{000 0000 0000 0000} is initial number. (first bit means to use 15 bit size)
		_vp8_picture_id = 0x8000;
	}

	return _vp8_picture_id;
}

void RtcStream::MakeRtpVideoHeader(const CodecSpecificInfo *info, RTPVideoHeader *rtp_video_header)
{
	switch(info->codec_type)
	{
		case common::MediaCodecId::Vp8:
			rtp_video_header->codec = common::MediaCodecId::Vp8;
			rtp_video_header->codec_header.vp8.InitRTPVideoHeaderVP8();
			// With Ulpfec, picture id is needed.
			rtp_video_header->codec_header.vp8.picture_id = AllocateVP8PictureID();
			rtp_video_header->codec_header.vp8.non_reference = info->codec_specific.vp8.non_reference;
			rtp_video_header->codec_header.vp8.temporal_idx = info->codec_specific.vp8.temporal_idx;
			rtp_video_header->codec_header.vp8.layer_sync = info->codec_specific.vp8.layer_sync;
			rtp_video_header->codec_header.vp8.tl0_pic_idx = info->codec_specific.vp8.tl0_pic_idx;
			rtp_video_header->codec_header.vp8.key_idx = info->codec_specific.vp8.key_idx;
			rtp_video_header->simulcast_idx = info->codec_specific.vp8.simulcast_idx;
			return;
		case common::MediaCodecId::H264:
			rtp_video_header->codec = common::MediaCodecId::H264;
			rtp_video_header->codec_header.h26X.packetization_mode = info->codec_specific.h26X.packetization_mode;
			rtp_video_header->simulcast_idx = info->codec_specific.h26X.simulcast_idx;
			return;

		case common::MediaCodecId::H265:
			rtp_video_header->codec = common::MediaCodecId::H265;
			rtp_video_header->codec_header.h26X.packetization_mode = info->codec_specific.h26X.packetization_mode;
			rtp_video_header->simulcast_idx = info->codec_specific.h26X.simulcast_idx;
			return;
		default:
			break;
	}
}

void RtcStream::AddPacketizer(common::MediaCodecId codec_id, uint32_t id, uint8_t payload_type, uint32_t ssrc)
{
	logtd("Add Packetizer : codec(%u) id(%u) pt(%d) ssrc(%u)", codec_id, id, payload_type, ssrc);

	auto packetizer = std::make_shared<RtpPacketizer>(RtpRtcpPacketizerInterface::GetSharedPtr());
	packetizer->SetPayloadType(payload_type);
	packetizer->SetSSRC(ssrc);
	
	switch(codec_id)
	{
		case MediaCodecId::Vp8:
		case MediaCodecId::H264:
		case MediaCodecId::H265:
			packetizer->SetVideoCodec(codec_id);
			packetizer->SetUlpfec(RED_PAYLOAD_TYPE, ULPFEC_PAYLOAD_TYPE);
			break;
		case MediaCodecId::Opus:
			packetizer->SetAudioCodec(codec_id);
			break;
		default:
			// No support codecs
			return;
	}

	_packetizers[id] = packetizer;
}

std::shared_ptr<RtpPacketizer> RtcStream::GetPacketizer(uint32_t id)
{
	if(!_packetizers.count(id))
	{
		return nullptr;
	}

	return _packetizers[id];
}
