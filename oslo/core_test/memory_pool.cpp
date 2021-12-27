#include <oslo/lib/memory.hpp>
#include <oslo/secure/common.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace
{
/** This allocator records the size of all allocations that happen */
template <class T>
class record_allocations_new_delete_allocator
{
public:
	using value_type = T;

	explicit record_allocations_new_delete_allocator (std::vector<size_t> * allocated) :
	allocated (allocated)
	{
	}

	template <typename U>
	record_allocations_new_delete_allocator (const record_allocations_new_delete_allocator<U> & a)
	{
		allocated = a.allocated;
	}

	template <typename U>
	record_allocations_new_delete_allocator & operator= (const record_allocations_new_delete_allocator<U> &) = delete;

	T * allocate (size_t num_to_allocate)
	{
		auto size_allocated = (sizeof (T) * num_to_allocate);
		allocated->push_back (size_allocated);
		return static_cast<T *> (::operator new (size_allocated));
	}

	void deallocate (T * p, size_t num_to_deallocate)
	{
		::operator delete (p);
	}

	std::vector<size_t> * allocated;
};

template <typename T>
size_t get_allocated_size ()
{
	std::vector<size_t> allocated;
	record_allocations_new_delete_allocator<T> alloc (&allocated);
	(void)std::allocate_shared<T, record_allocations_new_delete_allocator<T>> (alloc);
	debug_assert (allocated.size () == 1);
	return allocated.front ();
}
}

TEST (memory_pool, validate_cleanup)
{
	// This might be turned off, e.g on Mac for instance, so don't do this test
	if (!oslo::get_use_memory_pools ())
	{
		return;
	}

	oslo::make_shared<oslo::open_block> ();
	oslo::make_shared<oslo::receive_block> ();
	oslo::make_shared<oslo::send_block> ();
	oslo::make_shared<oslo::change_block> ();
	oslo::make_shared<oslo::state_block> ();
	oslo::make_shared<oslo::vote> ();

	ASSERT_TRUE (oslo::purge_singleton_pool_memory<oslo::open_block> ());
	ASSERT_TRUE (oslo::purge_singleton_pool_memory<oslo::receive_block> ());
	ASSERT_TRUE (oslo::purge_singleton_pool_memory<oslo::send_block> ());
	ASSERT_TRUE (oslo::purge_singleton_pool_memory<oslo::state_block> ());
	ASSERT_TRUE (oslo::purge_singleton_pool_memory<oslo::vote> ());

	// Change blocks have the same size as open_block so won't deallocate any memory
	ASSERT_FALSE (oslo::purge_singleton_pool_memory<oslo::change_block> ());

	ASSERT_EQ (oslo::determine_shared_ptr_pool_size<oslo::open_block> (), get_allocated_size<oslo::open_block> () - sizeof (size_t));
	ASSERT_EQ (oslo::determine_shared_ptr_pool_size<oslo::receive_block> (), get_allocated_size<oslo::receive_block> () - sizeof (size_t));
	ASSERT_EQ (oslo::determine_shared_ptr_pool_size<oslo::send_block> (), get_allocated_size<oslo::send_block> () - sizeof (size_t));
	ASSERT_EQ (oslo::determine_shared_ptr_pool_size<oslo::change_block> (), get_allocated_size<oslo::change_block> () - sizeof (size_t));
	ASSERT_EQ (oslo::determine_shared_ptr_pool_size<oslo::state_block> (), get_allocated_size<oslo::state_block> () - sizeof (size_t));
	ASSERT_EQ (oslo::determine_shared_ptr_pool_size<oslo::vote> (), get_allocated_size<oslo::vote> () - sizeof (size_t));
}
