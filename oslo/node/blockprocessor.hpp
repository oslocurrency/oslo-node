#pragma once

#include <oslo/lib/blocks.hpp>
#include <oslo/node/state_block_signature_verification.hpp>
#include <oslo/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <memory>
#include <unordered_set>

namespace oslo
{
class node;
class transaction;
class write_transaction;
class write_database_queue;

enum class block_origin
{
	local,
	remote
};

class block_post_events final
{
public:
	~block_post_events ();
	std::deque<std::function<void()>> events;
};

/**
 * Processing blocks is a potentially long IO operation.
 * This class isolates block insertion from other operations like servicing network operations
 */
class block_processor final
{
public:
	explicit block_processor (oslo::node &, oslo::write_database_queue &);
	~block_processor ();
	void stop ();
	void flush ();
	size_t size ();
	bool full ();
	bool half_full ();
	void add (oslo::unchecked_info const &, const bool = false);
	void add (std::shared_ptr<oslo::block>, uint64_t = 0);
	void force (std::shared_ptr<oslo::block>);
	void wait_write ();
	bool should_log ();
	bool have_blocks ();
	void process_blocks ();
	oslo::process_return process_one (oslo::write_transaction const &, block_post_events &, oslo::unchecked_info, const bool = false, oslo::block_origin const = oslo::block_origin::remote);
	oslo::process_return process_one (oslo::write_transaction const &, block_post_events &, std::shared_ptr<oslo::block>, const bool = false);
	std::atomic<bool> flushing{ false };
	// Delay required for average network propagartion before requesting confirmation
	static std::chrono::milliseconds constexpr confirmation_request_delay{ 1500 };

private:
	void queue_unchecked (oslo::write_transaction const &, oslo::block_hash const &);
	void process_batch (oslo::unique_lock<std::mutex> &);
	void process_live (oslo::block_hash const &, std::shared_ptr<oslo::block>, oslo::process_return const &, const bool = false, oslo::block_origin const = oslo::block_origin::remote);
	void process_old (oslo::write_transaction const &, std::shared_ptr<oslo::block> const &, oslo::block_origin const);
	void requeue_invalid (oslo::block_hash const &, oslo::unchecked_info const &);
	void process_verified_state_blocks (std::deque<oslo::unchecked_info> &, std::vector<int> const &, std::vector<oslo::block_hash> const &, std::vector<oslo::signature> const &);
	bool stopped{ false };
	bool active{ false };
	bool awaiting_write{ false };
	std::chrono::steady_clock::time_point next_log;
	std::deque<oslo::unchecked_info> blocks;
	std::deque<std::shared_ptr<oslo::block>> forced;
	oslo::condition_variable condition;
	oslo::node & node;
	oslo::write_database_queue & write_database_queue;
	std::mutex mutex;
	oslo::state_block_signature_verification state_block_signature_verification;

	friend std::unique_ptr<container_info_component> collect_container_info (block_processor & block_processor, const std::string & name);
};
std::unique_ptr<oslo::container_info_component> collect_container_info (block_processor & block_processor, const std::string & name);
}
