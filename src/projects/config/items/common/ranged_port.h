//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "./port.h"

namespace cfg
{
	struct RangedPort : public Port<std::vector<int>>
	{
	public:
		RangedPort() = default;
		
		explicit RangedPort(const char *port)
			: Port(port)
		{
		}

		CFG_DECLARE_REF_GETTER_OF(GetPortList, _port_value);

	protected:
		bool AddPorts(const ov::String &ports)
		{
			// ports == "40000-40001" or "40000"
			auto range = ports.Split("-");

			switch (range.size())
			{
				case 1: {
					// Single port
					int port = ov::Converter::ToInt32(range[0]);

					if (IsValidPort(port) == false)
					{
						// Invalid port
						return false;
					}

					_port_value.push_back(port);

					break;
				}

				case 2: {
					// Port range
					int start_port = ov::Converter::ToInt32(range[0]);
					int end_port = ov::Converter::ToInt32(range[1]);

					if (((IsValidPort(start_port) == false) || (IsValidPort(end_port) == false)) ||
						(start_port > end_port))
					{
						// Invalid port range
						return false;
					}

					for (int port = start_port; port <= end_port; port++)
					{
						_port_value.push_back(port);
					}

					break;
				}

				default:
					// Invalid port expression
					return false;
			}

			return true;
		}

		void MakeParseList() override
		{
			RegisterValue<ValueType::Text>(nullptr, &_port, nullptr, [this]() -> bool {
				// _port == "40000-40001,40004,40005/udp"
				_socket_type = ov::SocketType::Unknown;
				_port_value.clear();

				auto tokens = _port.Trim().Split("/");

				// Default: UDP
				_socket_type = (tokens.size() != 2) ? ov::SocketType::Udp : GetSocketType(tokens[1]);

				// Parse port list
				auto ports = tokens[0].Trim().Split(",");

				for (auto &port_item : ports)
				{
					if (AddPorts(port_item.Trim()) == false)
					{
						return false;
					}
				}

				return _socket_type != ov::SocketType::Unknown;
			});
		}
	};
}  // namespace cfg