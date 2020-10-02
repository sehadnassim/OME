//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "http_server.h"

#include <modules/physical_port/physical_port_manager.h>

#include "http_private.h"

HttpServer::~HttpServer()
{
	// PhysicalPort should be stopped before release HttpServer
	OV_ASSERT2(_physical_port == nullptr);
}

bool HttpServer::Start(const ov::SocketAddress &address)
{
	if (_physical_port != nullptr)
	{
		logtw("Server is running");
		return false;
	}

	_physical_port = PhysicalPortManager::Instance()->CreatePort(ov::SocketType::Tcp, address);

	if (_physical_port != nullptr)
	{
		return _physical_port->AddObserver(this);
	}

	return _physical_port != nullptr;
}

bool HttpServer::Stop()
{
	//TODO(Dimiden): Check possibility that _physical_port can be deleted from other http publisher.
	if (_physical_port != nullptr)
	{
		_physical_port->RemoveObserver(this);
		PhysicalPortManager::Instance()->DeletePort(_physical_port);
		_physical_port = nullptr;
	}

	// client들 정리
	_client_list_mutex.lock();
	auto client_list = std::move(_client_list);
	_client_list_mutex.unlock();

	for (auto &client : client_list)
	{
		client.second->GetResponse()->Close();
	}

	_interceptor_list.clear();

	return true;
}

ssize_t HttpServer::TryParseHeader(const std::shared_ptr<HttpClient> &client, const std::shared_ptr<const ov::Data> &data)
{
	auto request = client->GetRequest();

	OV_ASSERT2(request->ParseStatus() == HttpStatusCode::PartialContent);

	// 파싱이 필요한 상태 - ProcessData()를 호출하여 파싱 시도
	ssize_t processed_length = request->ProcessData(data);

	switch (request->ParseStatus())
	{
		case HttpStatusCode::OK:
			// 파싱이 이제 막 완료된 상태. 즉, 파싱이 완료된 후 최초 1번만 여기로 진입함
			break;

		case HttpStatusCode::PartialContent:
			// 데이터 더 필요 - 이 상태에서는 반드시 모든 데이터를 소진했어야 함
			OV_ASSERT2((processed_length >= 0LL) && (static_cast<size_t>(processed_length) == data->GetLength()));
			break;

		default:
			// 파싱 도중 오류 발생
			OV_ASSERT2(processed_length == -1L);
			break;
	}

	return processed_length;
}

std::shared_ptr<HttpClient> HttpServer::FindClient(const std::shared_ptr<ov::Socket> &remote)
{
	std::shared_lock<std::shared_mutex> guard(_client_list_mutex);

	auto item = _client_list.find(remote.get());

	if (item != _client_list.end())
	{
		return item->second;
	}

	return nullptr;
}

void HttpServer::ProcessData(const std::shared_ptr<HttpClient> &client, const std::shared_ptr<const ov::Data> &data)
{
	if (client != nullptr)
	{
		std::shared_ptr<HttpRequest> request = client->GetRequest();
		std::shared_ptr<HttpResponse> response = client->GetResponse();

		bool need_to_disconnect = false;

		// header parse (temp)
		// - http1.0 Connection default : close
		// - http1.1 Connection default : keep-alive
		if (request->ParseStatus() == HttpStatusCode::OK && request->GetRequestInterceptor() != nullptr)
		{
			if ((request->GetHttpVersionAsNumber() > 1.0 && request->GetHeader("Connection", "keep-alive") == "keep-alive") ||
				(request->GetHttpVersionAsNumber() <= 1.0 && request->GetHeader("Connection", "close") == "keep-alive"))
			{
				request->InitParseInfo();
			}
		}

		switch (request->ParseStatus())
		{
			case HttpStatusCode::OK: {
				auto interceptor = request->GetRequestInterceptor();

				if (interceptor != nullptr)
				{
					// If the request is parsed, bypass to the interceptor
					need_to_disconnect = (interceptor->OnHttpData(client, data) == HttpInterceptorResult::Disconnect);
				}
				else
				{
					OV_ASSERT2(false);
					need_to_disconnect = true;
				}

				break;
			}

			case HttpStatusCode::PartialContent: {
				// Need to parse HTTP header
				ssize_t processed_length = TryParseHeader(client, data);

				if (processed_length >= 0)
				{
					if (request->ParseStatus() == HttpStatusCode::OK)
					{
						// Parsing is completed

						// Find interceptor for the request
						{
							std::shared_lock<std::shared_mutex> guard(_interceptor_list_mutex);

							for (auto &interceptor : _interceptor_list)
							{
								if (interceptor->IsInterceptorForRequest(client))
								{
									request->SetRequestInterceptor(interceptor);
									break;
								}
							}
						}

						auto interceptor = request->GetRequestInterceptor();

						if (interceptor == nullptr)
						{
							response->SetStatusCode(HttpStatusCode::InternalServerError);

							need_to_disconnect = true;
							OV_ASSERT2(false);
						}

						auto remote = request->GetRemote();

						if (remote != nullptr)
						{
							logti("Client(%s) is requested uri: [%s]", remote->GetRemoteAddress()->ToString().CStr(), request->GetUri().CStr());
						}

						need_to_disconnect = need_to_disconnect || (interceptor->OnHttpPrepare(client) == HttpInterceptorResult::Disconnect);
						need_to_disconnect = need_to_disconnect || (interceptor->OnHttpData(client, data->Subdata(processed_length)) == HttpInterceptorResult::Disconnect);
					}
					else if (request->ParseStatus() == HttpStatusCode::PartialContent)
					{
						// Need more data
					}
				}
				else
				{
					// An error occurred with the request
					request->GetRequestInterceptor()->OnHttpError(client, HttpStatusCode::BadRequest);
					need_to_disconnect = true;
				}

				break;
			}

			default:
				// 이전에 parse 할 때 오류가 발생했다면 response한 뒤 close() 했으므로, 정상적인 상황이라면 여기에 진입하면 안됨
				logte("Invalid parse status: %d", request->ParseStatus());
				OV_ASSERT2(false);
				need_to_disconnect = true;
				break;
		}

		if (need_to_disconnect)
		{
			// 연결을 종료해야 함
			response->Response();
			response->Close();
		}
	}
}

std::shared_ptr<HttpClient> HttpServer::ProcessConnect(const std::shared_ptr<ov::Socket> &remote)
{
	logti("Client(%s) is connected on %s", remote->GetRemoteAddress()->ToString().CStr(), _physical_port->GetAddress().ToString().CStr());

	auto client_socket = std::dynamic_pointer_cast<ov::ClientSocket>(remote);

	if (client_socket == nullptr)
	{
		OV_ASSERT2(false);
		return nullptr;
	}

	auto request = std::make_shared<HttpRequest>(client_socket, _default_interceptor);
	auto response = std::make_shared<HttpResponse>(client_socket);

	if (response != nullptr)
	{
		// Set default headers
		response->SetHeader("Server", "OvenMediaEngine");
		response->SetHeader("Content-Type", "text/html");
	}

	std::lock_guard<std::shared_mutex> guard(_client_list_mutex);

	auto http_client = std::make_shared<HttpClient>(GetSharedPtr(), request, response);

	_client_list[remote.get()] = http_client;

	return std::move(http_client);
}

void HttpServer::OnConnected(const std::shared_ptr<ov::Socket> &remote)
{
	ProcessConnect(remote);
}

void HttpServer::OnDataReceived(const std::shared_ptr<ov::Socket> &remote, const ov::SocketAddress &address, const std::shared_ptr<const ov::Data> &data)
{
	auto client = FindClient(remote);

	if (client == nullptr)
	{
		// This can be called in situations where the client closes the connection from the server at the same time as the data is sent
		return;
	}

	ProcessData(client, data);
}

void HttpServer::OnDisconnected(const std::shared_ptr<ov::Socket> &remote, PhysicalPortDisconnectReason reason, const std::shared_ptr<const ov::Error> &error)
{
	std::lock_guard<std::shared_mutex> guard(_client_list_mutex);

	auto client_iterator = _client_list.find(remote.get());

	if (client_iterator != _client_list.end())
	{
		auto &client = client_iterator->second;
		auto request = client->GetRequest();
		auto response = client->GetResponse();

		if (reason == PhysicalPortDisconnectReason::Disconnect)
		{
			logti("The HTTP client(%s) has been disconnected from %s (%d)",
				  remote->GetRemoteAddress()->ToString().CStr(), _physical_port->GetAddress().ToString().CStr(), response->GetStatusCode());
		}
		else
		{
			logti("The HTTP client(%s) is disconnected from %s (%d)",
				  remote->GetRemoteAddress()->ToString().CStr(), _physical_port->GetAddress().ToString().CStr(), response->GetStatusCode());
		}

		auto interceptor = request->GetRequestInterceptor();

		if (interceptor != nullptr)
		{
			interceptor->OnHttpClosed(client);
		}
		else
		{
			logtw("Interceptor does not exists for HTTP client %p", client.get());
		}

		_client_list.erase(client_iterator);
	}
	else
	{
		logte("Could not find client %s from list", remote->ToString().CStr());
		OV_ASSERT2(false);
	}
}

bool HttpServer::AddInterceptor(const std::shared_ptr<HttpRequestInterceptor> &interceptor)
{
	// 기존에 등록된 processor가 있는지 확인
	std::lock_guard<std::shared_mutex> guard(_interceptor_list_mutex);

	auto item = std::find_if(_interceptor_list.begin(), _interceptor_list.end(), [&](std::shared_ptr<HttpRequestInterceptor> const &value) -> bool {
		return value == interceptor;
	});

	if (item != _interceptor_list.end())
	{
		// 기존에 등록되어 있음
		logtw("%p is already observer", interceptor.get());
		return false;
	}

	_interceptor_list.push_back(interceptor);
	return true;
}

bool HttpServer::RemoveInterceptor(const std::shared_ptr<HttpRequestInterceptor> &interceptor)
{
	std::lock_guard<std::shared_mutex> guard(_interceptor_list_mutex);

	// Find interceptor in the list
	auto item = std::find_if(_interceptor_list.begin(), _interceptor_list.end(), [&](std::shared_ptr<HttpRequestInterceptor> const &value) -> bool {
		return value == interceptor;
	});

	if (item == _interceptor_list.end())
	{
		// interceptor does not exists in the list
		logtw("%p is not found.", interceptor.get());
		return false;
	}

	_interceptor_list.erase(item);
	return true;
}

ov::Socket *HttpServer::FindClient(ClientIterator iterator)
{
	std::shared_lock<std::shared_mutex> guard(_client_list_mutex);

	for (auto &client : _client_list)
	{
		if (iterator(client.second))
		{
			return client.first;
		}
	}

	return nullptr;
}

bool HttpServer::DisconnectIf(ClientIterator iterator)
{
	std::vector<std::shared_ptr<HttpClient>> temp_list;

	{
		std::shared_lock<std::shared_mutex> guard(_client_list_mutex);

		for (auto client_iterator : _client_list)
		{
			auto &client = client_iterator.second;

			if (iterator(client))
			{
				temp_list.push_back(client);
			}
		};
	}

	for (auto client_iterator : temp_list)
	{
		client_iterator->GetResponse()->Close();
	}

	return true;
}
