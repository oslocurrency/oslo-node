#pragma once

#include <oslo/crypto_lib/random_pool.hpp>

#include <crypto/cryptopp/osrng.h>

namespace oslo
{
template <class Iter>
void random_pool_shuffle (Iter begin, Iter end)
{
	std::lock_guard<std::mutex> guard (random_pool::mutex);
	random_pool::get_pool ().Shuffle (begin, end);
}
}
