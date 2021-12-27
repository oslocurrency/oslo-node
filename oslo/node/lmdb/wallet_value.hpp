#pragma once

#include <oslo/lib/numbers.hpp>
#include <oslo/secure/blockstore.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace oslo
{
class wallet_value
{
public:
	wallet_value () = default;
	wallet_value (oslo::db_val<MDB_val> const &);
	wallet_value (oslo::uint256_union const &, uint64_t);
	oslo::db_val<MDB_val> val () const;
	oslo::uint256_union key;
	uint64_t work;
};
}
