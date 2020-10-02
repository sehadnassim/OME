//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "./json.h"
#include "./ovlibrary_private.h"

namespace ov
{
	ov::String Json::Stringify(const JsonObject &object)
	{
		return Stringify(object._value);
	}

	ov::String Json::Stringify(const ::Json::Value &value)
	{
		::Json::StreamWriterBuilder builder;
		std::unique_ptr<::Json::StreamWriter> const writer(builder.newStreamWriter());

		std::ostringstream stream;

		if(writer->write(value, &stream) == 0)
		{
			return String(stream.str().c_str());
		}

		return "";
	}

	JsonObject Json::Parse(const ov::String &str)
	{
		JsonObject object;

		auto error = object.Parse(str);

		if(error == nullptr)
		{
			return object;
		}

		logtw("Could not parse: %s", error->ToString().CStr());

		return JsonObject::NullObject();
	}

	JsonObject Json::Parse(const std::shared_ptr<const Data> &data)
	{
		JsonObject object;

		auto error = object.Parse(data);

		if(error == nullptr)
		{
			return object;
		}

		logtw("Could not parse: %s", error->ToString().CStr());

		return JsonObject::NullObject();
	}
}
