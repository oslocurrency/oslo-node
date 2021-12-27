#define IGNORE_GTEST_INCL
#include <oslo/core_test/testutil.hpp>
#include <oslo/crypto_lib/random_pool.hpp>
#include <oslo/node/common.hpp>
#include <oslo/node/testing.hpp>
#include <oslo/node/transport/udp.hpp>

#include <boost/property_tree/json_parser.hpp>

#include <cstdlib>

std::string oslo::error_system_messages::message (int ev) const
{
	switch (static_cast<oslo::error_system> (ev))
	{
		case oslo::error_system::generic:
			return "Unknown error";
		case oslo::error_system::deadline_expired:
			return "Deadline expired";
	}

	return "Invalid error code";
}

std::shared_ptr<oslo::node> oslo::system::add_node (oslo::node_flags node_flags_a, oslo::transport::transport_type type_a)
{
	return add_node (oslo::node_config (oslo::get_available_port (), logging), node_flags_a, type_a);
}

/** Returns the node added. */
std::shared_ptr<oslo::node> oslo::system::add_node (oslo::node_config const & node_config_a, oslo::node_flags node_flags_a, oslo::transport::transport_type type_a)
{
	auto node (std::make_shared<oslo::node> (io_ctx, oslo::unique_path (), alarm, node_config_a, work, node_flags_a, node_sequence++));
	debug_assert (!node->init_error ());
	node->start ();
	node->wallets.create (oslo::random_wallet_id ());
	nodes.reserve (nodes.size () + 1);
	nodes.push_back (node);
	if (nodes.size () > 1)
	{
		debug_assert (nodes.size () - 1 <= node->network_params.node.max_peers_per_ip || node->flags.disable_max_peers_per_ip); // Check that we don't start more nodes than limit for single IP address
		auto begin = nodes.end () - 2;
		for (auto i (begin), j (begin + 1), n (nodes.end ()); j != n; ++i, ++j)
		{
			auto node1 (*i);
			auto node2 (*j);
			auto starting1 (node1->network.size ());
			size_t starting_listener1 (node1->bootstrap.realtime_count);
			decltype (starting1) new1;
			auto starting2 (node2->network.size ());
			size_t starting_listener2 (node2->bootstrap.realtime_count);
			decltype (starting2) new2;
			if (type_a == oslo::transport::transport_type::tcp)
			{
				(*j)->network.merge_peer ((*i)->network.endpoint ());
			}
			else
			{
				// UDP connection
				auto channel (std::make_shared<oslo::transport::channel_udp> ((*j)->network.udp_channels, (*i)->network.endpoint (), node1->network_params.protocol.protocol_version));
				(*j)->network.send_keepalive (channel);
			}
			do
			{
				poll ();
				new1 = node1->network.size ();
				new2 = node2->network.size ();
			} while (new1 == starting1 || new2 == starting2);
			if (type_a == oslo::transport::transport_type::tcp && node_config_a.tcp_incoming_connections_max != 0 && !node_flags_a.disable_tcp_realtime)
			{
				// Wait for initial connection finish
				decltype (starting_listener1) new_listener1;
				decltype (starting_listener2) new_listener2;
				do
				{
					poll ();
					new_listener1 = node1->bootstrap.realtime_count;
					new_listener2 = node2->bootstrap.realtime_count;
				} while (new_listener1 == starting_listener1 || new_listener2 == starting_listener2);
			}
		}
		auto iterations1 (0);
		while (std::any_of (begin, nodes.end (), [](std::shared_ptr<oslo::node> const & node_a) { return node_a->bootstrap_initiator.in_progress (); }))
		{
			poll ();
			++iterations1;
			debug_assert (iterations1 < 10000);
		}
	}
	else
	{
		auto iterations1 (0);
		while (node->bootstrap_initiator.in_progress ())
		{
			poll ();
			++iterations1;
			debug_assert (iterations1 < 10000);
		}
	}

	return node;
}

oslo::system::system ()
{
	auto scale_str = std::getenv ("DEADLINE_SCALE_FACTOR");
	if (scale_str)
	{
		deadline_scaling_factor = std::stod (scale_str);
	}
	logging.init (oslo::unique_path ());
}

oslo::system::system (uint16_t count_a, oslo::transport::transport_type type_a, oslo::node_flags flags_a) :
system ()
{
	nodes.reserve (count_a);
	for (uint16_t i (0); i < count_a; ++i)
	{
		oslo::node_config config (oslo::get_available_port (), logging);
		add_node (config, flags_a, type_a);
	}
}

oslo::system::~system ()
{
	for (auto & i : nodes)
	{
		i->stop ();
	}

#ifndef _WIN32
	// Windows cannot remove the log and data files while they are still owned by this process.
	// They will be removed later

	// Clean up tmp directories created by the tests. Since it's sometimes useful to
	// see log files after test failures, an environment variable is supported to
	// retain the files.
	if (std::getenv ("TEST_KEEP_TMPDIRS") == nullptr)
	{
		oslo::remove_temporary_directories ();
	}
#endif
}

std::shared_ptr<oslo::wallet> oslo::system::wallet (size_t index_a)
{
	debug_assert (nodes.size () > index_a);
	auto size (nodes[index_a]->wallets.items.size ());
	(void)size;
	debug_assert (size == 1);
	return nodes[index_a]->wallets.items.begin ()->second;
}

oslo::account oslo::system::account (oslo::transaction const & transaction_a, size_t index_a)
{
	auto wallet_l (wallet (index_a));
	auto keys (wallet_l->store.begin (transaction_a));
	debug_assert (keys != wallet_l->store.end ());
	auto result (keys->first);
	debug_assert (++keys == wallet_l->store.end ());
	return oslo::account (result);
}

uint64_t oslo::system::work_generate_limited (oslo::block_hash const & root_a, uint64_t min_a, uint64_t max_a)
{
	debug_assert (min_a > 0);
	uint64_t result = 0;
	do
	{
		result = *work.generate (root_a, min_a);
	} while (oslo::work_difficulty (oslo::work_version::work_1, root_a, result) >= max_a);
	return result;
}

std::unique_ptr<oslo::state_block> oslo::upgrade_epoch (oslo::work_pool & pool_a, oslo::ledger & ledger_a, oslo::epoch epoch_a)
{
	auto transaction (ledger_a.store.tx_begin_write ());
	auto account = oslo::test_genesis_key.pub;
	auto latest = ledger_a.latest (transaction, account);
	auto balance = ledger_a.account_balance (transaction, account);

	oslo::state_block_builder builder;
	std::error_code ec;
	auto epoch = builder
	             .account (oslo::test_genesis_key.pub)
	             .previous (latest)
	             .balance (balance)
	             .link (ledger_a.epoch_link (epoch_a))
	             .representative (oslo::test_genesis_key.pub)
	             .sign (oslo::test_genesis_key.prv, oslo::test_genesis_key.pub)
	             .work (*pool_a.generate (latest, oslo::work_threshold (oslo::work_version::work_1, oslo::block_details (epoch_a, false, false, true))))
	             .build (ec);

	bool error{ true };
	if (!ec && epoch)
	{
		error = ledger_a.process (transaction, *epoch).code != oslo::process_result::progress;
	}

	return !error ? std::move (epoch) : nullptr;
}

void oslo::blocks_confirm (oslo::node & node_a, std::vector<std::shared_ptr<oslo::block>> const & blocks_a)
{
	// Finish processing all blocks
	node_a.block_processor.flush ();
	for (auto const & block : blocks_a)
	{
		// A sideband is required to start an election
		debug_assert (block->has_sideband ());
		node_a.block_confirm (block);
	}
}

std::unique_ptr<oslo::state_block> oslo::system::upgrade_genesis_epoch (oslo::node & node_a, oslo::epoch const epoch_a)
{
	return upgrade_epoch (work, node_a.ledger, epoch_a);
}

void oslo::system::deadline_set (std::chrono::duration<double, std::nano> const & delta_a)
{
	deadline = std::chrono::steady_clock::now () + delta_a * deadline_scaling_factor;
}

std::error_code oslo::system::poll (std::chrono::nanoseconds const & wait_time)
{
	std::error_code ec;
	io_ctx.run_one_for (wait_time);

	if (std::chrono::steady_clock::now () > deadline)
	{
		ec = oslo::error_system::deadline_expired;
		stop ();
	}
	return ec;
}

std::error_code oslo::system::poll_until_true (std::chrono::nanoseconds deadline_a, std::function<bool()> predicate_a)
{
	std::error_code ec;
	deadline_set (deadline_a);
	while (!ec && !predicate_a ())
	{
		ec = poll ();
	}
	return ec;
}

namespace
{
class traffic_generator : public std::enable_shared_from_this<traffic_generator>
{
public:
	traffic_generator (uint32_t count_a, uint32_t wait_a, std::shared_ptr<oslo::node> const & node_a, oslo::system & system_a) :
	count (count_a),
	wait (wait_a),
	node (node_a),
	system (system_a)
	{
	}
	void run ()
	{
		auto count_l (count - 1);
		count = count_l - 1;
		system.generate_activity (*node, accounts);
		if (count_l > 0)
		{
			auto this_l (shared_from_this ());
			node->alarm.add (std::chrono::steady_clock::now () + std::chrono::milliseconds (wait), [this_l]() { this_l->run (); });
		}
	}
	std::vector<oslo::account> accounts;
	uint32_t count;
	uint32_t wait;
	std::shared_ptr<oslo::node> node;
	oslo::system & system;
};
}

void oslo::system::generate_usage_traffic (uint32_t count_a, uint32_t wait_a)
{
	for (size_t i (0), n (nodes.size ()); i != n; ++i)
	{
		generate_usage_traffic (count_a, wait_a, i);
	}
}

void oslo::system::generate_usage_traffic (uint32_t count_a, uint32_t wait_a, size_t index_a)
{
	debug_assert (nodes.size () > index_a);
	debug_assert (count_a > 0);
	auto generate (std::make_shared<traffic_generator> (count_a, wait_a, nodes[index_a], *this));
	generate->run ();
}

void oslo::system::generate_rollback (oslo::node & node_a, std::vector<oslo::account> & accounts_a)
{
	auto transaction (node_a.store.tx_begin_write ());
	debug_assert (std::numeric_limits<CryptoPP::word32>::max () > accounts_a.size ());
	auto index (random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (accounts_a.size () - 1)));
	auto account (accounts_a[index]);
	oslo::account_info info;
	auto error (node_a.store.account_get (transaction, account, info));
	if (!error)
	{
		auto hash (info.open_block);
		oslo::genesis genesis;
		if (hash != genesis.hash ())
		{
			accounts_a[index] = accounts_a[accounts_a.size () - 1];
			accounts_a.pop_back ();
			std::vector<std::shared_ptr<oslo::block>> rollback_list;
			auto error = node_a.ledger.rollback (transaction, hash, rollback_list);
			(void)error;
			debug_assert (!error);
			for (auto & i : rollback_list)
			{
				node_a.wallets.watcher->remove (*i);
				node_a.active.erase (*i);
			}
		}
	}
}

void oslo::system::generate_receive (oslo::node & node_a)
{
	std::shared_ptr<oslo::block> send_block;
	{
		auto transaction (node_a.store.tx_begin_read ());
		oslo::account random_account;
		random_pool::generate_block (random_account.bytes.data (), sizeof (random_account.bytes));
		auto i (node_a.store.pending_begin (transaction, oslo::pending_key (random_account, 0)));
		if (i != node_a.store.pending_end ())
		{
			oslo::pending_key const & send_hash (i->first);
			send_block = node_a.store.block_get (transaction, send_hash.hash);
		}
	}
	if (send_block != nullptr)
	{
		auto receive_error (wallet (0)->receive_sync (send_block, oslo::genesis_account, std::numeric_limits<oslo::uint128_t>::max ()));
		(void)receive_error;
	}
}

void oslo::system::generate_activity (oslo::node & node_a, std::vector<oslo::account> & accounts_a)
{
	auto what (random_pool::generate_byte ());
	if (what < 0x1)
	{
		generate_rollback (node_a, accounts_a);
	}
	else if (what < 0x10)
	{
		generate_change_known (node_a, accounts_a);
	}
	else if (what < 0x20)
	{
		generate_change_unknown (node_a, accounts_a);
	}
	else if (what < 0x70)
	{
		generate_receive (node_a);
	}
	else if (what < 0xc0)
	{
		generate_send_existing (node_a, accounts_a);
	}
	else
	{
		generate_send_new (node_a, accounts_a);
	}
}

oslo::account oslo::system::get_random_account (std::vector<oslo::account> & accounts_a)
{
	debug_assert (std::numeric_limits<CryptoPP::word32>::max () > accounts_a.size ());
	auto index (random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (accounts_a.size () - 1)));
	auto result (accounts_a[index]);
	return result;
}

oslo::uint128_t oslo::system::get_random_amount (oslo::transaction const & transaction_a, oslo::node & node_a, oslo::account const & account_a)
{
	oslo::uint128_t balance (node_a.ledger.account_balance (transaction_a, account_a));
	oslo::uint128_union random_amount;
	oslo::random_pool::generate_block (random_amount.bytes.data (), sizeof (random_amount.bytes));
	return (((oslo::uint256_t{ random_amount.number () } * balance) / oslo::uint256_t{ std::numeric_limits<oslo::uint128_t>::max () }).convert_to<oslo::uint128_t> ());
}

void oslo::system::generate_send_existing (oslo::node & node_a, std::vector<oslo::account> & accounts_a)
{
	oslo::uint128_t amount;
	oslo::account destination;
	oslo::account source;
	{
		oslo::account account;
		random_pool::generate_block (account.bytes.data (), sizeof (account.bytes));
		auto transaction (node_a.store.tx_begin_read ());
		oslo::store_iterator<oslo::account, oslo::account_info> entry (node_a.store.latest_begin (transaction, account));
		if (entry == node_a.store.latest_end ())
		{
			entry = node_a.store.latest_begin (transaction);
		}
		debug_assert (entry != node_a.store.latest_end ());
		destination = oslo::account (entry->first);
		source = get_random_account (accounts_a);
		amount = get_random_amount (transaction, node_a, source);
	}
	if (!amount.is_zero ())
	{
		auto hash (wallet (0)->send_sync (source, destination, amount));
		(void)hash;
		debug_assert (!hash.is_zero ());
	}
}

void oslo::system::generate_change_known (oslo::node & node_a, std::vector<oslo::account> & accounts_a)
{
	oslo::account source (get_random_account (accounts_a));
	if (!node_a.latest (source).is_zero ())
	{
		oslo::account destination (get_random_account (accounts_a));
		auto change_error (wallet (0)->change_sync (source, destination));
		(void)change_error;
		debug_assert (!change_error);
	}
}

void oslo::system::generate_change_unknown (oslo::node & node_a, std::vector<oslo::account> & accounts_a)
{
	oslo::account source (get_random_account (accounts_a));
	if (!node_a.latest (source).is_zero ())
	{
		oslo::keypair key;
		oslo::account destination (key.pub);
		auto change_error (wallet (0)->change_sync (source, destination));
		(void)change_error;
		debug_assert (!change_error);
	}
}

void oslo::system::generate_send_new (oslo::node & node_a, std::vector<oslo::account> & accounts_a)
{
	debug_assert (node_a.wallets.items.size () == 1);
	oslo::uint128_t amount;
	oslo::account source;
	{
		auto transaction (node_a.store.tx_begin_read ());
		source = get_random_account (accounts_a);
		amount = get_random_amount (transaction, node_a, source);
	}
	if (!amount.is_zero ())
	{
		auto pub (node_a.wallets.items.begin ()->second->deterministic_insert ());
		accounts_a.push_back (pub);
		auto hash (wallet (0)->send_sync (source, pub, amount));
		(void)hash;
		debug_assert (!hash.is_zero ());
	}
}

void oslo::system::generate_mass_activity (uint32_t count_a, oslo::node & node_a)
{
	std::vector<oslo::account> accounts;
	wallet (0)->insert_adhoc (oslo::test_genesis_key.prv);
	accounts.push_back (oslo::test_genesis_key.pub);
	auto previous (std::chrono::steady_clock::now ());
	for (uint32_t i (0); i < count_a; ++i)
	{
		if ((i & 0xff) == 0)
		{
			auto now (std::chrono::steady_clock::now ());
			auto us (std::chrono::duration_cast<std::chrono::microseconds> (now - previous).count ());
			uint64_t count (0);
			uint64_t state (0);
			{
				auto transaction (node_a.store.tx_begin_read ());
				auto block_counts (node_a.store.block_count (transaction));
				count = block_counts.sum ();
				state = block_counts.state;
			}
			std::cerr << boost::str (boost::format ("Mass activity iteration %1% us %2% us/t %3% state: %4% old: %5%\n") % i % us % (us / 256) % state % (count - state));
			previous = now;
		}
		generate_activity (node_a, accounts);
	}
}

void oslo::system::stop ()
{
	for (auto i : nodes)
	{
		i->stop ();
	}
	work.stop ();
}

namespace oslo
{
void cleanup_test_directories_on_exit ()
{
	// Makes sure everything is cleaned up
	oslo::logging::release_file_sink ();
	// Clean up tmp directories created by the tests. Since it's sometimes useful to
	// see log files after test failures, an environment variable is supported to
	// retain the files.
	if (std::getenv ("TEST_KEEP_TMPDIRS") == nullptr)
	{
		oslo::remove_temporary_directories ();
	}
}
}
