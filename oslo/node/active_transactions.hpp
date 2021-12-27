#pragma once

#include <oslo/lib/numbers.hpp>
#include <oslo/node/voting.hpp>
#include <oslo/secure/common.hpp>

#include <boost/circular_buffer.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/optional.hpp>
#include <boost/thread/thread.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace mi = boost::multi_index;

namespace oslo
{
class node;
class block;
class block_sideband;
class election;
class vote;
class transaction;
class confirmation_height_processor;
class stat;

class cementable_account final
{
public:
	cementable_account (oslo::account const & account_a, size_t blocks_uncemented_a);
	oslo::account account;
	uint64_t blocks_uncemented{ 0 };
};

class election_timepoint final
{
public:
	std::chrono::steady_clock::time_point time;
	oslo::qualified_root root;
};

class inactive_cache_information final
{
public:
	std::chrono::steady_clock::time_point arrival;
	oslo::block_hash hash;
	std::vector<oslo::account> voters;
	bool bootstrap_started{ false };
	bool confirmed{ false }; // Did item reach votes quorum? (minimum config value)
};

class dropped_elections final
{
public:
	dropped_elections (oslo::stat &);
	void add (oslo::qualified_root const &);
	void erase (oslo::qualified_root const &);
	std::chrono::steady_clock::time_point find (oslo::qualified_root const &) const;
	size_t size () const;

	static size_t constexpr capacity{ 16 * 1024 };

	// clang-format off
	class tag_sequence {};
	class tag_root {};
	using ordered_dropped = boost::multi_index_container<oslo::election_timepoint,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequence>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<oslo::election_timepoint, decltype(oslo::election_timepoint::root), &oslo::election_timepoint::root>>>>;
	// clang-format on

private:
	ordered_dropped items;
	mutable std::mutex mutex;
	oslo::stat & stats;
};

class election_insertion_result final
{
public:
	std::shared_ptr<oslo::election> election;
	bool inserted{ false };
};

// Core class for determining consensus
// Holds all active blocks i.e. recently added blocks that need confirmation
class active_transactions final
{
	class conflict_info final
	{
	public:
		oslo::qualified_root root;
		double multiplier;
		double adjusted_multiplier;
		std::shared_ptr<oslo::election> election;
		oslo::epoch epoch;
		oslo::uint128_t previous_balance;
	};

	friend class oslo::election;

	// clang-format off
	class tag_account {};
	class tag_difficulty {};
	class tag_root {};
	class tag_sequence {};
	class tag_uncemented {};
	class tag_arrival {};
	class tag_hash {};
	// clang-format on

public:
	// clang-format off
	using ordered_roots = boost::multi_index_container<conflict_info,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<conflict_info, oslo::qualified_root, &conflict_info::root>>,
		mi::ordered_non_unique<mi::tag<tag_difficulty>,
			mi::member<conflict_info, double, &conflict_info::adjusted_multiplier>,
			std::greater<double>>>>;
	// clang-format on
	ordered_roots roots;
	using roots_iterator = active_transactions::ordered_roots::index_iterator<tag_root>::type;

	explicit active_transactions (oslo::node &, oslo::confirmation_height_processor &);
	~active_transactions ();
	// Start an election for a block
	// Call action with confirmed block, may be different than what we started with
	// clang-format off
	oslo::election_insertion_result insert (std::shared_ptr<oslo::block> const &, boost::optional<oslo::uint128_t> const & = boost::none, std::function<void(std::shared_ptr<oslo::block>)> const & = [](std::shared_ptr<oslo::block>) {});
	// clang-format on
	// Distinguishes replay votes, cannot be determined if the block is not in any election
	oslo::vote_code vote (std::shared_ptr<oslo::vote>);
	// Is the root of this block in the roots container
	bool active (oslo::block const &);
	bool active (oslo::qualified_root const &);
	std::shared_ptr<oslo::election> election (oslo::qualified_root const &) const;
	std::shared_ptr<oslo::block> winner (oslo::block_hash const &) const;
	// Activates the first unconfirmed block of \p account_a
	oslo::election_insertion_result activate (oslo::account const &);
	// Returns false if the election difficulty was updated
	bool update_difficulty (oslo::block const &);
	// Returns false if the election was restarted
	bool restart (std::shared_ptr<oslo::block> const &, oslo::write_transaction const &);
	double normalized_multiplier (oslo::block const &, boost::optional<roots_iterator> const & = boost::none) const;
	void add_adjust_difficulty (oslo::block_hash const &);
	void update_adjusted_multiplier ();
	void update_active_multiplier (oslo::unique_lock<std::mutex> &);
	uint64_t active_difficulty ();
	uint64_t limited_active_difficulty (oslo::block const &);
	uint64_t limited_active_difficulty (oslo::work_version const, uint64_t const);
	double active_multiplier ();
	std::deque<std::shared_ptr<oslo::block>> list_blocks ();
	void erase (oslo::block const &);
	bool empty ();
	size_t size ();
	void stop ();
	bool publish (std::shared_ptr<oslo::block> block_a);
	boost::optional<oslo::election_status_type> confirm_block (oslo::transaction const &, std::shared_ptr<oslo::block>);
	void block_cemented_callback (std::shared_ptr<oslo::block> const & block_a);
	void block_already_cemented_callback (oslo::block_hash const &);
	boost::optional<double> last_prioritized_multiplier{ boost::none };
	std::unordered_map<oslo::block_hash, std::shared_ptr<oslo::election>> blocks;
	std::deque<oslo::election_status> list_recently_cemented ();
	std::deque<oslo::election_status> recently_cemented;
	dropped_elections recently_dropped;

	void add_recently_cemented (oslo::election_status const &);
	void add_recently_confirmed (oslo::qualified_root const &, oslo::block_hash const &);
	void add_inactive_votes_cache (oslo::block_hash const &, oslo::account const &);
	oslo::inactive_cache_information find_inactive_votes_cache (oslo::block_hash const &);
	void erase_inactive_votes_cache (oslo::block_hash const &);
	oslo::confirmation_height_processor & confirmation_height_processor;
	oslo::node & node;
	mutable std::mutex mutex;
	boost::circular_buffer<double> multipliers_cb;
	double trended_active_multiplier;
	size_t priority_cementable_frontiers_size ();
	size_t priority_wallet_cementable_frontiers_size ();
	boost::circular_buffer<double> difficulty_trend ();
	size_t inactive_votes_cache_size ();
	size_t election_winner_details_size ();
	void add_election_winner_details (oslo::block_hash const &, std::shared_ptr<oslo::election> const &);
	void remove_election_winner_details (oslo::block_hash const &);

private:
	std::mutex election_winner_details_mutex;
	std::unordered_map<oslo::block_hash, std::shared_ptr<oslo::election>> election_winner_details;
	oslo::vote_generator generator;

	// Call action with confirmed block, may be different than what we started with
	// clang-format off
	oslo::election_insertion_result insert_impl (std::shared_ptr<oslo::block> const &, boost::optional<oslo::uint128_t> const & = boost::none, std::function<void(std::shared_ptr<oslo::block>)> const & = [](std::shared_ptr<oslo::block>) {});
	// clang-format on
	// Returns false if the election difficulty was updated
	bool update_difficulty_impl (roots_iterator const &, oslo::block const &);
	void request_loop ();
	void confirm_prioritized_frontiers (oslo::transaction const & transaction_a);
	void request_confirm (oslo::unique_lock<std::mutex> &);
	void frontiers_confirmation (oslo::unique_lock<std::mutex> &);
	oslo::account next_frontier_account{ 0 };
	std::chrono::steady_clock::time_point next_frontier_check{ std::chrono::steady_clock::now () };
	void activate_dependencies (oslo::unique_lock<std::mutex> &);
	std::vector<std::pair<oslo::block_hash, uint64_t>> pending_dependencies;
	oslo::condition_variable condition;
	bool started{ false };
	std::atomic<bool> stopped{ false };

	// Periodically check all elections
	std::chrono::milliseconds const check_all_elections_period;
	std::chrono::steady_clock::time_point last_check_all_elections{};

	// Maximum time an election can be kept active if it is extending the container
	std::chrono::seconds const election_time_to_live;

	// Elections above this position in the queue are prioritized
	size_t const prioritized_cutoff;

	static size_t constexpr recently_confirmed_size{ 65536 };
	using recent_confirmation = std::pair<oslo::qualified_root, oslo::block_hash>;
	// clang-format off
	boost::multi_index_container<recent_confirmation,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequence>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<recent_confirmation, oslo::qualified_root, &recent_confirmation::first>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<recent_confirmation, oslo::block_hash, &recent_confirmation::second>>>>
	recently_confirmed;
	using prioritize_num_uncemented = boost::multi_index_container<oslo::cementable_account,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_account>,
			mi::member<oslo::cementable_account, oslo::account, &oslo::cementable_account::account>>,
		mi::ordered_non_unique<mi::tag<tag_uncemented>,
			mi::member<oslo::cementable_account, uint64_t, &oslo::cementable_account::blocks_uncemented>,
			std::greater<uint64_t>>>>;
	// clang-format on
	prioritize_num_uncemented priority_wallet_cementable_frontiers;
	prioritize_num_uncemented priority_cementable_frontiers;
	void prioritize_frontiers_for_confirmation (oslo::transaction const &, std::chrono::milliseconds, std::chrono::milliseconds);
	std::unordered_set<oslo::wallet_id> wallet_ids_already_iterated;
	std::unordered_map<oslo::wallet_id, oslo::account> next_wallet_id_accounts;
	bool skip_wallets{ false };
	void prioritize_account_for_confirmation (prioritize_num_uncemented &, size_t &, oslo::account const &, oslo::account_info const &, uint64_t);
	static size_t constexpr max_priority_cementable_frontiers{ 100000 };
	static size_t constexpr confirmed_frontiers_max_pending_size{ 10000 };
	std::deque<oslo::block_hash> adjust_difficulty_list;
	// clang-format off
	using ordered_cache = boost::multi_index_container<oslo::inactive_cache_information,
	mi::indexed_by<
		mi::ordered_non_unique<mi::tag<tag_arrival>,
			mi::member<oslo::inactive_cache_information, std::chrono::steady_clock::time_point, &oslo::inactive_cache_information::arrival>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<oslo::inactive_cache_information, oslo::block_hash, &oslo::inactive_cache_information::hash>>>>;
	ordered_cache inactive_votes_cache;
	// clang-format on
	bool inactive_votes_bootstrap_check (std::vector<oslo::account> const &, oslo::block_hash const &, bool &);
	boost::thread thread;

	friend class election;
	friend std::unique_ptr<container_info_component> collect_container_info (active_transactions &, const std::string &);

	friend class active_transactions_activate_dependencies_invalid_Test;
	friend class active_transactions_dropped_cleanup_Test;
	friend class active_transactions_vote_replays_Test;
	friend class confirmation_height_prioritize_frontiers_Test;
	friend class confirmation_height_prioritize_frontiers_overwrite_Test;
	friend class active_transactions_confirmation_consistency_Test;
	friend class active_transactions_vote_generator_session_Test;
	friend class node_vote_by_hash_bundle_Test;
	friend class node_deferred_dependent_elections_Test;
	friend class election_bisect_dependencies_Test;
	friend class election_dependencies_open_link_Test;
};

std::unique_ptr<container_info_component> collect_container_info (active_transactions & active_transactions, const std::string & name);
}
