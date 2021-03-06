#pragma once

#include <oslo/node/bootstrap/bootstrap_attempt.hpp>
#include <oslo/node/bootstrap/bootstrap_bulk_pull.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <atomic>
#include <queue>
#include <unordered_set>

namespace mi = boost::multi_index;

namespace oslo
{
class node;
class lazy_state_backlog_item final
{
public:
	oslo::link link{ 0 };
	oslo::uint128_t balance{ 0 };
	unsigned retry_limit{ 0 };
};
class lazy_destinations_item final
{
public:
	oslo::account account{ 0 };
	uint64_t count{ 0 };
};
class bootstrap_attempt_lazy final : public bootstrap_attempt
{
public:
	explicit bootstrap_attempt_lazy (std::shared_ptr<oslo::node> node_a, uint64_t incremental_id_a, std::string id_a = "");
	~bootstrap_attempt_lazy ();
	bool process_block (std::shared_ptr<oslo::block>, oslo::account const &, uint64_t, oslo::bulk_pull::count_t, bool, unsigned) override;
	void run () override;
	void lazy_start (oslo::hash_or_account const &, bool confirmed = true) override;
	void lazy_add (oslo::hash_or_account const &, unsigned = std::numeric_limits<unsigned>::max ());
	void lazy_add (oslo::pull_info const &) override;
	void lazy_requeue (oslo::block_hash const &, oslo::block_hash const &, bool) override;
	bool lazy_finished ();
	bool lazy_has_expired () const override;
	uint32_t lazy_batch_size () override;
	void lazy_pull_flush (oslo::unique_lock<std::mutex> & lock_a);
	bool process_block_lazy (std::shared_ptr<oslo::block>, oslo::account const &, uint64_t, oslo::bulk_pull::count_t, unsigned);
	void lazy_block_state (std::shared_ptr<oslo::block>, unsigned);
	void lazy_block_state_backlog_check (std::shared_ptr<oslo::block>, oslo::block_hash const &);
	void lazy_backlog_cleanup ();
	void lazy_destinations_increment (oslo::account const &);
	void lazy_destinations_flush ();
	void lazy_blocks_insert (oslo::block_hash const &);
	void lazy_blocks_erase (oslo::block_hash const &);
	bool lazy_blocks_processed (oslo::block_hash const &);
	bool lazy_processed_or_exists (oslo::block_hash const &) override;
	void get_information (boost::property_tree::ptree &) override;
	std::unordered_set<size_t> lazy_blocks;
	std::unordered_map<oslo::block_hash, oslo::lazy_state_backlog_item> lazy_state_backlog;
	std::unordered_set<oslo::block_hash> lazy_undefined_links;
	std::unordered_map<oslo::block_hash, oslo::uint128_t> lazy_balances;
	std::unordered_set<oslo::block_hash> lazy_keys;
	std::deque<std::pair<oslo::hash_or_account, unsigned>> lazy_pulls;
	std::chrono::steady_clock::time_point lazy_start_time;
	class account_tag
	{
	};
	class count_tag
	{
	};
	// clang-format off
	boost::multi_index_container<lazy_destinations_item,
	mi::indexed_by<
		mi::ordered_non_unique<mi::tag<count_tag>,
			mi::member<lazy_destinations_item, uint64_t, &lazy_destinations_item::count>,
			std::greater<uint64_t>>,
		mi::hashed_unique<mi::tag<account_tag>,
			mi::member<lazy_destinations_item, oslo::account, &lazy_destinations_item::account>>>>
	lazy_destinations;
	// clang-format on
	std::atomic<size_t> lazy_blocks_count{ 0 };
	std::atomic<bool> lazy_destinations_flushed{ false };
	/** The maximum number of records to be read in while iterating over long lazy containers */
	static uint64_t constexpr batch_read_size = 256;
};
class bootstrap_attempt_wallet final : public bootstrap_attempt
{
public:
	explicit bootstrap_attempt_wallet (std::shared_ptr<oslo::node> node_a, uint64_t incremental_id_a, std::string id_a = "");
	~bootstrap_attempt_wallet ();
	void request_pending (oslo::unique_lock<std::mutex> &);
	void requeue_pending (oslo::account const &) override;
	void run () override;
	void wallet_start (std::deque<oslo::account> &) override;
	bool wallet_finished ();
	size_t wallet_size () override;
	void get_information (boost::property_tree::ptree &) override;
	std::deque<oslo::account> wallet_accounts;
};
}
