#pragma once

#include <oslo/node/node_observers.hpp>

#include <string>

namespace oslo
{
class node;

enum class payment_status
{
	not_a_status,
	unknown,
	nothing, // Timeout and nothing was received
	//insufficient, // Timeout and not enough was received
	//over, // More than requested received
	//success_fork, // Amount received but it involved a fork
	success // Amount received
};
class json_payment_observer final : public std::enable_shared_from_this<oslo::json_payment_observer>
{
public:
	json_payment_observer (oslo::node &, std::function<void(std::string const &)> const &, oslo::account const &, oslo::amount const &);
	void start (uint64_t);
	void observe ();
	void complete (oslo::payment_status);
	std::mutex mutex;
	oslo::condition_variable condition;
	oslo::node & node;
	oslo::account account;
	oslo::amount amount;
	std::function<void(std::string const &)> response;
	std::atomic_flag completed;
};
}
