#include <oslo/core_test/testutil.hpp>
#include <oslo/node/election.hpp>
#include <oslo/node/testing.hpp>

#include <gtest/gtest.h>

TEST (election, construction)
{
	oslo::system system (1);
	oslo::genesis genesis;
	auto & node = *system.nodes[0];
	genesis.open->sideband_set (oslo::block_sideband (oslo::genesis_account, 0, oslo::genesis_amount, 1, oslo::seconds_since_epoch (), oslo::epoch::epoch_0, false, false, false));
	auto election = node.active.insert (genesis.open).election;
	ASSERT_TRUE (election->idle ());
	election->transition_active ();
	ASSERT_FALSE (election->idle ());
	election->transition_passive ();
	ASSERT_FALSE (election->idle ());
}

namespace oslo
{
TEST (election, bisect_dependencies)
{
	oslo::system system;
	oslo::node_flags flags;
	flags.disable_request_loop = true;
	auto & node = *system.add_node (flags);
	oslo::genesis genesis;
	oslo::confirmation_height_info conf_info;
	ASSERT_FALSE (node.store.confirmation_height_get (node.store.tx_begin_read (), oslo::test_genesis_key.pub, conf_info));
	ASSERT_EQ (1, conf_info.height);
	std::vector<std::shared_ptr<oslo::block>> blocks;
	blocks.push_back (nullptr); // idx == height
	blocks.push_back (genesis.open);
	oslo::block_builder builder;
	auto amount = oslo::genesis_amount;
	for (int i = 0; i < 299; ++i)
	{
		auto latest = blocks.back ();
		blocks.push_back (builder.state ()
		                  .previous (latest->hash ())
		                  .account (oslo::test_genesis_key.pub)
		                  .representative (oslo::test_genesis_key.pub)
		                  .balance (--amount)
		                  .link (oslo::test_genesis_key.pub)
		                  .sign (oslo::test_genesis_key.prv, oslo::test_genesis_key.pub)
		                  .work (*system.work.generate (latest->hash ()))
		                  .build ());
		ASSERT_EQ (oslo::process_result::progress, node.process (*blocks.back ()).code);
	}
	ASSERT_EQ (301, blocks.size ());
	ASSERT_TRUE (node.active.empty ());
	{
		auto election = node.active.insert (blocks.back ()).election;
		ASSERT_NE (nullptr, election);
		ASSERT_EQ (300, election->blocks.begin ()->second->sideband ().height);
		oslo::unique_lock<std::mutex> lock (node.active.mutex);
		election->activate_dependencies ();
		node.active.activate_dependencies (lock);
	}
	// The first dependency activation also starts an election for the first unconfirmed block
	ASSERT_EQ (3, node.active.size ());
	{
		auto election = node.active.election (blocks[2]->qualified_root ());
		ASSERT_NE (nullptr, election);
		ASSERT_EQ (2, election->blocks.begin ()->second->sideband ().height);
	}

	auto check_height_and_activate_next = [&node, &blocks](uint64_t height_a) {
		auto election = node.active.election (blocks[height_a]->qualified_root ());
		ASSERT_NE (nullptr, election);
		ASSERT_EQ (height_a, election->blocks.begin ()->second->sideband ().height);
		oslo::unique_lock<std::mutex> lock (node.active.mutex);
		election->activate_dependencies ();
		node.active.activate_dependencies (lock);
	};
	check_height_and_activate_next (300 - 128); // ensure limited by 128 jumps
	ASSERT_EQ (4, node.active.size ());
	check_height_and_activate_next (87);
	ASSERT_EQ (5, node.active.size ());
	check_height_and_activate_next (44);
	ASSERT_EQ (6, node.active.size ());
	check_height_and_activate_next (23);
	ASSERT_EQ (7, node.active.size ());
	check_height_and_activate_next (12);
	ASSERT_EQ (8, node.active.size ());
	check_height_and_activate_next (7);
	ASSERT_EQ (9, node.active.size ());
	check_height_and_activate_next (4);
	ASSERT_EQ (10, node.active.size ());
	check_height_and_activate_next (3);
	ASSERT_EQ (10, node.active.size ()); // height 2 already inserted initially, no more blocks to activate
	check_height_and_activate_next (2);
	ASSERT_EQ (10, node.active.size ()); // conf height is 1, no more blocks to activate
	ASSERT_EQ (node.active.blocks.size (), node.active.roots.size ());
}

// Tests successful dependency activation of the open block of an account, and its corresponding source
TEST (election, dependencies_open_link)
{
	oslo::system system;
	oslo::node_flags flags;
	flags.disable_request_loop = true;
	auto & node = *system.add_node (flags);

	oslo::state_block_builder builder;
	oslo::keypair key;

	// Send to key
	auto gen_send = builder.make_block ()
	                .account (oslo::test_genesis_key.pub)
	                .previous (oslo::genesis_hash)
	                .representative (oslo::test_genesis_key.pub)
	                .link (key.pub)
	                .balance (oslo::genesis_amount - 1)
	                .sign (oslo::test_genesis_key.prv, oslo::test_genesis_key.pub)
	                .work (*system.work.generate (oslo::genesis_hash))
	                .build ();
	// Receive from genesis
	auto key_open = builder.make_block ()
	                .account (key.pub)
	                .previous (0)
	                .representative (key.pub)
	                .link (gen_send->hash ())
	                .balance (1)
	                .sign (key.prv, key.pub)
	                .work (*system.work.generate (key.pub))
	                .build ();

	// Send to self
	std::shared_ptr<oslo::block> key_send = builder.make_block ()
	                                        .account (key.pub)
	                                        .previous (key_open->hash ())
	                                        .representative (key.pub)
	                                        .link (key.pub)
	                                        .balance (0)
	                                        .sign (key.prv, key.pub)
	                                        .work (*system.work.generate (key_open->hash ()))
	                                        .build ();

	node.process (*gen_send);
	node.process (*key_open);
	node.process (*key_send);

	// Insert frontier
	node.block_confirm (key_send);
	ASSERT_EQ (1, node.active.size ());
	{
		auto election = node.active.election (key_send->qualified_root ());
		ASSERT_NE (nullptr, election);
		oslo::unique_lock<std::mutex> lock (node.active.mutex);
		election->activate_dependencies ();
		node.active.activate_dependencies (lock);
	}
	// Must have activated the open block
	ASSERT_EQ (2, node.active.size ());
	{
		auto election = node.active.election (key_open->qualified_root ());
		ASSERT_NE (nullptr, election);
		oslo::unique_lock<std::mutex> lock (node.active.mutex);
		election->activate_dependencies ();
		node.active.activate_dependencies (lock);
	}
	// Must have activated the open's source block
	ASSERT_EQ (3, node.active.size ());
	{
		auto election = node.active.election (gen_send->qualified_root ());
		ASSERT_NE (nullptr, election);
		oslo::unique_lock<std::mutex> lock (node.active.mutex);
		election->activate_dependencies ();
		node.active.activate_dependencies (lock);
	}
	// Nothing else to activate
	ASSERT_EQ (3, node.active.size ());
}
}
