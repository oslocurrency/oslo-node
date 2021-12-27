#include <oslo/core_test/testutil.hpp>
#include <oslo/lib/jsonconfig.hpp>
#include <oslo/node/confirmation_solicitor.hpp>
#include <oslo/node/testing.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST (confirmation_solicitor, batches)
{
	oslo::system system;
	oslo::node_flags node_flags;
	node_flags.disable_request_loop = true;
	node_flags.disable_rep_crawler = true;
	node_flags.disable_udp = false;
	auto & node1 = *system.add_node (node_flags);
	node_flags.disable_request_loop = true;
	auto & node2 = *system.add_node (node_flags);
	auto channel1 (node2.network.udp_channels.create (node1.network.endpoint ()));
	// Solicitor will only solicit from this representative
	oslo::representative representative (oslo::test_genesis_key.pub, oslo::genesis_amount, channel1);
	std::vector<oslo::representative> representatives{ representative };
	oslo::confirmation_solicitor solicitor (node2.network, node2.network_params.network);
	solicitor.prepare (representatives);
	// Ensure the representatives are correct
	ASSERT_EQ (1, representatives.size ());
	ASSERT_EQ (channel1, representatives.front ().channel);
	ASSERT_EQ (oslo::test_genesis_key.pub, representatives.front ().account);
	ASSERT_TIMELY (3s, node2.network.size () == 1);
	auto send (std::make_shared<oslo::send_block> (oslo::genesis_hash, oslo::keypair ().pub, oslo::genesis_amount - 100, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (oslo::genesis_hash)));
	send->sideband_set ({});
	{
		oslo::lock_guard<std::mutex> guard (node2.active.mutex);
		for (size_t i (0); i < oslo::network::confirm_req_hashes_max; ++i)
		{
			auto election (std::make_shared<oslo::election> (node2, send, nullptr, false));
			ASSERT_FALSE (solicitor.add (*election));
		}
		ASSERT_EQ (1, solicitor.max_confirm_req_batches);
		// Reached the maximum amount of requests for the channel
		auto election (std::make_shared<oslo::election> (node2, send, nullptr, false));
		ASSERT_TRUE (solicitor.add (*election));
		// Broadcasting should be immediate
		ASSERT_EQ (0, node2.stats.count (oslo::stat::type::message, oslo::stat::detail::publish, oslo::stat::dir::out));
		ASSERT_FALSE (solicitor.broadcast (*election));
	}
	// One publish through directed broadcasting and another through random flooding
	ASSERT_EQ (2, node2.stats.count (oslo::stat::type::message, oslo::stat::detail::publish, oslo::stat::dir::out));
	solicitor.flush ();
	ASSERT_EQ (1, node2.stats.count (oslo::stat::type::message, oslo::stat::detail::confirm_req, oslo::stat::dir::out));
}

TEST (confirmation_solicitor, different_hash)
{
	oslo::system system;
	oslo::node_flags node_flags;
	node_flags.disable_request_loop = true;
	node_flags.disable_rep_crawler = true;
	node_flags.disable_udp = false;
	auto & node1 = *system.add_node (node_flags);
	auto & node2 = *system.add_node (node_flags);
	auto channel1 (node2.network.udp_channels.create (node1.network.endpoint ()));
	// Solicitor will only solicit from this representative
	oslo::representative representative (oslo::test_genesis_key.pub, oslo::genesis_amount, channel1);
	std::vector<oslo::representative> representatives{ representative };
	oslo::confirmation_solicitor solicitor (node2.network, node2.network_params.network);
	solicitor.prepare (representatives);
	// Ensure the representatives are correct
	ASSERT_EQ (1, representatives.size ());
	ASSERT_EQ (channel1, representatives.front ().channel);
	ASSERT_EQ (oslo::test_genesis_key.pub, representatives.front ().account);
	ASSERT_TIMELY (3s, node2.network.size () == 1);
	auto send (std::make_shared<oslo::send_block> (oslo::genesis_hash, oslo::keypair ().pub, oslo::genesis_amount - 100, oslo::test_genesis_key.prv, oslo::test_genesis_key.pub, *system.work.generate (oslo::genesis_hash)));
	send->sideband_set ({});
	{
		oslo::lock_guard<std::mutex> guard (node2.active.mutex);
		auto election (std::make_shared<oslo::election> (node2, send, nullptr, false));
		// Add a vote for something else, not the winner
		election->last_votes[representative.account] = { std::chrono::steady_clock::now (), 1, 1 };
		// Ensure the request and broadcast goes through
		ASSERT_FALSE (solicitor.add (*election));
		ASSERT_FALSE (solicitor.broadcast (*election));
	}
	// One publish through directed broadcasting and another through random flooding
	ASSERT_EQ (2, node2.stats.count (oslo::stat::type::message, oslo::stat::detail::publish, oslo::stat::dir::out));
	solicitor.flush ();
	ASSERT_EQ (1, node2.stats.count (oslo::stat::type::message, oslo::stat::detail::confirm_req, oslo::stat::dir::out));
}
