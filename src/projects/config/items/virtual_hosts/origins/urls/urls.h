//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2020 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

namespace cfg
{
	struct Urls : public Item
	{
		CFG_DECLARE_REF_GETTER_OF(GetUrlList, _url_list)

	protected:
		void MakeParseList() override
		{
			RegisterValue("Url", &_url_list);
		}

		std::vector<Url> _url_list;
	};
}  // namespace cfg