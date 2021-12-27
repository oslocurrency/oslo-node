#pragma once

#include <oslo/lib/blocks.hpp>
#include <oslo/secure/common.hpp>

struct MDB_val;

namespace oslo
{
class account_info_v1 final
{
public:
	account_info_v1 () = default;
	explicit account_info_v1 (MDB_val const &);
	account_info_v1 (oslo::block_hash const &, oslo::block_hash const &, oslo::amount const &, uint64_t);
	oslo::block_hash head{ 0 };
	oslo::block_hash rep_block{ 0 };
	oslo::amount balance{ 0 };
	uint64_t modified{ 0 };
};
class pending_info_v3 final
{
public:
	pending_info_v3 () = default;
	explicit pending_info_v3 (MDB_val const &);
	pending_info_v3 (oslo::account const &, oslo::amount const &, oslo::account const &);
	oslo::account source{ 0 };
	oslo::amount amount{ 0 };
	oslo::account destination{ 0 };
};
class pending_info_v14 final
{
public:
	pending_info_v14 () = default;
	pending_info_v14 (oslo::account const &, oslo::amount const &, oslo::epoch);
	size_t db_size () const;
	bool deserialize (oslo::stream &);
	bool operator== (oslo::pending_info_v14 const &) const;
	oslo::account source{ 0 };
	oslo::amount amount{ 0 };
	oslo::epoch epoch{ oslo::epoch::epoch_0 };
};
class account_info_v5 final
{
public:
	account_info_v5 () = default;
	explicit account_info_v5 (MDB_val const &);
	account_info_v5 (oslo::block_hash const &, oslo::block_hash const &, oslo::block_hash const &, oslo::amount const &, uint64_t);
	oslo::block_hash head{ 0 };
	oslo::block_hash rep_block{ 0 };
	oslo::block_hash open_block{ 0 };
	oslo::amount balance{ 0 };
	uint64_t modified{ 0 };
};
class account_info_v13 final
{
public:
	account_info_v13 () = default;
	account_info_v13 (oslo::block_hash const &, oslo::block_hash const &, oslo::block_hash const &, oslo::amount const &, uint64_t, uint64_t, oslo::epoch);
	size_t db_size () const;
	oslo::block_hash head{ 0 };
	oslo::block_hash rep_block{ 0 };
	oslo::block_hash open_block{ 0 };
	oslo::amount balance{ 0 };
	uint64_t modified{ 0 };
	uint64_t block_count{ 0 };
	oslo::epoch epoch{ oslo::epoch::epoch_0 };
};
class account_info_v14 final
{
public:
	account_info_v14 () = default;
	account_info_v14 (oslo::block_hash const &, oslo::block_hash const &, oslo::block_hash const &, oslo::amount const &, uint64_t, uint64_t, uint64_t, oslo::epoch);
	size_t db_size () const;
	oslo::block_hash head{ 0 };
	oslo::block_hash rep_block{ 0 };
	oslo::block_hash open_block{ 0 };
	oslo::amount balance{ 0 };
	uint64_t modified{ 0 };
	uint64_t block_count{ 0 };
	uint64_t confirmation_height{ 0 };
	oslo::epoch epoch{ oslo::epoch::epoch_0 };
};
class block_sideband_v14 final
{
public:
	block_sideband_v14 () = default;
	block_sideband_v14 (oslo::block_type, oslo::account const &, oslo::block_hash const &, oslo::amount const &, uint64_t, uint64_t);
	void serialize (oslo::stream &) const;
	bool deserialize (oslo::stream &);
	static size_t size (oslo::block_type);
	oslo::block_type type{ oslo::block_type::invalid };
	oslo::block_hash successor{ 0 };
	oslo::account account{ 0 };
	oslo::amount balance{ 0 };
	uint64_t height{ 0 };
	uint64_t timestamp{ 0 };
};
class state_block_w_sideband_v14
{
public:
	std::shared_ptr<oslo::state_block> state_block;
	oslo::block_sideband_v14 sideband;
};
}
