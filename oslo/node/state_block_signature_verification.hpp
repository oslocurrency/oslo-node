#pragma once

#include <oslo/lib/locks.hpp>
#include <oslo/secure/common.hpp>

#include <deque>
#include <functional>
#include <thread>

namespace oslo
{
class epochs;
class logger_mt;
class node_config;
class signature_checker;

class state_block_signature_verification
{
public:
	state_block_signature_verification (oslo::signature_checker &, oslo::epochs &, oslo::node_config &, oslo::logger_mt &, uint64_t);
	~state_block_signature_verification ();
	void add (oslo::unchecked_info const & info_a);
	size_t size ();
	void stop ();
	bool is_active ();

	std::function<void(std::deque<oslo::unchecked_info> &, std::vector<int> const &, std::vector<oslo::block_hash> const &, std::vector<oslo::signature> const &)> blocks_verified_callback;
	std::function<void()> transition_inactive_callback;

private:
	oslo::signature_checker & signature_checker;
	oslo::epochs & epochs;
	oslo::node_config & node_config;
	oslo::logger_mt & logger;

	std::mutex mutex;
	bool stopped{ false };
	bool active{ false };
	std::deque<oslo::unchecked_info> state_blocks;
	oslo::condition_variable condition;
	std::thread thread;

	void run (uint64_t block_processor_verification_size);
	std::deque<oslo::unchecked_info> setup_items (size_t);
	void verify_state_blocks (std::deque<oslo::unchecked_info> &);
};

std::unique_ptr<oslo::container_info_component> collect_container_info (state_block_signature_verification & state_block_signature_verification, const std::string & name);
}
