#include <oslo/lib/config.hpp>
#include <oslo/lib/jsonconfig.hpp>
#include <oslo/lib/tomlconfig.hpp>
#include <oslo/lib/walletconfig.hpp>
#include <oslo/node/daemonconfig.hpp>

#include <sstream>
#include <vector>

oslo::daemon_config::daemon_config (boost::filesystem::path const & data_path_a) :
data_path (data_path_a)
{
}

oslo::error oslo::daemon_config::serialize_toml (oslo::tomlconfig & toml)
{
	oslo::tomlconfig rpc_l;
	rpc.serialize_toml (rpc_l);
	rpc_l.doc ("enable", "Enable or disable RPC\ntype:bool");
	rpc_l.put ("enable", rpc_enable);
	toml.put_child ("rpc", rpc_l);

	oslo::tomlconfig node_l;
	node.serialize_toml (node_l);
	oslo::tomlconfig node (node_l);
	toml.put_child ("node", node);

	oslo::tomlconfig opencl_l;
	opencl.serialize_toml (opencl_l);
	opencl_l.doc ("enable", "Enable or disable OpenCL work generation\ntype:bool");
	opencl_l.put ("enable", opencl_enable);
	toml.put_child ("opencl", opencl_l);

	oslo::tomlconfig pow_server_l;
	pow_server.serialize_toml (pow_server_l);
	oslo::tomlconfig pow_server (pow_server_l);
	toml.put_child ("oslo_pow_server", pow_server);

	return toml.get_error ();
}

oslo::error oslo::daemon_config::deserialize_toml (oslo::tomlconfig & toml)
{
	auto rpc_l (toml.get_optional_child ("rpc"));
	if (!toml.get_error () && rpc_l)
	{
		rpc_l->get_optional<bool> ("enable", rpc_enable);
		rpc.deserialize_toml (*rpc_l);
	}

	auto node_l (toml.get_optional_child ("node"));
	if (!toml.get_error () && node_l)
	{
		node.deserialize_toml (*node_l);
	}

	auto opencl_l (toml.get_optional_child ("opencl"));
	if (!toml.get_error () && opencl_l)
	{
		opencl_l->get_optional<bool> ("enable", opencl_enable);
		opencl.deserialize_toml (*opencl_l);
	}

	auto pow_l (toml.get_optional_child ("oslo_pow_server"));
	if (!toml.get_error () && pow_l)
	{
		pow_server.deserialize_toml (*pow_l);
	}

	return toml.get_error ();
}

oslo::error oslo::daemon_config::serialize_json (oslo::jsonconfig & json)
{
	json.put ("version", json_version ());
	json.put ("rpc_enable", rpc_enable);

	oslo::jsonconfig rpc_l;
	rpc.serialize_json (rpc_l);
	json.put_child ("rpc", rpc_l);

	oslo::jsonconfig node_l;
	node.serialize_json (node_l);
	oslo::jsonconfig node (node_l);
	json.put_child ("node", node);

	json.put ("opencl_enable", opencl_enable);
	oslo::jsonconfig opencl_l;
	opencl.serialize_json (opencl_l);
	json.put_child ("opencl", opencl_l);
	return json.get_error ();
}

oslo::error oslo::daemon_config::deserialize_json (bool & upgraded_a, oslo::jsonconfig & json)
{
	try
	{
		if (!json.empty ())
		{
			int version_l;
			json.get_optional<int> ("version", version_l);
			json.get_optional<bool> ("rpc_enable", rpc_enable);

			auto rpc_l (json.get_required_child ("rpc"));

			if (!rpc.deserialize_json (upgraded_a, rpc_l, data_path))
			{
				auto node_l (json.get_required_child ("node"));
				if (!json.get_error ())
				{
					node.deserialize_json (upgraded_a, node_l);
				}
			}

			if (!json.get_error ())
			{
				json.get_required<bool> ("opencl_enable", opencl_enable);
				auto opencl_l (json.get_required_child ("opencl"));
				if (!json.get_error ())
				{
					opencl.deserialize_json (opencl_l);
				}
			}
		}
		else
		{
			upgraded_a = true;
			serialize_json (json);
		}
	}
	catch (std::runtime_error const & ex)
	{
		json.get_error () = ex;
	}
	return json.get_error ();
}

oslo::error oslo::read_node_config_toml (boost::filesystem::path const & data_path_a, oslo::daemon_config & config_a, std::vector<std::string> const & config_overrides)
{
	oslo::error error;
	auto json_config_path = oslo::get_config_path (data_path_a);
	auto toml_config_path = oslo::get_node_toml_config_path (data_path_a);
	auto toml_qt_config_path = oslo::get_qtwallet_toml_config_path (data_path_a);
	if (boost::filesystem::exists (json_config_path))
	{
		if (boost::filesystem::exists (toml_config_path))
		{
			error = "Both json and toml node configuration files exists. "
			        "Either remove the config.json file and restart, or remove "
			        "the config-node.toml file to start migration on next launch.";
		}
		else
		{
			// Migrate
			oslo::daemon_config config_old_l;
			oslo::jsonconfig json;
			read_and_update_daemon_config (data_path_a, config_old_l, json);
			error = json.get_error ();

			// Move qt wallet entries to wallet config file
			if (!error && json.has_key ("wallet") && json.has_key ("account"))
			{
				if (!boost::filesystem::exists (toml_config_path))
				{
					oslo::wallet_config wallet_conf;
					error = wallet_conf.parse (json.get<std::string> ("wallet"), json.get<std::string> ("account"));
					if (!error)
					{
						oslo::tomlconfig wallet_toml_l;
						wallet_conf.serialize_toml (wallet_toml_l);
						wallet_toml_l.write (toml_qt_config_path);

						boost::system::error_code error_chmod;
						oslo::set_secure_perm_file (toml_qt_config_path, error_chmod);
					}
				}
				else
				{
					std::cout << "Not migrating wallet and account as wallet config file already exists" << std::endl;
				}
			}

			if (!error)
			{
				oslo::tomlconfig toml_l;
				config_old_l.serialize_toml (toml_l);

				// Only write out non-default values
				oslo::daemon_config config_defaults_l;
				oslo::tomlconfig toml_defaults_l;
				config_defaults_l.serialize_toml (toml_defaults_l);

				toml_l.erase_default_values (toml_defaults_l);
				if (!toml_l.empty ())
				{
					toml_l.write (toml_config_path);
					boost::system::error_code error_chmod;
					oslo::set_secure_perm_file (toml_config_path, error_chmod);
				}

				auto backup_path = data_path_a / "config_backup_toml_migration.json";
				boost::filesystem::rename (json_config_path, backup_path);
			}
		}
	}

	// Parse and deserialize
	oslo::tomlconfig toml;

	std::stringstream config_overrides_stream;
	for (auto const & entry : config_overrides)
	{
		config_overrides_stream << entry << std::endl;
	}
	config_overrides_stream << std::endl;

	// Make sure we don't create an empty toml file if it doesn't exist. Running without a toml file is the default.
	if (!error)
	{
		if (boost::filesystem::exists (toml_config_path))
		{
			error = toml.read (config_overrides_stream, toml_config_path);
		}
		else
		{
			error = toml.read (config_overrides_stream);
		}
	}

	if (!error)
	{
		error = config_a.deserialize_toml (toml);
	}

	return error;
}

oslo::error oslo::read_and_update_daemon_config (boost::filesystem::path const & data_path, oslo::daemon_config & config_a, oslo::jsonconfig & json_a)
{
	boost::system::error_code error_chmod;
	auto config_path = oslo::get_config_path (data_path);
	auto error (json_a.read_and_update (config_a, config_path));
	oslo::set_secure_perm_file (config_path, error_chmod);
	return error;
}
