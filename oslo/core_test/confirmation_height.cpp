#include <oslo/core_test/testutil.hpp>
#include <oslo/node/election.hpp>
#include <oslo/node/testing.hpp>

#include <gtest/gtest.h>

#include <boost/format.hpp>

using namespace std::chrono_literals;

namespace
{
void add_callback_stats (oslo::node & node, std::vector<oslo::block_hash> * observer_order = nullptr, std::mutex * mutex = nullptr)
{
	node.observers.blocks.add ([& stats = node.stats, observer_order, mutex](oslo::election_status const & status_a, oslo::account const &, oslo::amount const &, bool) {
		stats.inc (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out);
		if (mutex)
		{
			oslo::lock_guard<std::mutex> guard (*mutex);
			debug_assert (observer_order);
			observer_order->push_back (status_a.winner->hash ());
		}
	});
}
oslo::stat::detail get_stats_detail (oslo::confirmation_height_mode mode_a)
{
	debug_assert (mode_a == oslo::confirmation_height_mode::bounded || mode_a == oslo::confirmation_height_mode::unbounded);
	return (mode_a == oslo::confirmation_height_mode::bounded) ? oslo::stat::detail::blocks_confirmed_bounded : oslo::stat::detail::blocks_confirmed_unbounded;
}
}

TEST (confirmation_height, single)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		auto amount (std::numeric_limits<oslo::uint128_t>::max ());
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		auto node = system.add_node (node_flags);
		oslo::keypair key1;
		system.wallet (0)->insert_adhoc (oslo::test_genesis_key.prv);
		oslo::block_hash latest1 (node->latest (oslo::test_genesis_key.pub));
		auto send1 (std::make_shared<oslo::state_block> (oslo::test_genesis_key.pub, latest1, oslo::test_genesis_key.pub, amount - 100, key1.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest1)));

		// Check confirmation heights before, should be uninitialized (1 for genesis).
		oslo::confirmation_height_info confirmation_height_info;
		add_callback_stats (*node);
		auto transaction = node->store.tx_begin_read ();
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
		ASSERT_EQ (1, confirmation_height_info.height);
		ASSERT_EQ (oslo::genesis_hash, confirmation_height_info.frontier);

		node->process_active (send1);
		node->block_processor.flush ();

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 1)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_TRUE (node->ledger.block_confirmed (transaction, send1->hash ()));
			ASSERT_FALSE (node->store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
			ASSERT_EQ (2, confirmation_height_info.height);
			ASSERT_EQ (send1->hash (), confirmation_height_info.frontier);

			// Rollbacks should fail as these blocks have been cemented
			ASSERT_TRUE (node->ledger.rollback (transaction, latest1));
			ASSERT_TRUE (node->ledger.rollback (transaction, send1->hash ()));
			ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
			ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
			ASSERT_EQ (1, node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
			ASSERT_EQ (2, node->ledger.cache.cemented_count);

			ASSERT_EQ (0, node->active.election_winner_details_size ());
		}
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

TEST (confirmation_height, multiple_accounts)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);
		oslo::keypair key1;
		oslo::keypair key2;
		oslo::keypair key3;
		oslo::block_hash latest1 (system.nodes[0]->latest (oslo::test_genesis_key.pub));

		// Send to all accounts
		oslo::send_block send1 (latest1, key1.pub, system.nodes.front ()->config.online_weight_minimum.number () + 300, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest1));
		oslo::send_block send2 (send1.hash (), key2.pub, system.nodes.front ()->config.online_weight_minimum.number () + 200, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1.hash ()));
		oslo::send_block send3 (send2.hash (), key3.pub, system.nodes.front ()->config.online_weight_minimum.number () + 100, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send2.hash ()));

		// Open all accounts
		oslo::open_block open1 (send1.hash (), oslo::genesis_account, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));
		oslo::open_block open2 (send2.hash (), oslo::genesis_account, key2.pub, key2.prv, key2.pub, *system.work.generate (key2.pub));
		oslo::open_block open3 (send3.hash (), oslo::genesis_account, key3.pub, key3.prv, key3.pub, *system.work.generate (key3.pub));

		// Send and receive various blocks to these accounts
		oslo::send_block send4 (open1.hash (), key2.pub, 50, key1.prv, key1.pub, *system.work.generate (open1.hash ()));
		oslo::send_block send5 (send4.hash (), key2.pub, 10, key1.prv, key1.pub, *system.work.generate (send4.hash ()));

		oslo::receive_block receive1 (open2.hash (), send4.hash (), key2.prv, key2.pub, *system.work.generate (open2.hash ()));
		oslo::send_block send6 (receive1.hash (), key3.pub, 10, key2.prv, key2.pub, *system.work.generate (receive1.hash ()));
		oslo::receive_block receive2 (send6.hash (), send5.hash (), key2.prv, key2.pub, *system.work.generate (send6.hash ()));

		add_callback_stats (*node);

		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send3).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open3).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send4).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send5).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send6).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive2).code);

			// Check confirmation heights of all the accounts are uninitialized (0),
			// as we have any just added them to the ledger and not processed any live transactions yet.
			oslo::confirmation_height_info confirmation_height_info;
			ASSERT_FALSE (node->store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
			ASSERT_EQ (1, confirmation_height_info.height);
			ASSERT_EQ (oslo::genesis_hash, confirmation_height_info.frontier);
			ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height_info));
			ASSERT_EQ (0, confirmation_height_info.height);
			ASSERT_EQ (oslo::block_hash (0), confirmation_height_info.frontier);
			ASSERT_FALSE (node->store.confirmation_height_get (transaction, key2.pub, confirmation_height_info));
			ASSERT_EQ (0, confirmation_height_info.height);
			ASSERT_EQ (oslo::block_hash (0), confirmation_height_info.frontier);
			ASSERT_FALSE (node->store.confirmation_height_get (transaction, key3.pub, confirmation_height_info));
			ASSERT_EQ (0, confirmation_height_info.height);
			ASSERT_EQ (oslo::block_hash (0), confirmation_height_info.frontier);
		}

		// The nodes process a live receive which propagates across to all accounts
		auto receive3 = std::make_shared<oslo::receive_block> (open3.hash (), send6.hash (), key3.prv, key3.pub, *system.work.generate (open3.hash ()));
		node->process_active (receive3);
		node->block_processor.flush ();
		node->block_confirm (receive3);
		{
			auto election = node->active.election (receive3->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 10)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		oslo::account_info account_info;
		oslo::confirmation_height_info confirmation_height_info;
		auto & store = node->store;
		auto transaction = node->store.tx_begin_read ();
		ASSERT_TRUE (node->ledger.block_confirmed (transaction, receive3->hash ()));
		ASSERT_FALSE (store.account_get (transaction, oslo::test_genesis_key.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
		ASSERT_EQ (4, confirmation_height_info.height);
		ASSERT_EQ (send3.hash (), confirmation_height_info.frontier);
		ASSERT_EQ (4, account_info.block_count);
		ASSERT_FALSE (store.account_get (transaction, key1.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height_info));
		ASSERT_EQ (2, confirmation_height_info.height);
		ASSERT_EQ (send4.hash (), confirmation_height_info.frontier);
		ASSERT_EQ (3, account_info.block_count);
		ASSERT_FALSE (store.account_get (transaction, key2.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key2.pub, confirmation_height_info));
		ASSERT_EQ (3, confirmation_height_info.height);
		ASSERT_EQ (send6.hash (), confirmation_height_info.frontier);
		ASSERT_EQ (4, account_info.block_count);
		ASSERT_FALSE (store.account_get (transaction, key3.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key3.pub, confirmation_height_info));
		ASSERT_EQ (2, confirmation_height_info.height);
		ASSERT_EQ (receive3->hash (), confirmation_height_info.frontier);
		ASSERT_EQ (2, account_info.block_count);

		// The accounts for key1 and key2 have 1 more block in the chain than is confirmed.
		// So this can be rolled back, but the one before that cannot. Check that this is the case
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_FALSE (node->ledger.rollback (transaction, node->latest (key2.pub)));
			ASSERT_FALSE (node->ledger.rollback (transaction, node->latest (key1.pub)));
		}
		{
			// These rollbacks should fail
			auto transaction = node->store.tx_begin_write ();
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (key1.pub)));
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (key2.pub)));

			// Confirm the other latest can't be rolled back either
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (key3.pub)));
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (oslo::test_genesis_key.pub)));

			// Attempt some others which have been cemented
			ASSERT_TRUE (node->ledger.rollback (transaction, open1.hash ()));
			ASSERT_TRUE (node->ledger.rollback (transaction, send2.hash ()));
		}
		ASSERT_EQ (10, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (10, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (10, node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
		ASSERT_EQ (11, node->ledger.cache.cemented_count);

		ASSERT_EQ (0, node->active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

TEST (confirmation_height, gap_bootstrap)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		auto & node1 = *system.add_node (node_flags);
		oslo::genesis genesis;
		oslo::keypair destination;
		auto send1 (std::make_shared<oslo::state_block> (oslo::genesis_account, genesis.hash (), oslo::genesis_account, oslo::genesis_amount - oslo::Gxrb_ratio, destination.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, 0));
		node1.work_generate_blocking (*send1);
		auto send2 (std::make_shared<oslo::state_block> (oslo::genesis_account, send1->hash (), oslo::genesis_account, oslo::genesis_amount - 2 * oslo::Gxrb_ratio, destination.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, 0));
		node1.work_generate_blocking (*send2);
		auto send3 (std::make_shared<oslo::state_block> (oslo::genesis_account, send2->hash (), oslo::genesis_account, oslo::genesis_amount - 3 * oslo::Gxrb_ratio, destination.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, 0));
		node1.work_generate_blocking (*send3);
		auto open1 (std::make_shared<oslo::open_block> (send1->hash (), destination.pub, destination.pub, destination.prv, destination.pub, 0));
		node1.work_generate_blocking (*open1);

		// Receive
		auto receive1 (std::make_shared<oslo::receive_block> (open1->hash (), send2->hash (), destination.prv, destination.pub, 0));
		node1.work_generate_blocking (*receive1);
		auto receive2 (std::make_shared<oslo::receive_block> (receive1->hash (), send3->hash (), destination.prv, destination.pub, 0));
		node1.work_generate_blocking (*receive2);

		node1.block_processor.add (send1);
		node1.block_processor.add (send2);
		node1.block_processor.add (send3);
		node1.block_processor.add (receive1);
		node1.block_processor.flush ();

		add_callback_stats (node1);

		// Receive 2 comes in on the live network, however the chain has not been finished so it gets added to unchecked
		node1.process_active (receive2);
		node1.block_processor.flush ();

		// Confirmation heights should not be updated
		{
			auto transaction (node1.store.tx_begin_read ());
			auto unchecked_count (node1.store.unchecked_count (transaction));
			ASSERT_EQ (unchecked_count, 2);

			oslo::confirmation_height_info confirmation_height_info;
			ASSERT_FALSE (node1.store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
			ASSERT_EQ (1, confirmation_height_info.height);
			ASSERT_EQ (genesis.hash (), confirmation_height_info.frontier);
		}

		// Now complete the chain where the block comes in on the bootstrap network.
		node1.block_processor.add (open1);
		node1.block_processor.flush ();

		// Confirmation height should be unchanged and unchecked should now be 0
		{
			auto transaction (node1.store.tx_begin_read ());
			auto unchecked_count (node1.store.unchecked_count (transaction));
			ASSERT_EQ (unchecked_count, 0);

			oslo::confirmation_height_info confirmation_height_info;
			ASSERT_FALSE (node1.store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
			ASSERT_EQ (1, confirmation_height_info.height);
			ASSERT_EQ (genesis.hash (), confirmation_height_info.frontier);
			ASSERT_FALSE (node1.store.confirmation_height_get (transaction, destination.pub, confirmation_height_info));
			ASSERT_EQ (0, confirmation_height_info.height);
			ASSERT_EQ (oslo::block_hash (0), confirmation_height_info.frontier);
		}
		ASSERT_EQ (0, node1.stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (0, node1.stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (0, node1.stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
		ASSERT_EQ (1, node1.ledger.cache.cemented_count);

		ASSERT_EQ (0, node1.active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

TEST (confirmation_height, gap_live)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);
		node_config.peering_port = oslo::get_available_port ();
		system.add_node (node_config, node_flags);
		oslo::keypair destination;
		system.wallet (0)->insert_adhoc (oslo::test_genesis_key.prv);
		system.wallet (1)->insert_adhoc (destination.prv);

		oslo::genesis genesis;
		auto send1 (std::make_shared<oslo::state_block> (oslo::genesis_account, genesis.hash (), oslo::genesis_account, oslo::genesis_amount - oslo::Gxrb_ratio, destination.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, 0));
		node->work_generate_blocking (*send1);
		auto send2 (std::make_shared<oslo::state_block> (oslo::genesis_account, send1->hash (), oslo::genesis_account, oslo::genesis_amount - 2 * oslo::Gxrb_ratio, destination.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, 0));
		node->work_generate_blocking (*send2);
		auto send3 (std::make_shared<oslo::state_block> (oslo::genesis_account, send2->hash (), oslo::genesis_account, oslo::genesis_amount - 3 * oslo::Gxrb_ratio, destination.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, 0));
		node->work_generate_blocking (*send3);

		auto open1 (std::make_shared<oslo::open_block> (send1->hash (), destination.pub, destination.pub, destination.prv, destination.pub, 0));
		node->work_generate_blocking (*open1);
		auto receive1 (std::make_shared<oslo::receive_block> (open1->hash (), send2->hash (), destination.prv, destination.pub, 0));
		node->work_generate_blocking (*receive1);
		auto receive2 (std::make_shared<oslo::receive_block> (receive1->hash (), send3->hash (), destination.prv, destination.pub, 0));
		node->work_generate_blocking (*receive2);

		node->block_processor.add (send1);
		node->block_processor.add (send2);
		node->block_processor.add (send3);
		node->block_processor.add (receive1);
		node->block_processor.flush ();

		add_callback_stats (*node);

		// Receive 2 comes in on the live network, however the chain has not been finished so it gets added to unchecked
		node->process_active (receive2);
		node->block_processor.flush ();

		// Confirmation heights should not be updated
		{
			auto transaction = node->store.tx_begin_read ();
			oslo::confirmation_height_info confirmation_height_info;
			ASSERT_FALSE (node->store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
			ASSERT_EQ (1, confirmation_height_info.height);
			ASSERT_EQ (oslo::genesis_hash, confirmation_height_info.frontier);
		}

		// Now complete the chain where the block comes in on the live network
		node->process_active (open1);
		node->block_processor.flush ();
		node->block_confirm (open1);
		{
			auto election = node->active.election (open1->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 6)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		// This should confirm the open block and the source of the receive blocks
		auto transaction (node->store.tx_begin_read ());
		auto unchecked_count (node->store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 0);

		oslo::confirmation_height_info confirmation_height_info;
		ASSERT_TRUE (node->ledger.block_confirmed (transaction, receive2->hash ()));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
		ASSERT_EQ (4, confirmation_height_info.height);
		ASSERT_EQ (send3->hash (), confirmation_height_info.frontier);
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, destination.pub, confirmation_height_info));
		ASSERT_EQ (3, confirmation_height_info.height);
		ASSERT_EQ (receive2->hash (), confirmation_height_info.frontier);

		ASSERT_EQ (6, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (6, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (6, node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
		ASSERT_EQ (7, node->ledger.cache.cemented_count);

		ASSERT_EQ (0, node->active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

TEST (confirmation_height, send_receive_between_2_accounts)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);
		oslo::keypair key1;
		oslo::block_hash latest (node->latest (oslo::test_genesis_key.pub));

		oslo::send_block send1 (latest, key1.pub, node->config.online_weight_minimum.number () + 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest));

		oslo::open_block open1 (send1.hash (), oslo::genesis_account, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));
		oslo::send_block send2 (open1.hash (), oslo::genesis_account, 1000, key1.prv, key1.pub, *system.work.generate (open1.hash ()));
		oslo::send_block send3 (send2.hash (), oslo::genesis_account, 900, key1.prv, key1.pub, *system.work.generate (send2.hash ()));
		oslo::send_block send4 (send3.hash (), oslo::genesis_account, 500, key1.prv, key1.pub, *system.work.generate (send3.hash ()));

		oslo::receive_block receive1 (send1.hash (), send2.hash (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1.hash ()));
		oslo::receive_block receive2 (receive1.hash (), send3.hash (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (receive1.hash ()));
		oslo::receive_block receive3 (receive2.hash (), send4.hash (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (receive2.hash ()));

		oslo::send_block send5 (receive3.hash (), key1.pub, node->config.online_weight_minimum.number () + 1, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (receive3.hash ()));
		auto receive4 = std::make_shared<oslo::receive_block> (send4.hash (), send5.hash (), key1.prv, key1.pub, *system.work.generate (send4.hash ()));
		// Unpocketed send
		oslo::keypair key2;
		oslo::send_block send6 (send5.hash (), key2.pub, node->config.online_weight_minimum.number (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send5.hash ()));
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open1).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive1).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send3).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send4).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive3).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send5).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send6).code);
		}

		add_callback_stats (*node);

		node->process_active (receive4);
		node->block_processor.flush ();
		node->block_confirm (receive4);
		{
			auto election = node->active.election (receive4->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 10)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		auto transaction (node->store.tx_begin_read ());
		ASSERT_TRUE (node->ledger.block_confirmed (transaction, receive4->hash ()));
		oslo::account_info account_info;
		oslo::confirmation_height_info confirmation_height_info;
		ASSERT_FALSE (node->store.account_get (transaction, oslo::test_genesis_key.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
		ASSERT_EQ (6, confirmation_height_info.height);
		ASSERT_EQ (send5.hash (), confirmation_height_info.frontier);
		ASSERT_EQ (7, account_info.block_count);

		ASSERT_FALSE (node->store.account_get (transaction, key1.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height_info));
		ASSERT_EQ (5, confirmation_height_info.height);
		ASSERT_EQ (receive4->hash (), confirmation_height_info.frontier);
		ASSERT_EQ (5, account_info.block_count);

		ASSERT_EQ (10, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (10, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (10, node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
		ASSERT_EQ (11, node->ledger.cache.cemented_count);

		ASSERT_EQ (0, node->active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

TEST (confirmation_height, send_receive_self)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);
		oslo::block_hash latest (node->latest (oslo::test_genesis_key.pub));

		oslo::send_block send1 (latest, oslo::test_genesis_key.pub, oslo::genesis_amount - 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest));
		oslo::receive_block receive1 (send1.hash (), send1.hash (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1.hash ()));
		oslo::send_block send2 (receive1.hash (), oslo::test_genesis_key.pub, oslo::genesis_amount - 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (receive1.hash ()));
		oslo::send_block send3 (send2.hash (), oslo::test_genesis_key.pub, oslo::genesis_amount - 3, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send2.hash ()));

		oslo::receive_block receive2 (send3.hash (), send2.hash (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send3.hash ()));
		auto receive3 = std::make_shared<oslo::receive_block> (receive2.hash (), send3.hash (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (receive2.hash ()));

		// Send to another account to prevent automatic receiving on the genesis account
		oslo::keypair key1;
		oslo::send_block send4 (receive3->hash (), key1.pub, node->config.online_weight_minimum.number (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (receive3->hash ()));
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send3).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *receive3).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send4).code);
		}

		add_callback_stats (*node);

		node->block_confirm (receive3);
		{
			auto election = node->active.election (receive3->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 6)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		auto transaction (node->store.tx_begin_read ());
		ASSERT_TRUE (node->ledger.block_confirmed (transaction, receive3->hash ()));
		oslo::account_info account_info;
		ASSERT_FALSE (node->store.account_get (transaction, oslo::test_genesis_key.pub, account_info));
		oslo::confirmation_height_info confirmation_height_info;
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
		ASSERT_EQ (7, confirmation_height_info.height);
		ASSERT_EQ (receive3->hash (), confirmation_height_info.frontier);
		ASSERT_EQ (8, account_info.block_count);
		ASSERT_EQ (6, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (6, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (6, node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
		ASSERT_EQ (confirmation_height_info.height, node->ledger.cache.cemented_count);
		ASSERT_EQ (0, node->active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

TEST (confirmation_height, all_block_types)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);
		oslo::block_hash latest (node->latest (oslo::test_genesis_key.pub));
		oslo::keypair key1;
		oslo::keypair key2;
		auto & store = node->store;
		oslo::send_block send (latest, key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest));
		oslo::send_block send1 (send.hash (), key2.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send.hash ()));

		oslo::open_block open (send.hash (), oslo::test_genesis_key.pub, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));
		oslo::state_block state_open (key2.pub, 0, 0, oslo::Gxrb_ratio, send1.hash (), key2.prv, key2.pub, *system.work.generate (key2.pub));

		oslo::send_block send2 (open.hash (), key2.pub, 0, key1.prv, key1.pub, *system.work.generate (open.hash ()));
		oslo::state_block state_receive (key2.pub, state_open.hash (), 0, oslo::Gxrb_ratio * 2, send2.hash (), key2.prv, key2.pub, *system.work.generate (state_open.hash ()));

		oslo::state_block state_send (key2.pub, state_receive.hash (), 0, oslo::Gxrb_ratio, key1.pub, key2.prv, key2.pub, *system.work.generate (state_receive.hash ()));
		oslo::receive_block receive (send2.hash (), state_send.hash (), key1.prv, key1.pub, *system.work.generate (send2.hash ()));

		oslo::change_block change (receive.hash (), key2.pub, key1.prv, key1.pub, *system.work.generate (receive.hash ()));

		oslo::state_block state_change (key2.pub, state_send.hash (), oslo::test_genesis_key.pub, oslo::Gxrb_ratio, 0, key2.prv, key2.pub, *system.work.generate (state_send.hash ()));

		oslo::state_block epoch (key2.pub, state_change.hash (), oslo::test_genesis_key.pub, oslo::Gxrb_ratio, node->ledger.epoch_link (oslo::epoch::epoch_1), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (state_change.hash ()));

		oslo::state_block epoch1 (key1.pub, change.hash (), key2.pub, oslo::Gxrb_ratio, node->ledger.epoch_link (oslo::epoch::epoch_1), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (change.hash ()));
		oslo::state_block state_send1 (key1.pub, epoch1.hash (), 0, oslo::Gxrb_ratio - 1, key2.pub, key1.prv, key1.pub, *system.work.generate (epoch1.hash ()));
		oslo::state_block state_receive2 (key2.pub, epoch.hash (), 0, oslo::Gxrb_ratio + 1, state_send1.hash (), key2.prv, key2.pub, *system.work.generate (epoch.hash ()));

		auto state_send2 = std::make_shared<oslo::state_block> (key2.pub, state_receive2.hash (), 0, oslo::Gxrb_ratio, key1.pub, key2.prv, key2.pub, *system.work.generate (state_receive2.hash ()));
		oslo::state_block state_send3 (key2.pub, state_send2->hash (), 0, oslo::Gxrb_ratio - 1, key1.pub, key2.prv, key2.pub, *system.work.generate (state_send2->hash ()));

		oslo::state_block state_send4 (key1.pub, state_send1.hash (), 0, oslo::Gxrb_ratio - 2, oslo::test_genesis_key.pub, key1.prv, key1.pub, *system.work.generate (state_send1.hash ()));
		oslo::state_block state_receive3 (oslo::genesis_account, send1.hash (), oslo::genesis_account, oslo::genesis_amount - oslo::Gxrb_ratio * 2 + 1, state_send4.hash (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1.hash ()));

		{
			auto transaction (store.tx_begin_write ());
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, state_open).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, state_receive).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, state_send).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, change).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, state_change).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, epoch).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, epoch1).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, state_send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, state_receive2).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *state_send2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, state_send3).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, state_send4).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, state_receive3).code);
		}

		add_callback_stats (*node);
		node->block_confirm (state_send2);
		{
			auto election = node->active.election (state_send2->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 15)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		auto transaction (node->store.tx_begin_read ());
		ASSERT_TRUE (node->ledger.block_confirmed (transaction, state_send2->hash ()));
		oslo::account_info account_info;
		oslo::confirmation_height_info confirmation_height_info;
		ASSERT_FALSE (node->store.account_get (transaction, oslo::test_genesis_key.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, oslo::test_genesis_key.pub, confirmation_height_info));
		ASSERT_EQ (3, confirmation_height_info.height);
		ASSERT_EQ (send1.hash (), confirmation_height_info.frontier);
		ASSERT_LE (4, account_info.block_count);

		ASSERT_FALSE (node->store.account_get (transaction, key1.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height_info));
		ASSERT_EQ (state_send1.hash (), confirmation_height_info.frontier);
		ASSERT_EQ (6, confirmation_height_info.height);
		ASSERT_LE (7, account_info.block_count);

		ASSERT_FALSE (node->store.account_get (transaction, key2.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key2.pub, confirmation_height_info));
		ASSERT_EQ (7, confirmation_height_info.height);
		ASSERT_EQ (state_send2->hash (), confirmation_height_info.frontier);
		ASSERT_LE (8, account_info.block_count);

		ASSERT_EQ (15, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (15, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (15, node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
		ASSERT_EQ (16, node->ledger.cache.cemented_count);

		ASSERT_EQ (0, node->active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

/* Bulk of the this test was taken from the node.fork_flip test */
TEST (confirmation_height, conflict_rollback_cemented)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		boost::iostreams::stream_buffer<oslo::stringstream_mt_sink> sb;
		sb.open (oslo::stringstream_mt_sink{});
		oslo::boost_log_cerr_redirect redirect_cerr (&sb);
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		auto node1 = system.add_node (node_flags);
		auto node2 = system.add_node (node_flags);
		ASSERT_EQ (1, node1->network.size ());
		oslo::keypair key1;
		oslo::genesis genesis;
		auto send1 (std::make_shared<oslo::send_block> (genesis.hash (), key1.pub, oslo::genesis_amount - 100, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
		oslo::publish publish1 (send1);
		oslo::keypair key2;
		auto send2 (std::make_shared<oslo::send_block> (genesis.hash (), key2.pub, oslo::genesis_amount - 100, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
		oslo::publish publish2 (send2);
		auto channel1 (node1->network.udp_channels.create (node1->network.endpoint ()));
		node1->network.process_message (publish1, channel1);
		node1->block_processor.flush ();
		auto channel2 (node2->network.udp_channels.create (node1->network.endpoint ()));
		node2->network.process_message (publish2, channel2);
		node2->block_processor.flush ();
		ASSERT_EQ (1, node1->active.size ());
		ASSERT_EQ (1, node2->active.size ());
		system.wallet (0)->insert_adhoc (oslo::test_genesis_key.prv);
		node1->network.process_message (publish2, channel1);
		node1->block_processor.flush ();
		node2->network.process_message (publish1, channel2);
		node2->block_processor.flush ();
		oslo::unique_lock<std::mutex> lock (node2->active.mutex);
		auto conflict (node2->active.roots.find (oslo::qualified_root (genesis.hash (), genesis.hash ())));
		ASSERT_NE (node2->active.roots.end (), conflict);
		auto votes1 (conflict->election);
		ASSERT_NE (nullptr, votes1);
		ASSERT_EQ (1, votes1->last_votes.size ());
		lock.unlock ();
		// Force blocks to be cemented on both nodes
		{
			auto transaction (node1->store.tx_begin_write ());
			ASSERT_TRUE (node1->store.block_exists (transaction, publish1.block->hash ()));
			node1->store.confirmation_height_put (transaction, oslo::genesis_account, oslo::confirmation_height_info{ 2, send2->hash () });
		}
		{
			auto transaction (node2->store.tx_begin_write ());
			ASSERT_TRUE (node2->store.block_exists (transaction, publish2.block->hash ()));
			node2->store.confirmation_height_put (transaction, oslo::genesis_account, oslo::confirmation_height_info{ 2, send2->hash () });
		}

		auto rollback_log_entry = boost::str (boost::format ("Failed to roll back %1%") % send2->hash ().to_string ());
		system.deadline_set (20s);
		auto done (false);
		while (!done)
		{
			ASSERT_NO_ERROR (system.poll ());
			done = (sb.component ()->str ().find (rollback_log_entry) != std::string::npos);
		}
		auto transaction1 (node1->store.tx_begin_read ());
		auto transaction2 (node2->store.tx_begin_read ());
		lock.lock ();
		auto winner (*votes1->tally ().begin ());
		ASSERT_EQ (*publish1.block, *winner.second);
		ASSERT_EQ (oslo::genesis_amount - 100, winner.first);
		ASSERT_TRUE (node1->store.block_exists (transaction1, publish1.block->hash ()));
		ASSERT_TRUE (node2->store.block_exists (transaction2, publish2.block->hash ()));
		ASSERT_FALSE (node2->store.block_exists (transaction2, publish1.block->hash ()));
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

TEST (confirmation_heightDeathTest, rollback_added_block)
{
	// For ASSERT_DEATH_IF_SUPPORTED
	testing::FLAGS_gtest_death_test_style = "threadsafe";

	// valgrind can be noisy with death tests
	if (!oslo::running_within_valgrind ())
	{
		oslo::logger_mt logger;
		auto path (oslo::unique_path ());
		oslo::mdb_store store (logger, path);
		ASSERT_TRUE (!store.init_error ());
		oslo::genesis genesis;
		oslo::stat stats;
		oslo::ledger ledger (store, stats);
		oslo::write_database_queue write_database_queue;
		oslo::work_pool pool (std::numeric_limits<unsigned>::max ());
		oslo::keypair key1;
		auto send = std::make_shared<oslo::send_block> (genesis.hash (), key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *pool.generate (genesis.hash ()));
		{
			auto transaction (store.tx_begin_write ());
			store.initialize (transaction, genesis, ledger.cache);
		}

		auto block_hash_being_processed (send->hash ());
		uint64_t batch_write_size = 2048;
		std::atomic<bool> stopped{ false };
		oslo::confirmation_height_unbounded unbounded_processor (
		ledger, write_database_queue, 10ms, logger, stopped, block_hash_being_processed, batch_write_size, [](auto const &) {}, [](auto const &) {}, []() { return 0; });

		// Processing a block which doesn't exist should bail
		ASSERT_DEATH_IF_SUPPORTED (unbounded_processor.process (), "");

		oslo::confirmation_height_bounded bounded_processor (
		ledger, write_database_queue, 10ms, logger, stopped, block_hash_being_processed, batch_write_size, [](auto const &) {}, [](auto const &) {}, []() { return 0; });
		// Processing a block which doesn't exist should bail
		ASSERT_DEATH_IF_SUPPORTED (bounded_processor.process (), "");
	}
}

TEST (confirmation_height, observers)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		auto amount (std::numeric_limits<oslo::uint128_t>::max ());
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		auto node1 = system.add_node (node_flags);
		oslo::keypair key1;
		system.wallet (0)->insert_adhoc (oslo::test_genesis_key.prv);
		oslo::block_hash latest1 (node1->latest (oslo::test_genesis_key.pub));
		auto send1 (std::make_shared<oslo::send_block> (latest1, key1.pub, amount - node1->config.receive_minimum.number (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest1)));

		add_callback_stats (*node1);

		node1->process_active (send1);
		node1->block_processor.flush ();
		system.deadline_set (10s);
		while (node1->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 1)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
		auto transaction = node1->store.tx_begin_read ();
		ASSERT_TRUE (node1->ledger.block_confirmed (transaction, send1->hash ()));
		ASSERT_EQ (1, node1->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (1, node1->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (1, node1->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
		ASSERT_EQ (2, node1->ledger.cache.cemented_count);
		ASSERT_EQ (0, node1->active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

// This tests when a read has been done, but the block no longer exists by the time a write is done
TEST (confirmation_heightDeathTest, modified_chain)
{
	// For ASSERT_DEATH_IF_SUPPORTED
	testing::FLAGS_gtest_death_test_style = "threadsafe";

	// valgrind can be noisy with death tests
	if (!oslo::running_within_valgrind ())
	{
		oslo::logger_mt logger;
		auto path (oslo::unique_path ());
		oslo::mdb_store store (logger, path);
		ASSERT_TRUE (!store.init_error ());
		oslo::genesis genesis;
		oslo::stat stats;
		oslo::ledger ledger (store, stats);
		oslo::write_database_queue write_database_queue;
		oslo::work_pool pool (std::numeric_limits<unsigned>::max ());
		oslo::keypair key1;
		auto send = std::make_shared<oslo::send_block> (oslo::genesis_hash, key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *pool.generate (oslo::genesis_hash));
		{
			auto transaction (store.tx_begin_write ());
			store.initialize (transaction, genesis, ledger.cache);
			ASSERT_EQ (oslo::process_result::progress, ledger.process (transaction, *send).code);
		}

		auto block_hash_being_processed (send->hash ());
		uint64_t batch_write_size = 2048;
		std::atomic<bool> stopped{ false };
		oslo::confirmation_height_bounded bounded_processor (
		ledger, write_database_queue, 10ms, logger, stopped, block_hash_being_processed, batch_write_size, [](auto const &) {}, [](auto const &) {}, []() { return 0; });

		{
			// This reads the blocks in the account, but prevents any writes from occuring yet
			auto scoped_write_guard = write_database_queue.wait (oslo::writer::testing);
			bounded_processor.process ();
		}

		// Rollback the block and now try to write, the block no longer exists so should bail
		ledger.rollback (store.tx_begin_write (), send->hash ());
		{
			auto scoped_write_guard = write_database_queue.wait (oslo::writer::confirmation_height);
			ASSERT_DEATH_IF_SUPPORTED (bounded_processor.cement_blocks (scoped_write_guard), "");
		}

		ASSERT_EQ (oslo::process_result::progress, ledger.process (store.tx_begin_write (), *send).code);
		store.confirmation_height_put (store.tx_begin_write (), oslo::genesis_account, { 1, oslo::genesis_hash });

		oslo::confirmation_height_unbounded unbounded_processor (
		ledger, write_database_queue, 10ms, logger, stopped, block_hash_being_processed, batch_write_size, [](auto const &) {}, [](auto const &) {}, []() { return 0; });

		{
			// This reads the blocks in the account, but prevents any writes from occuring yet
			auto scoped_write_guard = write_database_queue.wait (oslo::writer::testing);
			unbounded_processor.process ();
		}

		// Rollback the block and now try to write, the block no longer exists so should bail
		ledger.rollback (store.tx_begin_write (), send->hash ());
		{
			auto scoped_write_guard = write_database_queue.wait (oslo::writer::confirmation_height);
			ASSERT_DEATH_IF_SUPPORTED (unbounded_processor.cement_blocks (scoped_write_guard), "");
		}
	}
}

// This tests when a read has been done, but the account no longer exists by the time a write is done
TEST (confirmation_heightDeathTest, modified_chain_account_removed)
{
	// For ASSERT_DEATH_IF_SUPPORTED
	testing::FLAGS_gtest_death_test_style = "threadsafe";

	// valgrind can be noisy with death tests
	if (!oslo::running_within_valgrind ())
	{
		oslo::logger_mt logger;
		auto path (oslo::unique_path ());
		oslo::mdb_store store (logger, path);
		ASSERT_TRUE (!store.init_error ());
		oslo::genesis genesis;
		oslo::stat stats;
		oslo::ledger ledger (store, stats);
		oslo::write_database_queue write_database_queue;
		oslo::work_pool pool (std::numeric_limits<unsigned>::max ());
		oslo::keypair key1;
		auto send = std::make_shared<oslo::send_block> (oslo::genesis_hash, key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *pool.generate (oslo::genesis_hash));
		auto open = std::make_shared<oslo::state_block> (key1.pub, 0, 0, oslo::Gxrb_ratio, send->hash (), key1.prv, key1.pub, *pool.generate (key1.pub));
		{
			auto transaction (store.tx_begin_write ());
			store.initialize (transaction, genesis, ledger.cache);
			ASSERT_EQ (oslo::process_result::progress, ledger.process (transaction, *send).code);
			ASSERT_EQ (oslo::process_result::progress, ledger.process (transaction, *open).code);
		}

		auto block_hash_being_processed (open->hash ());
		uint64_t batch_write_size = 2048;
		std::atomic<bool> stopped{ false };
		oslo::confirmation_height_unbounded unbounded_processor (
		ledger, write_database_queue, 10ms, logger, stopped, block_hash_being_processed, batch_write_size, [](auto const &) {}, [](auto const &) {}, []() { return 0; });

		{
			// This reads the blocks in the account, but prevents any writes from occuring yet
			auto scoped_write_guard = write_database_queue.wait (oslo::writer::testing);
			unbounded_processor.process ();
		}

		// Rollback the block and now try to write, the send should be cemented but the account which the open block belongs no longer exists so should bail
		ledger.rollback (store.tx_begin_write (), open->hash ());
		{
			auto scoped_write_guard = write_database_queue.wait (oslo::writer::confirmation_height);
			ASSERT_DEATH_IF_SUPPORTED (unbounded_processor.cement_blocks (scoped_write_guard), "");
		}

		// Reset conditions and test with the bounded processor
		ASSERT_EQ (oslo::process_result::progress, ledger.process (store.tx_begin_write (), *open).code);
		store.confirmation_height_put (store.tx_begin_write (), oslo::genesis_account, { 1, oslo::genesis_hash });

		oslo::confirmation_height_bounded bounded_processor (
		ledger, write_database_queue, 10ms, logger, stopped, block_hash_being_processed, batch_write_size, [](auto const &) {}, [](auto const &) {}, []() { return 0; });

		{
			// This reads the blocks in the account, but prevents any writes from occuring yet
			auto scoped_write_guard = write_database_queue.wait (oslo::writer::testing);
			bounded_processor.process ();
		}

		// Rollback the block and now try to write, the send should be cemented but the account which the open block belongs no longer exists so should bail
		ledger.rollback (store.tx_begin_write (), open->hash ());
		auto scoped_write_guard = write_database_queue.wait (oslo::writer::confirmation_height);
		ASSERT_DEATH_IF_SUPPORTED (bounded_processor.cement_blocks (scoped_write_guard), "");
	}
}

namespace oslo
{
TEST (confirmation_height, pending_observer_callbacks)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);

		system.wallet (0)->insert_adhoc (oslo::test_genesis_key.prv);
		oslo::block_hash latest (node->latest (oslo::test_genesis_key.pub));

		oslo::keypair key1;
		oslo::send_block send (latest, key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest));
		auto send1 = std::make_shared<oslo::send_block> (send.hash (), key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send.hash ()));

		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *send1).code);
		}

		add_callback_stats (*node);

		node->confirmation_height_processor.add (send1->hash ());

		system.deadline_set (10s);
		// Confirm the callback is not called under this circumstance because there is no election information
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 1 || node->ledger.stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::all, oslo::stat::dir::out) != 1)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		ASSERT_EQ (2, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (2, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (3, node->ledger.cache.cemented_count);
		ASSERT_EQ (0, node->active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

TEST (confirmation_height, prioritize_frontiers)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		// Prevent frontiers being confirmed as it will affect the priorization checking
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config);

		oslo::keypair key1;
		oslo::keypair key2;
		oslo::keypair key3;
		oslo::keypair key4;
		oslo::block_hash latest1 (node->latest (oslo::test_genesis_key.pub));

		// Send different numbers of blocks all accounts
		oslo::send_block send1 (latest1, key1.pub, node->config.online_weight_minimum.number () + 10000, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest1));
		oslo::send_block send2 (send1.hash (), key1.pub, node->config.online_weight_minimum.number () + 8500, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1.hash ()));
		oslo::send_block send3 (send2.hash (), key1.pub, node->config.online_weight_minimum.number () + 8000, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send2.hash ()));
		oslo::send_block send4 (send3.hash (), key2.pub, node->config.online_weight_minimum.number () + 7500, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send3.hash ()));
		oslo::send_block send5 (send4.hash (), key3.pub, node->config.online_weight_minimum.number () + 6500, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send4.hash ()));
		oslo::send_block send6 (send5.hash (), key4.pub, node->config.online_weight_minimum.number () + 6000, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send5.hash ()));

		// Open all accounts and add other sends to get different uncemented counts (as well as some which are the same)
		oslo::open_block open1 (send1.hash (), oslo::genesis_account, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));
		oslo::send_block send7 (open1.hash (), oslo::test_genesis_key.pub, 500, key1.prv, key1.pub, *system.work.generate (open1.hash ()));

		oslo::open_block open2 (send4.hash (), oslo::genesis_account, key2.pub, key2.prv, key2.pub, *system.work.generate (key2.pub));

		oslo::open_block open3 (send5.hash (), oslo::genesis_account, key3.pub, key3.prv, key3.pub, *system.work.generate (key3.pub));
		oslo::send_block send8 (open3.hash (), oslo::test_genesis_key.pub, 500, key3.prv, key3.pub, *system.work.generate (open3.hash ()));
		oslo::send_block send9 (send8.hash (), oslo::test_genesis_key.pub, 200, key3.prv, key3.pub, *system.work.generate (send8.hash ()));

		oslo::open_block open4 (send6.hash (), oslo::genesis_account, key4.pub, key4.prv, key4.pub, *system.work.generate (key4.pub));
		oslo::send_block send10 (open4.hash (), oslo::test_genesis_key.pub, 500, key4.prv, key4.pub, *system.work.generate (open4.hash ()));
		oslo::send_block send11 (send10.hash (), oslo::test_genesis_key.pub, 200, key4.prv, key4.pub, *system.work.generate (send10.hash ()));

		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send3).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send4).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send5).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send6).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send7).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open2).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open3).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send8).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send9).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open4).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send10).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send11).code);
		}

		auto transaction = node->store.tx_begin_read ();
		constexpr auto num_accounts = 5;
		auto priority_orders_match = [](auto const & cementable_frontiers, auto const & desired_order) {
			return std::equal (desired_order.begin (), desired_order.end (), cementable_frontiers.template get<1> ().begin (), cementable_frontiers.template get<1> ().end (), [](oslo::account const & account, oslo::cementable_account const & cementable_account) {
				return (account == cementable_account.account);
			});
		};
		{
			node->active.prioritize_frontiers_for_confirmation (transaction, std::chrono::seconds (1), std::chrono::seconds (1));
			ASSERT_EQ (node->active.priority_cementable_frontiers_size (), num_accounts);
			// Check the order of accounts is as expected (greatest number of uncemented blocks at the front). key3 and key4 have the same value, the order is unspecified so check both
			std::array<oslo::account, num_accounts> desired_order_1{ oslo::genesis_account, key3.pub, key4.pub, key1.pub, key2.pub };
			std::array<oslo::account, num_accounts> desired_order_2{ oslo::genesis_account, key4.pub, key3.pub, key1.pub, key2.pub };
			ASSERT_TRUE (priority_orders_match (node->active.priority_cementable_frontiers, desired_order_1) || priority_orders_match (node->active.priority_cementable_frontiers, desired_order_2));
		}

		{
			// Add some to the local node wallets and check ordering of both containers
			system.wallet (0)->insert_adhoc (oslo::test_genesis_key.prv);
			system.wallet (0)->insert_adhoc (key1.prv);
			system.wallet (0)->insert_adhoc (key2.prv);
			node->active.prioritize_frontiers_for_confirmation (transaction, std::chrono::seconds (1), std::chrono::seconds (1));
			ASSERT_EQ (node->active.priority_cementable_frontiers_size (), num_accounts - 3);
			ASSERT_EQ (node->active.priority_wallet_cementable_frontiers_size (), num_accounts - 2);
			std::array<oslo::account, 3> local_desired_order{ oslo::genesis_account, key1.pub, key2.pub };
			ASSERT_TRUE (priority_orders_match (node->active.priority_wallet_cementable_frontiers, local_desired_order));
			std::array<oslo::account, 2> desired_order_1{ key3.pub, key4.pub };
			std::array<oslo::account, 2> desired_order_2{ key4.pub, key3.pub };
			ASSERT_TRUE (priority_orders_match (node->active.priority_cementable_frontiers, desired_order_1) || priority_orders_match (node->active.priority_cementable_frontiers, desired_order_2));
		}

		{
			// Add the remainder of accounts to node wallets and check size/ordering is correct
			system.wallet (0)->insert_adhoc (key3.prv);
			system.wallet (0)->insert_adhoc (key4.prv);
			node->active.prioritize_frontiers_for_confirmation (transaction, std::chrono::seconds (1), std::chrono::seconds (1));
			ASSERT_EQ (node->active.priority_cementable_frontiers_size (), 0);
			ASSERT_EQ (node->active.priority_wallet_cementable_frontiers_size (), num_accounts);
			std::array<oslo::account, num_accounts> desired_order_1{ oslo::genesis_account, key3.pub, key4.pub, key1.pub, key2.pub };
			std::array<oslo::account, num_accounts> desired_order_2{ oslo::genesis_account, key4.pub, key3.pub, key1.pub, key2.pub };
			ASSERT_TRUE (priority_orders_match (node->active.priority_wallet_cementable_frontiers, desired_order_1) || priority_orders_match (node->active.priority_wallet_cementable_frontiers, desired_order_2));
		}

		// Check that accounts which already exist have their order modified when the uncemented count changes.
		oslo::send_block send12 (send9.hash (), oslo::test_genesis_key.pub, 100, key3.prv, key3.pub, *system.work.generate (send9.hash ()));
		oslo::send_block send13 (send12.hash (), oslo::test_genesis_key.pub, 90, key3.prv, key3.pub, *system.work.generate (send12.hash ()));
		oslo::send_block send14 (send13.hash (), oslo::test_genesis_key.pub, 80, key3.prv, key3.pub, *system.work.generate (send13.hash ()));
		oslo::send_block send15 (send14.hash (), oslo::test_genesis_key.pub, 70, key3.prv, key3.pub, *system.work.generate (send14.hash ()));
		oslo::send_block send16 (send15.hash (), oslo::test_genesis_key.pub, 60, key3.prv, key3.pub, *system.work.generate (send15.hash ()));
		oslo::send_block send17 (send16.hash (), oslo::test_genesis_key.pub, 50, key3.prv, key3.pub, *system.work.generate (send16.hash ()));
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send12).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send13).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send14).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send15).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send16).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send17).code);
		}
		transaction.refresh ();
		node->active.prioritize_frontiers_for_confirmation (transaction, std::chrono::seconds (1), std::chrono::seconds (1));
		ASSERT_TRUE (priority_orders_match (node->active.priority_wallet_cementable_frontiers, std::array<oslo::account, num_accounts>{ key3.pub, oslo::genesis_account, key4.pub, key1.pub, key2.pub }));
		node->active.confirm_prioritized_frontiers (transaction);

		// Check that the active transactions roots contains the frontiers
		system.deadline_set (std::chrono::seconds (10));
		while (node->active.size () != num_accounts)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		std::array<oslo::qualified_root, num_accounts> frontiers{ send17.qualified_root (), send6.qualified_root (), send7.qualified_root (), open2.qualified_root (), send11.qualified_root () };
		for (auto & frontier : frontiers)
		{
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			ASSERT_NE (node->active.roots.find (frontier), node->active.roots.end ());
		}
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}
}

TEST (confirmation_height, frontiers_confirmation_mode)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::genesis genesis;
		oslo::keypair key;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		// Always mode
		{
			oslo::system system;
			oslo::node_config node_config (oslo::get_available_port (), system.logging);
			node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::always;
			auto node = system.add_node (node_config, node_flags);
			oslo::state_block send (oslo::test_genesis_key.pub, genesis.hash (), oslo::test_genesis_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio, key.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *node->work_generate_blocking (genesis.hash ()));
			{
				auto transaction = node->store.tx_begin_write ();
				ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send).code);
			}
			system.deadline_set (5s);
			while (node->active.size () != 1)
			{
				ASSERT_NO_ERROR (system.poll ());
			}
		}
		// Auto mode
		{
			oslo::system system;
			oslo::node_config node_config (oslo::get_available_port (), system.logging);
			node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::automatic;
			auto node = system.add_node (node_config, node_flags);
			oslo::state_block send (oslo::test_genesis_key.pub, genesis.hash (), oslo::test_genesis_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio, key.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *node->work_generate_blocking (genesis.hash ()));
			{
				auto transaction = node->store.tx_begin_write ();
				ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send).code);
			}
			system.deadline_set (5s);
			while (node->active.size () != 1)
			{
				ASSERT_NO_ERROR (system.poll ());
			}
		}
		// Disabled mode
		{
			oslo::system system;
			oslo::node_config node_config (oslo::get_available_port (), system.logging);
			node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
			auto node = system.add_node (node_config, node_flags);
			oslo::state_block send (oslo::test_genesis_key.pub, genesis.hash (), oslo::test_genesis_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio, key.pub, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *node->work_generate_blocking (genesis.hash ()));
			{
				auto transaction = node->store.tx_begin_write ();
				ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send).code);
			}
			system.wallet (0)->insert_adhoc (oslo::test_genesis_key.prv);
			std::this_thread::sleep_for (std::chrono::seconds (1));
			ASSERT_EQ (0, node->active.size ());
		}
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

// The callback and confirmation history should only be updated after confirmation height is set (and not just after voting)
TEST (confirmation_height, callback_confirmed_history)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);

		oslo::block_hash latest (node->latest (oslo::test_genesis_key.pub));

		oslo::keypair key1;
		auto send = std::make_shared<oslo::send_block> (latest, key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest));
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *send).code);
		}

		auto send1 = std::make_shared<oslo::send_block> (send->hash (), key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send->hash ()));

		add_callback_stats (*node);

		node->process_active (send1);
		node->block_processor.flush ();
		node->block_confirm (send1);
		{
			node->process_active (send);
			node->block_processor.flush ();

			// The write guard prevents the confirmation height processor doing any writes
			auto write_guard = node->write_database_queue.wait (oslo::writer::testing);

			// Confirm send1
			{
				auto election = node->active.election (send1->qualified_root ());
				ASSERT_NE (nullptr, election);
				oslo::lock_guard<std::mutex> guard (node->active.mutex);
				election->confirm_once ();
			}
			system.deadline_set (10s);
			while (node->active.size () > 0)
			{
				ASSERT_NO_ERROR (system.poll ());
			}

			ASSERT_EQ (0, node->active.list_recently_cemented ().size ());
			{
				oslo::lock_guard<std::mutex> guard (node->active.mutex);
				ASSERT_EQ (0, node->active.blocks.size ());
			}

			auto transaction = node->store.tx_begin_read ();
			ASSERT_FALSE (node->ledger.block_confirmed (transaction, send->hash ()));

			system.deadline_set (10s);
			while (!node->write_database_queue.contains (oslo::writer::confirmation_height))
			{
				ASSERT_NO_ERROR (system.poll ());
			}

			// Confirm that no inactive callbacks have been called when the confirmation height processor has already iterated over it, waiting to write
			ASSERT_EQ (0, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::inactive_conf_height, oslo::stat::dir::out));
		}

		system.deadline_set (10s);
		while (node->write_database_queue.contains (oslo::writer::confirmation_height))
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		auto transaction = node->store.tx_begin_read ();
		ASSERT_TRUE (node->ledger.block_confirmed (transaction, send->hash ()));

		system.deadline_set (10s);
		while (node->active.size () > 0)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::active_quorum, oslo::stat::dir::out) != 1)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		ASSERT_EQ (1, node->active.list_recently_cemented ().size ());
		ASSERT_EQ (0, node->active.blocks.size ());

		// Confirm the callback is not called under this circumstance
		ASSERT_EQ (2, node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
		ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::active_quorum, oslo::stat::dir::out));
		ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::inactive_conf_height, oslo::stat::dir::out));
		ASSERT_EQ (2, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (2, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (3, node->ledger.cache.cemented_count);
		ASSERT_EQ (0, node->active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

namespace oslo
{
TEST (confirmation_height, dependent_election)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);

		oslo::block_hash latest (node->latest (oslo::test_genesis_key.pub));

		oslo::keypair key1;
		auto send = std::make_shared<oslo::send_block> (latest, key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest));
		auto send1 = std::make_shared<oslo::send_block> (send->hash (), key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send->hash ()));
		auto send2 = std::make_shared<oslo::send_block> (send1->hash (), key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 3, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1->hash ()));
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *send).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *send2).code);
		}

		add_callback_stats (*node);

		// This election should be confirmed as active_conf_height
		node->block_confirm (send1);
		// Start an election and confirm it
		node->block_confirm (send2);
		{
			auto election = node->active.election (send2->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 3)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::active_quorum, oslo::stat::dir::out));
		ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::active_conf_height, oslo::stat::dir::out));
		ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::inactive_conf_height, oslo::stat::dir::out));
		ASSERT_EQ (3, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (3, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (4, node->ledger.cache.cemented_count);

		ASSERT_EQ (0, node->active.election_winner_details_size ());
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

// This test checks that a receive block with uncemented blocks below cements them too.
TEST (confirmation_height, cemented_gap_below_receive)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);

		oslo::block_hash latest (node->latest (oslo::test_genesis_key.pub));

		oslo::keypair key1;
		system.wallet (0)->insert_adhoc (key1.prv);

		oslo::send_block send (latest, key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest));
		oslo::send_block send1 (send.hash (), key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send.hash ()));
		oslo::keypair dummy_key;
		oslo::send_block dummy_send (send1.hash (), dummy_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 3, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1.hash ()));

		oslo::open_block open (send.hash (), oslo::genesis_account, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));
		oslo::receive_block receive1 (open.hash (), send1.hash (), key1.prv, key1.pub, *system.work.generate (open.hash ()));
		oslo::send_block send2 (receive1.hash (), oslo::test_genesis_key.pub, oslo::Gxrb_ratio, key1.prv, key1.pub, *system.work.generate (receive1.hash ()));

		oslo::receive_block receive2 (dummy_send.hash (), send2.hash (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (dummy_send.hash ()));
		oslo::send_block dummy_send1 (receive2.hash (), dummy_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 3, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (receive2.hash ()));

		oslo::keypair key2;
		system.wallet (0)->insert_adhoc (key2.prv);
		oslo::send_block send3 (dummy_send1.hash (), key2.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 4, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (dummy_send1.hash ()));
		oslo::send_block dummy_send2 (send3.hash (), dummy_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 5, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send3.hash ()));

		auto open1 = std::make_shared<oslo::open_block> (send3.hash (), oslo::genesis_account, key2.pub, key2.prv, key2.pub, *system.work.generate (key2.pub));

		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, dummy_send).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send2).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, dummy_send1).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send3).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, dummy_send2).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *open1).code);
		}

		std::vector<oslo::block_hash> observer_order;
		std::mutex mutex;
		add_callback_stats (*node, &observer_order, &mutex);

		node->block_confirm (open1);
		{
			auto election = node->active.election (open1->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}
		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 10)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		auto transaction = node->store.tx_begin_read ();
		ASSERT_TRUE (node->ledger.block_confirmed (transaction, open1->hash ()));
		ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::active_quorum, oslo::stat::dir::out));
		ASSERT_EQ (0, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::active_conf_height, oslo::stat::dir::out));
		ASSERT_EQ (9, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::inactive_conf_height, oslo::stat::dir::out));
		ASSERT_EQ (10, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (10, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (11, node->ledger.cache.cemented_count);
		ASSERT_EQ (0, node->active.election_winner_details_size ());

		// Check that the order of callbacks is correct
		std::vector<oslo::block_hash> expected_order = { send.hash (), open.hash (), send1.hash (), receive1.hash (), send2.hash (), dummy_send.hash (), receive2.hash (), dummy_send1.hash (), send3.hash (), open1->hash () };
		oslo::lock_guard<std::mutex> guard (mutex);
		ASSERT_EQ (observer_order, expected_order);
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

// This test checks that a receive block with uncemented blocks below cements them too, compared with the test above, this
// is the first write in this chain.
TEST (confirmation_height, cemented_gap_below_no_cache)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);

		oslo::block_hash latest (node->latest (oslo::test_genesis_key.pub));

		oslo::keypair key1;
		system.wallet (0)->insert_adhoc (key1.prv);

		oslo::send_block send (latest, key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest));
		oslo::send_block send1 (send.hash (), key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send.hash ()));
		oslo::keypair dummy_key;
		oslo::send_block dummy_send (send1.hash (), dummy_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 3, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1.hash ()));

		oslo::open_block open (send.hash (), oslo::genesis_account, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));
		oslo::receive_block receive1 (open.hash (), send1.hash (), key1.prv, key1.pub, *system.work.generate (open.hash ()));
		oslo::send_block send2 (receive1.hash (), oslo::test_genesis_key.pub, oslo::Gxrb_ratio, key1.prv, key1.pub, *system.work.generate (receive1.hash ()));

		oslo::receive_block receive2 (dummy_send.hash (), send2.hash (), oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (dummy_send.hash ()));
		oslo::send_block dummy_send1 (receive2.hash (), dummy_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 3, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (receive2.hash ()));

		oslo::keypair key2;
		system.wallet (0)->insert_adhoc (key2.prv);
		oslo::send_block send3 (dummy_send1.hash (), key2.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 4, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (dummy_send1.hash ()));
		oslo::send_block dummy_send2 (send3.hash (), dummy_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 5, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send3.hash ()));

		auto open1 = std::make_shared<oslo::open_block> (send3.hash (), oslo::genesis_account, key2.pub, key2.prv, key2.pub, *system.work.generate (key2.pub));

		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, dummy_send).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, open).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send2).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, receive2).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, dummy_send1).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, send3).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, dummy_send2).code);

			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *open1).code);
		}

		// Force some blocks to be cemented so that the cached confirmed info variable is empty
		{
			auto transaction (node->store.tx_begin_write ());
			node->store.confirmation_height_put (transaction, oslo::genesis_account, oslo::confirmation_height_info{ 3, send1.hash () });
			node->store.confirmation_height_put (transaction, key1.pub, oslo::confirmation_height_info{ 2, receive1.hash () });
		}

		add_callback_stats (*node);

		node->block_confirm (open1);
		{
			auto election = node->active.election (open1->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}
		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 6)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		auto transaction = node->store.tx_begin_read ();
		ASSERT_TRUE (node->ledger.block_confirmed (transaction, open1->hash ()));
		ASSERT_EQ (node->active.election_winner_details_size (), 0);
		ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::active_quorum, oslo::stat::dir::out));
		ASSERT_EQ (0, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::active_conf_height, oslo::stat::dir::out));
		ASSERT_EQ (5, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::inactive_conf_height, oslo::stat::dir::out));
		ASSERT_EQ (6, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (6, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (7, node->ledger.cache.cemented_count);
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}

TEST (confirmation_height, election_winner_details_clearing)
{
	auto test_mode = [](oslo::confirmation_height_mode mode_a) {
		oslo::system system;
		oslo::node_flags node_flags;
		node_flags.confirmation_height_processor_mode = mode_a;
		oslo::node_config node_config (oslo::get_available_port (), system.logging);
		node_config.frontiers_confirmation = oslo::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config, node_flags);

		oslo::block_hash latest (node->latest (oslo::test_genesis_key.pub));

		oslo::keypair key1;
		auto send = std::make_shared<oslo::send_block> (latest, key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest));
		auto send1 = std::make_shared<oslo::send_block> (send->hash (), key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 2, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send->hash ()));
		auto send2 = std::make_shared<oslo::send_block> (send1->hash (), key1.pub, oslo::genesis_amount - oslo::Gxrb_ratio * 3, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1->hash ()));

		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *send).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *send1).code);
			ASSERT_EQ (oslo::process_result::progress, node->ledger.process (transaction, *send2).code);
		}

		add_callback_stats (*node);

		node->block_confirm (send1);
		{
			auto election = node->active.election (send1->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 2)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		ASSERT_EQ (0, node->active.election_winner_details_size ());
		node->block_confirm (send);
		{
			auto election = node->active.election (send->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}

		// Wait until this block is confirmed
		system.deadline_set (10s);
		while (node->active.election_winner_details_size () != 1 && !node->confirmation_height_processor.current ().is_zero ())
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::inactive_conf_height, oslo::stat::dir::out));

		node->block_confirm (send2);
		{
			auto election = node->active.election (send2->qualified_root ());
			ASSERT_NE (nullptr, election);
			oslo::lock_guard<std::mutex> guard (node->active.mutex);
			election->confirm_once ();
		}

		system.deadline_set (10s);
		while (node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out) != 3)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		// Add an already cemented block with fake election details. It should get removed
		node->active.add_election_winner_details (send2->hash (), nullptr);
		node->confirmation_height_processor.add (send2->hash ());

		system.deadline_set (10s);
		while (node->active.election_winner_details_size () > 0)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		ASSERT_EQ (1, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::inactive_conf_height, oslo::stat::dir::out));
		ASSERT_EQ (3, node->stats.count (oslo::stat::type::http_callback, oslo::stat::detail::http_callback, oslo::stat::dir::out));
		ASSERT_EQ (2, node->stats.count (oslo::stat::type::confirmation_observer, oslo::stat::detail::active_quorum, oslo::stat::dir::out));
		ASSERT_EQ (3, node->stats.count (oslo::stat::type::confirmation_height, oslo::stat::detail::blocks_confirmed, oslo::stat::dir::in));
		ASSERT_EQ (3, node->stats.count (oslo::stat::type::confirmation_height, get_stats_detail (mode_a), oslo::stat::dir::in));
		ASSERT_EQ (4, node->ledger.cache.cemented_count);
	};

	test_mode (oslo::confirmation_height_mode::bounded);
	test_mode (oslo::confirmation_height_mode::unbounded);
}
}

TEST (confirmation_height, election_winner_details_clearing_node_process_confirmed)
{
	// Make sure election_winner_details is also cleared if the block never enters the confirmation height processor from node::process_confirmed
	oslo::system system (1);
	auto node = system.nodes.front ();

	auto send = std::make_shared<oslo::send_block> (oslo::genesis_hash, oslo::test_genesis_key.pub, oslo::genesis_amount - oslo::Gxrb_ratio, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (oslo::genesis_hash));
	// Add to election_winner_details. Use an unrealistic iteration so that it should fall into the else case and do a cleanup
	node->active.add_election_winner_details (send->hash (), nullptr);
	oslo::election_status election;
	election.winner = send;
	node->process_confirmed (election, 1000000);
	ASSERT_EQ (0, node->active.election_winner_details_size ());
}
