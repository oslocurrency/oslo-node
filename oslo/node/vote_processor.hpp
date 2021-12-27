#pragma once

#include <oslo/lib/numbers.hpp>
#include <oslo/lib/utility.hpp>
#include <oslo/secure/common.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace oslo
{
class signature_checker;
class active_transactions;
class block_store;
class node_observers;
class stats;
class node_config;
class logger_mt;
class online_reps;
class ledger;
class network_params;

class transaction;
namespace transport
{
	class channel;
}

class vote_processor final
{
public:
	explicit vote_processor (oslo::signature_checker & checker_a, oslo::active_transactions & active_a, oslo::node_observers & observers_a, oslo::stat & stats_a, oslo::node_config & config_a, oslo::node_flags & flags_a, oslo::logger_mt & logger_a, oslo::online_reps & online_reps_a, oslo::ledger & ledger_a, oslo::network_params & network_params_a);
	/** Returns false if the vote was processed */
	bool vote (std::shared_ptr<oslo::vote>, std::shared_ptr<oslo::transport::channel>);
	/** Note: node.active.mutex lock is required */
	oslo::vote_code vote_blocking (std::shared_ptr<oslo::vote>, std::shared_ptr<oslo::transport::channel>, bool = false);
	void verify_votes (std::deque<std::pair<std::shared_ptr<oslo::vote>, std::shared_ptr<oslo::transport::channel>>> const &);
	void flush ();
	size_t size ();
	bool empty ();
	void calculate_weights ();
	void stop ();

private:
	void process_loop ();

	oslo::signature_checker & checker;
	oslo::active_transactions & active;
	oslo::node_observers & observers;
	oslo::stat & stats;
	oslo::node_config & config;
	oslo::logger_mt & logger;
	oslo::online_reps & online_reps;
	oslo::ledger & ledger;
	oslo::network_params & network_params;

	size_t max_votes;

	std::deque<std::pair<std::shared_ptr<oslo::vote>, std::shared_ptr<oslo::transport::channel>>> votes;
	/** Representatives levels for random early detection */
	std::unordered_set<oslo::account> representatives_1;
	std::unordered_set<oslo::account> representatives_2;
	std::unordered_set<oslo::account> representatives_3;
	oslo::condition_variable condition;
	std::mutex mutex;
	bool started;
	bool stopped;
	bool is_active;
	std::thread thread;

	friend std::unique_ptr<container_info_component> collect_container_info (vote_processor & vote_processor, const std::string & name);
	friend class vote_processor_weights_Test;
};

std::unique_ptr<container_info_component> collect_container_info (vote_processor & vote_processor, const std::string & name);
}
