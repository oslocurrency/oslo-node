#pragma once

#include <oslo/lib/numbers.hpp>
#include <oslo/lib/utility.hpp>
#include <oslo/node/active_transactions.hpp>
#include <oslo/node/transport/transport.hpp>

namespace oslo
{
class telemetry;
class node_observers final
{
public:
	using blocks_t = oslo::observer_set<oslo::election_status const &, oslo::account const &, oslo::uint128_t const &, bool>;
	blocks_t blocks;
	oslo::observer_set<bool> wallet;
	oslo::observer_set<std::shared_ptr<oslo::vote>, std::shared_ptr<oslo::transport::channel>, oslo::vote_code> vote;
	oslo::observer_set<oslo::block_hash const &> active_stopped;
	oslo::observer_set<oslo::account const &, bool> account_balance;
	oslo::observer_set<std::shared_ptr<oslo::transport::channel>> endpoint;
	oslo::observer_set<> disconnect;
	oslo::observer_set<uint64_t> difficulty;
	oslo::observer_set<oslo::root const &> work_cancel;
	oslo::observer_set<oslo::telemetry_data const &, oslo::endpoint const &> telemetry;
};

std::unique_ptr<container_info_component> collect_container_info (node_observers & node_observers, const std::string & name);
}
