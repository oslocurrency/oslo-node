#include <oslo/crypto_lib/random_pool.hpp>
#include <oslo/lib/locks.hpp>
#include <oslo/secure/buffer.hpp>
#include <oslo/secure/common.hpp>
#include <oslo/secure/network_filter.hpp>

oslo::network_filter::network_filter (size_t size_a) :
items (size_a, oslo::uint128_t{ 0 })
{
	oslo::random_pool::generate_block (key, key.size ());
}

bool oslo::network_filter::apply (uint8_t const * bytes_a, size_t count_a, oslo::uint128_t * digest_a)
{
	// Get hash before locking
	auto digest (hash (bytes_a, count_a));

	oslo::lock_guard<std::mutex> lock (mutex);
	auto & element (get_element (digest));
	bool existed (element == digest);
	if (!existed)
	{
		// Replace likely old element with a new one
		element = digest;
	}
	if (digest_a)
	{
		*digest_a = digest;
	}
	return existed;
}

void oslo::network_filter::clear (oslo::uint128_t const & digest_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	auto & element (get_element (digest_a));
	if (element == digest_a)
	{
		element = oslo::uint128_t{ 0 };
	}
}

void oslo::network_filter::clear (std::vector<oslo::uint128_t> const & digests_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	for (auto const & digest : digests_a)
	{
		auto & element (get_element (digest));
		if (element == digest)
		{
			element = oslo::uint128_t{ 0 };
		}
	}
}

void oslo::network_filter::clear (uint8_t const * bytes_a, size_t count_a)
{
	clear (hash (bytes_a, count_a));
}

template <typename OBJECT>
void oslo::network_filter::clear (OBJECT const & object_a)
{
	clear (hash (object_a));
}

void oslo::network_filter::clear ()
{
	oslo::lock_guard<std::mutex> lock (mutex);
	items.assign (items.size (), oslo::uint128_t{ 0 });
}

template <typename OBJECT>
oslo::uint128_t oslo::network_filter::hash (OBJECT const & object_a) const
{
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		object_a->serialize (stream);
	}
	return hash (bytes.data (), bytes.size ());
}

oslo::uint128_t & oslo::network_filter::get_element (oslo::uint128_t const & hash_a)
{
	debug_assert (!mutex.try_lock ());
	debug_assert (items.size () > 0);
	size_t index (hash_a % items.size ());
	return items[index];
}

oslo::uint128_t oslo::network_filter::hash (uint8_t const * bytes_a, size_t count_a) const
{
	oslo::uint128_union digest{ 0 };
	siphash_t siphash (key, static_cast<unsigned int> (key.size ()));
	siphash.CalculateDigest (digest.bytes.data (), bytes_a, count_a);
	return digest.number ();
}

// Explicitly instantiate
template oslo::uint128_t oslo::network_filter::hash (std::shared_ptr<oslo::block> const &) const;
template void oslo::network_filter::clear (std::shared_ptr<oslo::block> const &);
