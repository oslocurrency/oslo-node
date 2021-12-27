#pragma once

#include <oslo/lib/config.hpp>
#include <oslo/lib/errors.hpp>

namespace oslo
{
class jsonconfig;
class tomlconfig;
namespace websocket
{
	/** websocket configuration */
	class config final
	{
	public:
		config ();
		oslo::error deserialize_json (oslo::jsonconfig & json_a);
		oslo::error serialize_json (oslo::jsonconfig & json) const;
		oslo::error deserialize_toml (oslo::tomlconfig & toml_a);
		oslo::error serialize_toml (oslo::tomlconfig & toml) const;
		oslo::network_constants network_constants;
		bool enabled{ false };
		uint16_t port;
		std::string address;
	};
}
}
