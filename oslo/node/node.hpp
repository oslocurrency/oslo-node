#pragma once

#include <oslo/lib/alarm.hpp>
#include <oslo/lib/stats.hpp>
#include <oslo/lib/work.hpp>
#include <oslo/lib/worker.hpp>
#include <oslo/node/active_transactions.hpp>
#include <oslo/node/blockprocessor.hpp>
#include <oslo/node/bootstrap/bootstrap.hpp>
#include <oslo/node/bootstrap/bootstrap_attempt.hpp>
#include <oslo/node/bootstrap/bootstrap_server.hpp>
#include <oslo/node/confirmation_height_processor.hpp>
#include <oslo/node/distributed_work_factory.hpp>
#include <oslo/node/election.hpp>
#include <oslo/node/gap_cache.hpp>
#include <oslo/node/network.hpp>
#include <oslo/node/node_observers.hpp>
#include <oslo/node/nodeconfig.hpp>
#include <oslo/node/online_reps.hpp>
#include <oslo/node/payment_observer_processor.hpp>
#include <oslo/node/portmapping.hpp>
#include <oslo/node/repcrawler.hpp>
#include <oslo/node/request_aggregator.hpp>
#include <oslo/node/signatures.hpp>
#include <oslo/node/telemetry.hpp>
#include <oslo/node/vote_processor.hpp>
#include <oslo/node/wallet.hpp>
#include <oslo/node/write_database_queue.hpp>
#include <oslo/secure/ledger.hpp>
#include <oslo/secure/utility.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/program_options.hpp>
#include <boost/thread/latch.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace oslo
{
namespace websocket
{
	class listener;
}
class node;
class telemetry;
class work_pool;
class block_arrival_info final
{
public:
	std::chrono::steady_clock::time_point arrival;
	oslo::block_hash hash;
};
// This class tracks blocks that are probably live because they arrived in a UDP packet
// This gives a fairly reliable way to differentiate between blocks being inserted via bootstrap or new, live blocks.
class block_arrival final
{
public:
	// Return `true' to indicated an error if the block has already been inserted
	bool add (oslo::block_hash const &);
	bool recent (oslo::block_hash const &);
	// clang-format off
	class tag_sequence {};
	class tag_hash {};
	boost::multi_index_container<oslo::block_arrival_info,
		boost::multi_index::indexed_by<
			boost::multi_index::sequenced<boost::multi_index::tag<tag_sequence>>,
			boost::multi_index::hashed_unique<boost::multi_index::tag<tag_hash>,
				boost::multi_index::member<oslo::block_arrival_info, oslo::block_hash, &oslo::block_arrival_info::hash>>>>
	arrival;
	// clang-format on
	std::mutex mutex;
	static size_t constexpr arrival_size_min = 8 * 1024;
	static std::chrono::seconds constexpr arrival_time_min = std::chrono::seconds (300);
};

std::unique_ptr<container_info_component> collect_container_info (block_arrival & block_arrival, const std::string & name);

std::unique_ptr<container_info_component> collect_container_info (rep_crawler & rep_crawler, const std::string & name);

class node final : public std::enable_shared_from_this<oslo::node>
{
public:
	node (boost::asio::io_context &, uint16_t, boost::filesystem::path const &, oslo::alarm &, oslo::logging const &, oslo::work_pool &, oslo::node_flags = oslo::node_flags (), unsigned seq = 0);
	node (boost::asio::io_context &, boost::filesystem::path const &, oslo::alarm &, oslo::node_config const &, oslo::work_pool &, oslo::node_flags = oslo::node_flags (), unsigned seq = 0);
	~node ();
	template <typename T>
	void background (T action_a)
	{
		alarm.io_ctx.post (action_a);
	}
	bool copy_with_compaction (boost::filesystem::path const &);
	void keepalive (std::string const &, uint16_t);
	void start ();
	void stop ();
	std::shared_ptr<oslo::node> shared ();
	int store_version ();
	void receive_confirmed (oslo::transaction const &, std::shared_ptr<oslo::block>, oslo::block_hash const &);
	void process_confirmed_data (oslo::transaction const &, std::shared_ptr<oslo::block>, oslo::block_hash const &, oslo::account &, oslo::uint128_t &, bool &, oslo::account &);
	void process_confirmed (oslo::election_status const &, uint64_t = 0);
	void process_active (std::shared_ptr<oslo::block>);
	oslo::process_return process (oslo::block &);
	oslo::process_return process_local (std::shared_ptr<oslo::block>, bool const = false);
	void keepalive_preconfigured (std::vector<std::string> const &);
	oslo::block_hash latest (oslo::account const &);
	oslo::uint128_t balance (oslo::account const &);
	std::shared_ptr<oslo::block> block (oslo::block_hash const &);
	std::pair<oslo::uint128_t, oslo::uint128_t> balance_pending (oslo::account const &);
	oslo::uint128_t weight (oslo::account const &);
	oslo::block_hash rep_block (oslo::account const &);
	oslo::uint128_t minimum_principal_weight ();
	oslo::uint128_t minimum_principal_weight (oslo::uint128_t const &);
	void ongoing_rep_calculation ();
	void ongoing_bootstrap ();
	void ongoing_store_flush ();
	void ongoing_peer_store ();
	void ongoing_unchecked_cleanup ();
	void backup_wallet ();
	void search_pending ();
	void bootstrap_wallet ();
	void unchecked_cleanup ();
	int price (oslo::uint128_t const &, int);
	// The default difficulty updates to base only when the first epoch_2 block is processed
	uint64_t default_difficulty (oslo::work_version const) const;
	uint64_t max_work_generate_difficulty (oslo::work_version const) const;
	bool local_work_generation_enabled () const;
	bool work_generation_enabled () const;
	bool work_generation_enabled (std::vector<std::pair<std::string, uint16_t>> const &) const;
	boost::optional<uint64_t> work_generate_blocking (oslo::block &, uint64_t);
	boost::optional<uint64_t> work_generate_blocking (oslo::work_version const, oslo::root const &, uint64_t, boost::optional<oslo::account> const & = boost::none);
	void work_generate (oslo::work_version const, oslo::root const &, uint64_t, std::function<void(boost::optional<uint64_t>)>, boost::optional<oslo::account> const & = boost::none, bool const = false);
	void add_initial_peers ();
	void block_confirm (std::shared_ptr<oslo::block>);
	bool block_confirmed (oslo::block_hash const &);
	bool block_confirmed_or_being_confirmed (oslo::transaction const &, oslo::block_hash const &);
	void process_fork (oslo::transaction const &, std::shared_ptr<oslo::block>);
	void do_rpc_callback (boost::asio::ip::tcp::resolver::iterator i_a, std::string const &, uint16_t, std::shared_ptr<std::string>, std::shared_ptr<std::string>, std::shared_ptr<boost::asio::ip::tcp::resolver>);
	oslo::uint128_t delta () const;
	void ongoing_online_weight_calculation ();
	void ongoing_online_weight_calculation_queue ();
	bool online () const;
	bool init_error () const;
	bool epoch_upgrader (oslo::private_key const &, oslo::epoch, uint64_t, uint64_t);
	std::pair<uint64_t, decltype (oslo::ledger::bootstrap_weights)> get_bootstrap_weights () const;
	oslo::worker worker;
	oslo::write_database_queue write_database_queue;
	boost::asio::io_context & io_ctx;
	boost::latch node_initialized_latch;
	oslo::network_params network_params;
	oslo::node_config config;
	oslo::stat stats;
	std::shared_ptr<oslo::websocket::listener> websocket_server;
	oslo::node_flags flags;
	oslo::alarm & alarm;
	oslo::work_pool & work;
	oslo::distributed_work_factory distributed_work;
	oslo::logger_mt logger;
	std::unique_ptr<oslo::block_store> store_impl;
	oslo::block_store & store;
	std::unique_ptr<oslo::wallets_store> wallets_store_impl;
	oslo::wallets_store & wallets_store;
	oslo::gap_cache gap_cache;
	oslo::ledger ledger;
	oslo::signature_checker checker;
	oslo::network network;
	std::shared_ptr<oslo::telemetry> telemetry;
	oslo::bootstrap_initiator bootstrap_initiator;
	oslo::bootstrap_listener bootstrap;
	boost::filesystem::path application_path;
	oslo::node_observers observers;
	oslo::port_mapping port_mapping;
	oslo::vote_processor vote_processor;
	oslo::rep_crawler rep_crawler;
	unsigned warmed_up;
	oslo::block_processor block_processor;
	std::thread block_processor_thread;
	oslo::block_arrival block_arrival;
	oslo::online_reps online_reps;
	oslo::votes_cache votes_cache;
	oslo::keypair node_id;
	oslo::block_uniquer block_uniquer;
	oslo::vote_uniquer vote_uniquer;
	oslo::confirmation_height_processor confirmation_height_processor;
	oslo::active_transactions active;
	oslo::request_aggregator aggregator;
	oslo::payment_observer_processor payment_observer_processor;
	oslo::wallets wallets;
	const std::chrono::steady_clock::time_point startup_time;
	std::chrono::seconds unchecked_cutoff = std::chrono::seconds (7 * 24 * 60 * 60); // Week
	std::atomic<bool> unresponsive_work_peers{ false };
	std::atomic<bool> stopped{ false };
	static double constexpr price_max = 16.0;
	static double constexpr free_cutoff = 1024.0;
	// For tests only
	unsigned node_seq;
	// For tests only
	boost::optional<uint64_t> work_generate_blocking (oslo::block &);
	// For tests only
	boost::optional<uint64_t> work_generate_blocking (oslo::root const &, uint64_t);
	// For tests only
	boost::optional<uint64_t> work_generate_blocking (oslo::root const &);

private:
	void long_inactivity_cleanup ();
	void epoch_upgrader_impl (oslo::private_key const &, oslo::epoch, uint64_t, uint64_t);
	oslo::locked<std::future<void>> epoch_upgrading;
};

std::unique_ptr<container_info_component> collect_container_info (node & node, const std::string & name);

oslo::node_flags const & inactive_node_flag_defaults ();

class inactive_node final
{
public:
	inactive_node (boost::filesystem::path const & path_a, oslo::node_flags const & node_flags_a = oslo::inactive_node_flag_defaults ());
	~inactive_node ();
	std::shared_ptr<boost::asio::io_context> io_context;
	oslo::alarm alarm;
	oslo::work_pool work;
	std::shared_ptr<oslo::node> node;
};
std::unique_ptr<oslo::inactive_node> default_inactive_node (boost::filesystem::path const &, boost::program_options::variables_map const &);
}
