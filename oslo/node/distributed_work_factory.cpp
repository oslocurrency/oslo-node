#include <oslo/node/distributed_work_factory.hpp>
#include <oslo/node/node.hpp>

oslo::distributed_work_factory::distributed_work_factory (oslo::node & node_a) :
node (node_a)
{
}

oslo::distributed_work_factory::~distributed_work_factory ()
{
	stop ();
}

bool oslo::distributed_work_factory::make (oslo::work_version const version_a, oslo::root const & root_a, std::vector<std::pair<std::string, uint16_t>> const & peers_a, uint64_t difficulty_a, std::function<void(boost::optional<uint64_t>)> const & callback_a, boost::optional<oslo::account> const & account_a)
{
	return make (std::chrono::seconds (1), oslo::work_request{ version_a, root_a, difficulty_a, account_a, callback_a, peers_a });
}

bool oslo::distributed_work_factory::make (std::chrono::seconds const & backoff_a, oslo::work_request const & request_a)
{
	bool error_l{ true };
	if (!stopped)
	{
		cleanup_finished ();
		if (node.work_generation_enabled (request_a.peers))
		{
			auto distributed (std::make_shared<oslo::distributed_work> (node, request_a, backoff_a));
			{
				oslo::lock_guard<std::mutex> guard (mutex);
				items[request_a.root].emplace_back (distributed);
			}
			distributed->start ();
			error_l = false;
		}
	}
	return error_l;
}

void oslo::distributed_work_factory::cancel (oslo::root const & root_a, bool const local_stop)
{
	oslo::lock_guard<std::mutex> guard_l (mutex);
	auto existing_l (items.find (root_a));
	if (existing_l != items.end ())
	{
		for (auto & distributed_w : existing_l->second)
		{
			if (auto distributed_l = distributed_w.lock ())
			{
				// Send work_cancel to work peers and stop local work generation
				distributed_l->cancel ();
			}
		}
		items.erase (existing_l);
	}
}

void oslo::distributed_work_factory::cleanup_finished ()
{
	oslo::lock_guard<std::mutex> guard (mutex);
	for (auto it (items.begin ()), end (items.end ()); it != end;)
	{
		it->second.erase (std::remove_if (it->second.begin (), it->second.end (), [](auto distributed_a) {
			return distributed_a.expired ();
		}),
		it->second.end ());

		if (it->second.empty ())
		{
			it = items.erase (it);
		}
		else
		{
			++it;
		}
	}
}

void oslo::distributed_work_factory::stop ()
{
	if (!stopped.exchange (true))
	{
		// Cancel any ongoing work
		std::unordered_set<oslo::root> roots_l;
		oslo::unique_lock<std::mutex> lock_l (mutex);
		for (auto const & item_l : items)
		{
			roots_l.insert (item_l.first);
		}
		lock_l.unlock ();
		for (auto const & root_l : roots_l)
		{
			cancel (root_l, true);
		}
		lock_l.lock ();
		items.clear ();
	}
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (distributed_work_factory & distributed_work, const std::string & name)
{
	size_t item_count;
	{
		oslo::lock_guard<std::mutex> guard (distributed_work.mutex);
		item_count = distributed_work.items.size ();
	}

	auto sizeof_item_element = sizeof (decltype (distributed_work.items)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "items", item_count, sizeof_item_element }));
	return composite;
}
