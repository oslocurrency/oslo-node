#include <oslo/node/node_observers.hpp>

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (oslo::node_observers & node_observers, const std::string & name)
{
	auto composite = std::make_unique<oslo::container_info_composite> (name);
	composite->add_component (collect_container_info (node_observers.blocks, "blocks"));
	composite->add_component (collect_container_info (node_observers.wallet, "wallet"));
	composite->add_component (collect_container_info (node_observers.vote, "vote"));
	composite->add_component (collect_container_info (node_observers.active_stopped, "active_stopped"));
	composite->add_component (collect_container_info (node_observers.account_balance, "account_balance"));
	composite->add_component (collect_container_info (node_observers.endpoint, "endpoint"));
	composite->add_component (collect_container_info (node_observers.disconnect, "disconnect"));
	composite->add_component (collect_container_info (node_observers.work_cancel, "work_cancel"));
	return composite;
}
