#pragma once

#include <oslo/lib/numbers.hpp>
#include <oslo/lib/utility.hpp>

#include <memory>
#include <unordered_set>
#include <vector>

namespace oslo
{
class ledger;
class network_params;
class transaction;

/** Track online representatives and trend online weight */
class online_reps final
{
public:
	online_reps (oslo::ledger & ledger_a, oslo::network_params & network_params_a, oslo::uint128_t minimum_a);
	/** Add voting account \p rep_account to the set of online representatives */
	void observe (oslo::account const & rep_account);
	/** Called periodically to sample online weight */
	void sample ();
	/** Returns the trended online stake, but never less than configured minimum */
	oslo::uint128_t online_stake () const;
	/** List of online representatives */
	std::vector<oslo::account> list ();

private:
	oslo::uint128_t trend (oslo::transaction &);
	mutable std::mutex mutex;
	oslo::ledger & ledger;
	oslo::network_params & network_params;
	std::unordered_set<oslo::account> reps;
	oslo::uint128_t online;
	oslo::uint128_t minimum;

	friend std::unique_ptr<container_info_component> collect_container_info (online_reps & online_reps, const std::string & name);
};

std::unique_ptr<container_info_component> collect_container_info (online_reps & online_reps, const std::string & name);
}
