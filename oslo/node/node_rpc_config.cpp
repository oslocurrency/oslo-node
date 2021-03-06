#include <oslo/lib/config.hpp>
#include <oslo/lib/jsonconfig.hpp>
#include <oslo/lib/tomlconfig.hpp>
#include <oslo/node/node_rpc_config.hpp>

oslo::error oslo::node_rpc_config::serialize_json (oslo::jsonconfig & json) const
{
	json.put ("version", json_version ());
	json.put ("enable_sign_hash", enable_sign_hash);

	oslo::jsonconfig child_process_l;
	child_process_l.put ("enable", child_process.enable);
	child_process_l.put ("rpc_path", child_process.rpc_path);
	json.put_child ("child_process", child_process_l);
	return json.get_error ();
}

oslo::error oslo::node_rpc_config::serialize_toml (oslo::tomlconfig & toml) const
{
	toml.put ("enable_sign_hash", enable_sign_hash, "Allow or disallow signing of hashes.\ntype:bool");

	oslo::tomlconfig child_process_l;
	child_process_l.put ("enable", child_process.enable, "Enable or disable RPC child process. If false, an in-process RPC server is used.\ntype:bool");
	child_process_l.put ("rpc_path", child_process.rpc_path, "Path to the oslo_rpc executable. Must be set if child process is enabled.\ntype:string,path");
	toml.put_child ("child_process", child_process_l);
	return toml.get_error ();
}

oslo::error oslo::node_rpc_config::deserialize_toml (oslo::tomlconfig & toml)
{
	toml.get_optional ("enable_sign_hash", enable_sign_hash);
	toml.get_optional<bool> ("enable_sign_hash", enable_sign_hash);

	auto child_process_l (toml.get_optional_child ("child_process"));
	if (child_process_l)
	{
		child_process_l->get_optional<bool> ("enable", child_process.enable);
		child_process_l->get_optional<std::string> ("rpc_path", child_process.rpc_path);
	}

	return toml.get_error ();
}

oslo::error oslo::node_rpc_config::deserialize_json (bool & upgraded_a, oslo::jsonconfig & json, boost::filesystem::path const & data_path)
{
	auto version_l (json.get_optional<unsigned> ("version"));
	if (!version_l)
	{
		json.erase ("frontier_request_limit");
		json.erase ("chain_request_limit");

		// Don't migrate enable_sign_hash as this is not needed by the external rpc process, but first save it.
		json.get_optional ("enable_sign_hash", enable_sign_hash, false);

		json.erase ("enable_sign_hash");
		json.erase ("max_work_generate_difficulty");

		migrate (json, data_path);

		json.put ("enable_sign_hash", enable_sign_hash);

		// Remove options no longer needed after migration
		json.erase ("enable_control");
		json.erase ("address");
		json.erase ("port");
		json.erase ("max_json_depth");
		json.erase ("max_request_size");

		version_l = 1;
		json.put ("version", *version_l);

		oslo::jsonconfig child_process_l;
		child_process_l.put ("enable", child_process.enable);
		child_process_l.put ("rpc_path", child_process.rpc_path);
		json.put_child ("child_process", child_process_l);
		upgraded_a = true;
	}

	json.get_optional<bool> ("enable_sign_hash", enable_sign_hash);

	auto child_process_l (json.get_optional_child ("child_process"));
	if (child_process_l)
	{
		child_process_l->get_optional<bool> ("enable", child_process.enable);
		child_process_l->get_optional<std::string> ("rpc_path", child_process.rpc_path);
	}

	return json.get_error ();
}

void oslo::node_rpc_config::migrate (oslo::jsonconfig & json, boost::filesystem::path const & data_path)
{
	oslo::jsonconfig rpc_json;
	auto rpc_config_path = oslo::get_rpc_config_path (data_path);
	auto rpc_error (rpc_json.read (rpc_config_path));
	if (rpc_error || rpc_json.empty ())
	{
		// Migrate RPC info across
		json.write (rpc_config_path);
	}
}
