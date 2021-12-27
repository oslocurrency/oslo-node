#include <oslo/node/json_payment_observer.hpp>
#include <oslo/node/payment_observer_processor.hpp>

oslo::payment_observer_processor::payment_observer_processor (oslo::node_observers::blocks_t & blocks)
{
	blocks.add ([this](oslo::election_status const &, oslo::account const & account_a, oslo::uint128_t const &, bool) {
		observer_action (account_a);
	});
}

void oslo::payment_observer_processor::observer_action (oslo::account const & account_a)
{
	std::shared_ptr<oslo::json_payment_observer> observer;
	{
		oslo::lock_guard<std::mutex> lock (mutex);
		auto existing (payment_observers.find (account_a));
		if (existing != payment_observers.end ())
		{
			observer = existing->second;
		}
	}
	if (observer != nullptr)
	{
		observer->observe ();
	}
}

void oslo::payment_observer_processor::add (oslo::account const & account_a, std::shared_ptr<oslo::json_payment_observer> payment_observer_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	debug_assert (payment_observers.find (account_a) == payment_observers.end ());
	payment_observers[account_a] = payment_observer_a;
}

void oslo::payment_observer_processor::erase (oslo::account & account_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	debug_assert (payment_observers.find (account_a) != payment_observers.end ());
	payment_observers.erase (account_a);
}
