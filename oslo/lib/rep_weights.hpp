#pragma once

#include <oslo/lib/numbers.hpp>
#include <oslo/lib/utility.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace oslo
{
class block_store;
class transaction;

class rep_weights
{
public:
	void representation_add (oslo::account const & source_a, oslo::uint128_t const & amount_a);
	oslo::uint128_t representation_get (oslo::account const & account_a);
	void representation_put (oslo::account const & account_a, oslo::uint128_union const & representation_a);
	std::unordered_map<oslo::account, oslo::uint128_t> get_rep_amounts ();

private:
	std::mutex mutex;
	std::unordered_map<oslo::account, oslo::uint128_t> rep_amounts;
	void put (oslo::account const & account_a, oslo::uint128_union const & representation_a);
	oslo::uint128_t get (oslo::account const & account_a);

	friend std::unique_ptr<container_info_component> collect_container_info (rep_weights &, const std::string &);
};

std::unique_ptr<container_info_component> collect_container_info (rep_weights &, const std::string &);
}
