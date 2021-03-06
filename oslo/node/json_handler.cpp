#include <oslo/lib/config.hpp>
#include <oslo/lib/json_error_response.hpp>
#include <oslo/lib/timer.hpp>
#include <oslo/node/bootstrap/bootstrap_lazy.hpp>
#include <oslo/node/common.hpp>
#include <oslo/node/election.hpp>
#include <oslo/node/json_handler.hpp>
#include <oslo/node/json_payment_observer.hpp>
#include <oslo/node/node.hpp>
#include <oslo/node/node_rpc_config.hpp>
#include <oslo/node/telemetry.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <chrono>

namespace
{
void construct_json (oslo::container_info_component * component, boost::property_tree::ptree & parent);
using ipc_json_handler_no_arg_func_map = std::unordered_map<std::string, std::function<void(oslo::json_handler *)>>;
ipc_json_handler_no_arg_func_map create_ipc_json_handler_no_arg_func_map ();
auto ipc_json_handler_no_arg_funcs = create_ipc_json_handler_no_arg_func_map ();
bool block_confirmed (oslo::node & node, oslo::transaction & transaction, oslo::block_hash const & hash, bool include_active, bool include_only_confirmed);
const char * epoch_as_string (oslo::epoch);
}

oslo::json_handler::json_handler (oslo::node & node_a, oslo::node_rpc_config const & node_rpc_config_a, std::string const & body_a, std::function<void(std::string const &)> const & response_a, std::function<void()> stop_callback_a) :
body (body_a),
node (node_a),
response (response_a),
stop_callback (stop_callback_a),
node_rpc_config (node_rpc_config_a)
{
}

std::function<void()> oslo::json_handler::create_worker_task (std::function<void(std::shared_ptr<oslo::json_handler> const &)> const & action_a)
{
	return [rpc_l = shared_from_this (), action_a]() {
		try
		{
			action_a (rpc_l);
		}
		catch (std::runtime_error const &)
		{
			json_error_response (rpc_l->response, "Unable to parse JSON");
		}
		catch (...)
		{
			json_error_response (rpc_l->response, "Internal server error in RPC");
		}
	};
}

void oslo::json_handler::process_request (bool unsafe_a)
{
	try
	{
		std::stringstream istream (body);
		boost::property_tree::read_json (istream, request);
		action = request.get<std::string> ("action");
		auto no_arg_func_iter = ipc_json_handler_no_arg_funcs.find (action);
		if (no_arg_func_iter != ipc_json_handler_no_arg_funcs.cend ())
		{
			// First try the map of options with no arguments
			no_arg_func_iter->second (this);
		}
		else
		{
			// Try the rest of the options
			if (action == "wallet_seed")
			{
				if (unsafe_a || node.network_params.network.is_test_network ())
				{
					wallet_seed ();
				}
				else
				{
					json_error_response (response, "Unsafe RPC not allowed");
				}
			}
			else if (action == "chain")
			{
				chain ();
			}
			else if (action == "successors")
			{
				chain (true);
			}
			else if (action == "history")
			{
				request.put ("head", request.get<std::string> ("hash"));
				account_history ();
			}
			else if (action == "password_valid")
			{
				password_valid ();
			}
			else if (action == "wallet_locked")
			{
				password_valid (true);
			}
			else
			{
				json_error_response (response, "Unknown command");
			}
		}
	}
	catch (std::runtime_error const &)
	{
		json_error_response (response, "Unable to parse JSON");
	}
	catch (...)
	{
		json_error_response (response, "Internal server error in RPC");
	}
}

void oslo::json_handler::response_errors ()
{
	if (ec || response_l.empty ())
	{
		boost::property_tree::ptree response_error;
		response_error.put ("error", ec ? ec.message () : "Empty response");
		std::stringstream ostream;
		boost::property_tree::write_json (ostream, response_error);
		response (ostream.str ());
	}
	else
	{
		std::stringstream ostream;
		boost::property_tree::write_json (ostream, response_l);
		response (ostream.str ());
	}
}

std::shared_ptr<oslo::wallet> oslo::json_handler::wallet_impl ()
{
	if (!ec)
	{
		std::string wallet_text (request.get<std::string> ("wallet"));
		oslo::wallet_id wallet;
		if (!wallet.decode_hex (wallet_text))
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				return existing->second;
			}
			else
			{
				ec = oslo::error_common::wallet_not_found;
			}
		}
		else
		{
			ec = oslo::error_common::bad_wallet_number;
		}
	}
	return nullptr;
}

bool oslo::json_handler::wallet_locked_impl (oslo::transaction const & transaction_a, std::shared_ptr<oslo::wallet> wallet_a)
{
	bool result (false);
	if (!ec)
	{
		if (!wallet_a->store.valid_password (transaction_a))
		{
			ec = oslo::error_common::wallet_locked;
			result = true;
		}
	}
	return result;
}

bool oslo::json_handler::wallet_account_impl (oslo::transaction const & transaction_a, std::shared_ptr<oslo::wallet> wallet_a, oslo::account const & account_a)
{
	bool result (false);
	if (!ec)
	{
		if (wallet_a->store.find (transaction_a, account_a) != wallet_a->store.end ())
		{
			result = true;
		}
		else
		{
			ec = oslo::error_common::account_not_found_wallet;
		}
	}
	return result;
}

oslo::account oslo::json_handler::account_impl (std::string account_text, std::error_code ec_a)
{
	oslo::account result (0);
	if (!ec)
	{
		if (account_text.empty ())
		{
			account_text = request.get<std::string> ("account");
		}
		if (result.decode_account (account_text))
		{
			ec = ec_a;
		}
		else if (account_text[3] == '-' || account_text[4] == '-')
		{
			// oslo- and xrb- prefixes are deprecated
			response_l.put ("deprecated_account_format", "1");
		}
	}
	return result;
}

oslo::account_info oslo::json_handler::account_info_impl (oslo::transaction const & transaction_a, oslo::account const & account_a)
{
	oslo::account_info result;
	if (!ec)
	{
		if (node.store.account_get (transaction_a, account_a, result))
		{
			ec = oslo::error_common::account_not_found;
			node.bootstrap_initiator.bootstrap_lazy (account_a, false, false, account_a.to_account ());
		}
	}
	return result;
}

oslo::amount oslo::json_handler::amount_impl ()
{
	oslo::amount result (0);
	if (!ec)
	{
		std::string amount_text (request.get<std::string> ("amount"));
		if (result.decode_dec (amount_text))
		{
			ec = oslo::error_common::invalid_amount;
		}
	}
	return result;
}

std::shared_ptr<oslo::block> oslo::json_handler::block_impl (bool signature_work_required)
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	std::shared_ptr<oslo::block> result{ nullptr };
	if (!ec)
	{
		boost::property_tree::ptree block_l;
		if (json_block_l)
		{
			block_l = request.get_child ("block");
		}
		else
		{
			std::string block_text (request.get<std::string> ("block"));
			std::stringstream block_stream (block_text);
			try
			{
				boost::property_tree::read_json (block_stream, block_l);
			}
			catch (...)
			{
				ec = oslo::error_blocks::invalid_block;
			}
		}
		if (!ec)
		{
			if (!signature_work_required)
			{
				block_l.put ("signature", "0");
				block_l.put ("work", "0");
			}
			result = oslo::deserialize_block_json (block_l);
			if (result == nullptr)
			{
				ec = oslo::error_blocks::invalid_block;
			}
		}
	}
	return result;
}

oslo::block_hash oslo::json_handler::hash_impl (std::string search_text)
{
	oslo::block_hash result (0);
	if (!ec)
	{
		std::string hash_text (request.get<std::string> (search_text));
		if (result.decode_hex (hash_text))
		{
			ec = oslo::error_blocks::invalid_block_hash;
		}
	}
	return result;
}

oslo::amount oslo::json_handler::threshold_optional_impl ()
{
	oslo::amount result (0);
	boost::optional<std::string> threshold_text (request.get_optional<std::string> ("threshold"));
	if (!ec && threshold_text.is_initialized ())
	{
		if (result.decode_dec (threshold_text.get ()))
		{
			ec = oslo::error_common::bad_threshold;
		}
	}
	return result;
}

uint64_t oslo::json_handler::work_optional_impl ()
{
	uint64_t result (0);
	boost::optional<std::string> work_text (request.get_optional<std::string> ("work"));
	if (!ec && work_text.is_initialized ())
	{
		if (oslo::from_string_hex (work_text.get (), result))
		{
			ec = oslo::error_common::bad_work_format;
		}
	}
	return result;
}

uint64_t oslo::json_handler::difficulty_optional_impl (oslo::work_version const version_a)
{
	auto difficulty (node.default_difficulty (version_a));
	boost::optional<std::string> difficulty_text (request.get_optional<std::string> ("difficulty"));
	if (!ec && difficulty_text.is_initialized ())
	{
		if (oslo::from_string_hex (difficulty_text.get (), difficulty))
		{
			ec = oslo::error_rpc::bad_difficulty_format;
		}
	}
	return difficulty;
}

uint64_t oslo::json_handler::difficulty_ledger (oslo::block const & block_a)
{
	oslo::block_details details (oslo::epoch::epoch_0, false, false, false);
	bool details_found (false);
	auto transaction (node.store.tx_begin_read ());
	// Previous block find
	std::shared_ptr<oslo::block> block_previous (nullptr);
	auto previous (block_a.previous ());
	if (!previous.is_zero ())
	{
		block_previous = node.store.block_get (transaction, previous);
	}
	// Send check
	if (block_previous != nullptr)
	{
		details.is_send = node.store.block_balance (transaction, previous) > block_a.balance ().number ();
		details_found = true;
	}
	// Epoch check
	if (block_previous != nullptr)
	{
		details.epoch = block_previous->sideband ().details.epoch;
	}
	auto link (block_a.link ());
	if (!link.is_zero () && !details.is_send)
	{
		auto block_link (node.store.block_get (transaction, link));
		if (block_link != nullptr && node.store.pending_exists (transaction, oslo::pending_key (block_a.account (), link)))
		{
			details.epoch = std::max (details.epoch, block_link->sideband ().details.epoch);
			details.is_receive = true;
			details_found = true;
		}
	}
	return details_found ? oslo::work_threshold (block_a.work_version (), details) : node.default_difficulty (block_a.work_version ());
}

double oslo::json_handler::multiplier_optional_impl (oslo::work_version const version_a, uint64_t & difficulty)
{
	double multiplier (1.);
	boost::optional<std::string> multiplier_text (request.get_optional<std::string> ("multiplier"));
	if (!ec && multiplier_text.is_initialized ())
	{
		auto success = boost::conversion::try_lexical_convert<double> (multiplier_text.get (), multiplier);
		if (success && multiplier > 0.)
		{
			difficulty = oslo::difficulty::from_multiplier (multiplier, node.default_difficulty (version_a));
		}
		else
		{
			ec = oslo::error_rpc::bad_multiplier_format;
		}
	}
	return multiplier;
}

oslo::work_version oslo::json_handler::work_version_optional_impl (oslo::work_version const default_a)
{
	oslo::work_version result = default_a;
	boost::optional<std::string> version_text (request.get_optional<std::string> ("version"));
	if (!ec && version_text.is_initialized ())
	{
		if (*version_text == oslo::to_string (oslo::work_version::work_1))
		{
			result = oslo::work_version::work_1;
		}
		else
		{
			ec = oslo::error_rpc::bad_work_version;
		}
	}
	return result;
}

namespace
{
bool decode_unsigned (std::string const & text, uint64_t & number)
{
	bool result;
	size_t end;
	try
	{
		number = std::stoull (text, &end);
		result = false;
	}
	catch (std::invalid_argument const &)
	{
		result = true;
	}
	catch (std::out_of_range const &)
	{
		result = true;
	}
	result = result || end != text.size ();
	return result;
}
}

uint64_t oslo::json_handler::count_impl ()
{
	uint64_t result (0);
	if (!ec)
	{
		std::string count_text (request.get<std::string> ("count"));
		if (decode_unsigned (count_text, result) || result == 0)
		{
			ec = oslo::error_common::invalid_count;
		}
	}
	return result;
}

uint64_t oslo::json_handler::count_optional_impl (uint64_t result)
{
	boost::optional<std::string> count_text (request.get_optional<std::string> ("count"));
	if (!ec && count_text.is_initialized ())
	{
		if (decode_unsigned (count_text.get (), result))
		{
			ec = oslo::error_common::invalid_count;
		}
	}
	return result;
}

uint64_t oslo::json_handler::offset_optional_impl (uint64_t result)
{
	boost::optional<std::string> offset_text (request.get_optional<std::string> ("offset"));
	if (!ec && offset_text.is_initialized ())
	{
		if (decode_unsigned (offset_text.get (), result))
		{
			ec = oslo::error_rpc::invalid_offset;
		}
	}
	return result;
}

void oslo::json_handler::account_balance ()
{
	auto account (account_impl ());
	if (!ec)
	{
		auto balance (node.balance_pending (account));
		response_l.put ("balance", balance.first.convert_to<std::string> ());
		response_l.put ("pending", balance.second.convert_to<std::string> ());
	}
	response_errors ();
}

void oslo::json_handler::account_block_count ()
{
	auto account (account_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		auto info (account_info_impl (transaction, account));
		if (!ec)
		{
			response_l.put ("block_count", std::to_string (info.block_count));
		}
	}
	response_errors ();
}

void oslo::json_handler::account_create ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			const bool generate_work = rpc_l->request.get<bool> ("work", true);
			oslo::account new_key;
			auto index_text (rpc_l->request.get_optional<std::string> ("index"));
			if (index_text.is_initialized ())
			{
				uint64_t index;
				if (decode_unsigned (index_text.get (), index) || index > static_cast<uint64_t> (std::numeric_limits<uint32_t>::max ()))
				{
					rpc_l->ec = oslo::error_common::invalid_index;
				}
				else
				{
					new_key = wallet->deterministic_insert (static_cast<uint32_t> (index), generate_work);
				}
			}
			else
			{
				new_key = wallet->deterministic_insert (generate_work);
			}

			if (!rpc_l->ec)
			{
				if (!new_key.is_zero ())
				{
					rpc_l->response_l.put ("account", new_key.to_account ());
				}
				else
				{
					rpc_l->ec = oslo::error_common::wallet_locked;
				}
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::account_get ()
{
	std::string key_text (request.get<std::string> ("key"));
	oslo::public_key pub;
	if (!pub.decode_hex (key_text))
	{
		response_l.put ("account", pub.to_account ());
	}
	else
	{
		ec = oslo::error_common::bad_public_key;
	}
	response_errors ();
}

void oslo::json_handler::account_info ()
{
	auto account (account_impl ());
	if (!ec)
	{
		const bool representative = request.get<bool> ("representative", false);
		const bool weight = request.get<bool> ("weight", false);
		const bool pending = request.get<bool> ("pending", false);
		auto transaction (node.store.tx_begin_read ());
		auto info (account_info_impl (transaction, account));
		oslo::confirmation_height_info confirmation_height_info;
		if (node.store.confirmation_height_get (transaction, account, confirmation_height_info))
		{
			ec = oslo::error_common::account_not_found;
		}
		if (!ec)
		{
			response_l.put ("frontier", info.head.to_string ());
			response_l.put ("open_block", info.open_block.to_string ());
			response_l.put ("representative_block", node.ledger.representative (transaction, info.head).to_string ());
			std::string balance;
			oslo::uint128_union (info.balance).encode_dec (balance);
			response_l.put ("balance", balance);
			response_l.put ("modified_timestamp", std::to_string (info.modified));
			response_l.put ("block_count", std::to_string (info.block_count));
			response_l.put ("account_version", epoch_as_string (info.epoch ()));
			response_l.put ("confirmation_height", std::to_string (confirmation_height_info.height));
			response_l.put ("confirmation_height_frontier", confirmation_height_info.frontier.to_string ());
			if (representative)
			{
				response_l.put ("representative", info.representative.to_account ());
			}
			if (weight)
			{
				auto account_weight (node.ledger.weight (account));
				response_l.put ("weight", account_weight.convert_to<std::string> ());
			}
			if (pending)
			{
				auto account_pending (node.ledger.account_pending (transaction, account));
				response_l.put ("pending", account_pending.convert_to<std::string> ());
			}
		}
	}
	response_errors ();
}

void oslo::json_handler::account_key ()
{
	auto account (account_impl ());
	if (!ec)
	{
		response_l.put ("key", account.to_string ());
	}
	response_errors ();
}

void oslo::json_handler::account_list ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		boost::property_tree::ptree accounts;
		auto transaction (node.wallets.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), j (wallet->store.end ()); i != j; ++i)
		{
			boost::property_tree::ptree entry;
			entry.put ("", oslo::account (i->first).to_account ());
			accounts.push_back (std::make_pair ("", entry));
		}
		response_l.add_child ("accounts", accounts);
	}
	response_errors ();
}

void oslo::json_handler::account_move ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			std::string source_text (rpc_l->request.get<std::string> ("source"));
			auto accounts_text (rpc_l->request.get_child ("accounts"));
			oslo::wallet_id source;
			if (!source.decode_hex (source_text))
			{
				auto existing (rpc_l->node.wallets.items.find (source));
				if (existing != rpc_l->node.wallets.items.end ())
				{
					auto source (existing->second);
					std::vector<oslo::public_key> accounts;
					for (auto i (accounts_text.begin ()), n (accounts_text.end ()); i != n; ++i)
					{
						auto account (rpc_l->account_impl (i->second.get<std::string> ("")));
						accounts.push_back (account);
					}
					auto transaction (rpc_l->node.wallets.tx_begin_write ());
					auto error (wallet->store.move (transaction, source->store, accounts));
					rpc_l->response_l.put ("moved", error ? "0" : "1");
				}
				else
				{
					rpc_l->ec = oslo::error_rpc::source_not_found;
				}
			}
			else
			{
				rpc_l->ec = oslo::error_rpc::bad_source;
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::account_remove ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		auto account (rpc_l->account_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			rpc_l->wallet_locked_impl (transaction, wallet);
			rpc_l->wallet_account_impl (transaction, wallet, account);
			if (!rpc_l->ec)
			{
				wallet->store.erase (transaction, account);
				rpc_l->response_l.put ("removed", "1");
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::account_representative ()
{
	auto account (account_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		auto info (account_info_impl (transaction, account));
		if (!ec)
		{
			response_l.put ("representative", info.representative.to_account ());
		}
	}
	response_errors ();
}

void oslo::json_handler::account_representative_set ()
{
	node.worker.push_task (create_worker_task ([work_generation_enabled = node.work_generation_enabled ()](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		auto account (rpc_l->account_impl ());
		std::string representative_text (rpc_l->request.get<std::string> ("representative"));
		auto representative (rpc_l->account_impl (representative_text, oslo::error_rpc::bad_representative_number));
		if (!rpc_l->ec)
		{
			auto work (rpc_l->work_optional_impl ());
			if (!rpc_l->ec && work)
			{
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				rpc_l->wallet_locked_impl (transaction, wallet);
				rpc_l->wallet_account_impl (transaction, wallet, account);
				if (!rpc_l->ec)
				{
					auto block_transaction (rpc_l->node.store.tx_begin_read ());
					auto info (rpc_l->account_info_impl (block_transaction, account));
					if (!rpc_l->ec)
					{
						oslo::block_details details (info.epoch (), false, false, false);
						if (oslo::work_difficulty (oslo::work_version::work_1, info.head, work) < oslo::work_threshold (oslo::work_version::work_1, details))
						{
							rpc_l->ec = oslo::error_common::invalid_work;
						}
					}
				}
			}
			else if (!rpc_l->ec) // work == 0
			{
				if (!work_generation_enabled)
				{
					rpc_l->ec = oslo::error_common::disabled_work_generation;
				}
			}
			if (!rpc_l->ec)
			{
				bool generate_work (work == 0); // Disable work generation if "work" option is provided
				auto response_a (rpc_l->response);
				auto response_data (std::make_shared<boost::property_tree::ptree> (rpc_l->response_l));
				wallet->change_async (
				account, representative, [response_a, response_data](std::shared_ptr<oslo::block> block) {
					if (block != nullptr)
					{
						response_data->put ("block", block->hash ().to_string ());
						std::stringstream ostream;
						boost::property_tree::write_json (ostream, *response_data);
						response_a (ostream.str ());
					}
					else
					{
						json_error_response (response_a, "Error generating block");
					}
				},
				work, generate_work);
			}
		}
		// Because of change_async
		if (rpc_l->ec)
		{
			rpc_l->response_errors ();
		}
	}));
}

void oslo::json_handler::account_weight ()
{
	auto account (account_impl ());
	if (!ec)
	{
		auto balance (node.weight (account));
		response_l.put ("weight", balance.convert_to<std::string> ());
	}
	response_errors ();
}

void oslo::json_handler::accounts_balances ()
{
	boost::property_tree::ptree balances;
	for (auto & accounts : request.get_child ("accounts"))
	{
		auto account (account_impl (accounts.second.data ()));
		if (!ec)
		{
			boost::property_tree::ptree entry;
			auto balance (node.balance_pending (account));
			entry.put ("balance", balance.first.convert_to<std::string> ());
			entry.put ("pending", balance.second.convert_to<std::string> ());
			balances.push_back (std::make_pair (account.to_account (), entry));
		}
	}
	response_l.add_child ("balances", balances);
	response_errors ();
}

void oslo::json_handler::accounts_create ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		auto count (rpc_l->count_impl ());
		if (!rpc_l->ec)
		{
			const bool generate_work = rpc_l->request.get<bool> ("work", false);
			boost::property_tree::ptree accounts;
			for (auto i (0); accounts.size () < count; ++i)
			{
				oslo::account new_key (wallet->deterministic_insert (generate_work));
				if (!new_key.is_zero ())
				{
					boost::property_tree::ptree entry;
					entry.put ("", new_key.to_account ());
					accounts.push_back (std::make_pair ("", entry));
				}
			}
			rpc_l->response_l.add_child ("accounts", accounts);
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::accounts_frontiers ()
{
	boost::property_tree::ptree frontiers;
	auto transaction (node.store.tx_begin_read ());
	for (auto & accounts : request.get_child ("accounts"))
	{
		auto account (account_impl (accounts.second.data ()));
		if (!ec)
		{
			auto latest (node.ledger.latest (transaction, account));
			if (!latest.is_zero ())
			{
				frontiers.put (account.to_account (), latest.to_string ());
			}
		}
	}
	response_l.add_child ("frontiers", frontiers);
	response_errors ();
}

void oslo::json_handler::accounts_pending ()
{
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	const bool source = request.get<bool> ("source", false);
	const bool include_active = request.get<bool> ("include_active", false);
	const bool include_only_confirmed = request.get<bool> ("include_only_confirmed", false);
	const bool sorting = request.get<bool> ("sorting", false);
	auto simple (threshold.is_zero () && !source && !sorting); // if simple, response is a list of hashes for each account
	boost::property_tree::ptree pending;
	auto transaction (node.store.tx_begin_read ());
	for (auto & accounts : request.get_child ("accounts"))
	{
		auto account (account_impl (accounts.second.data ()));
		if (!ec)
		{
			boost::property_tree::ptree peers_l;
			for (auto i (node.store.pending_begin (transaction, oslo::pending_key (account, 0))), n (node.store.pending_end ()); i != n && oslo::pending_key (i->first).account == account && peers_l.size () < count; ++i)
			{
				oslo::pending_key const & key (i->first);
				if (block_confirmed (node, transaction, key.hash, include_active, include_only_confirmed))
				{
					if (simple)
					{
						boost::property_tree::ptree entry;
						entry.put ("", key.hash.to_string ());
						peers_l.push_back (std::make_pair ("", entry));
					}
					else
					{
						oslo::pending_info const & info (i->second);
						if (info.amount.number () >= threshold.number ())
						{
							if (source)
							{
								boost::property_tree::ptree pending_tree;
								pending_tree.put ("amount", info.amount.number ().convert_to<std::string> ());
								pending_tree.put ("source", info.source.to_account ());
								peers_l.add_child (key.hash.to_string (), pending_tree);
							}
							else
							{
								peers_l.put (key.hash.to_string (), info.amount.number ().convert_to<std::string> ());
							}
						}
					}
				}
			}
			if (sorting && !simple)
			{
				if (source)
				{
					peers_l.sort ([](const auto & child1, const auto & child2) -> bool {
						return child1.second.template get<oslo::uint128_t> ("amount") > child2.second.template get<oslo::uint128_t> ("amount");
					});
				}
				else
				{
					peers_l.sort ([](const auto & child1, const auto & child2) -> bool {
						return child1.second.template get<oslo::uint128_t> ("") > child2.second.template get<oslo::uint128_t> ("");
					});
				}
			}
			pending.add_child (account.to_account (), peers_l);
		}
	}
	response_l.add_child ("blocks", pending);
	response_errors ();
}

void oslo::json_handler::active_difficulty ()
{
	auto include_trend (request.get<bool> ("include_trend", false));
	auto multiplier_active = node.active.active_multiplier ();
	auto default_difficulty (node.default_difficulty (oslo::work_version::work_1));
	response_l.put ("network_minimum", oslo::to_string_hex (default_difficulty));
	response_l.put ("network_current", oslo::to_string_hex (oslo::difficulty::from_multiplier (multiplier_active, default_difficulty)));
	response_l.put ("multiplier", multiplier_active);
	if (include_trend)
	{
		boost::property_tree::ptree trend_entry_l;
		auto trend_l (node.active.difficulty_trend ());
		for (auto multiplier_l : trend_l)
		{
			boost::property_tree::ptree entry;
			entry.put ("", oslo::to_string (multiplier_l));
			trend_entry_l.push_back (std::make_pair ("", entry));
		}
		response_l.add_child ("difficulty_trend", trend_entry_l);
	}
	response_errors ();
}

void oslo::json_handler::available_supply ()
{
	auto genesis_balance (node.balance (node.network_params.ledger.genesis_account)); // Cold storage genesis
	auto landing_balance (node.balance (oslo::account ("059F68AAB29DE0D3A27443625C7EA9CDDB6517A8B76FE37727EF6A4D76832AD5"))); // Active unavailable account
	auto faucet_balance (node.balance (oslo::account ("8E319CE6F3025E5B2DF66DA7AB1467FE48F1679C13DD43BFDB29FA2E9FC40D3B"))); // Faucet account
	auto burned_balance ((node.balance_pending (oslo::account (0))).second); // Burning 0 account
	auto available (node.network_params.ledger.genesis_amount - genesis_balance - landing_balance - faucet_balance - burned_balance);
	response_l.put ("available", available.convert_to<std::string> ());
	response_errors ();
}

void oslo::json_handler::block_info ()
{
	auto hash (hash_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		auto block (node.store.block_get (transaction, hash));
		if (block != nullptr)
		{
			oslo::account account (block->account ().is_zero () ? block->sideband ().account : block->account ());
			response_l.put ("block_account", account.to_account ());
			auto amount (node.ledger.amount (transaction, hash));
			response_l.put ("amount", amount.convert_to<std::string> ());
			auto balance (node.ledger.balance (transaction, hash));
			response_l.put ("balance", balance.convert_to<std::string> ());
			response_l.put ("height", std::to_string (block->sideband ().height));
			response_l.put ("local_timestamp", std::to_string (block->sideband ().timestamp));
			auto confirmed (node.ledger.block_confirmed (transaction, hash));
			response_l.put ("confirmed", confirmed);

			bool json_block_l = request.get<bool> ("json_block", false);
			if (json_block_l)
			{
				boost::property_tree::ptree block_node_l;
				block->serialize_json (block_node_l);
				response_l.add_child ("contents", block_node_l);
			}
			else
			{
				std::string contents;
				block->serialize_json (contents);
				response_l.put ("contents", contents);
			}
			if (block->type () == oslo::block_type::state)
			{
				auto subtype (oslo::state_subtype (block->sideband ().details));
				response_l.put ("subtype", subtype);
			}
		}
		else
		{
			ec = oslo::error_blocks::not_found;
		}
	}
	response_errors ();
}

void oslo::json_handler::block_confirm ()
{
	auto hash (hash_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		auto block_l (node.store.block_get (transaction, hash));
		if (block_l != nullptr)
		{
			if (!node.ledger.block_confirmed (transaction, hash))
			{
				// Start new confirmation for unconfirmed (or not being confirmed) block
				if (!node.confirmation_height_processor.is_processing_block (hash))
				{
					node.block_confirm (std::move (block_l));
				}
			}
			else
			{
				// Add record in confirmation history for confirmed block
				oslo::election_status status{ block_l, 0, std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now ().time_since_epoch ()), std::chrono::duration_values<std::chrono::milliseconds>::zero (), 0, 1, 0, oslo::election_status_type::active_confirmation_height };
				{
					oslo::lock_guard<std::mutex> lock (node.active.mutex);
					node.active.add_recently_cemented (status);
				}
				// Trigger callback for confirmed block
				node.block_arrival.add (hash);
				auto account (node.ledger.account (transaction, hash));
				auto amount (node.ledger.amount (transaction, hash));
				bool is_state_send (false);
				if (auto state = dynamic_cast<oslo::state_block *> (block_l.get ()))
				{
					is_state_send = node.ledger.is_send (transaction, *state);
				}
				node.observers.blocks.notify (status, account, amount, is_state_send);
			}
			response_l.put ("started", "1");
		}
		else
		{
			ec = oslo::error_blocks::not_found;
		}
	}
	response_errors ();
}

void oslo::json_handler::blocks ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	boost::property_tree::ptree blocks;
	auto transaction (node.store.tx_begin_read ());
	for (boost::property_tree::ptree::value_type & hashes : request.get_child ("hashes"))
	{
		if (!ec)
		{
			std::string hash_text = hashes.second.data ();
			oslo::block_hash hash;
			if (!hash.decode_hex (hash_text))
			{
				auto block (node.store.block_get (transaction, hash));
				if (block != nullptr)
				{
					if (json_block_l)
					{
						boost::property_tree::ptree block_node_l;
						block->serialize_json (block_node_l);
						blocks.add_child (hash_text, block_node_l);
					}
					else
					{
						std::string contents;
						block->serialize_json (contents);
						blocks.put (hash_text, contents);
					}
				}
				else
				{
					ec = oslo::error_blocks::not_found;
				}
			}
			else
			{
				ec = oslo::error_blocks::bad_hash_number;
			}
		}
	}
	response_l.add_child ("blocks", blocks);
	response_errors ();
}

void oslo::json_handler::blocks_info ()
{
	const bool pending = request.get<bool> ("pending", false);
	const bool source = request.get<bool> ("source", false);
	const bool json_block_l = request.get<bool> ("json_block", false);
	const bool include_not_found = request.get<bool> ("include_not_found", false);

	boost::property_tree::ptree blocks;
	boost::property_tree::ptree blocks_not_found;
	auto transaction (node.store.tx_begin_read ());
	for (boost::property_tree::ptree::value_type & hashes : request.get_child ("hashes"))
	{
		if (!ec)
		{
			std::string hash_text = hashes.second.data ();
			oslo::block_hash hash;
			if (!hash.decode_hex (hash_text))
			{
				auto block (node.store.block_get (transaction, hash));
				if (block != nullptr)
				{
					boost::property_tree::ptree entry;
					oslo::account account (block->account ().is_zero () ? block->sideband ().account : block->account ());
					entry.put ("block_account", account.to_account ());
					auto amount (node.ledger.amount (transaction, hash));
					entry.put ("amount", amount.convert_to<std::string> ());
					auto balance (node.ledger.balance (transaction, hash));
					entry.put ("balance", balance.convert_to<std::string> ());
					entry.put ("height", std::to_string (block->sideband ().height));
					entry.put ("local_timestamp", std::to_string (block->sideband ().timestamp));
					auto confirmed (node.ledger.block_confirmed (transaction, hash));
					entry.put ("confirmed", confirmed);

					if (json_block_l)
					{
						boost::property_tree::ptree block_node_l;
						block->serialize_json (block_node_l);
						entry.add_child ("contents", block_node_l);
					}
					else
					{
						std::string contents;
						block->serialize_json (contents);
						entry.put ("contents", contents);
					}
					if (block->type () == oslo::block_type::state)
					{
						auto subtype (oslo::state_subtype (block->sideband ().details));
						entry.put ("subtype", subtype);
					}
					if (pending)
					{
						bool exists (false);
						auto destination (node.ledger.block_destination (transaction, *block));
						if (!destination.is_zero ())
						{
							exists = node.store.pending_exists (transaction, oslo::pending_key (destination, hash));
						}
						entry.put ("pending", exists ? "1" : "0");
					}
					if (source)
					{
						oslo::block_hash source_hash (node.ledger.block_source (transaction, *block));
						auto block_a (node.store.block_get (transaction, source_hash));
						if (block_a != nullptr)
						{
							auto source_account (node.ledger.account (transaction, source_hash));
							entry.put ("source_account", source_account.to_account ());
						}
						else
						{
							entry.put ("source_account", "0");
						}
					}
					blocks.push_back (std::make_pair (hash_text, entry));
				}
				else if (include_not_found)
				{
					boost::property_tree::ptree entry;
					entry.put ("", hash_text);
					blocks_not_found.push_back (std::make_pair ("", entry));
				}
				else
				{
					ec = oslo::error_blocks::not_found;
				}
			}
			else
			{
				ec = oslo::error_blocks::bad_hash_number;
			}
		}
	}
	if (!ec)
	{
		response_l.add_child ("blocks", blocks);
		if (include_not_found)
		{
			response_l.add_child ("blocks_not_found", blocks_not_found);
		}
	}
	response_errors ();
}

void oslo::json_handler::block_account ()
{
	auto hash (hash_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		if (node.store.block_exists (transaction, hash))
		{
			auto account (node.ledger.account (transaction, hash));
			response_l.put ("account", account.to_account ());
		}
		else
		{
			ec = oslo::error_blocks::not_found;
		}
	}
	response_errors ();
}

void oslo::json_handler::block_count ()
{
	response_l.put ("count", std::to_string (node.ledger.cache.block_count));
	response_l.put ("unchecked", std::to_string (node.ledger.cache.unchecked_count));
	response_l.put ("cemented", std::to_string (node.ledger.cache.cemented_count));
	response_errors ();
}

void oslo::json_handler::block_count_type ()
{
	auto transaction (node.store.tx_begin_read ());
	oslo::block_counts count (node.store.block_count (transaction));
	response_l.put ("send", std::to_string (count.send));
	response_l.put ("receive", std::to_string (count.receive));
	response_l.put ("open", std::to_string (count.open));
	response_l.put ("change", std::to_string (count.change));
	response_l.put ("state", std::to_string (count.state));
	response_errors ();
}

void oslo::json_handler::block_create ()
{
	std::string type (request.get<std::string> ("type"));
	oslo::wallet_id wallet (0);
	// Default to work_1 if not specified
	auto work_version (work_version_optional_impl (oslo::work_version::work_1));
	auto difficulty_l (difficulty_optional_impl (work_version));
	boost::optional<std::string> wallet_text (request.get_optional<std::string> ("wallet"));
	if (!ec && wallet_text.is_initialized ())
	{
		if (wallet.decode_hex (wallet_text.get ()))
		{
			ec = oslo::error_common::bad_wallet_number;
		}
	}
	oslo::account account (0);
	boost::optional<std::string> account_text (request.get_optional<std::string> ("account"));
	if (!ec && account_text.is_initialized ())
	{
		account = account_impl (account_text.get ());
	}
	oslo::account representative (0);
	boost::optional<std::string> representative_text (request.get_optional<std::string> ("representative"));
	if (!ec && representative_text.is_initialized ())
	{
		representative = account_impl (representative_text.get (), oslo::error_rpc::bad_representative_number);
	}
	oslo::account destination (0);
	boost::optional<std::string> destination_text (request.get_optional<std::string> ("destination"));
	if (!ec && destination_text.is_initialized ())
	{
		destination = account_impl (destination_text.get (), oslo::error_rpc::bad_destination);
	}
	oslo::block_hash source (0);
	boost::optional<std::string> source_text (request.get_optional<std::string> ("source"));
	if (!ec && source_text.is_initialized ())
	{
		if (source.decode_hex (source_text.get ()))
		{
			ec = oslo::error_rpc::bad_source;
		}
	}
	oslo::amount amount (0);
	boost::optional<std::string> amount_text (request.get_optional<std::string> ("amount"));
	if (!ec && amount_text.is_initialized ())
	{
		if (amount.decode_dec (amount_text.get ()))
		{
			ec = oslo::error_common::invalid_amount;
		}
	}
	auto work (work_optional_impl ());
	oslo::raw_key prv;
	prv.data.clear ();
	oslo::block_hash previous (0);
	oslo::amount balance (0);
	if (work == 0 && !node.work_generation_enabled ())
	{
		ec = oslo::error_common::disabled_work_generation;
	}
	if (!ec && wallet != 0 && account != 0)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			auto transaction (node.wallets.tx_begin_read ());
			auto block_transaction (node.store.tx_begin_read ());
			wallet_locked_impl (transaction, existing->second);
			wallet_account_impl (transaction, existing->second, account);
			if (!ec)
			{
				existing->second->store.fetch (transaction, account, prv);
				previous = node.ledger.latest (block_transaction, account);
				balance = node.ledger.account_balance (block_transaction, account);
			}
		}
		else
		{
			ec = oslo::error_common::wallet_not_found;
		}
	}
	boost::optional<std::string> key_text (request.get_optional<std::string> ("key"));
	if (!ec && key_text.is_initialized ())
	{
		if (prv.data.decode_hex (key_text.get ()))
		{
			ec = oslo::error_common::bad_private_key;
		}
	}
	boost::optional<std::string> previous_text (request.get_optional<std::string> ("previous"));
	if (!ec && previous_text.is_initialized ())
	{
		if (previous.decode_hex (previous_text.get ()))
		{
			ec = oslo::error_rpc::bad_previous;
		}
	}
	boost::optional<std::string> balance_text (request.get_optional<std::string> ("balance"));
	if (!ec && balance_text.is_initialized ())
	{
		if (balance.decode_dec (balance_text.get ()))
		{
			ec = oslo::error_rpc::invalid_balance;
		}
	}
	oslo::link link (0);
	boost::optional<std::string> link_text (request.get_optional<std::string> ("link"));
	if (!ec && link_text.is_initialized ())
	{
		if (link.decode_account (link_text.get ()))
		{
			if (link.decode_hex (link_text.get ()))
			{
				ec = oslo::error_rpc::bad_link;
			}
		}
	}
	else
	{
		// Retrieve link from source or destination
		if (source.is_zero ())
		{
			link = destination;
		}
		else
		{
			link = source;
		}
	}
	if (!ec)
	{
		auto rpc_l (shared_from_this ());
		// Serializes the block contents to the RPC response
		auto block_response_put_l = [rpc_l, this](oslo::block const & block_a) {
			boost::property_tree::ptree response_l;
			response_l.put ("hash", block_a.hash ().to_string ());
			response_l.put ("difficulty", oslo::to_string_hex (block_a.difficulty ()));
			bool json_block_l = request.get<bool> ("json_block", false);
			if (json_block_l)
			{
				boost::property_tree::ptree block_node_l;
				block_a.serialize_json (block_node_l);
				response_l.add_child ("block", block_node_l);
			}
			else
			{
				std::string contents;
				block_a.serialize_json (contents);
				response_l.put ("block", contents);
			}
			std::stringstream ostream;
			boost::property_tree::write_json (ostream, response_l);
			rpc_l->response (ostream.str ());
		};
		// Wrapper from argument to lambda capture, to extend the block's scope
		auto get_callback_l = [rpc_l, block_response_put_l](std::shared_ptr<oslo::block> block_a) {
			// Callback upon work generation success or failure
			return [block_a, rpc_l, block_response_put_l](boost::optional<uint64_t> const & work_a) {
				if (block_a != nullptr)
				{
					if (work_a.is_initialized ())
					{
						block_a->block_work_set (*work_a);
						block_response_put_l (*block_a);
					}
					else
					{
						rpc_l->ec = oslo::error_common::failure_work_generation;
					}
				}
				else
				{
					rpc_l->ec = oslo::error_common::generic;
				}
				if (rpc_l->ec)
				{
					rpc_l->response_errors ();
				}
			};
		};
		if (prv.data != 0)
		{
			oslo::account pub (oslo::pub_key (prv.as_private_key ()));
			// Fetching account balance & previous for send blocks (if aren't given directly)
			if (!previous_text.is_initialized () && !balance_text.is_initialized ())
			{
				auto transaction (node.store.tx_begin_read ());
				previous = node.ledger.latest (transaction, pub);
				balance = node.ledger.account_balance (transaction, pub);
			}
			// Double check current balance if previous block is specified
			else if (previous_text.is_initialized () && balance_text.is_initialized () && type == "send")
			{
				auto transaction (node.store.tx_begin_read ());
				if (node.store.block_exists (transaction, previous) && node.store.block_balance (transaction, previous) != balance.number ())
				{
					ec = oslo::error_rpc::block_create_balance_mismatch;
				}
			}
			// Check for incorrect account key
			if (!ec && account_text.is_initialized ())
			{
				if (account != pub)
				{
					ec = oslo::error_rpc::block_create_public_key_mismatch;
				}
			}
			oslo::block_builder builder_l;
			std::shared_ptr<oslo::block> block_l{ nullptr };
			oslo::root root_l;
			std::error_code ec_build;
			if (type == "state")
			{
				if (previous_text.is_initialized () && !representative.is_zero () && (!link.is_zero () || link_text.is_initialized ()))
				{
					block_l = builder_l.state ()
					          .account (pub)
					          .previous (previous)
					          .representative (representative)
					          .balance (balance)
					          .link (link)
					          .sign (prv, pub)
					          .build (ec_build);
					if (previous.is_zero ())
					{
						root_l = pub;
					}
					else
					{
						root_l = previous;
					}
				}
				else
				{
					ec = oslo::error_rpc::block_create_requirements_state;
				}
			}
			else if (type == "open")
			{
				if (representative != 0 && source != 0)
				{
					block_l = builder_l.open ()
					          .account (pub)
					          .source (source)
					          .representative (representative)
					          .sign (prv, pub)
					          .build (ec_build);
					root_l = pub;
				}
				else
				{
					ec = oslo::error_rpc::block_create_requirements_open;
				}
			}
			else if (type == "receive")
			{
				if (source != 0 && previous != 0)
				{
					block_l = builder_l.receive ()
					          .previous (previous)
					          .source (source)
					          .sign (prv, pub)
					          .build (ec_build);
					root_l = previous;
				}
				else
				{
					ec = oslo::error_rpc::block_create_requirements_receive;
				}
			}
			else if (type == "change")
			{
				if (representative != 0 && previous != 0)
				{
					block_l = builder_l.change ()
					          .previous (previous)
					          .representative (representative)
					          .sign (prv, pub)
					          .build (ec_build);
					root_l = previous;
				}
				else
				{
					ec = oslo::error_rpc::block_create_requirements_change;
				}
			}
			else if (type == "send")
			{
				if (destination != 0 && previous != 0 && balance != 0 && amount != 0)
				{
					if (balance.number () >= amount.number ())
					{
						block_l = builder_l.send ()
						          .previous (previous)
						          .destination (destination)
						          .balance (balance.number () - amount.number ())
						          .sign (prv, pub)
						          .build (ec_build);
						root_l = previous;
					}
					else
					{
						ec = oslo::error_common::insufficient_balance;
					}
				}
				else
				{
					ec = oslo::error_rpc::block_create_requirements_send;
				}
			}
			else
			{
				ec = oslo::error_blocks::invalid_type;
			}
			if (!ec && (!ec_build || ec_build == oslo::error_common::missing_work))
			{
				if (work == 0)
				{
					// Difficulty calculation
					if (request.count ("difficulty") == 0)
					{
						difficulty_l = difficulty_ledger (*block_l);
					}
					node.work_generate (work_version, root_l, difficulty_l, get_callback_l (block_l), oslo::account (pub));
				}
				else
				{
					block_l->block_work_set (work);
					block_response_put_l (*block_l);
				}
			}
		}
		else
		{
			ec = oslo::error_rpc::block_create_key_required;
		}
	}
	// Because of callback
	if (ec)
	{
		response_errors ();
	}
}

void oslo::json_handler::block_hash ()
{
	auto block (block_impl (true));

	if (!ec)
	{
		response_l.put ("hash", block->hash ().to_string ());
	}
	response_errors ();
}

void oslo::json_handler::bootstrap ()
{
	std::string address_text = request.get<std::string> ("address");
	std::string port_text = request.get<std::string> ("port");
	const bool bypass_frontier_confirmation = request.get<bool> ("bypass_frontier_confirmation", false);
	boost::system::error_code address_ec;
	auto address (boost::asio::ip::make_address_v6 (address_text, address_ec));
	if (!address_ec)
	{
		uint16_t port;
		if (!oslo::parse_port (port_text, port))
		{
			if (!node.flags.disable_legacy_bootstrap)
			{
				std::string bootstrap_id (request.get<std::string> ("id", ""));
				node.bootstrap_initiator.bootstrap (oslo::endpoint (address, port), true, bypass_frontier_confirmation, bootstrap_id);
				response_l.put ("success", "");
			}
			else
			{
				ec = oslo::error_rpc::disabled_bootstrap_legacy;
			}
		}
		else
		{
			ec = oslo::error_common::invalid_port;
		}
	}
	else
	{
		ec = oslo::error_common::invalid_ip_address;
	}
	response_errors ();
}

void oslo::json_handler::bootstrap_any ()
{
	const bool force = request.get<bool> ("force", false);
	if (!node.flags.disable_legacy_bootstrap)
	{
		std::string bootstrap_id (request.get<std::string> ("id", ""));
		node.bootstrap_initiator.bootstrap (force, bootstrap_id);
		response_l.put ("success", "");
	}
	else
	{
		ec = oslo::error_rpc::disabled_bootstrap_legacy;
	}
	response_errors ();
}

void oslo::json_handler::bootstrap_lazy ()
{
	auto hash (hash_impl ());
	const bool force = request.get<bool> ("force", false);
	if (!ec)
	{
		if (!node.flags.disable_lazy_bootstrap)
		{
			std::string bootstrap_id (request.get<std::string> ("id", ""));
			node.bootstrap_initiator.bootstrap_lazy (hash, force, true, bootstrap_id);
			response_l.put ("started", "1");
		}
		else
		{
			ec = oslo::error_rpc::disabled_bootstrap_lazy;
		}
	}
	response_errors ();
}

/*
 * @warning This is an internal/diagnostic RPC, do not rely on its interface being stable
 */
void oslo::json_handler::bootstrap_status ()
{
	auto attempts_count (node.bootstrap_initiator.attempts.size ());
	response_l.put ("bootstrap_threads", std::to_string (node.config.bootstrap_initiator_threads));
	response_l.put ("running_attempts_count", std::to_string (attempts_count));
	response_l.put ("total_attempts_count", std::to_string (node.bootstrap_initiator.attempts.incremental));
	boost::property_tree::ptree connections;
	{
		oslo::lock_guard<std::mutex> connections_lock (node.bootstrap_initiator.connections->mutex);
		connections.put ("clients", std::to_string (node.bootstrap_initiator.connections->clients.size ()));
		connections.put ("connections", std::to_string (node.bootstrap_initiator.connections->connections_count));
		connections.put ("idle", std::to_string (node.bootstrap_initiator.connections->idle.size ()));
		connections.put ("target_connections", std::to_string (node.bootstrap_initiator.connections->target_connections (node.bootstrap_initiator.connections->pulls.size (), attempts_count)));
		connections.put ("pulls", std::to_string (node.bootstrap_initiator.connections->pulls.size ()));
	}
	response_l.add_child ("connections", connections);
	boost::property_tree::ptree attempts;
	{
		oslo::lock_guard<std::mutex> attempts_lock (node.bootstrap_initiator.attempts.bootstrap_attempts_mutex);
		for (auto i : node.bootstrap_initiator.attempts.attempts)
		{
			boost::property_tree::ptree entry;
			auto & attempt (i.second);
			entry.put ("id", attempt->id);
			entry.put ("mode", attempt->mode_text ());
			entry.put ("started", static_cast<bool> (attempt->started));
			entry.put ("pulling", std::to_string (attempt->pulling));
			entry.put ("total_blocks", std::to_string (attempt->total_blocks));
			entry.put ("requeued_pulls", std::to_string (attempt->requeued_pulls));
			attempt->get_information (entry);
			entry.put ("duration", std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now () - attempt->attempt_start).count ());
			attempts.push_back (std::make_pair ("", entry));
		}
	}
	response_l.add_child ("attempts", attempts);
	response_errors ();
}

void oslo::json_handler::chain (bool successors)
{
	successors = successors != request.get<bool> ("reverse", false);
	auto hash (hash_impl ("block"));
	auto count (count_impl ());
	auto offset (offset_optional_impl (0));
	if (!ec)
	{
		boost::property_tree::ptree blocks;
		auto transaction (node.store.tx_begin_read ());
		while (!hash.is_zero () && blocks.size () < count)
		{
			auto block_l (node.store.block_get (transaction, hash));
			if (block_l != nullptr)
			{
				if (offset > 0)
				{
					--offset;
				}
				else
				{
					boost::property_tree::ptree entry;
					entry.put ("", hash.to_string ());
					blocks.push_back (std::make_pair ("", entry));
				}
				hash = successors ? node.store.block_successor (transaction, hash) : block_l->previous ();
			}
			else
			{
				hash.clear ();
			}
		}
		response_l.add_child ("blocks", blocks);
	}
	response_errors ();
}

void oslo::json_handler::confirmation_active ()
{
	uint64_t announcements (0);
	uint64_t confirmed (0);
	boost::optional<std::string> announcements_text (request.get_optional<std::string> ("announcements"));
	if (announcements_text.is_initialized ())
	{
		announcements = strtoul (announcements_text.get ().c_str (), NULL, 10);
	}
	boost::property_tree::ptree elections;
	{
		oslo::lock_guard<std::mutex> lock (node.active.mutex);
		for (auto i (node.active.roots.begin ()), n (node.active.roots.end ()); i != n; ++i)
		{
			if (i->election->confirmation_request_count >= announcements)
			{
				if (!i->election->confirmed ())
				{
					boost::property_tree::ptree entry;
					entry.put ("", i->root.to_string ());
					elections.push_back (std::make_pair ("", entry));
				}
				else
				{
					++confirmed;
				}
			}
		}
	}
	response_l.add_child ("confirmations", elections);
	response_l.put ("unconfirmed", elections.size ());
	response_l.put ("confirmed", confirmed);
	response_errors ();
}

void oslo::json_handler::confirmation_height_currently_processing ()
{
	auto hash = node.confirmation_height_processor.current ();
	if (!hash.is_zero ())
	{
		response_l.put ("hash", hash.to_string ());
	}
	else
	{
		ec = oslo::error_rpc::confirmation_height_not_processing;
	}
	response_errors ();
}

void oslo::json_handler::confirmation_history ()
{
	boost::property_tree::ptree elections;
	boost::property_tree::ptree confirmation_stats;
	std::chrono::milliseconds running_total (0);
	oslo::block_hash hash (0);
	boost::optional<std::string> hash_text (request.get_optional<std::string> ("hash"));
	if (hash_text.is_initialized ())
	{
		hash = hash_impl ();
	}
	if (!ec)
	{
		auto confirmed (node.active.list_recently_cemented ());
		for (auto i (confirmed.begin ()), n (confirmed.end ()); i != n; ++i)
		{
			if (hash.is_zero () || i->winner->hash () == hash)
			{
				boost::property_tree::ptree election;
				election.put ("hash", i->winner->hash ().to_string ());
				election.put ("duration", i->election_duration.count ());
				election.put ("time", i->election_end.count ());
				election.put ("tally", i->tally.to_string_dec ());
				election.put ("blocks", std::to_string (i->block_count));
				election.put ("voters", std::to_string (i->voter_count));
				election.put ("request_count", std::to_string (i->confirmation_request_count));
				elections.push_back (std::make_pair ("", election));
			}
			running_total += i->election_duration;
		}
	}
	confirmation_stats.put ("count", elections.size ());
	if (elections.size () >= 1)
	{
		confirmation_stats.put ("average", (running_total.count ()) / elections.size ());
	}
	response_l.add_child ("confirmation_stats", confirmation_stats);
	response_l.add_child ("confirmations", elections);
	response_errors ();
}

void oslo::json_handler::confirmation_info ()
{
	const bool representatives = request.get<bool> ("representatives", false);
	const bool contents = request.get<bool> ("contents", true);
	const bool json_block_l = request.get<bool> ("json_block", false);
	std::string root_text (request.get<std::string> ("root"));
	oslo::qualified_root root;
	if (!root.decode_hex (root_text))
	{
		auto election (node.active.election (root));
		oslo::lock_guard<std::mutex> guard (node.active.mutex);
		if (election != nullptr && !election->confirmed ())
		{
			response_l.put ("announcements", std::to_string (election->confirmation_request_count));
			response_l.put ("voters", std::to_string (election->last_votes.size ()));
			response_l.put ("last_winner", election->status.winner->hash ().to_string ());
			oslo::uint128_t total (0);
			auto tally_l (election->tally ());
			boost::property_tree::ptree blocks;
			for (auto i (tally_l.begin ()), n (tally_l.end ()); i != n; ++i)
			{
				boost::property_tree::ptree entry;
				auto const & tally (i->first);
				entry.put ("tally", tally.convert_to<std::string> ());
				total += tally;
				if (contents)
				{
					if (json_block_l)
					{
						boost::property_tree::ptree block_node_l;
						i->second->serialize_json (block_node_l);
						entry.add_child ("contents", block_node_l);
					}
					else
					{
						std::string contents;
						i->second->serialize_json (contents);
						entry.put ("contents", contents);
					}
				}
				if (representatives)
				{
					std::multimap<oslo::uint128_t, oslo::account, std::greater<oslo::uint128_t>> representatives;
					for (auto ii (election->last_votes.begin ()), nn (election->last_votes.end ()); ii != nn; ++ii)
					{
						if (i->second->hash () == ii->second.hash)
						{
							oslo::account const & representative (ii->first);
							auto amount (node.ledger.cache.rep_weights.representation_get (representative));
							representatives.emplace (std::move (amount), representative);
						}
					}
					boost::property_tree::ptree representatives_list;
					for (auto ii (representatives.begin ()), nn (representatives.end ()); ii != nn; ++ii)
					{
						representatives_list.put (ii->second.to_account (), ii->first.convert_to<std::string> ());
					}
					entry.add_child ("representatives", representatives_list);
				}
				blocks.add_child ((i->second->hash ()).to_string (), entry);
			}
			response_l.put ("total_tally", total.convert_to<std::string> ());
			response_l.add_child ("blocks", blocks);
		}
		else
		{
			ec = oslo::error_rpc::confirmation_not_found;
		}
	}
	else
	{
		ec = oslo::error_rpc::invalid_root;
	}
	response_errors ();
}

void oslo::json_handler::confirmation_quorum ()
{
	response_l.put ("quorum_delta", node.delta ().convert_to<std::string> ());
	response_l.put ("online_weight_quorum_percent", std::to_string (node.config.online_weight_quorum));
	response_l.put ("online_weight_minimum", node.config.online_weight_minimum.to_string_dec ());
	response_l.put ("online_stake_total", node.online_reps.online_stake ().convert_to<std::string> ());
	response_l.put ("peers_stake_total", node.rep_crawler.total_weight ().convert_to<std::string> ());
	response_l.put ("peers_stake_required", std::max (node.config.online_weight_minimum.number (), node.delta ()).convert_to<std::string> ());
	if (request.get<bool> ("peer_details", false))
	{
		boost::property_tree::ptree peers;
		for (auto & peer : node.rep_crawler.representatives ())
		{
			boost::property_tree::ptree peer_node;
			peer_node.put ("account", peer.account.to_account ());
			peer_node.put ("ip", peer.channel->to_string ());
			peer_node.put ("weight", peer.weight.to_string_dec ());
			peers.push_back (std::make_pair ("", peer_node));
		}
		response_l.add_child ("peers", peers);
	}
	response_errors ();
}

void oslo::json_handler::database_txn_tracker ()
{
	boost::property_tree::ptree json;

	if (node.config.diagnostics_config.txn_tracking.enable)
	{
		unsigned min_read_time_milliseconds = 0;
		boost::optional<std::string> min_read_time_text (request.get_optional<std::string> ("min_read_time"));
		if (min_read_time_text.is_initialized ())
		{
			auto success = boost::conversion::try_lexical_convert<unsigned> (*min_read_time_text, min_read_time_milliseconds);
			if (!success)
			{
				ec = oslo::error_common::invalid_amount;
			}
		}

		unsigned min_write_time_milliseconds = 0;
		if (!ec)
		{
			boost::optional<std::string> min_write_time_text (request.get_optional<std::string> ("min_write_time"));
			if (min_write_time_text.is_initialized ())
			{
				auto success = boost::conversion::try_lexical_convert<unsigned> (*min_write_time_text, min_write_time_milliseconds);
				if (!success)
				{
					ec = oslo::error_common::invalid_amount;
				}
			}
		}

		if (!ec)
		{
			node.store.serialize_mdb_tracker (json, std::chrono::milliseconds (min_read_time_milliseconds), std::chrono::milliseconds (min_write_time_milliseconds));
			response_l.put_child ("txn_tracking", json);
		}
	}
	else
	{
		ec = oslo::error_common::tracking_not_enabled;
	}

	response_errors ();
}

void oslo::json_handler::delegators ()
{
	auto account (account_impl ());
	if (!ec)
	{
		boost::property_tree::ptree delegators;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.latest_begin (transaction)), n (node.store.latest_end ()); i != n; ++i)
		{
			oslo::account_info const & info (i->second);
			if (info.representative == account)
			{
				std::string balance;
				oslo::uint128_union (info.balance).encode_dec (balance);
				oslo::account const & account (i->first);
				delegators.put (account.to_account (), balance);
			}
		}
		response_l.add_child ("delegators", delegators);
	}
	response_errors ();
}

void oslo::json_handler::delegators_count ()
{
	auto account (account_impl ());
	if (!ec)
	{
		uint64_t count (0);
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.latest_begin (transaction)), n (node.store.latest_end ()); i != n; ++i)
		{
			oslo::account_info const & info (i->second);
			if (info.representative == account)
			{
				++count;
			}
		}
		response_l.put ("count", std::to_string (count));
	}
	response_errors ();
}

void oslo::json_handler::deterministic_key ()
{
	std::string seed_text (request.get<std::string> ("seed"));
	std::string index_text (request.get<std::string> ("index"));
	oslo::raw_key seed;
	if (!seed.data.decode_hex (seed_text))
	{
		try
		{
			uint32_t index (std::stoul (index_text));
			oslo::private_key prv = oslo::deterministic_key (seed, index);
			oslo::public_key pub (oslo::pub_key (prv));
			response_l.put ("private", prv.to_string ());
			response_l.put ("public", pub.to_string ());
			response_l.put ("account", pub.to_account ());
		}
		catch (std::logic_error const &)
		{
			ec = oslo::error_common::invalid_index;
		}
	}
	else
	{
		ec = oslo::error_common::bad_seed;
	}
	response_errors ();
}

/*
 * @warning This is an internal/diagnostic RPC, do not rely on its interface being stable
 */
void oslo::json_handler::epoch_upgrade ()
{
	oslo::epoch epoch (oslo::epoch::invalid);
	uint8_t epoch_int (request.get<uint8_t> ("epoch"));
	switch (epoch_int)
	{
		case 1:
			epoch = oslo::epoch::epoch_1;
			break;
		case 2:
			epoch = oslo::epoch::epoch_2;
			break;
		default:
			break;
	}
	if (epoch != oslo::epoch::invalid)
	{
		uint64_t count_limit (count_optional_impl ());
		uint64_t threads (0);
		boost::optional<std::string> threads_text (request.get_optional<std::string> ("threads"));
		if (!ec && threads_text.is_initialized ())
		{
			if (decode_unsigned (threads_text.get (), threads))
			{
				ec = oslo::error_rpc::invalid_threads_count;
			}
		}
		std::string key_text (request.get<std::string> ("key"));
		oslo::private_key prv;
		if (!prv.decode_hex (key_text))
		{
			if (oslo::pub_key (prv) == node.ledger.epoch_signer (node.ledger.epoch_link (epoch)))
			{
				if (!node.epoch_upgrader (prv, epoch, count_limit, threads))
				{
					response_l.put ("started", "1");
				}
				else
				{
					response_l.put ("started", "0");
				}
			}
			else
			{
				ec = oslo::error_rpc::invalid_epoch_signer;
			}
		}
		else
		{
			ec = oslo::error_common::bad_private_key;
		}
	}
	else
	{
		ec = oslo::error_rpc::invalid_epoch;
	}
	response_errors ();
}

void oslo::json_handler::frontiers ()
{
	auto start (account_impl ());
	auto count (count_impl ());
	if (!ec)
	{
		boost::property_tree::ptree frontiers;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.latest_begin (transaction, start)), n (node.store.latest_end ()); i != n && frontiers.size () < count; ++i)
		{
			frontiers.put (i->first.to_account (), i->second.head.to_string ());
		}
		response_l.add_child ("frontiers", frontiers);
	}
	response_errors ();
}

void oslo::json_handler::account_count ()
{
	auto size (node.ledger.cache.account_count.load ());
	response_l.put ("count", std::to_string (size));
	response_errors ();
}

namespace
{
class history_visitor : public oslo::block_visitor
{
public:
	history_visitor (oslo::json_handler & handler_a, bool raw_a, oslo::transaction & transaction_a, boost::property_tree::ptree & tree_a, oslo::block_hash const & hash_a, std::vector<oslo::public_key> const & accounts_filter_a) :
	handler (handler_a),
	raw (raw_a),
	transaction (transaction_a),
	tree (tree_a),
	hash (hash_a),
	accounts_filter (accounts_filter_a)
	{
	}
	virtual ~history_visitor () = default;
	void send_block (oslo::send_block const & block_a)
	{
		if (should_ignore_account (block_a.hashables.destination))
		{
			return;
		}
		tree.put ("type", "send");
		auto account (block_a.hashables.destination.to_account ());
		tree.put ("account", account);
		auto amount (handler.node.ledger.amount (transaction, hash).convert_to<std::string> ());
		tree.put ("amount", amount);
		if (raw)
		{
			tree.put ("destination", account);
			tree.put ("balance", block_a.hashables.balance.to_string_dec ());
			tree.put ("previous", block_a.hashables.previous.to_string ());
		}
	}
	void receive_block (oslo::receive_block const & block_a)
	{
		tree.put ("type", "receive");
		auto account (handler.node.ledger.account (transaction, block_a.hashables.source).to_account ());
		tree.put ("account", account);
		auto amount (handler.node.ledger.amount (transaction, hash).convert_to<std::string> ());
		tree.put ("amount", amount);
		if (raw)
		{
			tree.put ("source", block_a.hashables.source.to_string ());
			tree.put ("previous", block_a.hashables.previous.to_string ());
		}
	}
	void open_block (oslo::open_block const & block_a)
	{
		if (raw)
		{
			tree.put ("type", "open");
			tree.put ("representative", block_a.hashables.representative.to_account ());
			tree.put ("source", block_a.hashables.source.to_string ());
			tree.put ("opened", block_a.hashables.account.to_account ());
		}
		else
		{
			// Report opens as a receive
			tree.put ("type", "receive");
		}
		if (block_a.hashables.source != network_params.ledger.genesis_account)
		{
			tree.put ("account", handler.node.ledger.account (transaction, block_a.hashables.source).to_account ());
			tree.put ("amount", handler.node.ledger.amount (transaction, hash).convert_to<std::string> ());
		}
		else
		{
			tree.put ("account", network_params.ledger.genesis_account.to_account ());
			tree.put ("amount", network_params.ledger.genesis_amount.convert_to<std::string> ());
		}
	}
	void change_block (oslo::change_block const & block_a)
	{
		if (raw && accounts_filter.empty ())
		{
			tree.put ("type", "change");
			tree.put ("representative", block_a.hashables.representative.to_account ());
			tree.put ("previous", block_a.hashables.previous.to_string ());
		}
	}
	void state_block (oslo::state_block const & block_a)
	{
		if (raw)
		{
			tree.put ("type", "state");
			tree.put ("representative", block_a.hashables.representative.to_account ());
			tree.put ("link", block_a.hashables.link.to_string ());
			tree.put ("balance", block_a.hashables.balance.to_string_dec ());
			tree.put ("previous", block_a.hashables.previous.to_string ());
		}
		auto balance (block_a.hashables.balance.number ());
		auto previous_balance (handler.node.ledger.balance (transaction, block_a.hashables.previous));
		if (balance < previous_balance)
		{
			if (should_ignore_account (block_a.hashables.link))
			{
				tree.clear ();
				return;
			}
			if (raw)
			{
				tree.put ("subtype", "send");
			}
			else
			{
				tree.put ("type", "send");
			}
			tree.put ("account", block_a.hashables.link.to_account ());
			tree.put ("amount", (previous_balance - balance).convert_to<std::string> ());
		}
		else
		{
			if (block_a.hashables.link.is_zero ())
			{
				if (raw && accounts_filter.empty ())
				{
					tree.put ("subtype", "change");
				}
			}
			else if (balance == previous_balance && handler.node.ledger.is_epoch_link (block_a.hashables.link))
			{
				if (raw && accounts_filter.empty ())
				{
					tree.put ("subtype", "epoch");
					tree.put ("account", handler.node.ledger.epoch_signer (block_a.link ()).to_account ());
				}
			}
			else
			{
				auto account (handler.node.ledger.account (transaction, block_a.hashables.link));
				if (should_ignore_account (account))
				{
					tree.clear ();
					return;
				}
				if (raw)
				{
					tree.put ("subtype", "receive");
				}
				else
				{
					tree.put ("type", "receive");
				}
				tree.put ("account", account.to_account ());
				tree.put ("amount", (balance - previous_balance).convert_to<std::string> ());
			}
		}
	}
	bool should_ignore_account (oslo::public_key const & account)
	{
		bool ignore (false);
		if (!accounts_filter.empty ())
		{
			if (std::find (accounts_filter.begin (), accounts_filter.end (), account) == accounts_filter.end ())
			{
				ignore = true;
			}
		}
		return ignore;
	}
	oslo::json_handler & handler;
	bool raw;
	oslo::transaction & transaction;
	boost::property_tree::ptree & tree;
	oslo::block_hash const & hash;
	oslo::network_params network_params;
	std::vector<oslo::public_key> const & accounts_filter;
};
}

void oslo::json_handler::account_history ()
{
	std::vector<oslo::public_key> accounts_to_filter;
	const auto accounts_filter_node = request.get_child_optional ("account_filter");
	if (accounts_filter_node.is_initialized ())
	{
		for (auto & a : (*accounts_filter_node))
		{
			auto account (account_impl (a.second.get<std::string> ("")));
			if (!ec)
			{
				accounts_to_filter.push_back (account);
			}
			else
			{
				break;
			}
		}
	}
	oslo::account account;
	oslo::block_hash hash;
	bool reverse (request.get_optional<bool> ("reverse") == true);
	auto head_str (request.get_optional<std::string> ("head"));
	auto transaction (node.store.tx_begin_read ());
	auto count (count_impl ());
	auto offset (offset_optional_impl (0));
	if (head_str)
	{
		if (!hash.decode_hex (*head_str))
		{
			if (node.store.block_exists (transaction, hash))
			{
				account = node.ledger.account (transaction, hash);
			}
			else
			{
				ec = oslo::error_blocks::not_found;
			}
		}
		else
		{
			ec = oslo::error_blocks::bad_hash_number;
		}
	}
	else
	{
		account = account_impl ();
		if (!ec)
		{
			if (reverse)
			{
				auto info (account_info_impl (transaction, account));
				if (!ec)
				{
					hash = info.open_block;
				}
			}
			else
			{
				hash = node.ledger.latest (transaction, account);
			}
		}
	}
	if (!ec)
	{
		boost::property_tree::ptree history;
		bool output_raw (request.get_optional<bool> ("raw") == true);
		response_l.put ("account", account.to_account ());
		auto block (node.store.block_get (transaction, hash));
		while (block != nullptr && count > 0)
		{
			if (offset > 0)
			{
				--offset;
			}
			else
			{
				boost::property_tree::ptree entry;
				history_visitor visitor (*this, output_raw, transaction, entry, hash, accounts_to_filter);
				block->visit (visitor);
				if (!entry.empty ())
				{
					entry.put ("local_timestamp", std::to_string (block->sideband ().timestamp));
					entry.put ("height", std::to_string (block->sideband ().height));
					entry.put ("hash", hash.to_string ());
					if (output_raw)
					{
						entry.put ("work", oslo::to_string_hex (block->block_work ()));
						entry.put ("signature", block->block_signature ().to_string ());
					}
					history.push_back (std::make_pair ("", entry));
					--count;
				}
			}
			hash = reverse ? node.store.block_successor (transaction, hash) : block->previous ();
			block = node.store.block_get (transaction, hash);
		}
		response_l.add_child ("history", history);
		if (!hash.is_zero ())
		{
			response_l.put (reverse ? "next" : "previous", hash.to_string ());
		}
	}
	response_errors ();
}

void oslo::json_handler::keepalive ()
{
	if (!ec)
	{
		std::string address_text (request.get<std::string> ("address"));
		std::string port_text (request.get<std::string> ("port"));
		uint16_t port;
		if (!oslo::parse_port (port_text, port))
		{
			node.keepalive (address_text, port);
			response_l.put ("started", "1");
		}
		else
		{
			ec = oslo::error_common::invalid_port;
		}
	}
	response_errors ();
}

void oslo::json_handler::key_create ()
{
	oslo::keypair pair;
	response_l.put ("private", pair.prv.data.to_string ());
	response_l.put ("public", pair.pub.to_string ());
	response_l.put ("account", pair.pub.to_account ());
	response_errors ();
}

void oslo::json_handler::key_expand ()
{
	std::string key_text (request.get<std::string> ("key"));
	oslo::private_key prv;
	if (!prv.decode_hex (key_text))
	{
		oslo::public_key pub (oslo::pub_key (prv));
		response_l.put ("private", prv.to_string ());
		response_l.put ("public", pub.to_string ());
		response_l.put ("account", pub.to_account ());
	}
	else
	{
		ec = oslo::error_common::bad_private_key;
	}
	response_errors ();
}

void oslo::json_handler::ledger ()
{
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	if (!ec)
	{
		oslo::account start (0);
		boost::optional<std::string> account_text (request.get_optional<std::string> ("account"));
		if (account_text.is_initialized ())
		{
			start = account_impl (account_text.get ());
		}
		uint64_t modified_since (0);
		boost::optional<std::string> modified_since_text (request.get_optional<std::string> ("modified_since"));
		if (modified_since_text.is_initialized ())
		{
			if (decode_unsigned (modified_since_text.get (), modified_since))
			{
				ec = oslo::error_rpc::invalid_timestamp;
			}
		}
		const bool sorting = request.get<bool> ("sorting", false);
		const bool representative = request.get<bool> ("representative", false);
		const bool weight = request.get<bool> ("weight", false);
		const bool pending = request.get<bool> ("pending", false);
		boost::property_tree::ptree accounts;
		auto transaction (node.store.tx_begin_read ());
		if (!ec && !sorting) // Simple
		{
			for (auto i (node.store.latest_begin (transaction, start)), n (node.store.latest_end ()); i != n && accounts.size () < count; ++i)
			{
				oslo::account_info const & info (i->second);
				if (info.modified >= modified_since && (pending || info.balance.number () >= threshold.number ()))
				{
					oslo::account const & account (i->first);
					boost::property_tree::ptree response_a;
					if (pending)
					{
						auto account_pending (node.ledger.account_pending (transaction, account));
						if (info.balance.number () + account_pending < threshold.number ())
						{
							continue;
						}
						response_a.put ("pending", account_pending.convert_to<std::string> ());
					}
					response_a.put ("frontier", info.head.to_string ());
					response_a.put ("open_block", info.open_block.to_string ());
					response_a.put ("representative_block", node.ledger.representative (transaction, info.head).to_string ());
					std::string balance;
					oslo::uint128_union (info.balance).encode_dec (balance);
					response_a.put ("balance", balance);
					response_a.put ("modified_timestamp", std::to_string (info.modified));
					response_a.put ("block_count", std::to_string (info.block_count));
					if (representative)
					{
						response_a.put ("representative", info.representative.to_account ());
					}
					if (weight)
					{
						auto account_weight (node.ledger.weight (account));
						response_a.put ("weight", account_weight.convert_to<std::string> ());
					}
					accounts.push_back (std::make_pair (account.to_account (), response_a));
				}
			}
		}
		else if (!ec) // Sorting
		{
			std::vector<std::pair<oslo::uint128_union, oslo::account>> ledger_l;
			for (auto i (node.store.latest_begin (transaction, start)), n (node.store.latest_end ()); i != n; ++i)
			{
				oslo::account_info const & info (i->second);
				oslo::uint128_union balance (info.balance);
				if (info.modified >= modified_since)
				{
					ledger_l.emplace_back (balance, i->first);
				}
			}
			std::sort (ledger_l.begin (), ledger_l.end ());
			std::reverse (ledger_l.begin (), ledger_l.end ());
			oslo::account_info info;
			for (auto i (ledger_l.begin ()), n (ledger_l.end ()); i != n && accounts.size () < count; ++i)
			{
				node.store.account_get (transaction, i->second, info);
				if (pending || info.balance.number () >= threshold.number ())
				{
					oslo::account const & account (i->second);
					boost::property_tree::ptree response_a;
					if (pending)
					{
						auto account_pending (node.ledger.account_pending (transaction, account));
						if (info.balance.number () + account_pending < threshold.number ())
						{
							continue;
						}
						response_a.put ("pending", account_pending.convert_to<std::string> ());
					}
					response_a.put ("frontier", info.head.to_string ());
					response_a.put ("open_block", info.open_block.to_string ());
					response_a.put ("representative_block", node.ledger.representative (transaction, info.head).to_string ());
					std::string balance;
					(i->first).encode_dec (balance);
					response_a.put ("balance", balance);
					response_a.put ("modified_timestamp", std::to_string (info.modified));
					response_a.put ("block_count", std::to_string (info.block_count));
					if (representative)
					{
						response_a.put ("representative", info.representative.to_account ());
					}
					if (weight)
					{
						auto account_weight (node.ledger.weight (account));
						response_a.put ("weight", account_weight.convert_to<std::string> ());
					}
					accounts.push_back (std::make_pair (account.to_account (), response_a));
				}
			}
		}
		response_l.add_child ("accounts", accounts);
	}
	response_errors ();
}

void oslo::json_handler::moslo_from_raw (oslo::uint128_t ratio)
{
	auto amount (amount_impl ());
	if (!ec)
	{
		auto result (amount.number () / ratio);
		response_l.put ("amount", result.convert_to<std::string> ());
	}
	response_errors ();
}

void oslo::json_handler::moslo_to_raw (oslo::uint128_t ratio)
{
	auto amount (amount_impl ());
	if (!ec)
	{
		auto result (amount.number () * ratio);
		if (result > amount.number ())
		{
			response_l.put ("amount", result.convert_to<std::string> ());
		}
		else
		{
			ec = oslo::error_common::invalid_amount_big;
		}
	}
	response_errors ();
}

/*
 * @warning This is an internal/diagnostic RPC, do not rely on its interface being stable
 */
void oslo::json_handler::node_id ()
{
	if (!ec)
	{
		response_l.put ("private", node.node_id.prv.data.to_string ());
		response_l.put ("public", node.node_id.pub.to_string ());
		response_l.put ("as_account", node.node_id.pub.to_account ());
		response_l.put ("node_id", node.node_id.pub.to_node_id ());
	}
	response_errors ();
}

/*
 * @warning This is an internal/diagnostic RPC, do not rely on its interface being stable
 */
void oslo::json_handler::node_id_delete ()
{
	response_l.put ("deprecated", "1");
	response_errors ();
}

void oslo::json_handler::password_change ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			rpc_l->wallet_locked_impl (transaction, wallet);
			if (!rpc_l->ec)
			{
				std::string password_text (rpc_l->request.get<std::string> ("password"));
				bool error (wallet->store.rekey (transaction, password_text));
				rpc_l->response_l.put ("changed", error ? "0" : "1");
				if (!error)
				{
					rpc_l->node.logger.try_log ("Wallet password changed");
				}
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::password_enter ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			std::string password_text (rpc_l->request.get<std::string> ("password"));
			auto transaction (wallet->wallets.tx_begin_write ());
			auto error (wallet->enter_password (transaction, password_text));
			rpc_l->response_l.put ("valid", error ? "0" : "1");
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::password_valid (bool wallet_locked)
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		auto valid (wallet->store.valid_password (transaction));
		if (!wallet_locked)
		{
			response_l.put ("valid", valid ? "1" : "0");
		}
		else
		{
			response_l.put ("locked", valid ? "0" : "1");
		}
	}
	response_errors ();
}

void oslo::json_handler::peers ()
{
	boost::property_tree::ptree peers_l;
	const bool peer_details = request.get<bool> ("peer_details", false);
	auto peers_list (node.network.list (std::numeric_limits<size_t>::max ()));
	std::sort (peers_list.begin (), peers_list.end (), [](const auto & lhs, const auto & rhs) {
		return lhs->get_endpoint () < rhs->get_endpoint ();
	});
	for (auto i (peers_list.begin ()), n (peers_list.end ()); i != n; ++i)
	{
		std::stringstream text;
		auto channel (*i);
		text << channel->to_string ();
		if (peer_details)
		{
			boost::property_tree::ptree pending_tree;
			pending_tree.put ("protocol_version", std::to_string (channel->get_network_version ()));
			auto node_id_l (channel->get_node_id_optional ());
			if (node_id_l.is_initialized ())
			{
				pending_tree.put ("node_id", node_id_l.get ().to_node_id ());
			}
			else
			{
				pending_tree.put ("node_id", "");
			}
			pending_tree.put ("type", channel->get_type () == oslo::transport::transport_type::tcp ? "tcp" : "udp");
			peers_l.push_back (boost::property_tree::ptree::value_type (text.str (), pending_tree));
		}
		else
		{
			peers_l.push_back (boost::property_tree::ptree::value_type (text.str (), boost::property_tree::ptree (std::to_string (channel->get_network_version ()))));
		}
	}
	response_l.add_child ("peers", peers_l);
	response_errors ();
}

void oslo::json_handler::pending ()
{
	auto account (account_impl ());
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	const bool source = request.get<bool> ("source", false);
	const bool min_version = request.get<bool> ("min_version", false);
	const bool include_active = request.get<bool> ("include_active", false);
	const bool include_only_confirmed = request.get<bool> ("include_only_confirmed", false);
	const bool sorting = request.get<bool> ("sorting", false);
	auto simple (threshold.is_zero () && !source && !min_version && !sorting); // if simple, response is a list of hashes
	if (!ec)
	{
		boost::property_tree::ptree peers_l;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.pending_begin (transaction, oslo::pending_key (account, 0))), n (node.store.pending_end ()); i != n && oslo::pending_key (i->first).account == account && peers_l.size () < count; ++i)
		{
			oslo::pending_key const & key (i->first);
			if (block_confirmed (node, transaction, key.hash, include_active, include_only_confirmed))
			{
				if (simple)
				{
					boost::property_tree::ptree entry;
					entry.put ("", key.hash.to_string ());
					peers_l.push_back (std::make_pair ("", entry));
				}
				else
				{
					oslo::pending_info const & info (i->second);
					if (info.amount.number () >= threshold.number ())
					{
						if (source || min_version)
						{
							boost::property_tree::ptree pending_tree;
							pending_tree.put ("amount", info.amount.number ().convert_to<std::string> ());
							if (source)
							{
								pending_tree.put ("source", info.source.to_account ());
							}
							if (min_version)
							{
								pending_tree.put ("min_version", epoch_as_string (info.epoch));
							}
							peers_l.add_child (key.hash.to_string (), pending_tree);
						}
						else
						{
							peers_l.put (key.hash.to_string (), info.amount.number ().convert_to<std::string> ());
						}
					}
				}
			}
		}
		if (sorting && !simple)
		{
			if (source || min_version)
			{
				peers_l.sort ([](const auto & child1, const auto & child2) -> bool {
					return child1.second.template get<oslo::uint128_t> ("amount") > child2.second.template get<oslo::uint128_t> ("amount");
				});
			}
			else
			{
				peers_l.sort ([](const auto & child1, const auto & child2) -> bool {
					return child1.second.template get<oslo::uint128_t> ("") > child2.second.template get<oslo::uint128_t> ("");
				});
			}
		}
		response_l.add_child ("blocks", peers_l);
	}
	response_errors ();
}

void oslo::json_handler::pending_exists ()
{
	auto hash (hash_impl ());
	const bool include_active = request.get<bool> ("include_active", false);
	const bool include_only_confirmed = request.get<bool> ("include_only_confirmed", false);
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		auto block (node.store.block_get (transaction, hash));
		if (block != nullptr)
		{
			auto exists (false);
			auto destination (node.ledger.block_destination (transaction, *block));
			if (!destination.is_zero ())
			{
				exists = node.store.pending_exists (transaction, oslo::pending_key (destination, hash));
			}
			exists = exists && (block_confirmed (node, transaction, block->hash (), include_active, include_only_confirmed));
			response_l.put ("exists", exists ? "1" : "0");
		}
		else
		{
			ec = oslo::error_blocks::not_found;
		}
	}
	response_errors ();
}

void oslo::json_handler::payment_begin ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		std::string id_text (rpc_l->request.get<std::string> ("wallet"));
		oslo::wallet_id id;
		if (!id.decode_hex (id_text))
		{
			auto existing (rpc_l->node.wallets.items.find (id));
			if (existing != rpc_l->node.wallets.items.end ())
			{
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				std::shared_ptr<oslo::wallet> wallet (existing->second);
				if (wallet->store.valid_password (transaction))
				{
					oslo::account account (0);
					do
					{
						auto existing (wallet->free_accounts.begin ());
						if (existing != wallet->free_accounts.end ())
						{
							account = *existing;
							wallet->free_accounts.erase (existing);
							if (wallet->store.find (transaction, account) == wallet->store.end ())
							{
								rpc_l->node.logger.always_log (boost::str (boost::format ("Transaction wallet %1% externally modified listing account %2% as free but no longer exists") % id.to_string () % account.to_account ()));
								account.clear ();
							}
							else
							{
								auto block_transaction (rpc_l->node.store.tx_begin_read ());
								if (!rpc_l->node.ledger.account_balance (block_transaction, account).is_zero ())
								{
									rpc_l->node.logger.always_log (boost::str (boost::format ("Skipping account %1% for use as a transaction account: non-zero balance") % account.to_account ()));
									account.clear ();
								}
							}
						}
						else
						{
							account = wallet->deterministic_insert (transaction);
							break;
						}
					} while (account.is_zero ());
					if (!account.is_zero ())
					{
						rpc_l->response_l.put ("deprecated", "1");
						rpc_l->response_l.put ("account", account.to_account ());
					}
					else
					{
						rpc_l->ec = oslo::error_rpc::payment_unable_create_account;
					}
				}
				else
				{
					rpc_l->ec = oslo::error_common::wallet_locked;
				}
			}
			else
			{
				rpc_l->ec = oslo::error_common::wallet_not_found;
			}
		}
		else
		{
			rpc_l->ec = oslo::error_common::bad_wallet_number;
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::payment_init ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			if (wallet->store.valid_password (transaction))
			{
				wallet->init_free_accounts (transaction);
				rpc_l->response_l.put ("deprecated", "1");
				rpc_l->response_l.put ("status", "Ready");
			}
			else
			{
				rpc_l->ec = oslo::error_common::wallet_locked;
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::payment_end ()
{
	auto account (account_impl ());
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		wallet_account_impl (transaction, wallet, account);
		if (!ec)
		{
			if (node.ledger.account_balance (block_transaction, account).is_zero ())
			{
				wallet->free_accounts.insert (account);
				response_l.put ("deprecated", "1");
				response_l.put ("ended", "1");
			}
			else
			{
				ec = oslo::error_rpc::payment_account_balance;
			}
		}
	}
	response_errors ();
}

void oslo::json_handler::payment_wait ()
{
	std::string timeout_text (request.get<std::string> ("timeout"));
	auto account (account_impl ());
	auto amount (amount_impl ());
	if (!ec)
	{
		uint64_t timeout;
		if (!decode_unsigned (timeout_text, timeout))
		{
			{
				auto observer (std::make_shared<oslo::json_payment_observer> (node, response, account, amount));
				observer->start (timeout);
				node.payment_observer_processor.add (account, observer);
			}
			node.payment_observer_processor.observer_action (account);
		}
		else
		{
			ec = oslo::error_rpc::bad_timeout;
		}
	}
	if (ec)
	{
		response_errors ();
	}
}

void oslo::json_handler::process ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		const bool watch_work_l = rpc_l->request.get<bool> ("watch_work", true);
		auto block (rpc_l->block_impl (true));

		// State blocks subtype check
		if (!rpc_l->ec && block->type () == oslo::block_type::state)
		{
			std::string subtype_text (rpc_l->request.get<std::string> ("subtype", ""));
			if (!subtype_text.empty ())
			{
				std::shared_ptr<oslo::state_block> block_state (std::static_pointer_cast<oslo::state_block> (block));
				auto transaction (rpc_l->node.store.tx_begin_read ());
				if (!block_state->hashables.previous.is_zero () && !rpc_l->node.store.block_exists (transaction, block_state->hashables.previous))
				{
					rpc_l->ec = oslo::error_process::gap_previous;
				}
				else
				{
					auto balance (rpc_l->node.ledger.account_balance (transaction, block_state->hashables.account));
					if (subtype_text == "send")
					{
						if (balance <= block_state->hashables.balance.number ())
						{
							rpc_l->ec = oslo::error_rpc::invalid_subtype_balance;
						}
						// Send with previous == 0 fails balance check. No previous != 0 check required
					}
					else if (subtype_text == "receive")
					{
						if (balance > block_state->hashables.balance.number ())
						{
							rpc_l->ec = oslo::error_rpc::invalid_subtype_balance;
						}
						// Receive can be point to open block. No previous != 0 check required
					}
					else if (subtype_text == "open")
					{
						if (!block_state->hashables.previous.is_zero ())
						{
							rpc_l->ec = oslo::error_rpc::invalid_subtype_previous;
						}
					}
					else if (subtype_text == "change")
					{
						if (balance != block_state->hashables.balance.number ())
						{
							rpc_l->ec = oslo::error_rpc::invalid_subtype_balance;
						}
						else if (block_state->hashables.previous.is_zero ())
						{
							rpc_l->ec = oslo::error_rpc::invalid_subtype_previous;
						}
					}
					else if (subtype_text == "epoch")
					{
						if (balance != block_state->hashables.balance.number ())
						{
							rpc_l->ec = oslo::error_rpc::invalid_subtype_balance;
						}
						else if (!rpc_l->node.ledger.is_epoch_link (block_state->hashables.link))
						{
							rpc_l->ec = oslo::error_rpc::invalid_subtype_epoch_link;
						}
					}
					else
					{
						rpc_l->ec = oslo::error_rpc::invalid_subtype;
					}
				}
			}
		}
		if (!rpc_l->ec)
		{
			if (!oslo::work_validate_entry (*block))
			{
				auto result (rpc_l->node.process_local (block, watch_work_l));
				switch (result.code)
				{
					case oslo::process_result::progress:
					{
						rpc_l->response_l.put ("hash", block->hash ().to_string ());
						break;
					}
					case oslo::process_result::gap_previous:
					{
						rpc_l->ec = oslo::error_process::gap_previous;
						break;
					}
					case oslo::process_result::gap_source:
					{
						rpc_l->ec = oslo::error_process::gap_source;
						break;
					}
					case oslo::process_result::old:
					{
						rpc_l->ec = oslo::error_process::old;
						break;
					}
					case oslo::process_result::bad_signature:
					{
						rpc_l->ec = oslo::error_process::bad_signature;
						break;
					}
					case oslo::process_result::negative_spend:
					{
						// TODO once we get RPC versioning, this should be changed to "negative spend"
						rpc_l->ec = oslo::error_process::negative_spend;
						break;
					}
					case oslo::process_result::balance_mismatch:
					{
						rpc_l->ec = oslo::error_process::balance_mismatch;
						break;
					}
					case oslo::process_result::unreceivable:
					{
						rpc_l->ec = oslo::error_process::unreceivable;
						break;
					}
					case oslo::process_result::block_position:
					{
						rpc_l->ec = oslo::error_process::block_position;
						break;
					}
					case oslo::process_result::fork:
					{
						const bool force = rpc_l->request.get<bool> ("force", false);
						if (force)
						{
							rpc_l->node.active.erase (*block);
							rpc_l->node.block_processor.force (block);
							rpc_l->response_l.put ("hash", block->hash ().to_string ());
						}
						else
						{
							rpc_l->ec = oslo::error_process::fork;
						}
						break;
					}
					case oslo::process_result::insufficient_work:
					{
						rpc_l->ec = oslo::error_process::insufficient_work;
						break;
					}
					default:
					{
						rpc_l->ec = oslo::error_process::other;
						break;
					}
				}
			}
			else
			{
				rpc_l->ec = oslo::error_blocks::work_low;
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::receive ()
{
	auto wallet (wallet_impl ());
	auto account (account_impl ());
	auto hash (hash_impl ("block"));
	if (!ec)
	{
		auto wallet_transaction (node.wallets.tx_begin_read ());
		wallet_locked_impl (wallet_transaction, wallet);
		wallet_account_impl (wallet_transaction, wallet, account);
		if (!ec)
		{
			auto block_transaction (node.store.tx_begin_read ());
			auto block (node.store.block_get (block_transaction, hash));
			if (block != nullptr)
			{
				if (node.store.pending_exists (block_transaction, oslo::pending_key (account, hash)))
				{
					auto work (work_optional_impl ());
					if (!ec && work)
					{
						oslo::account_info info;
						oslo::root head;
						oslo::epoch epoch = block->sideband ().details.epoch;
						if (!node.store.account_get (block_transaction, account, info))
						{
							head = info.head;
							// When receiving, epoch version is the higher between the previous and the source blocks
							epoch = std::max (info.epoch (), epoch);
						}
						else
						{
							head = account;
						}
						oslo::block_details details (epoch, false, true, false);
						if (oslo::work_difficulty (oslo::work_version::work_1, head, work) < oslo::work_threshold (oslo::work_version::work_1, details))
						{
							ec = oslo::error_common::invalid_work;
						}
					}
					else if (!ec) // && work == 0
					{
						if (!node.work_generation_enabled ())
						{
							ec = oslo::error_common::disabled_work_generation;
						}
					}
					if (!ec)
					{
						// Representative is only used by receive_action when opening accounts
						// Set a wallet default representative for new accounts
						oslo::account representative (wallet->store.representative (wallet_transaction));
						bool generate_work (work == 0); // Disable work generation if "work" option is provided
						auto response_a (response);
						wallet->receive_async (
						std::move (block), representative, node.network_params.ledger.genesis_amount, [response_a](std::shared_ptr<oslo::block> block_a) {
							if (block_a != nullptr)
							{
								boost::property_tree::ptree response_l;
								response_l.put ("block", block_a->hash ().to_string ());
								std::stringstream ostream;
								boost::property_tree::write_json (ostream, response_l);
								response_a (ostream.str ());
							}
							else
							{
								json_error_response (response_a, "Error generating block");
							}
						},
						work, generate_work);
					}
				}
				else
				{
					ec = oslo::error_process::unreceivable;
				}
			}
			else
			{
				ec = oslo::error_blocks::not_found;
			}
		}
	}
	// Because of receive_async
	if (ec)
	{
		response_errors ();
	}
}

void oslo::json_handler::receive_minimum ()
{
	if (!ec)
	{
		response_l.put ("amount", node.config.receive_minimum.to_string_dec ());
	}
	response_errors ();
}

void oslo::json_handler::receive_minimum_set ()
{
	auto amount (amount_impl ());
	if (!ec)
	{
		node.config.receive_minimum = amount;
		response_l.put ("success", "");
	}
	response_errors ();
}

void oslo::json_handler::representatives ()
{
	auto count (count_optional_impl ());
	if (!ec)
	{
		const bool sorting = request.get<bool> ("sorting", false);
		boost::property_tree::ptree representatives;
		auto rep_amounts = node.ledger.cache.rep_weights.get_rep_amounts ();
		if (!sorting) // Simple
		{
			std::map<oslo::account, oslo::uint128_t> ordered (rep_amounts.begin (), rep_amounts.end ());
			for (auto & rep_amount : rep_amounts)
			{
				auto const & account (rep_amount.first);
				auto const & amount (rep_amount.second);
				representatives.put (account.to_account (), amount.convert_to<std::string> ());

				if (representatives.size () > count)
				{
					break;
				}
			}
		}
		else // Sorting
		{
			std::vector<std::pair<oslo::uint128_t, std::string>> representation;

			for (auto & rep_amount : rep_amounts)
			{
				auto const & account (rep_amount.first);
				auto const & amount (rep_amount.second);
				representation.emplace_back (amount, account.to_account ());
			}
			std::sort (representation.begin (), representation.end ());
			std::reverse (representation.begin (), representation.end ());
			for (auto i (representation.begin ()), n (representation.end ()); i != n && representatives.size () < count; ++i)
			{
				representatives.put (i->second, (i->first).convert_to<std::string> ());
			}
		}
		response_l.add_child ("representatives", representatives);
	}
	response_errors ();
}

void oslo::json_handler::representatives_online ()
{
	const auto accounts_node = request.get_child_optional ("accounts");
	const bool weight = request.get<bool> ("weight", false);
	std::vector<oslo::public_key> accounts_to_filter;
	if (accounts_node.is_initialized ())
	{
		for (auto & a : (*accounts_node))
		{
			auto account (account_impl (a.second.get<std::string> ("")));
			if (!ec)
			{
				accounts_to_filter.push_back (account);
			}
			else
			{
				break;
			}
		}
	}
	if (!ec)
	{
		boost::property_tree::ptree representatives;
		auto reps (node.online_reps.list ());
		for (auto & i : reps)
		{
			if (accounts_node.is_initialized ())
			{
				if (accounts_to_filter.empty ())
				{
					break;
				}
				auto found_acc = std::find (accounts_to_filter.begin (), accounts_to_filter.end (), i);
				if (found_acc == accounts_to_filter.end ())
				{
					continue;
				}
				else
				{
					accounts_to_filter.erase (found_acc);
				}
			}
			if (weight)
			{
				boost::property_tree::ptree weight_node;
				auto account_weight (node.ledger.weight (i));
				weight_node.put ("weight", account_weight.convert_to<std::string> ());
				representatives.add_child (i.to_account (), weight_node);
			}
			else
			{
				boost::property_tree::ptree entry;
				entry.put ("", i.to_account ());
				representatives.push_back (std::make_pair ("", entry));
			}
		}
		response_l.add_child ("representatives", representatives);
	}
	response_errors ();
}

void oslo::json_handler::republish ()
{
	auto count (count_optional_impl (1024U));
	uint64_t sources (0);
	uint64_t destinations (0);
	boost::optional<std::string> sources_text (request.get_optional<std::string> ("sources"));
	if (!ec && sources_text.is_initialized ())
	{
		if (decode_unsigned (sources_text.get (), sources))
		{
			ec = oslo::error_rpc::invalid_sources;
		}
	}
	boost::optional<std::string> destinations_text (request.get_optional<std::string> ("destinations"));
	if (!ec && destinations_text.is_initialized ())
	{
		if (decode_unsigned (destinations_text.get (), destinations))
		{
			ec = oslo::error_rpc::invalid_destinations;
		}
	}
	auto hash (hash_impl ());
	if (!ec)
	{
		boost::property_tree::ptree blocks;
		auto transaction (node.store.tx_begin_read ());
		auto block (node.store.block_get (transaction, hash));
		if (block != nullptr)
		{
			std::deque<std::shared_ptr<oslo::block>> republish_bundle;
			for (auto i (0); !hash.is_zero () && i < count; ++i)
			{
				block = node.store.block_get (transaction, hash);
				if (sources != 0) // Republish source chain
				{
					oslo::block_hash source (node.ledger.block_source (transaction, *block));
					auto block_a (node.store.block_get (transaction, source));
					std::vector<oslo::block_hash> hashes;
					while (block_a != nullptr && hashes.size () < sources)
					{
						hashes.push_back (source);
						source = block_a->previous ();
						block_a = node.store.block_get (transaction, source);
					}
					std::reverse (hashes.begin (), hashes.end ());
					for (auto & hash_l : hashes)
					{
						block_a = node.store.block_get (transaction, hash_l);
						republish_bundle.push_back (std::move (block_a));
						boost::property_tree::ptree entry_l;
						entry_l.put ("", hash_l.to_string ());
						blocks.push_back (std::make_pair ("", entry_l));
					}
				}
				republish_bundle.push_back (std::move (block)); // Republish block
				boost::property_tree::ptree entry;
				entry.put ("", hash.to_string ());
				blocks.push_back (std::make_pair ("", entry));
				if (destinations != 0) // Republish destination chain
				{
					auto block_b (node.store.block_get (transaction, hash));
					auto destination (node.ledger.block_destination (transaction, *block_b));
					if (!destination.is_zero ())
					{
						if (!node.store.pending_exists (transaction, oslo::pending_key (destination, hash)))
						{
							oslo::block_hash previous (node.ledger.latest (transaction, destination));
							auto block_d (node.store.block_get (transaction, previous));
							oslo::block_hash source;
							std::vector<oslo::block_hash> hashes;
							while (block_d != nullptr && hash != source)
							{
								hashes.push_back (previous);
								source = node.ledger.block_source (transaction, *block_d);
								previous = block_d->previous ();
								block_d = node.store.block_get (transaction, previous);
							}
							std::reverse (hashes.begin (), hashes.end ());
							if (hashes.size () > destinations)
							{
								hashes.resize (destinations);
							}
							for (auto & hash_l : hashes)
							{
								block_d = node.store.block_get (transaction, hash_l);
								republish_bundle.push_back (std::move (block_d));
								boost::property_tree::ptree entry_l;
								entry_l.put ("", hash_l.to_string ());
								blocks.push_back (std::make_pair ("", entry_l));
							}
						}
					}
				}
				hash = node.store.block_successor (transaction, hash);
			}
			node.network.flood_block_many (std::move (republish_bundle), nullptr, 25);
			response_l.put ("success", ""); // obsolete
			response_l.add_child ("blocks", blocks);
		}
		else
		{
			ec = oslo::error_blocks::not_found;
		}
	}
	response_errors ();
}

void oslo::json_handler::search_pending ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto error (wallet->search_pending ());
		response_l.put ("started", !error);
	}
	response_errors ();
}

void oslo::json_handler::search_pending_all ()
{
	if (!ec)
	{
		node.wallets.search_pending_all ();
		response_l.put ("success", "");
	}
	response_errors ();
}

void oslo::json_handler::send ()
{
	auto wallet (wallet_impl ());
	auto amount (amount_impl ());
	// Sending 0 amount is invalid with state blocks
	if (!ec && amount.is_zero ())
	{
		ec = oslo::error_common::invalid_amount;
	}
	std::string source_text (request.get<std::string> ("source"));
	auto source (account_impl (source_text, oslo::error_rpc::bad_source));
	std::string destination_text (request.get<std::string> ("destination"));
	auto destination (account_impl (destination_text, oslo::error_rpc::bad_destination));
	if (!ec)
	{
		auto work (work_optional_impl ());
		oslo::uint128_t balance (0);
		if (!ec && work == 0 && !node.work_generation_enabled ())
		{
			ec = oslo::error_common::disabled_work_generation;
		}
		if (!ec)
		{
			auto transaction (node.wallets.tx_begin_read ());
			auto block_transaction (node.store.tx_begin_read ());
			wallet_locked_impl (transaction, wallet);
			wallet_account_impl (transaction, wallet, source);
			auto info (account_info_impl (block_transaction, source));
			if (!ec)
			{
				balance = (info.balance).number ();
			}
			if (!ec && work)
			{
				oslo::block_details details (info.epoch (), true, false, false);
				if (oslo::work_difficulty (oslo::work_version::work_1, info.head, work) < oslo::work_threshold (oslo::work_version::work_1, details))
				{
					ec = oslo::error_common::invalid_work;
				}
			}
		}
		if (!ec)
		{
			bool generate_work (work == 0); // Disable work generation if "work" option is provided
			boost::optional<std::string> send_id (request.get_optional<std::string> ("id"));
			auto response_a (response);
			auto response_data (std::make_shared<boost::property_tree::ptree> (response_l));
			wallet->send_async (
			source, destination, amount.number (), [balance, amount, response_a, response_data](std::shared_ptr<oslo::block> block_a) {
				if (block_a != nullptr)
				{
					response_data->put ("block", block_a->hash ().to_string ());
					std::stringstream ostream;
					boost::property_tree::write_json (ostream, *response_data);
					response_a (ostream.str ());
				}
				else
				{
					if (balance >= amount.number ())
					{
						json_error_response (response_a, "Error generating block");
					}
					else
					{
						std::error_code ec (oslo::error_common::insufficient_balance);
						json_error_response (response_a, ec.message ());
					}
				}
			},
			work, generate_work, send_id);
		}
	}
	// Because of send_async
	if (ec)
	{
		response_errors ();
	}
}

void oslo::json_handler::sign ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	// Retrieving hash
	oslo::block_hash hash (0);
	boost::optional<std::string> hash_text (request.get_optional<std::string> ("hash"));
	if (hash_text.is_initialized ())
	{
		hash = hash_impl ();
	}
	// Retrieving block
	std::shared_ptr<oslo::block> block;
	if (!ec && request.count ("block"))
	{
		block = block_impl (true);
		if (block != nullptr)
		{
			hash = block->hash ();
		}
	}

	// Hash or block are not initialized
	if (!ec && hash.is_zero ())
	{
		ec = oslo::error_blocks::invalid_block;
	}
	// Hash is initialized without config permission
	else if (!ec && !hash.is_zero () && block == nullptr && !node_rpc_config.enable_sign_hash)
	{
		ec = oslo::error_rpc::sign_hash_disabled;
	}
	if (!ec)
	{
		oslo::raw_key prv;
		prv.data.clear ();
		// Retrieving private key from request
		boost::optional<std::string> key_text (request.get_optional<std::string> ("key"));
		if (key_text.is_initialized ())
		{
			if (prv.data.decode_hex (key_text.get ()))
			{
				ec = oslo::error_common::bad_private_key;
			}
		}
		else
		{
			// Retrieving private key from wallet
			boost::optional<std::string> account_text (request.get_optional<std::string> ("account"));
			boost::optional<std::string> wallet_text (request.get_optional<std::string> ("wallet"));
			if (wallet_text.is_initialized () && account_text.is_initialized ())
			{
				auto account (account_impl ());
				auto wallet (wallet_impl ());
				if (!ec)
				{
					auto transaction (node.wallets.tx_begin_read ());
					wallet_locked_impl (transaction, wallet);
					wallet_account_impl (transaction, wallet, account);
					if (!ec)
					{
						wallet->store.fetch (transaction, account, prv);
					}
				}
			}
		}
		// Signing
		if (prv.data != 0)
		{
			oslo::public_key pub (oslo::pub_key (prv.as_private_key ()));
			oslo::signature signature (oslo::sign_message (prv, pub, hash));
			response_l.put ("signature", signature.to_string ());
			if (block != nullptr)
			{
				block->signature_set (signature);

				if (json_block_l)
				{
					boost::property_tree::ptree block_node_l;
					block->serialize_json (block_node_l);
					response_l.add_child ("block", block_node_l);
				}
				else
				{
					std::string contents;
					block->serialize_json (contents);
					response_l.put ("block", contents);
				}
			}
		}
		else
		{
			ec = oslo::error_rpc::block_create_key_required;
		}
	}
	response_errors ();
}

void oslo::json_handler::stats ()
{
	auto sink = node.stats.log_sink_json ();
	std::string type (request.get<std::string> ("type", ""));
	bool use_sink = false;
	if (type == "counters")
	{
		node.stats.log_counters (*sink);
		use_sink = true;
	}
	else if (type == "objects")
	{
		construct_json (collect_container_info (node, "node").get (), response_l);
	}
	else if (type == "samples")
	{
		node.stats.log_samples (*sink);
		use_sink = true;
	}
	else
	{
		ec = oslo::error_rpc::invalid_missing_type;
	}
	if (!ec && use_sink)
	{
		auto stat_tree_l (*static_cast<boost::property_tree::ptree *> (sink->to_object ()));
		stat_tree_l.put ("stat_duration_seconds", node.stats.last_reset ().count ());
		std::stringstream ostream;
		boost::property_tree::write_json (ostream, stat_tree_l);
		response (ostream.str ());
	}
	else
	{
		response_errors ();
	}
}

void oslo::json_handler::stats_clear ()
{
	node.stats.clear ();
	response_l.put ("success", "");
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, response_l);
	response (ostream.str ());
}

void oslo::json_handler::stop ()
{
	response_l.put ("success", "");
	response_errors ();
	if (!ec)
	{
		node.stop ();
		stop_callback ();
	}
}

void oslo::json_handler::telemetry ()
{
	auto rpc_l (shared_from_this ());

	auto address_text (request.get_optional<std::string> ("address"));
	auto port_text (request.get_optional<std::string> ("port"));

	if (address_text.is_initialized () || port_text.is_initialized ())
	{
		// Check both are specified
		std::shared_ptr<oslo::transport::channel> channel;
		if (address_text.is_initialized () && port_text.is_initialized ())
		{
			uint16_t port;
			if (!oslo::parse_port (*port_text, port))
			{
				boost::asio::ip::address address;
				if (!oslo::parse_address (*address_text, address))
				{
					oslo::endpoint endpoint (address, port);
					if (address.is_loopback () && port == rpc_l->node.network.endpoint ().port ())
					{
						// Requesting telemetry metrics locally
						auto telemetry_data = oslo::local_telemetry_data (rpc_l->node.ledger.cache, rpc_l->node.network, rpc_l->node.config.bandwidth_limit, rpc_l->node.network_params, rpc_l->node.startup_time, rpc_l->node.active.active_difficulty (), rpc_l->node.node_id);

						oslo::jsonconfig config_l;
						auto const should_ignore_identification_metrics = false;
						auto err = telemetry_data.serialize_json (config_l, should_ignore_identification_metrics);
						auto const & ptree = config_l.get_tree ();

						if (!err)
						{
							rpc_l->response_l.insert (rpc_l->response_l.begin (), ptree.begin (), ptree.end ());
						}

						rpc_l->response_errors ();
						return;
					}
					else
					{
						channel = node.network.find_channel (oslo::transport::map_endpoint_to_v6 (endpoint));
						if (!channel)
						{
							ec = oslo::error_rpc::peer_not_found;
						}
					}
				}
				else
				{
					ec = oslo::error_common::invalid_ip_address;
				}
			}
			else
			{
				ec = oslo::error_common::invalid_port;
			}
		}
		else
		{
			ec = oslo::error_rpc::requires_port_and_address;
		}

		if (!ec)
		{
			debug_assert (channel);
			if (node.telemetry)
			{
				node.telemetry->get_metrics_single_peer_async (channel, [rpc_l](auto const & telemetry_response_a) {
					if (!telemetry_response_a.error)
					{
						oslo::jsonconfig config_l;
						auto const should_ignore_identification_metrics = false;
						auto err = telemetry_response_a.telemetry_data.serialize_json (config_l, should_ignore_identification_metrics);
						auto const & ptree = config_l.get_tree ();

						if (!err)
						{
							rpc_l->response_l.insert (rpc_l->response_l.begin (), ptree.begin (), ptree.end ());
						}
						else
						{
							rpc_l->ec = oslo::error_rpc::generic;
						}
					}
					else
					{
						rpc_l->ec = oslo::error_rpc::generic;
					}

					rpc_l->response_errors ();
				});
			}
			else
			{
				response_errors ();
			}
		}
		else
		{
			response_errors ();
		}
	}
	else
	{
		// By default, consolidated (average or mode) telemetry metrics are returned,
		// setting "raw" to true returns metrics from all nodes requested.
		auto raw = request.get_optional<bool> ("raw");
		auto output_raw = raw.value_or (false);
		if (node.telemetry)
		{
			auto telemetry_responses = node.telemetry->get_metrics ();
			if (output_raw)
			{
				boost::property_tree::ptree metrics;
				for (auto & telemetry_metrics : telemetry_responses)
				{
					oslo::jsonconfig config_l;
					auto const should_ignore_identification_metrics = false;
					auto err = telemetry_metrics.second.serialize_json (config_l, should_ignore_identification_metrics);
					config_l.put ("address", telemetry_metrics.first.address ());
					config_l.put ("port", telemetry_metrics.first.port ());
					if (!err)
					{
						metrics.push_back (std::make_pair ("", config_l.get_tree ()));
					}
					else
					{
						ec = oslo::error_rpc::generic;
					}
				}

				response_l.put_child ("metrics", metrics);
			}
			else
			{
				oslo::jsonconfig config_l;
				std::vector<oslo::telemetry_data> telemetry_datas;
				telemetry_datas.reserve (telemetry_responses.size ());
				std::transform (telemetry_responses.begin (), telemetry_responses.end (), std::back_inserter (telemetry_datas), [](auto const & endpoint_telemetry_data) {
					return endpoint_telemetry_data.second;
				});

				auto average_telemetry_metrics = oslo::consolidate_telemetry_data (telemetry_datas);
				// Don't add node_id/signature in consolidated metrics
				auto const should_ignore_identification_metrics = true;
				auto err = average_telemetry_metrics.serialize_json (config_l, should_ignore_identification_metrics);
				auto const & ptree = config_l.get_tree ();

				if (!err)
				{
					response_l.insert (response_l.begin (), ptree.begin (), ptree.end ());
				}
				else
				{
					ec = oslo::error_rpc::generic;
				}
			}
		}

		response_errors ();
	}
}

void oslo::json_handler::unchecked ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	auto count (count_optional_impl ());
	if (!ec)
	{
		boost::property_tree::ptree unchecked;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.unchecked_begin (transaction)), n (node.store.unchecked_end ()); i != n && unchecked.size () < count; ++i)
		{
			oslo::unchecked_info const & info (i->second);
			if (json_block_l)
			{
				boost::property_tree::ptree block_node_l;
				info.block->serialize_json (block_node_l);
				unchecked.add_child (info.block->hash ().to_string (), block_node_l);
			}
			else
			{
				std::string contents;
				info.block->serialize_json (contents);
				unchecked.put (info.block->hash ().to_string (), contents);
			}
		}
		response_l.add_child ("blocks", unchecked);
	}
	response_errors ();
}

void oslo::json_handler::unchecked_clear ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto transaction (rpc_l->node.store.tx_begin_write ({ tables::unchecked }));
		rpc_l->node.store.unchecked_clear (transaction);
		rpc_l->node.ledger.cache.unchecked_count = 0;
		rpc_l->response_l.put ("success", "");
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::unchecked_get ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	auto hash (hash_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.unchecked_begin (transaction)), n (node.store.unchecked_end ()); i != n; ++i)
		{
			oslo::unchecked_key const & key (i->first);
			if (key.hash == hash)
			{
				oslo::unchecked_info const & info (i->second);
				response_l.put ("modified_timestamp", std::to_string (info.modified));

				if (json_block_l)
				{
					boost::property_tree::ptree block_node_l;
					info.block->serialize_json (block_node_l);
					response_l.add_child ("contents", block_node_l);
				}
				else
				{
					std::string contents;
					info.block->serialize_json (contents);
					response_l.put ("contents", contents);
				}
				break;
			}
		}
		if (response_l.empty ())
		{
			ec = oslo::error_blocks::not_found;
		}
	}
	response_errors ();
}

void oslo::json_handler::unchecked_keys ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	auto count (count_optional_impl ());
	oslo::block_hash key (0);
	boost::optional<std::string> hash_text (request.get_optional<std::string> ("key"));
	if (!ec && hash_text.is_initialized ())
	{
		if (key.decode_hex (hash_text.get ()))
		{
			ec = oslo::error_rpc::bad_key;
		}
	}
	if (!ec)
	{
		boost::property_tree::ptree unchecked;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.unchecked_begin (transaction, oslo::unchecked_key (key, 0))), n (node.store.unchecked_end ()); i != n && unchecked.size () < count; ++i)
		{
			boost::property_tree::ptree entry;
			oslo::unchecked_info const & info (i->second);
			entry.put ("key", i->first.key ().to_string ());
			entry.put ("hash", info.block->hash ().to_string ());
			entry.put ("modified_timestamp", std::to_string (info.modified));
			if (json_block_l)
			{
				boost::property_tree::ptree block_node_l;
				info.block->serialize_json (block_node_l);
				entry.add_child ("contents", block_node_l);
			}
			else
			{
				std::string contents;
				info.block->serialize_json (contents);
				entry.put ("contents", contents);
			}
			unchecked.push_back (std::make_pair ("", entry));
		}
		response_l.add_child ("unchecked", unchecked);
	}
	response_errors ();
}

void oslo::json_handler::unopened ()
{
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	oslo::account start (1); // exclude burn account by default
	boost::optional<std::string> account_text (request.get_optional<std::string> ("account"));
	if (account_text.is_initialized ())
	{
		start = account_impl (account_text.get ());
	}
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		auto iterator (node.store.pending_begin (transaction, oslo::pending_key (start, 0)));
		auto end (node.store.pending_end ());
		oslo::account current_account (start);
		oslo::uint128_t current_account_sum{ 0 };
		boost::property_tree::ptree accounts;
		while (iterator != end && accounts.size () < count)
		{
			oslo::pending_key key (iterator->first);
			oslo::account account (key.account);
			oslo::pending_info info (iterator->second);
			if (node.store.account_exists (transaction, account))
			{
				if (account.number () == std::numeric_limits<oslo::uint256_t>::max ())
				{
					break;
				}
				// Skip existing accounts
				iterator = node.store.pending_begin (transaction, oslo::pending_key (account.number () + 1, 0));
			}
			else
			{
				if (account != current_account)
				{
					if (current_account_sum > 0)
					{
						if (current_account_sum >= threshold.number ())
						{
							accounts.put (current_account.to_account (), current_account_sum.convert_to<std::string> ());
						}
						current_account_sum = 0;
					}
					current_account = account;
				}
				current_account_sum += info.amount.number ();
				++iterator;
			}
		}
		// last one after iterator reaches end
		if (accounts.size () < count && current_account_sum > 0 && current_account_sum >= threshold.number ())
		{
			accounts.put (current_account.to_account (), current_account_sum.convert_to<std::string> ());
		}
		response_l.add_child ("accounts", accounts);
	}
	response_errors ();
}

void oslo::json_handler::uptime ()
{
	response_l.put ("seconds", std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now () - node.startup_time).count ());
	response_errors ();
}

void oslo::json_handler::version ()
{
	response_l.put ("rpc_version", "1");
	response_l.put ("store_version", std::to_string (node.store_version ()));
	response_l.put ("protocol_version", std::to_string (node.network_params.protocol.protocol_version));
	response_l.put ("node_vendor", boost::str (boost::format ("Oslo %1%") % OSLO_VERSION_STRING));
	response_l.put ("store_vendor", node.store.vendor_get ());
	response_l.put ("network", node.network_params.network.get_current_network_as_string ());
	response_l.put ("network_identifier", node.network_params.ledger.genesis_hash.to_string ());
	response_l.put ("build_info", BUILD_INFO);
	response_errors ();
}

void oslo::json_handler::validate_account_number ()
{
	auto account (account_impl ());
	(void)account;
	response_l.put ("valid", ec ? "0" : "1");
	ec = std::error_code (); // error is just invalid account
	response_errors ();
}

void oslo::json_handler::wallet_add ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			std::string key_text (rpc_l->request.get<std::string> ("key"));
			oslo::raw_key key;
			if (!key.data.decode_hex (key_text))
			{
				const bool generate_work = rpc_l->request.get<bool> ("work", true);
				auto pub (wallet->insert_adhoc (key, generate_work));
				if (!pub.is_zero ())
				{
					rpc_l->response_l.put ("account", pub.to_account ());
				}
				else
				{
					rpc_l->ec = oslo::error_common::wallet_locked;
				}
			}
			else
			{
				rpc_l->ec = oslo::error_common::bad_private_key;
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::wallet_add_watch ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			if (wallet->store.valid_password (transaction))
			{
				for (auto & accounts : rpc_l->request.get_child ("accounts"))
				{
					auto account (rpc_l->account_impl (accounts.second.data ()));
					if (!rpc_l->ec)
					{
						if (wallet->insert_watch (transaction, account))
						{
							rpc_l->ec = oslo::error_common::bad_public_key;
						}
					}
				}
				if (!rpc_l->ec)
				{
					rpc_l->response_l.put ("success", "");
				}
			}
			else
			{
				rpc_l->ec = oslo::error_common::wallet_locked;
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::wallet_info ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		oslo::uint128_t balance (0);
		oslo::uint128_t pending (0);
		uint64_t count (0);
		uint64_t deterministic_count (0);
		uint64_t adhoc_count (0);
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			oslo::account const & account (i->first);
			balance = balance + node.ledger.account_balance (block_transaction, account);
			pending = pending + node.ledger.account_pending (block_transaction, account);
			oslo::key_type key_type (wallet->store.key_type (i->second));
			if (key_type == oslo::key_type::deterministic)
			{
				deterministic_count++;
			}
			else if (key_type == oslo::key_type::adhoc)
			{
				adhoc_count++;
			}
			count++;
		}
		uint32_t deterministic_index (wallet->store.deterministic_index_get (transaction));
		response_l.put ("balance", balance.convert_to<std::string> ());
		response_l.put ("pending", pending.convert_to<std::string> ());
		response_l.put ("accounts_count", std::to_string (count));
		response_l.put ("deterministic_count", std::to_string (deterministic_count));
		response_l.put ("adhoc_count", std::to_string (adhoc_count));
		response_l.put ("deterministic_index", std::to_string (deterministic_index));
	}
	response_errors ();
}

void oslo::json_handler::wallet_balances ()
{
	auto wallet (wallet_impl ());
	auto threshold (threshold_optional_impl ());
	if (!ec)
	{
		boost::property_tree::ptree balances;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			oslo::account const & account (i->first);
			oslo::uint128_t balance = node.ledger.account_balance (block_transaction, account);
			if (balance >= threshold.number ())
			{
				boost::property_tree::ptree entry;
				oslo::uint128_t pending = node.ledger.account_pending (block_transaction, account);
				entry.put ("balance", balance.convert_to<std::string> ());
				entry.put ("pending", pending.convert_to<std::string> ());
				balances.push_back (std::make_pair (account.to_account (), entry));
			}
		}
		response_l.add_child ("balances", balances);
	}
	response_errors ();
}

void oslo::json_handler::wallet_change_seed ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			std::string seed_text (rpc_l->request.get<std::string> ("seed"));
			oslo::raw_key seed;
			if (!seed.data.decode_hex (seed_text))
			{
				auto count (static_cast<uint32_t> (rpc_l->count_optional_impl (0)));
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				if (wallet->store.valid_password (transaction))
				{
					oslo::public_key account (wallet->change_seed (transaction, seed, count));
					rpc_l->response_l.put ("success", "");
					rpc_l->response_l.put ("last_restored_account", account.to_account ());
					auto index (wallet->store.deterministic_index_get (transaction));
					debug_assert (index > 0);
					rpc_l->response_l.put ("restored_count", std::to_string (index));
				}
				else
				{
					rpc_l->ec = oslo::error_common::wallet_locked;
				}
			}
			else
			{
				rpc_l->ec = oslo::error_common::bad_seed;
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::wallet_contains ()
{
	auto account (account_impl ());
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		auto exists (wallet->store.find (transaction, account) != wallet->store.end ());
		response_l.put ("exists", exists ? "1" : "0");
	}
	response_errors ();
}

void oslo::json_handler::wallet_create ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		oslo::raw_key seed;
		auto seed_text (rpc_l->request.get_optional<std::string> ("seed"));
		if (seed_text.is_initialized () && seed.data.decode_hex (seed_text.get ()))
		{
			rpc_l->ec = oslo::error_common::bad_seed;
		}
		if (!rpc_l->ec)
		{
			auto wallet_id = random_wallet_id ();
			auto wallet (rpc_l->node.wallets.create (wallet_id));
			auto existing (rpc_l->node.wallets.items.find (wallet_id));
			if (existing != rpc_l->node.wallets.items.end ())
			{
				rpc_l->response_l.put ("wallet", wallet_id.to_string ());
			}
			else
			{
				rpc_l->ec = oslo::error_common::wallet_lmdb_max_dbs;
			}
			if (!rpc_l->ec && seed_text.is_initialized ())
			{
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				oslo::public_key account (wallet->change_seed (transaction, seed));
				rpc_l->response_l.put ("last_restored_account", account.to_account ());
				auto index (wallet->store.deterministic_index_get (transaction));
				debug_assert (index > 0);
				rpc_l->response_l.put ("restored_count", std::to_string (index));
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::wallet_destroy ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		std::string wallet_text (rpc_l->request.get<std::string> ("wallet"));
		oslo::wallet_id wallet;
		if (!wallet.decode_hex (wallet_text))
		{
			auto existing (rpc_l->node.wallets.items.find (wallet));
			if (existing != rpc_l->node.wallets.items.end ())
			{
				rpc_l->node.wallets.destroy (wallet);
				bool destroyed (rpc_l->node.wallets.items.find (wallet) == rpc_l->node.wallets.items.end ());
				rpc_l->response_l.put ("destroyed", destroyed ? "1" : "0");
			}
			else
			{
				rpc_l->ec = oslo::error_common::wallet_not_found;
			}
		}
		else
		{
			rpc_l->ec = oslo::error_common::bad_wallet_number;
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::wallet_export ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		std::string json;
		wallet->store.serialize_json (transaction, json);
		response_l.put ("json", json);
	}
	response_errors ();
}

void oslo::json_handler::wallet_frontiers ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		boost::property_tree::ptree frontiers;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			oslo::account const & account (i->first);
			auto latest (node.ledger.latest (block_transaction, account));
			if (!latest.is_zero ())
			{
				frontiers.put (account.to_account (), latest.to_string ());
			}
		}
		response_l.add_child ("frontiers", frontiers);
	}
	response_errors ();
}

void oslo::json_handler::wallet_history ()
{
	uint64_t modified_since (1);
	boost::optional<std::string> modified_since_text (request.get_optional<std::string> ("modified_since"));
	if (modified_since_text.is_initialized ())
	{
		if (decode_unsigned (modified_since_text.get (), modified_since))
		{
			ec = oslo::error_rpc::invalid_timestamp;
		}
	}
	auto wallet (wallet_impl ());
	if (!ec)
	{
		std::multimap<uint64_t, boost::property_tree::ptree, std::greater<uint64_t>> entries;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			oslo::account const & account (i->first);
			oslo::account_info info;
			if (!node.store.account_get (block_transaction, account, info))
			{
				auto timestamp (info.modified);
				auto hash (info.head);
				while (timestamp >= modified_since && !hash.is_zero ())
				{
					auto block (node.store.block_get (block_transaction, hash));
					timestamp = block->sideband ().timestamp;
					if (block != nullptr && timestamp >= modified_since)
					{
						boost::property_tree::ptree entry;
						std::vector<oslo::public_key> no_filter;
						history_visitor visitor (*this, false, block_transaction, entry, hash, no_filter);
						block->visit (visitor);
						if (!entry.empty ())
						{
							entry.put ("block_account", account.to_account ());
							entry.put ("hash", hash.to_string ());
							entry.put ("local_timestamp", std::to_string (timestamp));
							entries.insert (std::make_pair (timestamp, entry));
						}
						hash = block->previous ();
					}
					else
					{
						hash.clear ();
					}
				}
			}
		}
		boost::property_tree::ptree history;
		for (auto i (entries.begin ()), n (entries.end ()); i != n; ++i)
		{
			history.push_back (std::make_pair ("", i->second));
		}
		response_l.add_child ("history", history);
	}
	response_errors ();
}

void oslo::json_handler::wallet_key_valid ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		auto valid (wallet->store.valid_password (transaction));
		response_l.put ("valid", valid ? "1" : "0");
	}
	response_errors ();
}

void oslo::json_handler::wallet_ledger ()
{
	const bool representative = request.get<bool> ("representative", false);
	const bool weight = request.get<bool> ("weight", false);
	const bool pending = request.get<bool> ("pending", false);
	uint64_t modified_since (0);
	boost::optional<std::string> modified_since_text (request.get_optional<std::string> ("modified_since"));
	if (modified_since_text.is_initialized ())
	{
		modified_since = strtoul (modified_since_text.get ().c_str (), NULL, 10);
	}
	auto wallet (wallet_impl ());
	if (!ec)
	{
		boost::property_tree::ptree accounts;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			oslo::account const & account (i->first);
			oslo::account_info info;
			if (!node.store.account_get (block_transaction, account, info))
			{
				if (info.modified >= modified_since)
				{
					boost::property_tree::ptree entry;
					entry.put ("frontier", info.head.to_string ());
					entry.put ("open_block", info.open_block.to_string ());
					entry.put ("representative_block", node.ledger.representative (block_transaction, info.head).to_string ());
					std::string balance;
					oslo::uint128_union (info.balance).encode_dec (balance);
					entry.put ("balance", balance);
					entry.put ("modified_timestamp", std::to_string (info.modified));
					entry.put ("block_count", std::to_string (info.block_count));
					if (representative)
					{
						entry.put ("representative", info.representative.to_account ());
					}
					if (weight)
					{
						auto account_weight (node.ledger.weight (account));
						entry.put ("weight", account_weight.convert_to<std::string> ());
					}
					if (pending)
					{
						auto account_pending (node.ledger.account_pending (block_transaction, account));
						entry.put ("pending", account_pending.convert_to<std::string> ());
					}
					accounts.push_back (std::make_pair (account.to_account (), entry));
				}
			}
		}
		response_l.add_child ("accounts", accounts);
	}
	response_errors ();
}

void oslo::json_handler::wallet_lock ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		oslo::raw_key empty;
		empty.data.clear ();
		wallet->store.password.value_set (empty);
		response_l.put ("locked", "1");
		node.logger.try_log ("Wallet locked");
	}
	response_errors ();
}

void oslo::json_handler::wallet_pending ()
{
	auto wallet (wallet_impl ());
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	const bool source = request.get<bool> ("source", false);
	const bool min_version = request.get<bool> ("min_version", false);
	const bool include_active = request.get<bool> ("include_active", false);
	const bool include_only_confirmed = request.get<bool> ("include_only_confirmed", false);
	if (!ec)
	{
		boost::property_tree::ptree pending;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			oslo::account const & account (i->first);
			boost::property_tree::ptree peers_l;
			for (auto ii (node.store.pending_begin (block_transaction, oslo::pending_key (account, 0))), nn (node.store.pending_end ()); ii != nn && oslo::pending_key (ii->first).account == account && peers_l.size () < count; ++ii)
			{
				oslo::pending_key key (ii->first);
				if (block_confirmed (node, block_transaction, key.hash, include_active, include_only_confirmed))
				{
					if (threshold.is_zero () && !source)
					{
						boost::property_tree::ptree entry;
						entry.put ("", key.hash.to_string ());
						peers_l.push_back (std::make_pair ("", entry));
					}
					else
					{
						oslo::pending_info info (ii->second);
						if (info.amount.number () >= threshold.number ())
						{
							if (source || min_version)
							{
								boost::property_tree::ptree pending_tree;
								pending_tree.put ("amount", info.amount.number ().convert_to<std::string> ());
								if (source)
								{
									pending_tree.put ("source", info.source.to_account ());
								}
								if (min_version)
								{
									pending_tree.put ("min_version", epoch_as_string (info.epoch));
								}
								peers_l.add_child (key.hash.to_string (), pending_tree);
							}
							else
							{
								peers_l.put (key.hash.to_string (), info.amount.number ().convert_to<std::string> ());
							}
						}
					}
				}
			}
			if (!peers_l.empty ())
			{
				pending.add_child (account.to_account (), peers_l);
			}
		}
		response_l.add_child ("blocks", pending);
	}
	response_errors ();
}

void oslo::json_handler::wallet_representative ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		response_l.put ("representative", wallet->store.representative (transaction).to_account ());
	}
	response_errors ();
}

void oslo::json_handler::wallet_representative_set ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		std::string representative_text (rpc_l->request.get<std::string> ("representative"));
		auto representative (rpc_l->account_impl (representative_text, oslo::error_rpc::bad_representative_number));
		if (!rpc_l->ec)
		{
			bool update_existing_accounts (rpc_l->request.get<bool> ("update_existing_accounts", false));
			{
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				if (wallet->store.valid_password (transaction) || !update_existing_accounts)
				{
					wallet->store.representative_set (transaction, representative);
					rpc_l->response_l.put ("set", "1");
				}
				else
				{
					rpc_l->ec = oslo::error_common::wallet_locked;
				}
			}
			// Change representative for all wallet accounts
			if (!rpc_l->ec && update_existing_accounts)
			{
				std::vector<oslo::account> accounts;
				{
					auto transaction (rpc_l->node.wallets.tx_begin_read ());
					auto block_transaction (rpc_l->node.store.tx_begin_read ());
					for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
					{
						oslo::account const & account (i->first);
						oslo::account_info info;
						if (!rpc_l->node.store.account_get (block_transaction, account, info))
						{
							if (info.representative != representative)
							{
								accounts.push_back (account);
							}
						}
					}
				}
				for (auto & account : accounts)
				{
					wallet->change_async (
					account, representative, [](std::shared_ptr<oslo::block>) {}, 0, false);
				}
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::wallet_republish ()
{
	auto wallet (wallet_impl ());
	auto count (count_impl ());
	if (!ec)
	{
		boost::property_tree::ptree blocks;
		std::deque<std::shared_ptr<oslo::block>> republish_bundle;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			oslo::account const & account (i->first);
			auto latest (node.ledger.latest (block_transaction, account));
			std::shared_ptr<oslo::block> block;
			std::vector<oslo::block_hash> hashes;
			while (!latest.is_zero () && hashes.size () < count)
			{
				hashes.push_back (latest);
				block = node.store.block_get (block_transaction, latest);
				latest = block->previous ();
			}
			std::reverse (hashes.begin (), hashes.end ());
			for (auto & hash : hashes)
			{
				block = node.store.block_get (block_transaction, hash);
				republish_bundle.push_back (std::move (block));
				boost::property_tree::ptree entry;
				entry.put ("", hash.to_string ());
				blocks.push_back (std::make_pair ("", entry));
			}
		}
		node.network.flood_block_many (std::move (republish_bundle), nullptr, 25);
		response_l.add_child ("blocks", blocks);
	}
	response_errors ();
}

void oslo::json_handler::wallet_seed ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		if (wallet->store.valid_password (transaction))
		{
			oslo::raw_key seed;
			wallet->store.seed (seed, transaction);
			response_l.put ("seed", seed.data.to_string ());
		}
		else
		{
			ec = oslo::error_common::wallet_locked;
		}
	}
	response_errors ();
}

void oslo::json_handler::wallet_work_get ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		boost::property_tree::ptree works;
		auto transaction (node.wallets.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			oslo::account const & account (i->first);
			uint64_t work (0);
			auto error_work (wallet->store.work_get (transaction, account, work));
			(void)error_work;
			works.put (account.to_account (), oslo::to_string_hex (work));
		}
		response_l.add_child ("works", works);
	}
	response_errors ();
}

void oslo::json_handler::work_generate ()
{
	boost::optional<oslo::account> account;
	auto account_opt (request.get_optional<std::string> ("account"));
	// Default to work_1 if not specified
	auto work_version (work_version_optional_impl (oslo::work_version::work_1));
	if (!ec && account_opt.is_initialized ())
	{
		account = account_impl (account_opt.get ());
	}
	if (!ec)
	{
		auto hash (hash_impl ());
		auto difficulty (difficulty_optional_impl (work_version));
		multiplier_optional_impl (work_version, difficulty);
		if (!ec && (difficulty > node.max_work_generate_difficulty (work_version) || difficulty < oslo::work_threshold_entry (work_version)))
		{
			ec = oslo::error_rpc::difficulty_limit;
		}
		// Retrieving optional block
		std::shared_ptr<oslo::block> block;
		if (!ec && request.count ("block"))
		{
			block = block_impl (true);
			if (block != nullptr)
			{
				if (hash != block->root ())
				{
					ec = oslo::error_rpc::block_root_mismatch;
				}
				if (request.count ("version") == 0)
				{
					work_version = block->work_version ();
				}
				else if (!ec && work_version != block->work_version ())
				{
					ec = oslo::error_rpc::block_work_version_mismatch;
				}
				// Difficulty calculation
				if (!ec && request.count ("difficulty") == 0 && request.count ("multiplier") == 0)
				{
					difficulty = difficulty_ledger (*block);
				}
				// If optional block difficulty is higher than requested difficulty, send error
				if (!ec && block->difficulty () >= difficulty)
				{
					ec = oslo::error_rpc::block_work_enough;
				}
			}
		}
		if (!ec && response_l.empty ())
		{
			auto use_peers (request.get<bool> ("use_peers", false));
			auto rpc_l (shared_from_this ());
			auto callback = [rpc_l, hash, work_version, this](boost::optional<uint64_t> const & work_a) {
				if (work_a)
				{
					boost::property_tree::ptree response_l;
					response_l.put ("hash", hash.to_string ());
					uint64_t work (work_a.value ());
					response_l.put ("work", oslo::to_string_hex (work));
					std::stringstream ostream;
					auto result_difficulty (oslo::work_difficulty (work_version, hash, work));
					response_l.put ("difficulty", oslo::to_string_hex (result_difficulty));
					auto result_multiplier = oslo::difficulty::to_multiplier (result_difficulty, node.default_difficulty (work_version));
					response_l.put ("multiplier", oslo::to_string (result_multiplier));
					boost::property_tree::write_json (ostream, response_l);
					rpc_l->response (ostream.str ());
				}
				else
				{
					json_error_response (rpc_l->response, "Cancelled");
				}
			};
			if (!use_peers)
			{
				if (node.local_work_generation_enabled ())
				{
					auto error = node.distributed_work.make (work_version, hash, {}, difficulty, callback, {});
					if (error)
					{
						ec = oslo::error_common::failure_work_generation;
					}
				}
				else
				{
					ec = oslo::error_common::disabled_local_work_generation;
				}
			}
			else
			{
				if (!account_opt.is_initialized ())
				{
					// Fetch account from block if not given
					auto transaction_l (node.store.tx_begin_read ());
					if (node.store.block_exists (transaction_l, hash))
					{
						account = node.store.block_account (transaction_l, hash);
					}
				}
				auto secondary_work_peers_l (request.get<bool> ("secondary_work_peers", false));
				auto const & peers_l (secondary_work_peers_l ? node.config.secondary_work_peers : node.config.work_peers);
				if (node.work_generation_enabled (peers_l))
				{
					node.work_generate (work_version, hash, difficulty, callback, account, secondary_work_peers_l);
				}
				else
				{
					ec = oslo::error_common::disabled_work_generation;
				}
			}
		}
	}
	// Because of callback
	if (ec)
	{
		response_errors ();
	}
}

void oslo::json_handler::work_cancel ()
{
	auto hash (hash_impl ());
	if (!ec)
	{
		node.observers.work_cancel.notify (hash);
	}
	response_errors ();
}

void oslo::json_handler::work_get ()
{
	auto wallet (wallet_impl ());
	auto account (account_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		wallet_account_impl (transaction, wallet, account);
		if (!ec)
		{
			uint64_t work (0);
			auto error_work (wallet->store.work_get (transaction, account, work));
			(void)error_work;
			response_l.put ("work", oslo::to_string_hex (work));
		}
	}
	response_errors ();
}

void oslo::json_handler::work_set ()
{
	node.worker.push_task (create_worker_task ([](std::shared_ptr<oslo::json_handler> const & rpc_l) {
		auto wallet (rpc_l->wallet_impl ());
		auto account (rpc_l->account_impl ());
		auto work (rpc_l->work_optional_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			rpc_l->wallet_account_impl (transaction, wallet, account);
			if (!rpc_l->ec)
			{
				wallet->store.work_put (transaction, account, work);
				rpc_l->response_l.put ("success", "");
			}
		}
		rpc_l->response_errors ();
	}));
}

void oslo::json_handler::work_validate ()
{
	auto hash (hash_impl ());
	auto work (work_optional_impl ());
	// Default to work_1 if not specified
	auto work_version (work_version_optional_impl (oslo::work_version::work_1));
	auto difficulty (difficulty_optional_impl (work_version));
	multiplier_optional_impl (work_version, difficulty);
	if (!ec)
	{
		/* Transition to epoch_2 difficulty levels breaks previous behavior.
		 * When difficulty is not given, the default difficulty to validate changes when the first epoch_2 block is seen, breaking previous behavior.
		 * For this reason, when difficulty is not given, the "valid" field is no longer included in the response to break loudly any client expecting it.
		 * Instead, use the new fields:
		 * * valid_all: the work is valid at the current highest difficulty threshold
		 * * valid_receive: the work is valid for a receive block in an epoch_2 upgraded account
		 */

		auto result_difficulty (oslo::work_difficulty (work_version, hash, work));
		if (request.count ("difficulty"))
		{
			response_l.put ("valid", (result_difficulty >= difficulty) ? "1" : "0");
		}
		response_l.put ("valid_all", (result_difficulty >= node.default_difficulty (work_version)) ? "1" : "0");
		response_l.put ("valid_receive", (result_difficulty >= oslo::work_threshold (work_version, oslo::block_details (oslo::epoch::epoch_2, false, true, false))) ? "1" : "0");
		response_l.put ("difficulty", oslo::to_string_hex (result_difficulty));
		auto result_multiplier = oslo::difficulty::to_multiplier (result_difficulty, node.default_difficulty (work_version));
		response_l.put ("multiplier", oslo::to_string (result_multiplier));
	}
	response_errors ();
}

void oslo::json_handler::work_peer_add ()
{
	std::string address_text = request.get<std::string> ("address");
	std::string port_text = request.get<std::string> ("port");
	uint16_t port;
	if (!oslo::parse_port (port_text, port))
	{
		node.config.work_peers.push_back (std::make_pair (address_text, port));
		response_l.put ("success", "");
	}
	else
	{
		ec = oslo::error_common::invalid_port;
	}
	response_errors ();
}

void oslo::json_handler::work_peers ()
{
	boost::property_tree::ptree work_peers_l;
	for (auto i (node.config.work_peers.begin ()), n (node.config.work_peers.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		entry.put ("", boost::str (boost::format ("%1%:%2%") % i->first % i->second));
		work_peers_l.push_back (std::make_pair ("", entry));
	}
	response_l.add_child ("work_peers", work_peers_l);
	response_errors ();
}

void oslo::json_handler::work_peers_clear ()
{
	node.config.work_peers.clear ();
	response_l.put ("success", "");
	response_errors ();
}

void oslo::inprocess_rpc_handler::process_request (std::string const &, std::string const & body_a, std::function<void(std::string const &)> response_a)
{
	// Note that if the rpc action is async, the shared_ptr<json_handler> lifetime will be extended by the action handler
	auto handler (std::make_shared<oslo::json_handler> (node, node_rpc_config, body_a, response_a, [this]() {
		this->stop_callback ();
		this->stop ();
	}));
	handler->process_request ();
}

void oslo::inprocess_rpc_handler::process_request_v2 (rpc_handler_request_params const & params_a, std::string const & body_a, std::function<void(std::shared_ptr<std::string>)> response_a)
{
	std::string body_l = params_a.json_envelope (body_a);
	auto handler (std::make_shared<oslo::ipc::flatbuffers_handler> (node, ipc_server, nullptr, node.config.ipc_config));
	handler->process_json (reinterpret_cast<const uint8_t *> (body_l.data ()), body_l.size (), response_a);
}

namespace
{
void construct_json (oslo::container_info_component * component, boost::property_tree::ptree & parent)
{
	// We are a leaf node, print name and exit
	if (!component->is_composite ())
	{
		auto & leaf_info = static_cast<oslo::container_info_leaf *> (component)->get_info ();
		boost::property_tree::ptree child;
		child.put ("count", leaf_info.count);
		child.put ("size", leaf_info.count * leaf_info.sizeof_element);
		parent.add_child (leaf_info.name, child);
		return;
	}

	auto composite = static_cast<oslo::container_info_composite *> (component);

	boost::property_tree::ptree current;
	for (auto & child : composite->get_children ())
	{
		construct_json (child.get (), current);
	}

	parent.add_child (composite->get_name (), current);
}

// Any RPC handlers which require no arguments (excl default arguments) should go here.
// This is to prevent large if/else chains which compilers can have limits for (MSVC for instance has 128).
ipc_json_handler_no_arg_func_map create_ipc_json_handler_no_arg_func_map ()
{
	ipc_json_handler_no_arg_func_map no_arg_funcs;
	no_arg_funcs.emplace ("account_balance", &oslo::json_handler::account_balance);
	no_arg_funcs.emplace ("account_block_count", &oslo::json_handler::account_block_count);
	no_arg_funcs.emplace ("account_count", &oslo::json_handler::account_count);
	no_arg_funcs.emplace ("account_create", &oslo::json_handler::account_create);
	no_arg_funcs.emplace ("account_get", &oslo::json_handler::account_get);
	no_arg_funcs.emplace ("account_history", &oslo::json_handler::account_history);
	no_arg_funcs.emplace ("account_info", &oslo::json_handler::account_info);
	no_arg_funcs.emplace ("account_key", &oslo::json_handler::account_key);
	no_arg_funcs.emplace ("account_list", &oslo::json_handler::account_list);
	no_arg_funcs.emplace ("account_move", &oslo::json_handler::account_move);
	no_arg_funcs.emplace ("account_remove", &oslo::json_handler::account_remove);
	no_arg_funcs.emplace ("account_representative", &oslo::json_handler::account_representative);
	no_arg_funcs.emplace ("account_representative_set", &oslo::json_handler::account_representative_set);
	no_arg_funcs.emplace ("account_weight", &oslo::json_handler::account_weight);
	no_arg_funcs.emplace ("accounts_balances", &oslo::json_handler::accounts_balances);
	no_arg_funcs.emplace ("accounts_create", &oslo::json_handler::accounts_create);
	no_arg_funcs.emplace ("accounts_frontiers", &oslo::json_handler::accounts_frontiers);
	no_arg_funcs.emplace ("accounts_pending", &oslo::json_handler::accounts_pending);
	no_arg_funcs.emplace ("active_difficulty", &oslo::json_handler::active_difficulty);
	no_arg_funcs.emplace ("available_supply", &oslo::json_handler::available_supply);
	no_arg_funcs.emplace ("block_info", &oslo::json_handler::block_info);
	no_arg_funcs.emplace ("block", &oslo::json_handler::block_info);
	no_arg_funcs.emplace ("block_confirm", &oslo::json_handler::block_confirm);
	no_arg_funcs.emplace ("blocks", &oslo::json_handler::blocks);
	no_arg_funcs.emplace ("blocks_info", &oslo::json_handler::blocks_info);
	no_arg_funcs.emplace ("block_account", &oslo::json_handler::block_account);
	no_arg_funcs.emplace ("block_count", &oslo::json_handler::block_count);
	no_arg_funcs.emplace ("block_count_type", &oslo::json_handler::block_count_type);
	no_arg_funcs.emplace ("block_create", &oslo::json_handler::block_create);
	no_arg_funcs.emplace ("block_hash", &oslo::json_handler::block_hash);
	no_arg_funcs.emplace ("bootstrap", &oslo::json_handler::bootstrap);
	no_arg_funcs.emplace ("bootstrap_any", &oslo::json_handler::bootstrap_any);
	no_arg_funcs.emplace ("bootstrap_lazy", &oslo::json_handler::bootstrap_lazy);
	no_arg_funcs.emplace ("bootstrap_status", &oslo::json_handler::bootstrap_status);
	no_arg_funcs.emplace ("confirmation_active", &oslo::json_handler::confirmation_active);
	no_arg_funcs.emplace ("confirmation_height_currently_processing", &oslo::json_handler::confirmation_height_currently_processing);
	no_arg_funcs.emplace ("confirmation_history", &oslo::json_handler::confirmation_history);
	no_arg_funcs.emplace ("confirmation_info", &oslo::json_handler::confirmation_info);
	no_arg_funcs.emplace ("confirmation_quorum", &oslo::json_handler::confirmation_quorum);
	no_arg_funcs.emplace ("database_txn_tracker", &oslo::json_handler::database_txn_tracker);
	no_arg_funcs.emplace ("delegators", &oslo::json_handler::delegators);
	no_arg_funcs.emplace ("delegators_count", &oslo::json_handler::delegators_count);
	no_arg_funcs.emplace ("deterministic_key", &oslo::json_handler::deterministic_key);
	no_arg_funcs.emplace ("epoch_upgrade", &oslo::json_handler::epoch_upgrade);
	no_arg_funcs.emplace ("frontiers", &oslo::json_handler::frontiers);
	no_arg_funcs.emplace ("frontier_count", &oslo::json_handler::account_count);
	no_arg_funcs.emplace ("keepalive", &oslo::json_handler::keepalive);
	no_arg_funcs.emplace ("key_create", &oslo::json_handler::key_create);
	no_arg_funcs.emplace ("key_expand", &oslo::json_handler::key_expand);
	no_arg_funcs.emplace ("ledger", &oslo::json_handler::ledger);
	no_arg_funcs.emplace ("node_id", &oslo::json_handler::node_id);
	no_arg_funcs.emplace ("node_id_delete", &oslo::json_handler::node_id_delete);
	no_arg_funcs.emplace ("password_change", &oslo::json_handler::password_change);
	no_arg_funcs.emplace ("password_enter", &oslo::json_handler::password_enter);
	no_arg_funcs.emplace ("wallet_unlock", &oslo::json_handler::password_enter);
	no_arg_funcs.emplace ("payment_begin", &oslo::json_handler::payment_begin);
	no_arg_funcs.emplace ("payment_init", &oslo::json_handler::payment_init);
	no_arg_funcs.emplace ("payment_end", &oslo::json_handler::payment_end);
	no_arg_funcs.emplace ("payment_wait", &oslo::json_handler::payment_wait);
	no_arg_funcs.emplace ("peers", &oslo::json_handler::peers);
	no_arg_funcs.emplace ("pending", &oslo::json_handler::pending);
	no_arg_funcs.emplace ("pending_exists", &oslo::json_handler::pending_exists);
	no_arg_funcs.emplace ("process", &oslo::json_handler::process);
	no_arg_funcs.emplace ("receive", &oslo::json_handler::receive);
	no_arg_funcs.emplace ("receive_minimum", &oslo::json_handler::receive_minimum);
	no_arg_funcs.emplace ("receive_minimum_set", &oslo::json_handler::receive_minimum_set);
	no_arg_funcs.emplace ("representatives", &oslo::json_handler::representatives);
	no_arg_funcs.emplace ("representatives_online", &oslo::json_handler::representatives_online);
	no_arg_funcs.emplace ("republish", &oslo::json_handler::republish);
	no_arg_funcs.emplace ("search_pending", &oslo::json_handler::search_pending);
	no_arg_funcs.emplace ("search_pending_all", &oslo::json_handler::search_pending_all);
	no_arg_funcs.emplace ("send", &oslo::json_handler::send);
	no_arg_funcs.emplace ("sign", &oslo::json_handler::sign);
	no_arg_funcs.emplace ("stats", &oslo::json_handler::stats);
	no_arg_funcs.emplace ("stats_clear", &oslo::json_handler::stats_clear);
	no_arg_funcs.emplace ("stop", &oslo::json_handler::stop);
	no_arg_funcs.emplace ("telemetry", &oslo::json_handler::telemetry);
	no_arg_funcs.emplace ("unchecked", &oslo::json_handler::unchecked);
	no_arg_funcs.emplace ("unchecked_clear", &oslo::json_handler::unchecked_clear);
	no_arg_funcs.emplace ("unchecked_get", &oslo::json_handler::unchecked_get);
	no_arg_funcs.emplace ("unchecked_keys", &oslo::json_handler::unchecked_keys);
	no_arg_funcs.emplace ("unopened", &oslo::json_handler::unopened);
	no_arg_funcs.emplace ("uptime", &oslo::json_handler::uptime);
	no_arg_funcs.emplace ("validate_account_number", &oslo::json_handler::validate_account_number);
	no_arg_funcs.emplace ("version", &oslo::json_handler::version);
	no_arg_funcs.emplace ("wallet_add", &oslo::json_handler::wallet_add);
	no_arg_funcs.emplace ("wallet_add_watch", &oslo::json_handler::wallet_add_watch);
	no_arg_funcs.emplace ("wallet_balances", &oslo::json_handler::wallet_balances);
	no_arg_funcs.emplace ("wallet_change_seed", &oslo::json_handler::wallet_change_seed);
	no_arg_funcs.emplace ("wallet_contains", &oslo::json_handler::wallet_contains);
	no_arg_funcs.emplace ("wallet_create", &oslo::json_handler::wallet_create);
	no_arg_funcs.emplace ("wallet_destroy", &oslo::json_handler::wallet_destroy);
	no_arg_funcs.emplace ("wallet_export", &oslo::json_handler::wallet_export);
	no_arg_funcs.emplace ("wallet_frontiers", &oslo::json_handler::wallet_frontiers);
	no_arg_funcs.emplace ("wallet_history", &oslo::json_handler::wallet_history);
	no_arg_funcs.emplace ("wallet_info", &oslo::json_handler::wallet_info);
	no_arg_funcs.emplace ("wallet_balance_total", &oslo::json_handler::wallet_info);
	no_arg_funcs.emplace ("wallet_key_valid", &oslo::json_handler::wallet_key_valid);
	no_arg_funcs.emplace ("wallet_ledger", &oslo::json_handler::wallet_ledger);
	no_arg_funcs.emplace ("wallet_lock", &oslo::json_handler::wallet_lock);
	no_arg_funcs.emplace ("wallet_pending", &oslo::json_handler::wallet_pending);
	no_arg_funcs.emplace ("wallet_representative", &oslo::json_handler::wallet_representative);
	no_arg_funcs.emplace ("wallet_representative_set", &oslo::json_handler::wallet_representative_set);
	no_arg_funcs.emplace ("wallet_republish", &oslo::json_handler::wallet_republish);
	no_arg_funcs.emplace ("wallet_work_get", &oslo::json_handler::wallet_work_get);
	no_arg_funcs.emplace ("work_generate", &oslo::json_handler::work_generate);
	no_arg_funcs.emplace ("work_cancel", &oslo::json_handler::work_cancel);
	no_arg_funcs.emplace ("work_get", &oslo::json_handler::work_get);
	no_arg_funcs.emplace ("work_set", &oslo::json_handler::work_set);
	no_arg_funcs.emplace ("work_validate", &oslo::json_handler::work_validate);
	no_arg_funcs.emplace ("work_peer_add", &oslo::json_handler::work_peer_add);
	no_arg_funcs.emplace ("work_peers", &oslo::json_handler::work_peers);
	no_arg_funcs.emplace ("work_peers_clear", &oslo::json_handler::work_peers_clear);
	return no_arg_funcs;
}

/** Due to the asynchronous nature of updating confirmation heights, it can also be necessary to check active roots */
bool block_confirmed (oslo::node & node, oslo::transaction & transaction, oslo::block_hash const & hash, bool include_active, bool include_only_confirmed)
{
	bool is_confirmed = false;
	if (include_active && !include_only_confirmed)
	{
		is_confirmed = true;
	}
	// Check whether the confirmation height is set
	else if (node.ledger.block_confirmed (transaction, hash))
	{
		is_confirmed = true;
	}
	// This just checks it's not currently undergoing an active transaction
	else if (!include_only_confirmed)
	{
		auto block (node.store.block_get (transaction, hash));
		is_confirmed = (block != nullptr && !node.active.active (*block));
	}

	return is_confirmed;
}

const char * epoch_as_string (oslo::epoch epoch)
{
	switch (epoch)
	{
		case oslo::epoch::epoch_2:
			return "2";
		case oslo::epoch::epoch_1:
			return "1";
		default:
			return "0";
	}
}
}
