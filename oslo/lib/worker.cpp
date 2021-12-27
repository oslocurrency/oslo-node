#include <oslo/lib/threading.hpp>
#include <oslo/lib/worker.hpp>

oslo::worker::worker () :
thread ([this]() {
	oslo::thread_role::set (oslo::thread_role::name::worker);
	this->run ();
})
{
}

void oslo::worker::run ()
{
	oslo::unique_lock<std::mutex> lk (mutex);
	while (!stopped)
	{
		if (!queue.empty ())
		{
			auto func = queue.front ();
			queue.pop_front ();
			lk.unlock ();
			func ();
			// So that we reduce locking for anything being pushed as that will
			// most likely be on an io-thread
			std::this_thread::yield ();
			lk.lock ();
		}
		else
		{
			cv.wait (lk);
		}
	}
}

oslo::worker::~worker ()
{
	stop ();
}

void oslo::worker::push_task (std::function<void()> func_a)
{
	{
		oslo::lock_guard<std::mutex> guard (mutex);
		if (!stopped)
		{
			queue.emplace_back (func_a);
		}
	}

	cv.notify_one ();
}

void oslo::worker::stop ()
{
	{
		oslo::unique_lock<std::mutex> lk (mutex);
		stopped = true;
		queue.clear ();
	}
	cv.notify_one ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (oslo::worker & worker, const std::string & name)
{
	size_t count;
	{
		oslo::lock_guard<std::mutex> guard (worker.mutex);
		count = worker.queue.size ();
	}
	auto sizeof_element = sizeof (decltype (worker.queue)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<oslo::container_info_leaf> (oslo::container_info{ "queue", count, sizeof_element }));
	return composite;
}
