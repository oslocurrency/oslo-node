#include <oslo/node/online_reps.hpp>
#include <oslo/secure/blockstore.hpp>
#include <oslo/secure/common.hpp>
#include <oslo/secure/ledger.hpp>

oslo::online_reps::online_reps (oslo::ledger & ledger_a, oslo::network_params & network_params_a, oslo::uint128_t minimum_a) :
ledger (ledger_a),
network_params (network_params_a),
minimum (minimum_a)
{
	if (!ledger.store.init_error ())
	{
		auto transaction (ledger.store.tx_begin_read ());
		online = trend (transaction);
	}
}

void oslo::online_reps::observe (oslo::account const & rep_a)
{
	if (ledger.weight (rep_a) > 0)
	{
		oslo::lock_guard<std::mutex> lock (mutex);
		reps.insert (rep_a);
	}
}

void oslo::online_reps::sample ()
{
	auto transaction (ledger.store.tx_begin_write ({ tables::online_weight }));
	// Discard oldest entries
	while (ledger.store.online_weight_count (transaction) >= network_params.node.max_weight_samples)
	{
		auto oldest (ledger.store.online_weight_begin (transaction));
		debug_assert (oldest != ledger.store.online_weight_end ());
		ledger.store.online_weight_del (transaction, oldest->first);
	}
	// Calculate current active rep weight
	oslo::uint128_t current;
	std::unordered_set<oslo::account> reps_copy;
	{
		oslo::lock_guard<std::mutex> lock (mutex);
		reps_copy.swap (reps);
	}
	for (auto & i : reps_copy)
	{
		current += ledger.weight (i);
	}
	ledger.store.online_weight_put (transaction, std::chrono::system_clock::now ().time_since_epoch ().count (), current);
	auto trend_l (trend (transaction));
	oslo::lock_guard<std::mutex> lock (mutex);
	online = trend_l;
}

oslo::uint128_t oslo::online_reps::trend (oslo::transaction & transaction_a)
{
	std::vector<oslo::uint128_t> items;
	items.reserve (network_params.node.max_weight_samples + 1);
	items.push_back (minimum);
	for (auto i (ledger.store.online_weight_begin (transaction_a)), n (ledger.store.online_weight_end ()); i != n; ++i)
	{
		items.push_back (i->second.number ());
	}

	// Pick median value for our target vote weight
	auto median_idx = items.size () / 2;
	nth_element (items.begin (), items.begin () + median_idx, items.end ());
	return oslo::uint128_t{ items[median_idx] };
}

oslo::uint128_t oslo::online_reps::online_stake () const
{
	oslo::lock_guard<std::mutex> lock (mutex);
	return std::max (online, minimum);
}

std::vector<oslo::account> oslo::online_reps::list ()
{
	std::vector<oslo::account> result;
	oslo::lock_guard<std::mutex> lock (mutex);
	result.insert (result.end (), reps.begin (), reps.end ());
	return result;
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (online_reps & online_reps, const std::string & name)
{
	size_t count;
	{
		oslo::lock_guard<std::mutex> guard (online_reps.mutex);
		count = online_reps.reps.size ();
	}

	auto sizeof_element = sizeof (decltype (online_reps.reps)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "reps", count, sizeof_element }));
	return composite;
}
