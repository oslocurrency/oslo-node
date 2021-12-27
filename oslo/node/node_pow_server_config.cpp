#include <oslo/lib/tomlconfig.hpp>
#include <oslo/node/node_pow_server_config.hpp>

oslo::error oslo::node_pow_server_config::serialize_toml (oslo::tomlconfig & toml) const
{
	toml.put ("enable", enable, "Value is currently not in use. Enable or disable starting Oslo PoW Server as a child process.\ntype:bool");
	toml.put ("oslo_pow_server_path", pow_server_path, "Value is currently not in use. Path to the oslo_pow_server executable.\ntype:string,path");
	return toml.get_error ();
}

oslo::error oslo::node_pow_server_config::deserialize_toml (oslo::tomlconfig & toml)
{
	toml.get_optional<bool> ("enable", enable);
	toml.get_optional<std::string> ("oslo_pow_server_path", pow_server_path);

	return toml.get_error ();
}
