#pragma once

#include <oslo/lib/config.hpp>
#include <oslo/lib/locks.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/lib/utility.hpp>

#include <boost/optional.hpp>
#include <boost/thread/thread.hpp>

#include <atomic>
#include <memory>

namespace oslo
{
enum class work_version
{
	unspecified,
	work_1
};
std::string to_string (oslo::work_version const version_a);

class block;
class block_details;
bool work_validate_entry (oslo::block const &);
bool work_validate_entry (oslo::work_version const, oslo::root const &, uint64_t const);

uint64_t work_difficulty (oslo::work_version const, oslo::root const &, uint64_t const);

uint64_t work_threshold_base (oslo::work_version const);
uint64_t work_threshold_entry (oslo::work_version const);
// Ledger threshold
uint64_t work_threshold (oslo::work_version const, oslo::block_details const);

namespace work_v1
{
	uint64_t value (oslo::root const & root_a, uint64_t work_a);
	uint64_t threshold_base ();
	uint64_t threshold_entry ();
	uint64_t threshold (oslo::block_details const);
}

double normalized_multiplier (double const, uint64_t const);
double denormalized_multiplier (double const, uint64_t const);
class opencl_work;
class work_item final
{
public:
	work_item (oslo::work_version const version_a, oslo::root const & item_a, uint64_t difficulty_a, std::function<void(boost::optional<uint64_t> const &)> const & callback_a) :
	version (version_a), item (item_a), difficulty (difficulty_a), callback (callback_a)
	{
	}
	oslo::work_version const version;
	oslo::root const item;
	uint64_t const difficulty;
	std::function<void(boost::optional<uint64_t> const &)> const callback;
};
class work_pool final
{
public:
	work_pool (unsigned, std::chrono::nanoseconds = std::chrono::nanoseconds (0), std::function<boost::optional<uint64_t> (oslo::work_version const, oslo::root const &, uint64_t, std::atomic<int> &)> = nullptr);
	~work_pool ();
	void loop (uint64_t);
	void stop ();
	void cancel (oslo::root const &);
	void generate (oslo::work_version const, oslo::root const &, uint64_t, std::function<void(boost::optional<uint64_t> const &)>);
	boost::optional<uint64_t> generate (oslo::work_version const, oslo::root const &, uint64_t);
	// For tests only
	boost::optional<uint64_t> generate (oslo::root const &);
	boost::optional<uint64_t> generate (oslo::root const &, uint64_t);
	size_t size ();
	oslo::network_constants network_constants;
	std::atomic<int> ticket;
	bool done;
	std::vector<boost::thread> threads;
	std::list<oslo::work_item> pending;
	std::mutex mutex;
	oslo::condition_variable producer_condition;
	std::chrono::nanoseconds pow_rate_limiter;
	std::function<boost::optional<uint64_t> (oslo::work_version const, oslo::root const &, uint64_t, std::atomic<int> &)> opencl;
	oslo::observer_set<bool> work_observers;
};

std::unique_ptr<container_info_component> collect_container_info (work_pool & work_pool, const std::string & name);
}
