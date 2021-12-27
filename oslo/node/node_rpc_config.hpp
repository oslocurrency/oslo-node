#pragma once

#include <oslo/lib/rpcconfig.hpp>

#include <string>

namespace boost
{
namespace filesystem
{
	class path;
}
}

namespace oslo
{
class tomlconfig;
class rpc_child_process_config final
{
public:
	bool enable{ false };
	std::string rpc_path{ get_default_rpc_filepath () };
};

class node_rpc_config final
{
public:
	oslo::error serialize_json (oslo::jsonconfig &) const;
	oslo::error deserialize_json (bool & upgraded_a, oslo::jsonconfig &, boost::filesystem::path const & data_path);
	oslo::error serialize_toml (oslo::tomlconfig & toml) const;
	oslo::error deserialize_toml (oslo::tomlconfig & toml);

	bool enable_sign_hash{ false };
	oslo::rpc_child_process_config child_process;
	static unsigned json_version ()
	{
		return 1;
	}

private:
	void migrate (oslo::jsonconfig & json, boost::filesystem::path const & data_path);
};
}
