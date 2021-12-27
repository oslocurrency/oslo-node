#pragma once

#include <oslo/node/common.hpp>
#include <oslo/node/socket.hpp>

#include <unordered_set>

namespace oslo
{
class bootstrap_attempt;
class pull_info
{
public:
	using count_t = oslo::bulk_pull::count_t;
	pull_info () = default;
	pull_info (oslo::hash_or_account const &, oslo::block_hash const &, oslo::block_hash const &, uint64_t, count_t = 0, unsigned = 16);
	oslo::hash_or_account account_or_head{ 0 };
	oslo::block_hash head{ 0 };
	oslo::block_hash head_original{ 0 };
	oslo::block_hash end{ 0 };
	count_t count{ 0 };
	unsigned attempts{ 0 };
	uint64_t processed{ 0 };
	unsigned retry_limit{ 0 };
	uint64_t bootstrap_id{ 0 };
};
class bootstrap_client;
class bulk_pull_client final : public std::enable_shared_from_this<oslo::bulk_pull_client>
{
public:
	bulk_pull_client (std::shared_ptr<oslo::bootstrap_client>, std::shared_ptr<oslo::bootstrap_attempt>, oslo::pull_info const &);
	~bulk_pull_client ();
	void request ();
	void receive_block ();
	void throttled_receive_block ();
	void received_type ();
	void received_block (boost::system::error_code const &, size_t, oslo::block_type);
	oslo::block_hash first ();
	std::shared_ptr<oslo::bootstrap_client> connection;
	std::shared_ptr<oslo::bootstrap_attempt> attempt;
	oslo::block_hash expected;
	oslo::account known_account;
	oslo::pull_info pull;
	uint64_t pull_blocks;
	uint64_t unexpected_count;
	bool network_error{ false };
};
class bulk_pull_account_client final : public std::enable_shared_from_this<oslo::bulk_pull_account_client>
{
public:
	bulk_pull_account_client (std::shared_ptr<oslo::bootstrap_client>, std::shared_ptr<oslo::bootstrap_attempt>, oslo::account const &);
	~bulk_pull_account_client ();
	void request ();
	void receive_pending ();
	std::shared_ptr<oslo::bootstrap_client> connection;
	std::shared_ptr<oslo::bootstrap_attempt> attempt;
	oslo::account account;
	uint64_t pull_blocks;
};
class bootstrap_server;
class bulk_pull;
class bulk_pull_server final : public std::enable_shared_from_this<oslo::bulk_pull_server>
{
public:
	bulk_pull_server (std::shared_ptr<oslo::bootstrap_server> const &, std::unique_ptr<oslo::bulk_pull>);
	void set_current_end ();
	std::shared_ptr<oslo::block> get_next ();
	void send_next ();
	void sent_action (boost::system::error_code const &, size_t);
	void send_finished ();
	void no_block_sent (boost::system::error_code const &, size_t);
	std::shared_ptr<oslo::bootstrap_server> connection;
	std::unique_ptr<oslo::bulk_pull> request;
	oslo::block_hash current;
	bool include_start;
	oslo::bulk_pull::count_t max_count;
	oslo::bulk_pull::count_t sent_count;
};
class bulk_pull_account;
class bulk_pull_account_server final : public std::enable_shared_from_this<oslo::bulk_pull_account_server>
{
public:
	bulk_pull_account_server (std::shared_ptr<oslo::bootstrap_server> const &, std::unique_ptr<oslo::bulk_pull_account>);
	void set_params ();
	std::pair<std::unique_ptr<oslo::pending_key>, std::unique_ptr<oslo::pending_info>> get_next ();
	void send_frontier ();
	void send_next_block ();
	void sent_action (boost::system::error_code const &, size_t);
	void send_finished ();
	void complete (boost::system::error_code const &, size_t);
	std::shared_ptr<oslo::bootstrap_server> connection;
	std::unique_ptr<oslo::bulk_pull_account> request;
	std::unordered_set<oslo::uint256_union> deduplication;
	oslo::pending_key current_key;
	bool pending_address_only;
	bool pending_include_address;
	bool invalid_request;
};
}
