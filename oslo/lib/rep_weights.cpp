#include <oslo/lib/rep_weights.hpp>
#include <oslo/secure/blockstore.hpp>

void oslo::rep_weights::representation_add (oslo::account const & source_rep, oslo::uint128_t const & amount_a)
{
	oslo::lock_guard<std::mutex> guard (mutex);
	auto source_previous (get (source_rep));
	put (source_rep, source_previous + amount_a);
}

void oslo::rep_weights::representation_put (oslo::account const & account_a, oslo::uint128_union const & representation_a)
{
	oslo::lock_guard<std::mutex> guard (mutex);
	put (account_a, representation_a);
}

oslo::uint128_t oslo::rep_weights::representation_get (oslo::account const & account_a)
{
	oslo::lock_guard<std::mutex> lk (mutex);
	return get (account_a);
}

/** Makes a copy */
std::unordered_map<oslo::account, oslo::uint128_t> oslo::rep_weights::get_rep_amounts ()
{
	oslo::lock_guard<std::mutex> guard (mutex);
	return rep_amounts;
}

void oslo::rep_weights::put (oslo::account const & account_a, oslo::uint128_union const & representation_a)
{
	auto it = rep_amounts.find (account_a);
	auto amount = representation_a.number ();
	if (it != rep_amounts.end ())
	{
		it->second = amount;
	}
	else
	{
		rep_amounts.emplace (account_a, amount);
	}
}

oslo::uint128_t oslo::rep_weights::get (oslo::account const & account_a)
{
	auto it = rep_amounts.find (account_a);
	if (it != rep_amounts.end ())
	{
		return it->second;
	}
	else
	{
		return oslo::uint128_t{ 0 };
	}
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (oslo::rep_weights & rep_weights, const std::string & name)
{
	size_t rep_amounts_count;

	{
		oslo::lock_guard<std::mutex> guard (rep_weights.mutex);
		rep_amounts_count = rep_weights.rep_amounts.size ();
	}
	auto sizeof_element = sizeof (decltype (rep_weights.rep_amounts)::value_type);
	auto composite = std::make_unique<oslo::container_info_composite> (name);
	composite->add_component (std::make_unique<oslo::container_info_leaf> (container_info{ "rep_amounts", rep_amounts_count, sizeof_element }));
	return composite;
}
