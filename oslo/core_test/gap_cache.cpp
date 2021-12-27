#include <oslo/core_test/testutil.hpp>
#include <oslo/node/testing.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST (gap_cache, add_new)
{
	oslo::system system (1);
	oslo::gap_cache cache (*system.nodes[0]);
	auto block1 (std::make_shared<oslo::send_block> (0, 1, 2, oslo::keypair ().prv, 4, 5));
	cache.add (block1->hash ());
}

TEST (gap_cache, add_existing)
{
	oslo::system system (1);
	oslo::gap_cache cache (*system.nodes[0]);
	auto block1 (std::make_shared<oslo::send_block> (0, 1, 2, oslo::keypair ().prv, 4, 5));
	cache.add (block1->hash ());
	oslo::unique_lock<std::mutex> lock (cache.mutex);
	auto existing1 (cache.blocks.get<1> ().find (block1->hash ()));
	ASSERT_NE (cache.blocks.get<1> ().end (), existing1);
	auto arrival (existing1->arrival);
	lock.unlock ();
	system.deadline_set (20s);
	while (arrival == std::chrono::steady_clock::now ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	cache.add (block1->hash ());
	ASSERT_EQ (1, cache.size ());
	lock.lock ();
	auto existing2 (cache.blocks.get<1> ().find (block1->hash ()));
	ASSERT_NE (cache.blocks.get<1> ().end (), existing2);
	ASSERT_GT (existing2->arrival, arrival);
}

TEST (gap_cache, comparison)
{
	oslo::system system (1);
	oslo::gap_cache cache (*system.nodes[0]);
	auto block1 (std::make_shared<oslo::send_block> (1, 0, 2, oslo::keypair ().prv, 4, 5));
	cache.add (block1->hash ());
	oslo::unique_lock<std::mutex> lock (cache.mutex);
	auto existing1 (cache.blocks.get<1> ().find (block1->hash ()));
	ASSERT_NE (cache.blocks.get<1> ().end (), existing1);
	auto arrival (existing1->arrival);
	lock.unlock ();
	system.deadline_set (20s);
	while (std::chrono::steady_clock::now () == arrival)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	auto block3 (std::make_shared<oslo::send_block> (0, 42, 1, oslo::keypair ().prv, 3, 4));
	cache.add (block3->hash ());
	ASSERT_EQ (2, cache.size ());
	lock.lock ();
	auto existing2 (cache.blocks.get<1> ().find (block3->hash ()));
	ASSERT_NE (cache.blocks.get<1> ().end (), existing2);
	ASSERT_GT (existing2->arrival, arrival);
	ASSERT_EQ (arrival, cache.blocks.get<1> ().begin ()->arrival);
}

// Upon receiving enough votes for a gapped block, a lazy bootstrap should be initiated
TEST (gap_cache, gap_bootstrap)
{
	oslo::node_flags node_flags;
	node_flags.disable_legacy_bootstrap = true;
	node_flags.disable_request_loop = true; // to avoid fallback behavior of broadcasting blocks
	oslo::system system (2, oslo::transport::transport_type::tcp, node_flags);

	auto & node1 (*system.nodes[0]);
	auto & node2 (*system.nodes[1]);
	oslo::block_hash latest (node1.latest (oslo::test_genesis_key.pub));
	oslo::keypair key;
	auto send (std::make_shared<oslo::send_block> (latest, key.pub, oslo::genesis_amount - 100, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (latest)));
	node1.process (*send);
	ASSERT_EQ (oslo::genesis_amount - 100, node1.balance (oslo::genesis_account));
	ASSERT_EQ (oslo::genesis_amount, node2.balance (oslo::genesis_account));
	// Confirm send block, allowing voting on the upcoming block
	node1.block_confirm (send);
	{
		auto election = node1.active.election (send->qualified_root ());
		ASSERT_NE (nullptr, election);
		oslo::lock_guard<std::mutex> guard (node1.active.mutex);
		election->confirm_once ();
	}
	ASSERT_TIMELY (2s, node1.block_confirmed (send->hash ()));
	node1.active.erase (*send);
	system.wallet (0)->insert_adhoc (oslo::test_genesis_key.prv);
	auto latest_block (system.wallet (0)->send_action (oslo::test_genesis_key.pub, key.pub, 100));
	ASSERT_NE (nullptr, latest_block);
	ASSERT_EQ (oslo::genesis_amount - 200, node1.balance (oslo::genesis_account));
	ASSERT_EQ (oslo::genesis_amount, node2.balance (oslo::genesis_account));
	ASSERT_TIMELY (10s, node2.balance (oslo::genesis_account) == oslo::genesis_amount - 200);
}

TEST (gap_cache, two_dependencies)
{
	oslo::system system (1);
	auto & node1 (*system.nodes[0]);
	oslo::keypair key;
	oslo::genesis genesis;
	auto send1 (std::make_shared<oslo::send_block> (genesis.hash (), key.pub, 1, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	auto send2 (std::make_shared<oslo::send_block> (send1->hash (), key.pub, 0, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (send1->hash ())));
	auto open (std::make_shared<oslo::open_block> (send1->hash (), key.pub, key.pub, key.prv, key.pub, *system.work.generate (key.pub)));
	ASSERT_EQ (0, node1.gap_cache.size ());
	node1.block_processor.add (send2, oslo::seconds_since_epoch ());
	node1.block_processor.flush ();
	ASSERT_EQ (1, node1.gap_cache.size ());
	node1.block_processor.add (open, oslo::seconds_since_epoch ());
	node1.block_processor.flush ();
	ASSERT_EQ (2, node1.gap_cache.size ());
	node1.block_processor.add (send1, oslo::seconds_since_epoch ());
	node1.block_processor.flush ();
	ASSERT_EQ (0, node1.gap_cache.size ());
	auto transaction (node1.store.tx_begin_read ());
	ASSERT_TRUE (node1.store.block_exists (transaction, send1->hash ()));
	ASSERT_TRUE (node1.store.block_exists (transaction, send2->hash ()));
	ASSERT_TRUE (node1.store.block_exists (transaction, open->hash ()));
}
