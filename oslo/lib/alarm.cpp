#include <oslo/lib/alarm.hpp>
#include <oslo/lib/threading.hpp>

bool oslo::operation::operator> (oslo::operation const & other_a) const
{
	return wakeup > other_a.wakeup;
}

oslo::alarm::alarm (boost::asio::io_context & io_ctx_a) :
io_ctx (io_ctx_a),
thread ([this]() {
	oslo::thread_role::set (oslo::thread_role::name::alarm);
	run ();
})
{
}

oslo::alarm::~alarm ()
{
	add (std::chrono::steady_clock::now (), nullptr);
	thread.join ();
}

void oslo::alarm::run ()
{
	oslo::unique_lock<std::mutex> lock (mutex);
	auto done (false);
	while (!done)
	{
		if (!operations.empty ())
		{
			auto & operation (operations.top ());
			if (operation.function)
			{
				if (operation.wakeup <= std::chrono::steady_clock::now ())
				{
					io_ctx.post (operation.function);
					operations.pop ();
				}
				else
				{
					auto wakeup (operation.wakeup);
					condition.wait_until (lock, wakeup);
				}
			}
			else
			{
				done = true;
			}
		}
		else
		{
			condition.wait (lock);
		}
	}
}

void oslo::alarm::add (std::chrono::steady_clock::time_point const & wakeup_a, std::function<void()> const & operation)
{
	{
		oslo::lock_guard<std::mutex> guard (mutex);
		operations.push (oslo::operation ({ wakeup_a, operation }));
	}
	condition.notify_all ();
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (alarm & alarm, const std::string & name)
{
	size_t count;
	{
		oslo::lock_guard<std::mutex> guard (alarm.mutex);
		count = alarm.operations.size ();
	}
	auto sizeof_element = sizeof (decltype (alarm.operations)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "operations", count, sizeof_element }));
	return composite;
}
