#pragma once

#include <oslo/node/node_observers.hpp>

namespace oslo
{
class json_payment_observer;

class payment_observer_processor final
{
public:
	explicit payment_observer_processor (oslo::node_observers::blocks_t & blocks);
	void observer_action (oslo::account const & account_a);
	void add (oslo::account const & account_a, std::shared_ptr<oslo::json_payment_observer> payment_observer_a);
	void erase (oslo::account & account_a);

private:
	std::mutex mutex;
	std::unordered_map<oslo::account, std::shared_ptr<oslo::json_payment_observer>> payment_observers;
};
}
