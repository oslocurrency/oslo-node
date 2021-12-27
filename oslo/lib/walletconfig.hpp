#pragma once

#include <oslo/lib/errors.hpp>
#include <oslo/lib/numbers.hpp>

#include <string>

namespace oslo
{
class tomlconfig;

/** Configuration options for the Qt wallet */
class wallet_config final
{
public:
	wallet_config ();
	/** Update this instance by parsing the given wallet and account */
	oslo::error parse (std::string const & wallet_a, std::string const & account_a);
	oslo::error serialize_toml (oslo::tomlconfig & toml_a) const;
	oslo::error deserialize_toml (oslo::tomlconfig & toml_a);
	oslo::wallet_id wallet;
	oslo::account account{ 0 };
};
}
