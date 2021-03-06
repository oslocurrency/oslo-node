#include <oslo/lib/config.hpp>
#include <oslo/lib/jsonconfig.hpp>
#include <oslo/lib/logger_mt.hpp>
#include <oslo/lib/tomlconfig.hpp>
#include <oslo/node/logging.hpp>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>

#ifdef BOOST_WINDOWS
#include <boost/log/sinks/event_log_backend.hpp>
#else
#define BOOST_LOG_USE_NATIVE_SYSLOG
#include <boost/log/sinks/syslog_backend.hpp>
#endif

BOOST_LOG_ATTRIBUTE_KEYWORD (severity, "Severity", oslo::severity_level)

boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend>> oslo::logging::file_sink;
std::atomic_flag oslo::logging::logging_already_added ATOMIC_FLAG_INIT;

void oslo::logging::init (boost::filesystem::path const & application_path_a)
{
	if (!logging_already_added.test_and_set ())
	{
		boost::log::add_common_attributes ();
		auto format = boost::log::expressions::stream << boost::log::expressions::attr<severity_level, severity_tag> ("Severity") << boost::log::expressions::smessage;
		auto format_with_timestamp = boost::log::expressions::stream << "[" << boost::log::expressions::attr<boost::posix_time::ptime> ("TimeStamp") << "]: " << boost::log::expressions::attr<severity_level, severity_tag> ("Severity") << boost::log::expressions::smessage;

		if (log_to_cerr ())
		{
			boost::log::add_console_log (std::cerr, boost::log::keywords::format = format_with_timestamp);
		}

		oslo::network_constants network_constants;
		if (!network_constants.is_test_network ())
		{
#ifdef BOOST_WINDOWS
			if (oslo::event_log_reg_entry_exists () || oslo::is_windows_elevated ())
			{
				static auto event_sink = boost::make_shared<boost::log::sinks::synchronous_sink<boost::log::sinks::simple_event_log_backend>> (boost::log::keywords::log_name = "Oslo", boost::log::keywords::log_source = "Oslo");
				event_sink->set_formatter (format);

				// Currently only mapping sys log errors
				boost::log::sinks::event_log::custom_event_type_mapping<oslo::severity_level> mapping ("Severity");
				mapping[oslo::severity_level::error] = boost::log::sinks::event_log::error;
				event_sink->locked_backend ()->set_event_type_mapper (mapping);

				// Only allow messages or error or greater severity to the event log
				event_sink->set_filter (severity >= oslo::severity_level::error);
				boost::log::core::get ()->add_sink (event_sink);
			}
#else
			static auto sys_sink = boost::make_shared<boost::log::sinks::synchronous_sink<boost::log::sinks::syslog_backend>> (boost::log::keywords::facility = boost::log::sinks::syslog::user, boost::log::keywords::use_impl = boost::log::sinks::syslog::impl_types::native);
			sys_sink->set_formatter (format);

			// Currently only mapping sys log errors
			boost::log::sinks::syslog::custom_severity_mapping<oslo::severity_level> mapping ("Severity");
			mapping[oslo::severity_level::error] = boost::log::sinks::syslog::error;
			sys_sink->locked_backend ()->set_severity_mapper (mapping);

			// Only allow messages or error or greater severity to the sys log
			sys_sink->set_filter (severity >= oslo::severity_level::error);
			boost::log::core::get ()->add_sink (sys_sink);
#endif
		}

//clang-format off
#if BOOST_VERSION < 107000
		if (stable_log_filename)
		{
			stable_log_filename = false;
			std::cerr << "The stable_log_filename config setting is only available when building with Boost 1.70 or later. Reverting to old behavior." << std::endl;
		}
#endif

		auto path = application_path_a / "log";
		if (stable_log_filename)
		{
#if BOOST_VERSION >= 107000
			// Logging to node.log and node_<pattern> instead of log_<pattern>.log is deliberate. This way,
			// existing log monitoring scripts expecting the old logfile structure will fail immediately instead
			// of reading only rotated files with old entries.
			file_sink = boost::log::add_file_log (boost::log::keywords::target = path,
			boost::log::keywords::file_name = path / "node.log",
			boost::log::keywords::target_file_name = path / "node_%Y-%m-%d_%H-%M-%S.%N.log",
			boost::log::keywords::open_mode = std::ios_base::out | std::ios_base::app, // append to node.log if it exists
			boost::log::keywords::enable_final_rotation = false, // for stable log filenames, don't rotate on destruction
			boost::log::keywords::rotation_size = rotation_size, // max file size in bytes before rotation
			boost::log::keywords::auto_flush = flush,
			boost::log::keywords::scan_method = boost::log::sinks::file::scan_method::scan_matching,
			boost::log::keywords::max_size = max_size, // max total size in bytes of all log files
			boost::log::keywords::format = format_with_timestamp);
#else
			debug_assert (false);
#endif
		}
		else
		{
			file_sink = boost::log::add_file_log (boost::log::keywords::target = path,
			boost::log::keywords::file_name = path / "log_%Y-%m-%d_%H-%M-%S.%N.log",
			boost::log::keywords::rotation_size = rotation_size,
			boost::log::keywords::auto_flush = flush,
			boost::log::keywords::scan_method = boost::log::sinks::file::scan_method::scan_matching,
			boost::log::keywords::max_size = max_size,
			boost::log::keywords::format = format_with_timestamp);
		}
	}
	//clang-format on
}

void oslo::logging::release_file_sink ()
{
	if (logging_already_added.test_and_set ())
	{
		boost::log::core::get ()->remove_sink (oslo::logging::file_sink);
		oslo::logging::file_sink.reset ();
	}
}

oslo::error oslo::logging::serialize_toml (oslo::tomlconfig & toml) const
{
	toml.put ("ledger", ledger_logging_value, "Log ledger related messages.\ntype:bool");
	toml.put ("ledger_duplicate", ledger_duplicate_logging_value, "Log when a duplicate block is attempted inserted into the ledger.\ntype:bool");
	toml.put ("vote", vote_logging_value, "Vote logging. Enabling this option leads to a high volume.\nof log messages which may affect node performance.\ntype:bool");
	toml.put ("network", network_logging_value, "Log network related messages.\ntype:bool");
	toml.put ("network_timeout", network_timeout_logging_value, "Log TCP timeouts.\ntype:bool");
	toml.put ("network_message", network_message_logging_value, "Log network errors and message details.\ntype:bool");
	toml.put ("network_publish", network_publish_logging_value, "Log publish related network messages.\ntype:bool");
	toml.put ("network_packet", network_packet_logging_value, "Log network packet activity.\ntype:bool");
	toml.put ("network_keepalive", network_keepalive_logging_value, "Log keepalive related messages.\ntype:bool");
	toml.put ("network_node_id_handshake", network_node_id_handshake_logging_value, "Log node-id handshake related messages.\ntype:bool");
	toml.put ("network_telemetry", network_telemetry_logging_value, "Log telemetry related messages.\ntype:bool");
	toml.put ("network_rejected", network_rejected_logging_value, "Log message when a connection is rejected.\ntype:bool");
	toml.put ("node_lifetime_tracing", node_lifetime_tracing_value, "Log node startup and shutdown messages.\ntype:bool");
	toml.put ("insufficient_work", insufficient_work_logging_value, "Log if insufficient work is detected.\ntype:bool");
	toml.put ("log_ipc", log_ipc_value, "Log IPC related activity.\ntype:bool");
	toml.put ("bulk_pull", bulk_pull_logging_value, "Log bulk pull errors and messages.\ntype:bool");
	toml.put ("work_generation_time", work_generation_time_value, "Log work generation timing information.\ntype:bool");
	toml.put ("upnp_details", upnp_details_logging_value, "Log UPNP discovery details..\nWarning: this may include information.\nabout discovered devices, such as product identification. Please review before sharing logs.\ntype:bool");
	toml.put ("timing", timing_logging_value, "Log detailed timing information for various node operations.\ntype:bool");
	toml.put ("active_update", active_update_value, "Log when a block is updated while in active transactions.\ntype:bool");
	toml.put ("log_to_cerr", log_to_cerr_value, "Log to standard error in addition to the log file. Not recommended for production systems.\ntype:bool");
	toml.put ("max_size", max_size, "Maximum log file size in bytes.\ntype:uint64");
	toml.put ("rotation_size", rotation_size, "Log file rotation size in character count.\ntype:uint64");
	toml.put ("flush", flush, "If enabled, immediately flush new entries to log file.\nWarning: this may negatively affect logging performance.\ntype:bool");
	toml.put ("min_time_between_output", min_time_between_log_output.count (), "Minimum time that must pass for low priority entries to be logged.\nWarning: decreasing this value may result in a very large amount of logs.\ntype:milliseconds");
	toml.put ("single_line_record", single_line_record_value, "Keep log entries on single lines.\ntype:bool");
	toml.put ("stable_log_filename", stable_log_filename, "Append to log/node.log without a timestamp in the filename.\nThe file is not emptied on startup if it exists, but appended to.\ntype:bool");

	return toml.get_error ();
}

oslo::error oslo::logging::deserialize_toml (oslo::tomlconfig & toml)
{
	toml.get<bool> ("ledger", ledger_logging_value);
	toml.get<bool> ("ledger_duplicate", ledger_duplicate_logging_value);
	toml.get<bool> ("vote", vote_logging_value);
	toml.get<bool> ("network", network_logging_value);
	toml.get<bool> ("network_timeout", network_timeout_logging_value);
	toml.get<bool> ("network_message", network_message_logging_value);
	toml.get<bool> ("network_publish", network_publish_logging_value);
	toml.get<bool> ("network_packet", network_packet_logging_value);
	toml.get<bool> ("network_keepalive", network_keepalive_logging_value);
	toml.get<bool> ("network_node_id_handshake", network_node_id_handshake_logging_value);
	toml.get<bool> ("network_telemetry_logging", network_telemetry_logging_value);
	toml.get<bool> ("network_rejected_logging", network_rejected_logging_value);
	toml.get<bool> ("node_lifetime_tracing", node_lifetime_tracing_value);
	toml.get<bool> ("insufficient_work", insufficient_work_logging_value);
	toml.get<bool> ("log_ipc", log_ipc_value);
	toml.get<bool> ("bulk_pull", bulk_pull_logging_value);
	toml.get<bool> ("work_generation_time", work_generation_time_value);
	toml.get<bool> ("upnp_details", upnp_details_logging_value);
	toml.get<bool> ("timing", timing_logging_value);
	toml.get<bool> ("active_update", active_update_value);
	toml.get<bool> ("log_to_cerr", log_to_cerr_value);
	toml.get<bool> ("flush", flush);
	toml.get<bool> ("single_line_record", single_line_record_value);
	toml.get<uintmax_t> ("max_size", max_size);
	toml.get<uintmax_t> ("rotation_size", rotation_size);
	auto min_time_between_log_output_l = min_time_between_log_output.count ();
	toml.get ("min_time_between_output", min_time_between_log_output_l);
	min_time_between_log_output = std::chrono::milliseconds (min_time_between_log_output_l);
	toml.get ("stable_log_filename", stable_log_filename);

	return toml.get_error ();
}

oslo::error oslo::logging::serialize_json (oslo::jsonconfig & json) const
{
	json.put ("version", json_version ());
	json.put ("ledger", ledger_logging_value);
	json.put ("ledger_duplicate", ledger_duplicate_logging_value);
	json.put ("vote", vote_logging_value);
	json.put ("network", network_logging_value);
	json.put ("network_timeout", network_timeout_logging_value);
	json.put ("network_message", network_message_logging_value);
	json.put ("network_publish", network_publish_logging_value);
	json.put ("network_packet", network_packet_logging_value);
	json.put ("network_keepalive", network_keepalive_logging_value);
	json.put ("network_node_id_handshake", network_node_id_handshake_logging_value);
	json.put ("network_telemetry_logging", network_telemetry_logging_value);
	json.put ("network_rejected_logging", network_rejected_logging_value);
	json.put ("node_lifetime_tracing", node_lifetime_tracing_value);
	json.put ("insufficient_work", insufficient_work_logging_value);
	json.put ("log_ipc", log_ipc_value);
	json.put ("bulk_pull", bulk_pull_logging_value);
	json.put ("work_generation_time", work_generation_time_value);
	json.put ("upnp_details", upnp_details_logging_value);
	json.put ("timing", timing_logging_value);
	json.put ("log_to_cerr", log_to_cerr_value);
	json.put ("max_size", max_size);
	json.put ("rotation_size", rotation_size);
	json.put ("flush", flush);
	json.put ("min_time_between_output", min_time_between_log_output.count ());
	json.put ("single_line_record", single_line_record_value);
	return json.get_error ();
}

bool oslo::logging::upgrade_json (unsigned version_a, oslo::jsonconfig & json)
{
	json.put ("version", json_version ());
	switch (version_a)
	{
		case 1:
			json.put ("vote", vote_logging_value);
		case 2:
			json.put ("rotation_size", rotation_size);
			json.put ("flush", true);
		case 3:
			json.put ("network_node_id_handshake", false);
		case 4:
			json.put ("upnp_details", "false");
			json.put ("timing", "false");
		case 5:
			uintmax_t config_max_size;
			json.get<uintmax_t> ("max_size", config_max_size);
			max_size = std::max (max_size, config_max_size);
			json.put ("max_size", max_size);
			json.put ("log_ipc", true);
		case 6:
			json.put ("min_time_between_output", min_time_between_log_output.count ());
			json.put ("network_timeout", network_timeout_logging_value);
			json.erase ("log_rpc");
			break;
		case 7:
			json.put ("single_line_record", single_line_record_value);
		case 8:
			break;
		default:
			throw std::runtime_error ("Unknown logging_config version");
			break;
	}
	return version_a < json_version ();
}

oslo::error oslo::logging::deserialize_json (bool & upgraded_a, oslo::jsonconfig & json)
{
	int version_l{ 1 };
	if (!json.has_key ("version"))
	{
		json.put ("version", version_l);

		auto work_peers_l (json.get_optional_child ("work_peers"));
		if (!work_peers_l)
		{
			oslo::jsonconfig peers;
			json.put_child ("work_peers", peers);
		}
		upgraded_a = true;
	}
	else
	{
		json.get_required<int> ("version", version_l);
	}

	upgraded_a |= upgrade_json (version_l, json);
	json.get<bool> ("ledger", ledger_logging_value);
	json.get<bool> ("ledger_duplicate", ledger_duplicate_logging_value);
	json.get<bool> ("vote", vote_logging_value);
	json.get<bool> ("network", network_logging_value);
	json.get<bool> ("network_timeout", network_timeout_logging_value);
	json.get<bool> ("network_message", network_message_logging_value);
	json.get<bool> ("network_publish", network_publish_logging_value);
	json.get<bool> ("network_packet", network_packet_logging_value);
	json.get<bool> ("network_keepalive", network_keepalive_logging_value);
	json.get<bool> ("network_node_id_handshake", network_node_id_handshake_logging_value);
	json.get<bool> ("node_lifetime_tracing", node_lifetime_tracing_value);
	json.get<bool> ("insufficient_work", insufficient_work_logging_value);
	json.get<bool> ("log_ipc", log_ipc_value);
	json.get<bool> ("bulk_pull", bulk_pull_logging_value);
	json.get<bool> ("work_generation_time", work_generation_time_value);
	json.get<bool> ("upnp_details", upnp_details_logging_value);
	json.get<bool> ("timing", timing_logging_value);
	json.get<bool> ("log_to_cerr", log_to_cerr_value);
	json.get<bool> ("flush", flush);
	json.get<bool> ("single_line_record", single_line_record_value);
	json.get<uintmax_t> ("max_size", max_size);
	json.get<uintmax_t> ("rotation_size", rotation_size);
	uintmax_t min_time_between_log_output_raw;
	json.get<uintmax_t> ("min_time_between_output", min_time_between_log_output_raw);
	min_time_between_log_output = std::chrono::milliseconds (min_time_between_log_output_raw);
	return json.get_error ();
}

bool oslo::logging::ledger_logging () const
{
	return ledger_logging_value;
}

bool oslo::logging::ledger_duplicate_logging () const
{
	return ledger_logging () && ledger_duplicate_logging_value;
}

bool oslo::logging::vote_logging () const
{
	return vote_logging_value;
}

bool oslo::logging::network_logging () const
{
	return network_logging_value;
}

bool oslo::logging::network_timeout_logging () const
{
	return network_logging () && network_timeout_logging_value;
}

bool oslo::logging::network_message_logging () const
{
	return network_logging () && network_message_logging_value;
}

bool oslo::logging::network_publish_logging () const
{
	return network_logging () && network_publish_logging_value;
}

bool oslo::logging::network_packet_logging () const
{
	return network_logging () && network_packet_logging_value;
}

bool oslo::logging::network_keepalive_logging () const
{
	return network_logging () && network_keepalive_logging_value;
}

bool oslo::logging::network_node_id_handshake_logging () const
{
	return network_logging () && network_node_id_handshake_logging_value;
}

bool oslo::logging::network_telemetry_logging () const
{
	return network_logging () && network_telemetry_logging_value;
}

bool oslo::logging::network_rejected_logging () const
{
	return network_logging () && network_rejected_logging_value;
}

bool oslo::logging::node_lifetime_tracing () const
{
	return node_lifetime_tracing_value;
}

bool oslo::logging::insufficient_work_logging () const
{
	return network_logging () && insufficient_work_logging_value;
}

bool oslo::logging::log_ipc () const
{
	return network_logging () && log_ipc_value;
}

bool oslo::logging::bulk_pull_logging () const
{
	return network_logging () && bulk_pull_logging_value;
}

bool oslo::logging::callback_logging () const
{
	return network_logging ();
}

bool oslo::logging::work_generation_time () const
{
	return work_generation_time_value;
}

bool oslo::logging::upnp_details_logging () const
{
	return upnp_details_logging_value;
}

bool oslo::logging::timing_logging () const
{
	return timing_logging_value;
}

bool oslo::logging::active_update_logging () const
{
	return active_update_value;
}

bool oslo::logging::log_to_cerr () const
{
	return log_to_cerr_value;
}

bool oslo::logging::single_line_record () const
{
	return single_line_record_value;
}
