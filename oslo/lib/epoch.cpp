#include <oslo/lib/epoch.hpp>
#include <oslo/lib/utility.hpp>

oslo::link const & oslo::epochs::link (oslo::epoch epoch_a) const
{
	return epochs_m.at (epoch_a).link;
}

bool oslo::epochs::is_epoch_link (oslo::link const & link_a) const
{
	return std::any_of (epochs_m.begin (), epochs_m.end (), [&link_a](auto const & item_a) { return item_a.second.link == link_a; });
}

oslo::public_key const & oslo::epochs::signer (oslo::epoch epoch_a) const
{
	return epochs_m.at (epoch_a).signer;
}

oslo::epoch oslo::epochs::epoch (oslo::link const & link_a) const
{
	auto existing (std::find_if (epochs_m.begin (), epochs_m.end (), [&link_a](auto const & item_a) { return item_a.second.link == link_a; }));
	debug_assert (existing != epochs_m.end ());
	return existing->first;
}

void oslo::epochs::add (oslo::epoch epoch_a, oslo::public_key const & signer_a, oslo::link const & link_a)
{
	debug_assert (epochs_m.find (epoch_a) == epochs_m.end ());
	epochs_m[epoch_a] = { signer_a, link_a };
}

bool oslo::epochs::is_sequential (oslo::epoch epoch_a, oslo::epoch new_epoch_a)
{
	auto head_epoch = std::underlying_type_t<oslo::epoch> (epoch_a);
	bool is_valid_epoch (head_epoch >= std::underlying_type_t<oslo::epoch> (oslo::epoch::epoch_0));
	return is_valid_epoch && (std::underlying_type_t<oslo::epoch> (new_epoch_a) == (head_epoch + 1));
}

std::underlying_type_t<oslo::epoch> oslo::normalized_epoch (oslo::epoch epoch_a)
{
	// Currently assumes that the epoch versions in the enum are sequential.
	auto start = std::underlying_type_t<oslo::epoch> (oslo::epoch::epoch_0);
	auto end = std::underlying_type_t<oslo::epoch> (epoch_a);
	debug_assert (end >= start);
	return end - start;
}
