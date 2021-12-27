#pragma once

#include <oslo/lib/errors.hpp>
#include <oslo/node/node.hpp>

#include <chrono>

namespace oslo
{
/** Test-system related error codes */
enum class error_system
{
	generic = 1,
	deadline_expired
};
class system final
{
public:
	system ();
	system (uint16_t, oslo::transport::transport_type = oslo::transport::transport_type::tcp, oslo::node_flags = oslo::node_flags ());
	~system ();
	void generate_activity (oslo::node &, std::vector<oslo::account> &);
	void generate_mass_activity (uint32_t, oslo::node &);
	void generate_usage_traffic (uint32_t, uint32_t, size_t);
	void generate_usage_traffic (uint32_t, uint32_t);
	oslo::account get_random_account (std::vector<oslo::account> &);
	oslo::uint128_t get_random_amount (oslo::transaction const &, oslo::node &, oslo::account const &);
	void generate_rollback (oslo::node &, std::vector<oslo::account> &);
	void generate_change_known (oslo::node &, std::vector<oslo::account> &);
	void generate_change_unknown (oslo::node &, std::vector<oslo::account> &);
	void generate_receive (oslo::node &);
	void generate_send_new (oslo::node &, std::vector<oslo::account> &);
	void generate_send_existing (oslo::node &, std::vector<oslo::account> &);
	std::unique_ptr<oslo::state_block> upgrade_genesis_epoch (oslo::node &, oslo::epoch const);
	std::shared_ptr<oslo::wallet> wallet (size_t);
	oslo::account account (oslo::transaction const &, size_t);
	/** Generate work with difficulty between \p min_difficulty_a (inclusive) and \p max_difficulty_a (exclusive) */
	uint64_t work_generate_limited (oslo::block_hash const & root_a, uint64_t min_difficulty_a, uint64_t max_difficulty_a);
	/**
	 * Polls, sleep if there's no work to be done (default 50ms), then check the deadline
	 * @returns 0 or oslo::deadline_expired
	 */
	std::error_code poll (const std::chrono::nanoseconds & sleep_time = std::chrono::milliseconds (50));
	std::error_code poll_until_true (std::chrono::nanoseconds deadline, std::function<bool()>);
	void stop ();
	void deadline_set (const std::chrono::duration<double, std::nano> & delta);
	std::shared_ptr<oslo::node> add_node (oslo::node_flags = oslo::node_flags (), oslo::transport::transport_type = oslo::transport::transport_type::tcp);
	std::shared_ptr<oslo::node> add_node (oslo::node_config const &, oslo::node_flags = oslo::node_flags (), oslo::transport::transport_type = oslo::transport::transport_type::tcp);
	boost::asio::io_context io_ctx;
	oslo::alarm alarm{ io_ctx };
	std::vector<std::shared_ptr<oslo::node>> nodes;
	oslo::logging logging;
	oslo::work_pool work{ std::max (std::thread::hardware_concurrency (), 1u) };
	std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<double>> deadline{ std::chrono::steady_clock::time_point::max () };
	double deadline_scaling_factor{ 1.0 };
	unsigned node_sequence{ 0 };
};
std::unique_ptr<oslo::state_block> upgrade_epoch (oslo::work_pool &, oslo::ledger &, oslo::epoch);
void blocks_confirm (oslo::node &, std::vector<std::shared_ptr<oslo::block>> const &);
}
REGISTER_ERROR_CODES (oslo, error_system);
