#pragma once

#include <base/common_types.h>
#include <base/publisher/stream.h>
#include <modules/ovt_packetizer/ovt_packetizer.h>

#include "monitoring/monitoring.h"

class FileStream : public pub::Stream, public OvtPacketizerInterface
{
public:
	static std::shared_ptr<FileStream> Create(const std::shared_ptr<pub::Application> application,
											 const info::Stream &info,
											 uint32_t worker_count);
	explicit FileStream(const std::shared_ptr<pub::Application> application,
					   const info::Stream &info);
	~FileStream() final;

	void SendVideoFrame(const std::shared_ptr<MediaPacket> &media_packet) override;
	void SendAudioFrame(const std::shared_ptr<MediaPacket> &media_packet) override;

	bool OnOvtPacketized(std::shared_ptr<OvtPacket> &packet) override;

	bool RemoveSessionByConnectorId(int connector_id);

	Json::Value&		GetDescription();

private:
	bool Start(uint32_t worker_count) override;
	bool Stop() override;

	std::mutex 							_packetizer_lock;
	std::shared_ptr<OvtPacketizer>		_packetizer;

	std::shared_ptr<mon::StreamMetrics>		_stream_metrics;
};
