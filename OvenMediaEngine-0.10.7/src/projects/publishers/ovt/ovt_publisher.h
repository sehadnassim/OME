#pragma once

#include "modules/ovt_packetizer/ovt_packet.h"

#include "base/common_types.h"
#include "base/ovlibrary/url.h"
#include "base/publisher/publisher.h"
#include "base/media_route/media_route_application_interface.h"

#include "ovt_application.h"

#include <orchestrator/orchestrator.h>

class OvtPublisher : public pub::Publisher, public PhysicalPortObserver
{
public:
	static std::shared_ptr<OvtPublisher> Create(const cfg::Server &server_config, const std::shared_ptr<MediaRouteInterface> &router);

	OvtPublisher(const cfg::Server &server_config, const std::shared_ptr<MediaRouteInterface> &router);
	~OvtPublisher() override;
	bool Stop() override;

private:
	bool Start() override;

	//--------------------------------------------------------------------
	// Implementation of Publisher
	//--------------------------------------------------------------------
	PublisherType GetPublisherType() const override
	{
		return PublisherType::Ovt;
	}
	const char *GetPublisherName() const override
	{
		return "OVTPublisher";
	}

	std::shared_ptr<pub::Application> OnCreatePublisherApplication(const info::Application &application_info) override;
	bool OnDeletePublisherApplication(const std::shared_ptr<pub::Application> &application) override;
	
	bool GetMonitoringCollectionData(std::vector<std::shared_ptr<pub::MonitoringCollectionData>> &collections) override;

	//--------------------------------------------------------------------

	//--------------------------------------------------------------------
	// Implementation of PhysicalPortObserver
	//--------------------------------------------------------------------
	void OnConnected(const std::shared_ptr<ov::Socket> &remote) override;
	void OnDataReceived(const std::shared_ptr<ov::Socket> &remote, const ov::SocketAddress &address, const std::shared_ptr<const ov::Data> &data) override;
	void OnDisconnected(const std::shared_ptr<ov::Socket> &remote, PhysicalPortDisconnectReason reason, const std::shared_ptr<const ov::Error> &error) override;
	//--------------------------------------------------------------------


	void HandleDescribeRequest(const std::shared_ptr<ov::Socket> &remote, uint32_t request_id, const std::shared_ptr<const ov::Url> &url);
	void HandlePlayRequest(const std::shared_ptr<ov::Socket> &remote, uint32_t request_id, const std::shared_ptr<const ov::Url> &url);
	void HandleStopRequest(const std::shared_ptr<ov::Socket> &remote, uint32_t session_id, uint32_t request_id, const std::shared_ptr<const ov::Url> &url);

	void ResponseResult(const std::shared_ptr<ov::Socket> &remote, uint8_t payload_type, uint32_t session_id, uint32_t request_id, uint32_t code, const ov::String &msg);
	void ResponseResult(const std::shared_ptr<ov::Socket> &remote, uint8_t payload_type, uint32_t session_id, uint32_t request_id, uint32_t code, const ov::String &msg, const ov::String &key, const Json::Value &value);

	void SendResponse(const std::shared_ptr<ov::Socket> &remote, uint8_t payload_type, uint32_t session_id, const ov::String &payload);


	bool LinkRemoteWithStream(int remote_id, std::shared_ptr<OvtStream> &stream);
	bool UnlinkRemoteFromStream(int remote_id);

	std::shared_ptr<PhysicalPort> _server_port;

	// When a client is disconnected ungracefully, this map helps to find stream and delete the session quickly
	std::multimap<int, std::shared_ptr<OvtStream>>	_remote_stream_map;
};
