#pragma once

#include <oslo/lib/numbers.hpp>

#include <type_traits>
#include <unordered_map>

namespace oslo
{
/**
 * Tag for which epoch an entry belongs to
 */
enum class epoch : uint8_t
{
	invalid = 0,
	unspecified = 1,
	epoch_begin = 2,
	epoch_0 = 2,
	epoch_1 = 3,
	epoch_2 = 4,
	max = epoch_2
};

/* This turns epoch_0 into 0 for instance */
std::underlying_type_t<oslo::epoch> normalized_epoch (oslo::epoch epoch_a);
}
namespace std
{
template <>
struct hash<::oslo::epoch>
{
	std::size_t operator() (::oslo::epoch const & epoch_a) const
	{
		std::hash<std::underlying_type_t<::oslo::epoch>> hash;
		return hash (static_cast<std::underlying_type_t<::oslo::epoch>> (epoch_a));
	}
};
}
namespace oslo
{
class epoch_info
{
public:
	oslo::public_key signer;
	oslo::link link;
};
class epochs
{
public:
	bool is_epoch_link (oslo::link const & link_a) const;
	oslo::link const & link (oslo::epoch epoch_a) const;
	oslo::public_key const & signer (oslo::epoch epoch_a) const;
	oslo::epoch epoch (oslo::link const & link_a) const;
	void add (oslo::epoch epoch_a, oslo::public_key const & signer_a, oslo::link const & link_a);
	/** Checks that new_epoch is 1 version higher than epoch */
	static bool is_sequential (oslo::epoch epoch_a, oslo::epoch new_epoch_a);

private:
	std::unordered_map<oslo::epoch, oslo::epoch_info> epochs_m;
};
}
