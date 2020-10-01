//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#include "segment_publisher.h"
#include "publisher_private.h"

#include <modules/signed_url/signed_url.h>
#include <monitoring/monitoring.h>
#include <orchestrator/orchestrator.h>
#include <publishers/segment/segment_stream/segment_stream.h>

SegmentPublisher::SegmentPublisher(const cfg::Server &server_config, const std::shared_ptr<MediaRouteInterface> &router)
	: Publisher(server_config, router)
{
}

SegmentPublisher::~SegmentPublisher()
{
	logtd("Publisher has been destroyed");
}

bool SegmentPublisher::Start(std::map<int, std::shared_ptr<HttpServer>> &http_server_manager, const cfg::SingularPort &port_config, const cfg::SingularPort &tls_port_config, const std::shared_ptr<SegmentStreamServer> &stream_server)
{
	auto server_config = GetServerConfig();
	auto ip = server_config.GetIp();

	auto port = port_config.GetPort();
	auto tls_port = tls_port_config.GetPort();
	bool has_port = (port != 0);
	bool has_tls_port = (tls_port != 0);

	ov::SocketAddress address(ip, port);
	ov::SocketAddress tls_address(ip, tls_port);

	// Register as observer
	stream_server->AddObserver(SegmentStreamObserver::GetSharedPtr());

	// Apply CORS settings
	// TODO(Dimiden): The Cross Domain configure must be at VHost Level.
	//stream_server->SetCrossDomain(cross_domains);

	// Start the DASH Server
	if (stream_server->Start(has_port ? &address : nullptr, has_tls_port ? &tls_address : nullptr,
							 http_server_manager, DEFAULT_SEGMENT_WORKER_THREAD_COUNT) == false)
	{
		logte("An error occurred while start %s Publisher", GetPublisherName());
		return false;
	}

	_stream_server = stream_server;

	logti("%s is listening on %s%s%s%s...",
		  GetPublisherName(),
		  has_port ? address.ToString().CStr() : "",
		  (has_port && has_tls_port) ? ", " : "",
		  has_tls_port ? "TLS: " : "",
		  has_tls_port ? tls_address.ToString().CStr() : "");

	return Publisher::Start();
}

bool SegmentPublisher::Stop()
{
	_run_thread = false;
	if(_worker_thread.joinable())
	{
		_worker_thread.join();
	}

	_stream_server->RemoveObserver(SegmentStreamObserver::GetSharedPtr());
	_stream_server->Stop();
	return Publisher::Stop();
}

bool SegmentPublisher::GetMonitoringCollectionData(std::vector<std::shared_ptr<pub::MonitoringCollectionData>> &collections)
{
	return (_stream_server != nullptr) ? _stream_server->GetMonitoringCollectionData(collections) : false;
}

bool SegmentPublisher::OnPlayListRequest(const std::shared_ptr<HttpClient> &client,
										 const ov::String &app_name, const ov::String &stream_name,
										 const ov::String &file_name,
										 ov::String &play_list)
{
	auto request = client->GetRequest();
	auto uri = request->GetUri();
	auto parsed_url = ov::Url::Parse(uri.CStr(), true);

	if (parsed_url == nullptr)
	{
		logte("Could not parse the url: %s", uri.CStr());
		client->GetResponse()->SetStatusCode(HttpStatusCode::BadRequest);

		// Returns true when the observer search can be ended.
		return true;
	}

	// These names are used for testing purposes
	// TODO(dimiden): Need to delete this code after testing
	std::shared_ptr<PlaylistRequestInfo> playlist_request_info;
	if (app_name.HasSuffix("_insecure") == false)
	{
		if (HandleSignedUrl(app_name, stream_name, client, parsed_url, playlist_request_info) == false)
		{
			client->GetResponse()->SetStatusCode(HttpStatusCode::Forbidden);

			// Returns true when the observer search can be ended.
			return true;
		}
	}

	auto stream = GetStreamAs<SegmentStream>(app_name, stream_name);
	if (stream == nullptr)
	{
		auto orchestrator = Orchestrator::GetInstance();

		// These names are used for testing purposes
		// TODO(dimiden): Need to delete this code after testing
		if (
			app_name.HasSuffix("#rtsp_live") || app_name.HasSuffix("#rtsp_playback") ||
			app_name.HasSuffix("#rtsp_live_insecure") || app_name.HasSuffix("#rtsp_playback_insecure"))
		{
			auto &query_map = parsed_url->QueryMap();

			auto rtsp_uri_item = query_map.find("rtspURI");

			if (rtsp_uri_item == query_map.end())
			{
				logte("There is no rtspURI parameter in the query string: %s", uri.CStr());

				logtd("Query map:");
				for ([[maybe_unused]] auto &query : query_map)
				{
					logtd("    %s = %s", query.first.CStr(), query.second.CStr());
				}

				client->GetResponse()->SetStatusCode(HttpStatusCode::BadRequest);

				// Returns true when the observer search can be ended.
				return true;
			}

			auto rtsp_uri = rtsp_uri_item->second;

			if (orchestrator->RequestPullStream(app_name, stream_name, rtsp_uri) == false)
			{
				logte("Could not request pull stream for URL: %s", rtsp_uri.CStr());
				client->GetResponse()->SetStatusCode(HttpStatusCode::NotAcceptable);

				// Returns true when the observer search can be ended.
				return true;
			}

			// Connection Request log
			// 2019-11-06 09:46:45.390 , RTSP.SS ,REQUEST,INFO,,,Live,rtsp://50.1.111.154:10915/1135/1/,220.103.225.254_44757_1573001205_389304_128855562
			stat_log(STAT_LOG_HLS_EDGE_REQUEST, "%s,%s,%s,%s,,,%s,%s,%s",
					 ov::Clock::Now().CStr(),
					 "HLS.SS",
					 "REQUEST",
					 "INFO",
					 app_name.CStr(),
					 rtsp_uri.CStr(),
					 playlist_request_info != nullptr ? playlist_request_info->GetSessionId().CStr() : client->GetRequest()->GetRemote()->GetRemoteAddress()->GetIpAddress().CStr());

			logti("URL %s is requested", rtsp_uri.CStr());

			stream = GetStreamAs<SegmentStream>(app_name, stream_name);
		}
		else
		{
			// If the stream does not exists, request to the provider
			if (orchestrator->RequestPullStream(app_name, stream_name) == false)
			{
				logte("Could not request pull stream for URL : %s/%s/%s", app_name.CStr(), stream_name.CStr(), file_name.CStr());
				client->GetResponse()->SetStatusCode(HttpStatusCode::NotAcceptable);

				// Returns true when the observer search can be ended.
				return true;
			}
			else
			{
				stream = GetStreamAs<SegmentStream>(app_name, stream_name);
			}
		}
	}

	if (stream == nullptr)
	{
		logtw("Could not get a playlist for %s [%p, %s/%s, %s]", GetPublisherName(), stream.get(), app_name.CStr(), stream_name.CStr(), file_name.CStr());

		// This means it need to query the next observer.
		return false;
	}

	if (stream->GetPlayList(play_list) == false)
	{
		logtw("Could not get a playlist for %s [%p, %s/%s, %s]", GetPublisherName(), stream.get(), app_name.CStr(), stream_name.CStr(), file_name.CStr());
		client->GetResponse()->SetStatusCode(HttpStatusCode::Accepted);
		// Returns true when the observer search can be ended.
		return true;
	}

	client->GetResponse()->SetStatusCode(HttpStatusCode::OK);
	return true;
}

bool SegmentPublisher::OnSegmentRequest(const std::shared_ptr<HttpClient> &client,
										const ov::String &app_name, const ov::String &stream_name,
										const ov::String &file_name,
										std::shared_ptr<SegmentData> &segment)
{
	auto stream = GetStreamAs<SegmentStream>(app_name, stream_name);

	if (stream != nullptr)
	{
		segment = stream->GetSegmentData(file_name);

		if (segment == nullptr)
		{
			logtw("Could not find a segment for %s [%s/%s, %s]", GetPublisherName(), app_name.CStr(), stream_name.CStr(), file_name.CStr());
			return false;
		}
		else if (segment->data == nullptr)
		{
			logtw("Could not obtain segment data from %s for [%p, %s/%s, %s]", GetPublisherName(), segment.get(), app_name.CStr(), stream_name.CStr(), file_name.CStr());
			return false;
		}
	}
	else
	{
		logtw("Could not find a stream for %s [%s/%s, %s]", GetPublisherName(), app_name.CStr(), stream_name.CStr(), file_name.CStr());
		return false;
	}

	// To manage sessions
	logti("Segment requested (%s/%s/%s) from %s : Segment number : %u Duration : %u", 
						app_name.CStr(), stream_name.CStr(), file_name.CStr(), 
						client->GetRequest()->GetRemote()->GetRemoteAddress()->ToString().CStr(),
						segment->sequence_number, segment->duration);

	auto request_info = SegmentRequestInfo(GetPublisherType(),
												*std::static_pointer_cast<info::Stream>(stream),
												client->GetRequest()->GetRemote()->GetRemoteAddress()->GetIpAddress(),
												segment->sequence_number,
												segment->duration);
	UpdateSegmentRequestInfo(request_info);

	return true;
}

bool SegmentPublisher::StartSessionTableManager()
{
	_run_thread = true;
	_worker_thread = std::thread(&SegmentPublisher::RequestTableUpdateThread, this);

	return true;
}

void SegmentPublisher::RequestTableUpdateThread()
{
	auto _last_logging_time = std::chrono::system_clock::now();

	while (_run_thread)
	{
		// This time, only log for HLS - later it will be changed
		if (GetPublisherType() == PublisherType::Hls)
		{
			// Concurrent user log
			std::chrono::system_clock::time_point current;
			uint32_t duration;

			current = std::chrono::system_clock::now();
			duration = std::chrono::duration_cast<std::chrono::seconds>(current - _last_logging_time).count();
			if (duration > 60)
			{
				// 2018-12-24 23:06:25.035,RTSP.SS,CONN_COUNT,INFO,,,[Live users], [Playback users]
				std::shared_ptr<info::Application> rtsp_live_app_info;
				std::shared_ptr<mon::ApplicationMetrics> rtsp_live_app_metrics;
				std::shared_ptr<info::Application> rtsp_play_app_info;
				std::shared_ptr<mon::ApplicationMetrics> rtsp_play_app_metrics;

				rtsp_live_app_metrics = nullptr;
				rtsp_play_app_metrics = nullptr;
				
				// This log only for the "default" host and the "rtsp_live"/"rtsp_playback" applications 
				rtsp_live_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_live")));
				if (rtsp_live_app_info != nullptr)
				{
					rtsp_live_app_metrics = ApplicationMetrics(*rtsp_live_app_info);
				}
				rtsp_play_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_playback")));
				if (rtsp_play_app_info != nullptr)
				{
					rtsp_play_app_metrics = ApplicationMetrics(*rtsp_play_app_info);
				}

				stat_log(STAT_LOG_HLS_EDGE_VIEWERS, "%s,%s,%s,%s,,,%u,%u",
						ov::Clock::Now().CStr(),
						"HLS.SS",
						"CONN_COUNT",
						"INFO",
						rtsp_live_app_metrics != nullptr ? rtsp_live_app_metrics->GetTotalConnections() : 0,
						rtsp_play_app_metrics != nullptr ? rtsp_play_app_metrics->GetTotalConnections() : 0);

				_last_logging_time = std::chrono::system_clock::now();
			}
		}

		// Remove a quite old session request info
		// Only HLS/DASH/CMAF publishers have some items.
		std::unique_lock<std::recursive_mutex> segment_table_lock(_segment_request_table_lock);

		for (auto item = _segment_request_table.begin(); item != _segment_request_table.end();)
		{
			auto request_info = item->second;
			if (request_info->IsExpiredRequest())
			{
				// remove and report
				auto stream_metrics = StreamMetrics(request_info->GetStreamInfo());
				if (stream_metrics != nullptr)
				{
					stream_metrics->OnSessionDisconnected(request_info->GetPublisherType());

					auto playlist_request_info = GetSessionRequestInfoBySegmentRequestInfo(*request_info);
					stat_log(STAT_LOG_HLS_EDGE_SESSION, "%s,%s,%s,%s,,,%s,%s,%s",
							 ov::Clock::Now().CStr(),
							 "HLS.SS",
							 "SESSION",
							 "INFO",
							 "deleteClientSession",
							 request_info->GetStreamInfo().GetName().CStr(),
							 playlist_request_info != nullptr ? playlist_request_info->GetSessionId().CStr() : request_info->GetIpAddress().CStr());

					std::shared_ptr<info::Application> rtsp_live_app_info;
					std::shared_ptr<mon::ApplicationMetrics> rtsp_live_app_metrics;
					std::shared_ptr<info::Application> rtsp_play_app_info;
					std::shared_ptr<mon::ApplicationMetrics> rtsp_play_app_metrics;

					rtsp_live_app_metrics = nullptr;
					rtsp_play_app_metrics = nullptr;

					// This log only for the "default" host and the "rtsp_live"/"rtsp_playback" applications 
					rtsp_live_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_live")));
					if (rtsp_live_app_info != nullptr)
					{
						rtsp_live_app_metrics = ApplicationMetrics(*rtsp_live_app_info);
					}
					rtsp_play_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_playback")));
					if (rtsp_play_app_info != nullptr)
					{
						rtsp_play_app_metrics = ApplicationMetrics(*rtsp_play_app_info);
					}

					stat_log(STAT_LOG_HLS_EDGE_SESSION, "%s,%s,%s,%s,,,%s:%d,%s:%d,%s,%s",
							ov::Clock::Now().CStr(),
							"HLS.SS",
							"SESSION",
							"INFO",
							"Live",
							rtsp_live_app_metrics != nullptr ? rtsp_live_app_metrics->GetTotalConnections() : 0,
							"Playback",
							rtsp_play_app_metrics != nullptr ? rtsp_play_app_metrics->GetTotalConnections() : 0,
							request_info->GetStreamInfo().GetName().CStr(),
							playlist_request_info != nullptr ? playlist_request_info->GetSessionId().CStr() : request_info->GetIpAddress().CStr());
					
				}

				item = _segment_request_table.erase(item);
			}
			else
			{
				++item;
			}
		}

		segment_table_lock.unlock();

		std::unique_lock<std::recursive_mutex> playlist_table_lock(_playlist_request_table_lock);

		for (auto item = _playlist_request_table.begin(); item != _playlist_request_table.end();)
		{
			auto request_info = item->second;
			if (request_info->IsTooOld())
			{
				// remove and report
				logti("Remove the permission of the authorized session : %s/%s - %s - %s",
					  request_info->GetAppName().CStr(), request_info->GetStreamName().CStr(),
					  request_info->GetSessionId().CStr(), request_info->GetIpAddress().CStr());
				item = _playlist_request_table.erase(item);
			}
			else
			{
				++item;
			}
		}

		playlist_table_lock.unlock();

		sleep(3);
	}
}

const std::shared_ptr<PlaylistRequestInfo> SegmentPublisher::GetSessionRequestInfoBySegmentRequestInfo(const SegmentRequestInfo &info)
{
	for (const auto &item : _playlist_request_table)
	{
		auto &request_info = item.second;
		if (request_info->GetPublisherType() == info.GetPublisherType() &&
			request_info->GetIpAddress() == info.GetIpAddress() &&
			request_info->GetStreamName() == info.GetStreamInfo().GetName() &&
			request_info->GetAppName() == info.GetStreamInfo().GetApplicationInfo().GetName())
		{
			return request_info;
		}
	}

	return nullptr;
}

void SegmentPublisher::UpdatePlaylistRequestInfo(const std::shared_ptr<PlaylistRequestInfo> &info)
{
	std::unique_lock<std::recursive_mutex> table_lock(_playlist_request_table_lock);
	// Change info to new one
	//TODO(Getroot): In the future, by comparing the existing data with the creation time, it can be identified as normal.
	if (_playlist_request_table.count(info->GetSessionId().CStr()) == 0)
	{
		logti("Authorize session : %s/%s - %s - %s", info->GetAppName().CStr(), info->GetStreamName().CStr(),
			  info->GetSessionId().CStr(), info->GetIpAddress().CStr());
	}

	_playlist_request_table[info->GetSessionId().CStr()] = info;
}

bool SegmentPublisher::IsAuthorizedSession(const PlaylistRequestInfo &info)
{
	auto select_count = _playlist_request_table.count(info.GetSessionId().CStr());
	if (select_count > 0)
	{
		auto item = _playlist_request_table.at(info.GetSessionId().CStr());
		if (item->IsRequestFromSameUser(info))
		{
			return true;
		}
	}
	return false;
}

void SegmentPublisher::UpdateSegmentRequestInfo(SegmentRequestInfo &info)
{
	bool new_session = true;
	std::unique_lock<std::recursive_mutex> table_lock(_segment_request_table_lock);
	
	auto select_count = _segment_request_table.count(info.GetIpAddress().CStr());
	if (select_count > 0)
	{
		// select * where IP=info.ip from _session_table
		auto it = _segment_request_table.equal_range(info.GetIpAddress().CStr());
		for (auto itr = it.first; itr != it.second;)
		{
			auto item = itr->second;

			if (item->IsNextRequest(info))
			{
				auto count = item->GetCount();
				info.SetCount(count++);

				itr = _segment_request_table.erase(itr);
				new_session = false;
				break;
			}
			else
			{
				++itr;
			}
		}
	}

	_segment_request_table.insert(std::pair<std::string, std::shared_ptr<SegmentRequestInfo>>(info.GetIpAddress().CStr(), std::make_shared<SegmentRequestInfo>(info)));

	table_lock.unlock();

	// It is a new viewer!
	if (new_session)
	{
		// New Session!!!
		auto stream_metrics = StreamMetrics(info.GetStreamInfo());
		if (stream_metrics != nullptr)
		{
			stream_metrics->OnSessionConnected(info.GetPublisherType());

			auto playlist_request_info = GetSessionRequestInfoBySegmentRequestInfo(info);
			stat_log(STAT_LOG_HLS_EDGE_SESSION, "%s,%s,%s,%s,,,%s,%s,%s",
					 ov::Clock::Now().CStr(),
					 "HLS.SS",
					 "SESSION",
					 "INFO",
					 "createClientSession",
					 info.GetStreamInfo().GetName().CStr(),
					 playlist_request_info != nullptr ? playlist_request_info->GetSessionId().CStr() : info.GetIpAddress().CStr());

			std::shared_ptr<info::Application> rtsp_live_app_info;
			std::shared_ptr<mon::ApplicationMetrics> rtsp_live_app_metrics;
			std::shared_ptr<info::Application> rtsp_play_app_info;
			std::shared_ptr<mon::ApplicationMetrics> rtsp_play_app_metrics;

			rtsp_live_app_metrics = nullptr;
			rtsp_play_app_metrics = nullptr;

			// This log only for the "default" host and the "rtsp_live"/"rtsp_playback" applications 
			rtsp_live_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_live")));
			if (rtsp_live_app_info != nullptr)
			{
				rtsp_live_app_metrics = ApplicationMetrics(*rtsp_live_app_info);
			}
			rtsp_play_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_playback")));
			if (rtsp_play_app_info != nullptr)
			{
				rtsp_play_app_metrics = ApplicationMetrics(*rtsp_play_app_info);
			}

			stat_log(STAT_LOG_HLS_EDGE_SESSION, "%s,%s,%s,%s,,,%s:%d,%s:%d,%s,%s",
					ov::Clock::Now().CStr(),
					"HLS.SS",
					"SESSION",
					"INFO",
					"Live",
					rtsp_live_app_metrics != nullptr ? rtsp_live_app_metrics->GetTotalConnections() : 0,
					"Playback",
					rtsp_play_app_metrics != nullptr ? rtsp_play_app_metrics->GetTotalConnections() : 0,
					info.GetStreamInfo().GetName().CStr(),
					playlist_request_info != nullptr ? playlist_request_info->GetSessionId().CStr() : info.GetIpAddress().CStr());
			
		}
	}
}

bool SegmentPublisher::HandleSignedUrl(const ov::String &app_name, const ov::String &stream_name,
									   const std::shared_ptr<HttpClient> &client, const std::shared_ptr<const ov::Url> &request_url,
									   std::shared_ptr<PlaylistRequestInfo> &request_info)
{
	auto orchestrator = Orchestrator::GetInstance();
	auto &server_config = GetServerConfig();
	auto vhost_name = orchestrator->GetVhostNameFromDomain(request_url->Domain());

	if (vhost_name.IsEmpty())
	{
		logtw("Could not resolve the domain: %s", request_url->Domain().CStr());
		return false;
	}

	// TODO(Dimiden) : Modify blow codes
	// GetVirtualHostByName is deprecated so blow codes are insane, later it will be modified.
	auto vhost_list = server_config.GetVirtualHostList();
	for (const auto &vhost_item : vhost_list)
	{
		if (vhost_item.GetName() != vhost_name)
		{
			continue;
		}
		// Found

		// Handle Signed URL if needed
		auto &signed_url_config = vhost_item.GetSignedUrl();
		if (!signed_url_config.IsParsed() || signed_url_config.GetCryptoKey().IsEmpty())
		{
			// The vhost doesn't use the signed url feature.
			return true;
		}

		auto request = client->GetRequest();
		auto remote_address = request->GetRemote()->GetRemoteAddress();
		if (remote_address == nullptr)
		{
			OV_ASSERT2(false);
			logtc("Invalid remote address found");
			return false;
		}

		auto &query_map = request_url->QueryMap();

		// Load config (crypto key, query string key)
		auto crypto_key = signed_url_config.GetCryptoKey();
		auto query_string_key = signed_url_config.GetQueryStringKey();

		// Find a encoded string in query string
		auto item = query_map.find(query_string_key);
		if (item == query_map.end())
		{
			logtw("Could not find key %s in query string in URL: %s", query_string_key.CStr(), request_url->Source().CStr());
			return false;
		}

		// Find a rtspURI in query string
		auto rtsp_item = query_map.find("rtspURI");
		if (rtsp_item == query_map.end())
		{
			logte("Could not find rtspURI in query string");
			return false;
		}

		// Decoding and parsing
		auto signed_url = SignedUrl::Load(SignedUrlType::Type0, crypto_key, item->second);
		if (signed_url == nullptr)
		{
			logte("Could not obtain decrypted information of the signed url: %s, key: %s, value: %s", request_url->Source().CStr(), query_string_key.CStr(), item->second.CStr());
			return false;
		}

		auto url_to_compare = request_url->ToUrlString(false);
		url_to_compare.AppendFormat("?rtspURI=%s", ov::Url::Encode(rtsp_item->second).CStr());

		std::vector<ov::String> messages;
		bool result = true;
		auto now = signed_url->GetNowMS();
		request_info = std::make_shared<PlaylistRequestInfo>(GetPublisherType(),
															 app_name, stream_name,
															 remote_address->GetIpAddress(),
															 signed_url->GetSessionID());

		if (signed_url->IsTokenExpired())
		{
			// Even if the token has expired, it can still be passed if the session ID had been approved.
			if (!IsAuthorizedSession(*request_info))
			{
				messages.push_back(ov::String::FormatString("Token is expired: %lld (Now: %lld)", signed_url->GetTokenExpiredTime(), now));
				result = false;
			}
		}

		if (signed_url->IsStreamExpired())
		{
			messages.push_back(ov::String::FormatString("Stream is expired: %lld (Now: %lld)", signed_url->GetStreamExpiredTime(), now));
			result = false;
		}

		if (signed_url->IsAllowedClient(*remote_address) == false)
		{
			messages.push_back(ov::String::FormatString("Not allowed: %s (Expected: %s)",
														remote_address->ToString().CStr(),
														signed_url->GetClientIP().CStr()));
			result = false;
		}

		if (signed_url->GetUrl().UpperCaseString() != url_to_compare.UpperCaseString())
		{
			messages.push_back(ov::String::FormatString("Invalid URL: %s (Expected: %s)",
														signed_url->GetUrl().CStr(), url_to_compare.CStr()));
			result = false;
		}

		if (result == false)
		{
			logtw("Failed to authenticate client %s\nReason:\n    - %s",
				  request->GetRemote()->ToString().CStr(),
				  ov::String::Join(messages, "\n    - ").CStr());

			return false;
		}

		// Update the authorized session info
		UpdatePlaylistRequestInfo(request_info);

		return true;
	}

	return false;
}
