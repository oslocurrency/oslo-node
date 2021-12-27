#include <oslo/core_test/testutil.hpp>
#include <oslo/lib/ipc_client.hpp>
#include <oslo/lib/tomlconfig.hpp>
#include <oslo/node/ipc/ipc_access_config.hpp>
#include <oslo/node/ipc/ipc_server.hpp>
#include <oslo/node/testing.hpp>
#include <oslo/rpc/rpc.hpp>

#include <gtest/gtest.h>

#include <boost/property_tree/json_parser.hpp>

#include <chrono>
#include <memory>
#include <sstream>
#include <vector>

using namespace std::chrono_literals;

TEST (ipc, asynchronous)
{
	oslo::system system (1);
	system.nodes[0]->config.ipc_config.transport_tcp.enabled = true;
	system.nodes[0]->config.ipc_config.transport_tcp.port = 24077;
	oslo::node_rpc_config node_rpc_config;
	oslo::ipc::ipc_server ipc (*system.nodes[0], node_rpc_config);
	oslo::ipc::ipc_client client (system.nodes[0]->io_ctx);

	auto req (oslo::ipc::prepare_request (oslo::ipc::payload_encoding::json_v1, std::string (R"({"action": "block_count"})")));
	auto res (std::make_shared<std::vector<uint8_t>> ());
	std::atomic<bool> call_completed{ false };
	client.async_connect ("::1", 24077, [&client, &req, &res, &call_completed](oslo::error err) {
		client.async_write (req, [&client, &req, &res, &call_completed](oslo::error err_a, size_t size_a) {
			ASSERT_NO_ERROR (static_cast<std::error_code> (err_a));
			ASSERT_EQ (size_a, req.size ());
			// Read length
			client.async_read (res, sizeof (uint32_t), [&client, &res, &call_completed](oslo::error err_read_a, size_t size_read_a) {
				ASSERT_NO_ERROR (static_cast<std::error_code> (err_read_a));
				ASSERT_EQ (size_read_a, sizeof (uint32_t));
				uint32_t payload_size_l = boost::endian::big_to_native (*reinterpret_cast<uint32_t *> (res->data ()));
				// Read json payload
				client.async_read (res, payload_size_l, [&res, &call_completed](oslo::error err_read_a, size_t size_read_a) {
					std::string payload (res->begin (), res->end ());
					std::stringstream ss;
					ss << payload;

					// Make sure the response is valid json
					boost::property_tree::ptree blocks;
					boost::property_tree::read_json (ss, blocks);
					ASSERT_EQ (blocks.get<int> ("count"), 1);
					call_completed = true;
				});
			});
		});
	});
	system.deadline_set (5s);
	while (!call_completed)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ipc.stop ();
}

TEST (ipc, synchronous)
{
	oslo::system system (1);
	system.nodes[0]->config.ipc_config.transport_tcp.enabled = true;
	system.nodes[0]->config.ipc_config.transport_tcp.port = 24077;
	oslo::node_rpc_config node_rpc_config;
	oslo::ipc::ipc_server ipc (*system.nodes[0], node_rpc_config);
	oslo::ipc::ipc_client client (system.nodes[0]->io_ctx);

	// Start blocking IPC client in a separate thread
	std::atomic<bool> call_completed{ false };
	std::thread client_thread ([&client, &call_completed]() {
		client.connect ("::1", 24077);
		std::string response (oslo::ipc::request (oslo::ipc::payload_encoding::json_v1, client, std::string (R"({"action": "block_count"})")));
		std::stringstream ss;
		ss << response;
		// Make sure the response is valid json
		boost::property_tree::ptree blocks;
		boost::property_tree::read_json (ss, blocks);
		ASSERT_EQ (blocks.get<int> ("count"), 1);

		call_completed = true;
	});
	client_thread.detach ();

	system.deadline_set (5s);
	while (!call_completed)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ipc.stop ();
}

TEST (ipc, config_upgrade_v0_v1)
{
	auto path1 (oslo::unique_path ());
	auto path2 (oslo::unique_path ());
	oslo::ipc::ipc_config config1;
	oslo::ipc::ipc_config config2;
	oslo::jsonconfig tree;
	config1.serialize_json (tree);
	oslo::jsonconfig local = tree.get_required_child ("local");
	local.erase ("version");
	local.erase ("allow_unsafe");
	bool upgraded (false);
	ASSERT_FALSE (config2.deserialize_json (upgraded, tree));
	oslo::jsonconfig local2 = tree.get_required_child ("local");
	ASSERT_TRUE (upgraded);
	ASSERT_LE (1, local2.get<int> ("version"));
	ASSERT_FALSE (local2.get<bool> ("allow_unsafe"));
}

TEST (ipc, permissions_default_user)
{
	// Test empty/nonexistant access config. The default user still exists with default permissions.
	std::stringstream ss;
	ss << R"toml(
	)toml";

	oslo::tomlconfig toml;
	toml.read (ss);

	oslo::ipc::access access;
	access.deserialize_toml (toml);
	ASSERT_TRUE (access.has_access ("", oslo::ipc::access_permission::api_account_weight));
}

TEST (ipc, permissions_deny_default)
{
	// All users have api_account_weight permissions by default. This removes the permission for a specific user.
	std::stringstream ss;
	ss << R"toml(
	[[user]]
	id = "user1"
	deny = "api_account_weight"
	)toml";

	oslo::tomlconfig toml;
	toml.read (ss);

	oslo::ipc::access access;
	access.deserialize_toml (toml);
	ASSERT_FALSE (access.has_access ("user1", oslo::ipc::access_permission::api_account_weight));
}

TEST (ipc, permissions_groups)
{
	// Make sure role permissions are adopted by user
	std::stringstream ss;
	ss << R"toml(
	[[role]]
	id = "mywalletadmin"
	allow = "wallet_read, wallet_write"

	[[user]]
	id = "user1"
	roles = "mywalletadmin"
	deny = "api_account_weight"
	)toml";

	oslo::tomlconfig toml;
	toml.read (ss);

	oslo::ipc::access access;
	access.deserialize_toml (toml);
	ASSERT_FALSE (access.has_access ("user1", oslo::ipc::access_permission::api_account_weight));
	ASSERT_TRUE (access.has_access_to_all ("user1", { oslo::ipc::access_permission::wallet_read, oslo::ipc::access_permission::wallet_write }));
}

TEST (ipc, permissions_oneof)
{
	// Test one of two permissions
	std::stringstream ss;
	ss << R"toml(
	[[user]]
	id = "user1"
	allow = "api_account_weight"
	[[user]]
	id = "user2"
	allow = "api_account_weight, account_query"
	[[user]]
	id = "user3"
	deny = "api_account_weight, account_query"
	)toml";

	oslo::tomlconfig toml;
	toml.read (ss);

	oslo::ipc::access access;
	access.deserialize_toml (toml);
	ASSERT_TRUE (access.has_access ("user1", oslo::ipc::access_permission::api_account_weight));
	ASSERT_TRUE (access.has_access ("user2", oslo::ipc::access_permission::api_account_weight));
	ASSERT_FALSE (access.has_access ("user3", oslo::ipc::access_permission::api_account_weight));
	ASSERT_TRUE (access.has_access_to_oneof ("user1", { oslo::ipc::access_permission::account_query, oslo::ipc::access_permission::api_account_weight }));
	ASSERT_TRUE (access.has_access_to_oneof ("user2", { oslo::ipc::access_permission::account_query, oslo::ipc::access_permission::api_account_weight }));
	ASSERT_FALSE (access.has_access_to_oneof ("user3", { oslo::ipc::access_permission::account_query, oslo::ipc::access_permission::api_account_weight }));
}

TEST (ipc, permissions_default_user_order)
{
	// If changing the default user, it must come first
	std::stringstream ss;
	ss << R"toml(
	[[user]]
	id = "user1"
	[[user]]
	id = ""
	)toml";

	oslo::tomlconfig toml;
	toml.read (ss);

	oslo::ipc::access access;
	ASSERT_TRUE (access.deserialize_toml (toml));
}
