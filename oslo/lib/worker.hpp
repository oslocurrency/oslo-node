#pragma once

#include <oslo/lib/locks.hpp>
#include <oslo/lib/utility.hpp>

#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace oslo
{
class worker final
{
public:
	worker ();
	~worker ();
	void run ();
	void push_task (std::function<void()> func);
	void stop ();

private:
	oslo::condition_variable cv;
	std::deque<std::function<void()>> queue;
	std::mutex mutex;
	bool stopped{ false };
	std::thread thread;

	friend std::unique_ptr<container_info_component> collect_container_info (worker &, const std::string &);
};

std::unique_ptr<container_info_component> collect_container_info (worker & worker, const std::string & name);
}