#pragma once

#include <oslo/lib/numbers.hpp>
#include <oslo/node/distributed_work.hpp>

#include <boost/optional/optional.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace oslo
{
class node;
class distributed_work;
class root;

class distributed_work_factory final
{
public:
	distributed_work_factory (oslo::node &);
	~distributed_work_factory ();
	bool make (oslo::work_version const, oslo::root const &, std::vector<std::pair<std::string, uint16_t>> const &, uint64_t, std::function<void(boost::optional<uint64_t>)> const &, boost::optional<oslo::account> const & = boost::none);
	bool make (std::chrono::seconds const &, oslo::work_request const &);
	void cancel (oslo::root const &, bool const local_stop = false);
	void cleanup_finished ();
	void stop ();

	oslo::node & node;
	std::unordered_map<oslo::root, std::vector<std::weak_ptr<oslo::distributed_work>>> items;
	std::mutex mutex;
	std::atomic<bool> stopped{ false };
};

class container_info_component;
std::unique_ptr<container_info_component> collect_container_info (distributed_work_factory & distributed_work, const std::string & name);
}
