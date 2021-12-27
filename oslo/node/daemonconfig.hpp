#pragma once

#include <oslo/lib/errors.hpp>
#include <oslo/node/node_pow_server_config.hpp>
#include <oslo/node/node_rpc_config.hpp>
#include <oslo/node/nodeconfig.hpp>
#include <oslo/node/openclconfig.hpp>

#include <vector>

namespace oslo
{
class jsonconfig;
class tomlconfig;
class daemon_config
{
public:
	daemon_config () = default;
	daemon_config (boost::filesystem::path const & data_path);
	oslo::error deserialize_json (bool &, oslo::jsonconfig &);
	oslo::error serialize_json (oslo::jsonconfig &);
	oslo::error deserialize_toml (oslo::tomlconfig &);
	oslo::error serialize_toml (oslo::tomlconfig &);
	bool rpc_enable{ false };
	oslo::node_rpc_config rpc;
	oslo::node_config node;
	bool opencl_enable{ false };
	oslo::opencl_config opencl;
	oslo::node_pow_server_config pow_server;
	boost::filesystem::path data_path;
	unsigned json_version () const
	{
		return 2;
	}
};

oslo::error read_node_config_toml (boost::filesystem::path const &, oslo::daemon_config & config_a, std::vector<std::string> const & config_overrides = std::vector<std::string> ());
oslo::error read_and_update_daemon_config (boost::filesystem::path const &, oslo::daemon_config & config_a, oslo::jsonconfig & json_a);
}
