#include <oslo/lib/utility.hpp>
#include <oslo/node/write_database_queue.hpp>

#include <algorithm>

oslo::write_guard::write_guard (std::function<void()> guard_finish_callback_a) :
guard_finish_callback (guard_finish_callback_a)
{
}

oslo::write_guard::write_guard (oslo::write_guard && write_guard_a) noexcept :
guard_finish_callback (std::move (write_guard_a.guard_finish_callback)),
owns (write_guard_a.owns)
{
	write_guard_a.owns = false;
	write_guard_a.guard_finish_callback = nullptr;
}

oslo::write_guard & oslo::write_guard::operator= (oslo::write_guard && write_guard_a) noexcept
{
	owns = write_guard_a.owns;
	guard_finish_callback = std::move (write_guard_a.guard_finish_callback);

	write_guard_a.owns = false;
	write_guard_a.guard_finish_callback = nullptr;
	return *this;
}

oslo::write_guard::~write_guard ()
{
	if (owns)
	{
		guard_finish_callback ();
	}
}

bool oslo::write_guard::is_owned () const
{
	return owns;
}

void oslo::write_guard::release ()
{
	debug_assert (owns);
	if (owns)
	{
		guard_finish_callback ();
	}
	owns = false;
}

oslo::write_database_queue::write_database_queue () :
guard_finish_callback ([& queue = queue, &mutex = mutex, &cv = cv]() {
	{
		oslo::lock_guard<std::mutex> guard (mutex);
		queue.pop_front ();
	}
	cv.notify_all ();
})
{
}

oslo::write_guard oslo::write_database_queue::wait (oslo::writer writer)
{
	oslo::unique_lock<std::mutex> lk (mutex);
	// Add writer to the end of the queue if it's not already waiting
	auto exists = std::find (queue.cbegin (), queue.cend (), writer) != queue.cend ();
	if (!exists)
	{
		queue.push_back (writer);
	}

	while (queue.front () != writer)
	{
		cv.wait (lk);
	}

	return write_guard (guard_finish_callback);
}

bool oslo::write_database_queue::contains (oslo::writer writer)
{
	oslo::lock_guard<std::mutex> guard (mutex);
	return std::find (queue.cbegin (), queue.cend (), writer) != queue.cend ();
}

bool oslo::write_database_queue::process (oslo::writer writer)
{
	auto result = false;
	{
		oslo::lock_guard<std::mutex> guard (mutex);
		// Add writer to the end of the queue if it's not already waiting
		auto exists = std::find (queue.cbegin (), queue.cend (), writer) != queue.cend ();
		if (!exists)
		{
			queue.push_back (writer);
		}

		result = (queue.front () == writer);
	}

	if (!result)
	{
		cv.notify_all ();
	}

	return result;
}

oslo::write_guard oslo::write_database_queue::pop ()
{
	return write_guard (guard_finish_callback);
}
