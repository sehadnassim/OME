//==============================================================================
//
//  RtmpProvider
//
//  Created by Getroot
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include <config/config.h>
#include <unistd.h>
#include <iostream>

#include "rtmp_provider.h"
#include "rtmp_application.h"
#include "rtmp_stream.h"
#include "rtmp_provider_private.h"

#include <modules/physical_port/physical_port_manager.h>
#include <base/info/media_extradata.h>

namespace pvd
{
	std::shared_ptr<RtmpProvider> RtmpProvider::Create(const cfg::Server &server_config, const std::shared_ptr<MediaRouteInterface> &router)
	{
		auto provider = std::make_shared<RtmpProvider>(server_config, router);
		if (!provider->Start())
		{
			logte("An error occurred while creating RtmpProvider");
			return nullptr;
		}
		return provider;
	}

	RtmpProvider::RtmpProvider(const cfg::Server &server_config, const std::shared_ptr<MediaRouteInterface> &router)
		: PushProvider(server_config, router)
	{
		logtd("Created Rtmp Provider module.");
	}

	RtmpProvider::~RtmpProvider()
	{
		logti("Terminated Rtmp Provider module.");
	}

	bool RtmpProvider::Start()
	{
		if (_physical_port != nullptr)
		{
			logtw("RTMP server is already running");
			return false;
		}

		auto server = GetServerConfig();
		auto rtmp_address = ov::SocketAddress(server.GetIp(), static_cast<uint16_t>(server.GetBind().GetProviders().GetRtmp().GetPort().GetPort()));

		_physical_port = PhysicalPortManager::Instance()->CreatePort(ov::SocketType::Tcp, rtmp_address);
		if (_physical_port == nullptr)
		{
			logte("Could not initialize phyiscal port for RTMP server: %s", rtmp_address.ToString().CStr());
			return false;
		}

		_physical_port->AddObserver(this);

		return Provider::Start();
	}

	bool RtmpProvider::Stop()
	{
		_physical_port->RemoveObserver(this);
		PhysicalPortManager::Instance()->DeletePort(_physical_port);

		_physical_port = nullptr;

		return Provider::Stop();
	}

	std::shared_ptr<pvd::Application> RtmpProvider::OnCreateProviderApplication(const info::Application &application_info)
	{
		return RtmpApplication::Create(GetSharedPtrAs<pvd::PushProvider>(), application_info);
	}

	bool RtmpProvider::OnDeleteProviderApplication(const std::shared_ptr<pvd::Application> &application)
	{
		return PushProvider::OnDeleteProviderApplication(application);
	}

	void RtmpProvider::OnConnected(const std::shared_ptr<ov::Socket> &remote)
	{
		auto channel_id = remote->GetId();
		auto stream = RtmpStream::Create(StreamSourceType::Rtmp, channel_id, remote, GetSharedPtrAs<pvd::PushProvider>());

		logti("A RTMP client has connected from %d - %s", remote->GetId(), remote->ToString().CStr());

		PushProvider::OnSignallingChannelCreated(remote->GetId(), stream);
	}

	void RtmpProvider::OnDataReceived(const std::shared_ptr<ov::Socket> &remote,
						const ov::SocketAddress &address,
						const std::shared_ptr<const ov::Data> &data)
	{
		PushProvider::OnDataReceived(remote->GetId(), data);
	}

	void RtmpProvider::OnDisconnected(const std::shared_ptr<ov::Socket> &remote,
						PhysicalPortDisconnectReason reason,
						const std::shared_ptr<const ov::Error> &error)
	{
		auto channel = GetChannel(remote->GetId());
		if(channel == nullptr)
		{
			logte("Failed to find channel to delete stream (remote : %s)", remote->ToString().CStr());
			return;
		}

		logti("The RTMP client has disconnected: [%s/%s], remote: %s",
					channel->GetApplicationName(), channel->GetName().CStr(),
					remote->ToString().CStr());

		PushProvider::OnChannelDeleted(remote->GetId());
	}
}