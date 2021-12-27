#pragma once

#include <oslo/crypto_lib/random_pool.hpp>
#include <oslo/lib/diagnosticsconfig.hpp>
#include <oslo/lib/lmdbconfig.hpp>
#include <oslo/lib/logger_mt.hpp>
#include <oslo/lib/memory.hpp>
#include <oslo/lib/rocksdbconfig.hpp>
#include <oslo/secure/buffer.hpp>
#include <oslo/secure/common.hpp>
#include <oslo/secure/versioning.hpp>

#include <boost/endian/conversion.hpp>
#include <boost/polymorphic_cast.hpp>

#include <stack>

namespace oslo
{
// Move to versioning with a specific version if required for a future upgrade
class state_block_w_sideband
{
public:
	std::shared_ptr<oslo::state_block> state_block;
	oslo::block_sideband sideband;
};

/**
 * Encapsulates database specific container
 */
template <typename Val>
class db_val
{
public:
	db_val (Val const & value_a) :
	value (value_a)
	{
	}

	db_val () :
	db_val (0, nullptr)
	{
	}

	db_val (oslo::uint128_union const & val_a) :
	db_val (sizeof (val_a), const_cast<oslo::uint128_union *> (&val_a))
	{
	}

	db_val (oslo::uint256_union const & val_a) :
	db_val (sizeof (val_a), const_cast<oslo::uint256_union *> (&val_a))
	{
	}

	db_val (oslo::account_info const & val_a) :
	db_val (val_a.db_size (), const_cast<oslo::account_info *> (&val_a))
	{
	}

	db_val (oslo::account_info_v13 const & val_a) :
	db_val (val_a.db_size (), const_cast<oslo::account_info_v13 *> (&val_a))
	{
	}

	db_val (oslo::account_info_v14 const & val_a) :
	db_val (val_a.db_size (), const_cast<oslo::account_info_v14 *> (&val_a))
	{
	}

	db_val (oslo::pending_info const & val_a) :
	db_val (val_a.db_size (), const_cast<oslo::pending_info *> (&val_a))
	{
		static_assert (std::is_standard_layout<oslo::pending_info>::value, "Standard layout is required");
	}

	db_val (oslo::pending_info_v14 const & val_a) :
	db_val (val_a.db_size (), const_cast<oslo::pending_info_v14 *> (&val_a))
	{
		static_assert (std::is_standard_layout<oslo::pending_info_v14>::value, "Standard layout is required");
	}

	db_val (oslo::pending_key const & val_a) :
	db_val (sizeof (val_a), const_cast<oslo::pending_key *> (&val_a))
	{
		static_assert (std::is_standard_layout<oslo::pending_key>::value, "Standard layout is required");
	}

	db_val (oslo::unchecked_info const & val_a) :
	buffer (std::make_shared<std::vector<uint8_t>> ())
	{
		{
			oslo::vectorstream stream (*buffer);
			val_a.serialize (stream);
		}
		convert_buffer_to_value ();
	}

	db_val (oslo::unchecked_key const & val_a) :
	db_val (sizeof (val_a), const_cast<oslo::unchecked_key *> (&val_a))
	{
		static_assert (std::is_standard_layout<oslo::unchecked_key>::value, "Standard layout is required");
	}

	db_val (oslo::confirmation_height_info const & val_a) :
	buffer (std::make_shared<std::vector<uint8_t>> ())
	{
		{
			oslo::vectorstream stream (*buffer);
			val_a.serialize (stream);
		}
		convert_buffer_to_value ();
	}

	db_val (oslo::block_info const & val_a) :
	db_val (sizeof (val_a), const_cast<oslo::block_info *> (&val_a))
	{
		static_assert (std::is_standard_layout<oslo::block_info>::value, "Standard layout is required");
	}

	db_val (oslo::endpoint_key const & val_a) :
	db_val (sizeof (val_a), const_cast<oslo::endpoint_key *> (&val_a))
	{
		static_assert (std::is_standard_layout<oslo::endpoint_key>::value, "Standard layout is required");
	}

	db_val (std::shared_ptr<oslo::block> const & val_a) :
	buffer (std::make_shared<std::vector<uint8_t>> ())
	{
		{
			oslo::vectorstream stream (*buffer);
			oslo::serialize_block (stream, *val_a);
		}
		convert_buffer_to_value ();
	}

	db_val (uint64_t val_a) :
	buffer (std::make_shared<std::vector<uint8_t>> ())
	{
		{
			boost::endian::native_to_big_inplace (val_a);
			oslo::vectorstream stream (*buffer);
			oslo::write (stream, val_a);
		}
		convert_buffer_to_value ();
	}

	explicit operator oslo::account_info () const
	{
		oslo::account_info result;
		debug_assert (size () == result.db_size ());
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + result.db_size (), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator oslo::account_info_v13 () const
	{
		oslo::account_info_v13 result;
		debug_assert (size () == result.db_size ());
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + result.db_size (), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator oslo::account_info_v14 () const
	{
		oslo::account_info_v14 result;
		debug_assert (size () == result.db_size ());
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + result.db_size (), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator oslo::block_info () const
	{
		oslo::block_info result;
		debug_assert (size () == sizeof (result));
		static_assert (sizeof (oslo::block_info::account) + sizeof (oslo::block_info::balance) == sizeof (result), "Packed class");
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator oslo::pending_info_v14 () const
	{
		oslo::pending_info_v14 result;
		debug_assert (size () == result.db_size ());
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + result.db_size (), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator oslo::pending_info () const
	{
		oslo::pending_info result;
		debug_assert (size () == result.db_size ());
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + result.db_size (), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator oslo::pending_key () const
	{
		oslo::pending_key result;
		debug_assert (size () == sizeof (result));
		static_assert (sizeof (oslo::pending_key::account) + sizeof (oslo::pending_key::hash) == sizeof (result), "Packed class");
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator oslo::confirmation_height_info () const
	{
		oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		oslo::confirmation_height_info result;
		bool error (result.deserialize (stream));
		(void)error;
		debug_assert (!error);
		return result;
	}

	explicit operator oslo::unchecked_info () const
	{
		oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		oslo::unchecked_info result;
		bool error (result.deserialize (stream));
		(void)error;
		debug_assert (!error);
		return result;
	}

	explicit operator oslo::unchecked_key () const
	{
		oslo::unchecked_key result;
		debug_assert (size () == sizeof (result));
		static_assert (sizeof (oslo::unchecked_key::previous) + sizeof (oslo::pending_key::hash) == sizeof (result), "Packed class");
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator oslo::uint128_union () const
	{
		return convert<oslo::uint128_union> ();
	}

	explicit operator oslo::amount () const
	{
		return convert<oslo::amount> ();
	}

	explicit operator oslo::block_hash () const
	{
		return convert<oslo::block_hash> ();
	}

	explicit operator oslo::public_key () const
	{
		return convert<oslo::public_key> ();
	}

	explicit operator oslo::uint256_union () const
	{
		return convert<oslo::uint256_union> ();
	}

	explicit operator std::array<char, 64> () const
	{
		oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		std::array<char, 64> result;
		auto error = oslo::try_read (stream, result);
		(void)error;
		debug_assert (!error);
		return result;
	}

	explicit operator oslo::endpoint_key () const
	{
		oslo::endpoint_key result;
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator state_block_w_sideband () const
	{
		oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (false);
		oslo::state_block_w_sideband block_w_sideband;
		block_w_sideband.state_block = std::make_shared<oslo::state_block> (error, stream);
		debug_assert (!error);

		error = block_w_sideband.sideband.deserialize (stream, oslo::block_type::state);
		debug_assert (!error);

		return block_w_sideband;
	}

	explicit operator state_block_w_sideband_v14 () const
	{
		oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (false);
		oslo::state_block_w_sideband_v14 block_w_sideband;
		block_w_sideband.state_block = std::make_shared<oslo::state_block> (error, stream);
		debug_assert (!error);

		block_w_sideband.sideband.type = oslo::block_type::state;
		error = block_w_sideband.sideband.deserialize (stream);
		debug_assert (!error);

		return block_w_sideband;
	}

	explicit operator oslo::no_value () const
	{
		return no_value::dummy;
	}

	explicit operator std::shared_ptr<oslo::block> () const
	{
		oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		std::shared_ptr<oslo::block> result (oslo::deserialize_block (stream));
		return result;
	}

	template <typename Block>
	std::shared_ptr<Block> convert_to_block () const
	{
		oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (false);
		auto result (std::make_shared<Block> (error, stream));
		debug_assert (!error);
		return result;
	}

	explicit operator std::shared_ptr<oslo::send_block> () const
	{
		return convert_to_block<oslo::send_block> ();
	}

	explicit operator std::shared_ptr<oslo::receive_block> () const
	{
		return convert_to_block<oslo::receive_block> ();
	}

	explicit operator std::shared_ptr<oslo::open_block> () const
	{
		return convert_to_block<oslo::open_block> ();
	}

	explicit operator std::shared_ptr<oslo::change_block> () const
	{
		return convert_to_block<oslo::change_block> ();
	}

	explicit operator std::shared_ptr<oslo::state_block> () const
	{
		return convert_to_block<oslo::state_block> ();
	}

	explicit operator std::shared_ptr<oslo::vote> () const
	{
		oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (false);
		auto result (oslo::make_shared<oslo::vote> (error, stream));
		debug_assert (!error);
		return result;
	}

	explicit operator uint64_t () const
	{
		uint64_t result;
		oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (oslo::try_read (stream, result));
		(void)error;
		debug_assert (!error);
		boost::endian::big_to_native_inplace (result);
		return result;
	}

	operator Val * () const
	{
		// Allow passing a temporary to a non-c++ function which doesn't have constness
		return const_cast<Val *> (&value);
	}

	operator Val const & () const
	{
		return value;
	}

	// Must be specialized
	void * data () const;
	size_t size () const;
	db_val (size_t size_a, void * data_a);
	void convert_buffer_to_value ();

	Val value;
	std::shared_ptr<std::vector<uint8_t>> buffer;

private:
	template <typename T>
	T convert () const
	{
		T result;
		debug_assert (size () == sizeof (result));
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), result.bytes.data ());
		return result;
	}
};

class transaction;
class block_store;

/**
 * Summation visitor for blocks, supporting amount and balance computations. These
 * computations are mutually dependant. The natural solution is to use mutual recursion
 * between balance and amount visitors, but this leads to very deep stacks. Hence, the
 * summation visitor uses an iterative approach.
 */
class summation_visitor final : public oslo::block_visitor
{
	enum summation_type
	{
		invalid = 0,
		balance = 1,
		amount = 2
	};

	/** Represents an invocation frame */
	class frame final
	{
	public:
		frame (summation_type type_a, oslo::block_hash balance_hash_a, oslo::block_hash amount_hash_a) :
		type (type_a), balance_hash (balance_hash_a), amount_hash (amount_hash_a)
		{
		}

		/** The summation type guides the block visitor handlers */
		summation_type type{ invalid };
		/** Accumulated balance or amount */
		oslo::uint128_t sum{ 0 };
		/** The current balance hash */
		oslo::block_hash balance_hash{ 0 };
		/** The current amount hash */
		oslo::block_hash amount_hash{ 0 };
		/** If true, this frame is awaiting an invocation result */
		bool awaiting_result{ false };
		/** Set by the invoked frame, representing the return value */
		oslo::uint128_t incoming_result{ 0 };
	};

public:
	summation_visitor (oslo::transaction const &, oslo::block_store const &, bool is_v14_upgrade = false);
	virtual ~summation_visitor () = default;
	/** Computes the balance as of \p block_hash */
	oslo::uint128_t compute_balance (oslo::block_hash const & block_hash);
	/** Computes the amount delta between \p block_hash and its predecessor */
	oslo::uint128_t compute_amount (oslo::block_hash const & block_hash);

protected:
	oslo::transaction const & transaction;
	oslo::block_store const & store;
	oslo::network_params network_params;

	/** The final result */
	oslo::uint128_t result{ 0 };
	/** The current invocation frame */
	frame * current{ nullptr };
	/** Invocation frames */
	std::stack<frame> frames;
	/** Push a copy of \p hash of the given summation \p type */
	oslo::summation_visitor::frame push (oslo::summation_visitor::summation_type type, oslo::block_hash const & hash);
	void sum_add (oslo::uint128_t addend_a);
	void sum_set (oslo::uint128_t value_a);
	/** The epilogue yields the result to previous frame, if any */
	void epilogue ();

	oslo::uint128_t compute_internal (oslo::summation_visitor::summation_type type, oslo::block_hash const &);
	void send_block (oslo::send_block const &) override;
	void receive_block (oslo::receive_block const &) override;
	void open_block (oslo::open_block const &) override;
	void change_block (oslo::change_block const &) override;
	void state_block (oslo::state_block const &) override;

private:
	bool is_v14_upgrade;
	std::shared_ptr<oslo::block> block_get (oslo::transaction const &, oslo::block_hash const &) const;
};

/**
 * Determine the representative for this block
 */
class representative_visitor final : public oslo::block_visitor
{
public:
	representative_visitor (oslo::transaction const & transaction_a, oslo::block_store & store_a);
	~representative_visitor () = default;
	void compute (oslo::block_hash const & hash_a);
	void send_block (oslo::send_block const & block_a) override;
	void receive_block (oslo::receive_block const & block_a) override;
	void open_block (oslo::open_block const & block_a) override;
	void change_block (oslo::change_block const & block_a) override;
	void state_block (oslo::state_block const & block_a) override;
	oslo::transaction const & transaction;
	oslo::block_store & store;
	oslo::block_hash current;
	oslo::block_hash result;
};
template <typename T, typename U>
class store_iterator_impl
{
public:
	virtual ~store_iterator_impl () = default;
	virtual oslo::store_iterator_impl<T, U> & operator++ () = 0;
	virtual bool operator== (oslo::store_iterator_impl<T, U> const & other_a) const = 0;
	virtual bool is_end_sentinal () const = 0;
	virtual void fill (std::pair<T, U> &) const = 0;
	oslo::store_iterator_impl<T, U> & operator= (oslo::store_iterator_impl<T, U> const &) = delete;
	bool operator== (oslo::store_iterator_impl<T, U> const * other_a) const
	{
		return (other_a != nullptr && *this == *other_a) || (other_a == nullptr && is_end_sentinal ());
	}
	bool operator!= (oslo::store_iterator_impl<T, U> const & other_a) const
	{
		return !(*this == other_a);
	}
};
/**
 * Iterates the key/value pairs of a transaction
 */
template <typename T, typename U>
class store_iterator final
{
public:
	store_iterator (std::nullptr_t)
	{
	}
	store_iterator (std::unique_ptr<oslo::store_iterator_impl<T, U>> impl_a) :
	impl (std::move (impl_a))
	{
		impl->fill (current);
	}
	store_iterator (oslo::store_iterator<T, U> && other_a) :
	current (std::move (other_a.current)),
	impl (std::move (other_a.impl))
	{
	}
	oslo::store_iterator<T, U> & operator++ ()
	{
		++*impl;
		impl->fill (current);
		return *this;
	}
	oslo::store_iterator<T, U> & operator= (oslo::store_iterator<T, U> && other_a) noexcept
	{
		impl = std::move (other_a.impl);
		current = std::move (other_a.current);
		return *this;
	}
	oslo::store_iterator<T, U> & operator= (oslo::store_iterator<T, U> const &) = delete;
	std::pair<T, U> * operator-> ()
	{
		return &current;
	}
	bool operator== (oslo::store_iterator<T, U> const & other_a) const
	{
		return (impl == nullptr && other_a.impl == nullptr) || (impl != nullptr && *impl == other_a.impl.get ()) || (other_a.impl != nullptr && *other_a.impl == impl.get ());
	}
	bool operator!= (oslo::store_iterator<T, U> const & other_a) const
	{
		return !(*this == other_a);
	}

private:
	std::pair<T, U> current;
	std::unique_ptr<oslo::store_iterator_impl<T, U>> impl;
};

// Keep this in alphabetical order
enum class tables
{
	accounts,
	blocks_info, // LMDB only
	cached_counts, // RocksDB only
	change_blocks,
	confirmation_height,
	frontiers,
	meta,
	online_weight,
	open_blocks,
	peers,
	pending,
	receive_blocks,
	representation,
	send_blocks,
	state_blocks,
	unchecked,
	vote
};

class transaction_impl
{
public:
	virtual ~transaction_impl () = default;
	virtual void * get_handle () const = 0;
};

class read_transaction_impl : public transaction_impl
{
public:
	virtual void reset () = 0;
	virtual void renew () = 0;
};

class write_transaction_impl : public transaction_impl
{
public:
	virtual void commit () const = 0;
	virtual void renew () = 0;
	virtual bool contains (oslo::tables table_a) const = 0;
};

class transaction
{
public:
	virtual ~transaction () = default;
	virtual void * get_handle () const = 0;
};

/**
 * RAII wrapper of a read MDB_txn where the constructor starts the transaction
 * and the destructor aborts it.
 */
class read_transaction final : public transaction
{
public:
	explicit read_transaction (std::unique_ptr<oslo::read_transaction_impl> read_transaction_impl);
	void * get_handle () const override;
	void reset () const;
	void renew () const;
	void refresh () const;

private:
	std::unique_ptr<oslo::read_transaction_impl> impl;
};

/**
 * RAII wrapper of a read-write MDB_txn where the constructor starts the transaction
 * and the destructor commits it.
 */
class write_transaction final : public transaction
{
public:
	explicit write_transaction (std::unique_ptr<oslo::write_transaction_impl> write_transaction_impl);
	void * get_handle () const override;
	void commit () const;
	void renew ();
	bool contains (oslo::tables table_a) const;

private:
	std::unique_ptr<oslo::write_transaction_impl> impl;
};

class ledger_cache;

/**
 * Manages block storage and iteration
 */
class block_store
{
public:
	virtual ~block_store () = default;
	virtual void initialize (oslo::write_transaction const &, oslo::genesis const &, oslo::ledger_cache &) = 0;
	virtual void block_put (oslo::write_transaction const &, oslo::block_hash const &, oslo::block const &) = 0;
	virtual oslo::block_hash block_successor (oslo::transaction const &, oslo::block_hash const &) const = 0;
	virtual void block_successor_clear (oslo::write_transaction const &, oslo::block_hash const &) = 0;
	virtual std::shared_ptr<oslo::block> block_get (oslo::transaction const &, oslo::block_hash const &) const = 0;
	virtual std::shared_ptr<oslo::block> block_get_no_sideband (oslo::transaction const &, oslo::block_hash const &) const = 0;
	virtual std::shared_ptr<oslo::block> block_get_v14 (oslo::transaction const &, oslo::block_hash const &, oslo::block_sideband_v14 * = nullptr, bool * = nullptr) const = 0;
	virtual std::shared_ptr<oslo::block> block_random (oslo::transaction const &) = 0;
	virtual void block_del (oslo::write_transaction const &, oslo::block_hash const &, oslo::block_type) = 0;
	virtual bool block_exists (oslo::transaction const &, oslo::block_hash const &) = 0;
	virtual bool block_exists (oslo::transaction const &, oslo::block_type, oslo::block_hash const &) = 0;
	virtual oslo::block_counts block_count (oslo::transaction const &) = 0;
	virtual bool root_exists (oslo::transaction const &, oslo::root const &) = 0;
	virtual bool source_exists (oslo::transaction const &, oslo::block_hash const &) = 0;
	virtual oslo::account block_account (oslo::transaction const &, oslo::block_hash const &) const = 0;
	virtual oslo::account block_account_calculated (oslo::block const &) const = 0;

	virtual void frontier_put (oslo::write_transaction const &, oslo::block_hash const &, oslo::account const &) = 0;
	virtual oslo::account frontier_get (oslo::transaction const &, oslo::block_hash const &) const = 0;
	virtual void frontier_del (oslo::write_transaction const &, oslo::block_hash const &) = 0;

	virtual void account_put (oslo::write_transaction const &, oslo::account const &, oslo::account_info const &) = 0;
	virtual bool account_get (oslo::transaction const &, oslo::account const &, oslo::account_info &) = 0;
	virtual void account_del (oslo::write_transaction const &, oslo::account const &) = 0;
	virtual bool account_exists (oslo::transaction const &, oslo::account const &) = 0;
	virtual size_t account_count (oslo::transaction const &) = 0;
	virtual void confirmation_height_clear (oslo::write_transaction const &, oslo::account const &, uint64_t) = 0;
	virtual void confirmation_height_clear (oslo::write_transaction const &) = 0;
	virtual oslo::store_iterator<oslo::account, oslo::account_info> latest_begin (oslo::transaction const &, oslo::account const &) const = 0;
	virtual oslo::store_iterator<oslo::account, oslo::account_info> latest_begin (oslo::transaction const &) const = 0;
	virtual oslo::store_iterator<oslo::account, oslo::account_info> latest_end () const = 0;

	virtual void pending_put (oslo::write_transaction const &, oslo::pending_key const &, oslo::pending_info const &) = 0;
	virtual void pending_del (oslo::write_transaction const &, oslo::pending_key const &) = 0;
	virtual bool pending_get (oslo::transaction const &, oslo::pending_key const &, oslo::pending_info &) = 0;
	virtual bool pending_exists (oslo::transaction const &, oslo::pending_key const &) = 0;
	virtual bool pending_any (oslo::transaction const &, oslo::account const &) = 0;
	virtual oslo::store_iterator<oslo::pending_key, oslo::pending_info> pending_begin (oslo::transaction const &, oslo::pending_key const &) = 0;
	virtual oslo::store_iterator<oslo::pending_key, oslo::pending_info> pending_begin (oslo::transaction const &) = 0;
	virtual oslo::store_iterator<oslo::pending_key, oslo::pending_info> pending_end () = 0;

	virtual bool block_info_get (oslo::transaction const &, oslo::block_hash const &, oslo::block_info &) const = 0;
	virtual oslo::uint128_t block_balance (oslo::transaction const &, oslo::block_hash const &) = 0;
	virtual oslo::uint128_t block_balance_calculated (std::shared_ptr<oslo::block> const &) const = 0;
	virtual oslo::epoch block_version (oslo::transaction const &, oslo::block_hash const &) = 0;

	virtual void unchecked_clear (oslo::write_transaction const &) = 0;
	virtual void unchecked_put (oslo::write_transaction const &, oslo::unchecked_key const &, oslo::unchecked_info const &) = 0;
	virtual void unchecked_put (oslo::write_transaction const &, oslo::block_hash const &, std::shared_ptr<oslo::block> const &) = 0;
	virtual std::vector<oslo::unchecked_info> unchecked_get (oslo::transaction const &, oslo::block_hash const &) = 0;
	virtual bool unchecked_exists (oslo::transaction const & transaction_a, oslo::unchecked_key const & unchecked_key_a) = 0;
	virtual void unchecked_del (oslo::write_transaction const &, oslo::unchecked_key const &) = 0;
	virtual oslo::store_iterator<oslo::unchecked_key, oslo::unchecked_info> unchecked_begin (oslo::transaction const &) const = 0;
	virtual oslo::store_iterator<oslo::unchecked_key, oslo::unchecked_info> unchecked_begin (oslo::transaction const &, oslo::unchecked_key const &) const = 0;
	virtual oslo::store_iterator<oslo::unchecked_key, oslo::unchecked_info> unchecked_end () const = 0;
	virtual size_t unchecked_count (oslo::transaction const &) = 0;

	// Return latest vote for an account from store
	virtual std::shared_ptr<oslo::vote> vote_get (oslo::transaction const &, oslo::account const &) = 0;
	// Populate vote with the next sequence number
	virtual std::shared_ptr<oslo::vote> vote_generate (oslo::transaction const &, oslo::account const &, oslo::raw_key const &, std::shared_ptr<oslo::block>) = 0;
	virtual std::shared_ptr<oslo::vote> vote_generate (oslo::transaction const &, oslo::account const &, oslo::raw_key const &, std::vector<oslo::block_hash>) = 0;
	// Return either vote or the stored vote with a higher sequence number
	virtual std::shared_ptr<oslo::vote> vote_max (oslo::transaction const &, std::shared_ptr<oslo::vote>) = 0;
	// Return latest vote for an account considering the vote cache
	virtual std::shared_ptr<oslo::vote> vote_current (oslo::transaction const &, oslo::account const &) = 0;
	virtual void flush (oslo::write_transaction const &) = 0;
	virtual oslo::store_iterator<oslo::account, std::shared_ptr<oslo::vote>> vote_begin (oslo::transaction const &) = 0;
	virtual oslo::store_iterator<oslo::account, std::shared_ptr<oslo::vote>> vote_end () = 0;

	virtual void online_weight_put (oslo::write_transaction const &, uint64_t, oslo::amount const &) = 0;
	virtual void online_weight_del (oslo::write_transaction const &, uint64_t) = 0;
	virtual oslo::store_iterator<uint64_t, oslo::amount> online_weight_begin (oslo::transaction const &) const = 0;
	virtual oslo::store_iterator<uint64_t, oslo::amount> online_weight_end () const = 0;
	virtual size_t online_weight_count (oslo::transaction const &) const = 0;
	virtual void online_weight_clear (oslo::write_transaction const &) = 0;

	virtual void version_put (oslo::write_transaction const &, int) = 0;
	virtual int version_get (oslo::transaction const &) const = 0;

	virtual void peer_put (oslo::write_transaction const & transaction_a, oslo::endpoint_key const & endpoint_a) = 0;
	virtual void peer_del (oslo::write_transaction const & transaction_a, oslo::endpoint_key const & endpoint_a) = 0;
	virtual bool peer_exists (oslo::transaction const & transaction_a, oslo::endpoint_key const & endpoint_a) const = 0;
	virtual size_t peer_count (oslo::transaction const & transaction_a) const = 0;
	virtual void peer_clear (oslo::write_transaction const & transaction_a) = 0;
	virtual oslo::store_iterator<oslo::endpoint_key, oslo::no_value> peers_begin (oslo::transaction const & transaction_a) const = 0;
	virtual oslo::store_iterator<oslo::endpoint_key, oslo::no_value> peers_end () const = 0;

	virtual void confirmation_height_put (oslo::write_transaction const & transaction_a, oslo::account const & account_a, oslo::confirmation_height_info const & confirmation_height_info_a) = 0;
	virtual bool confirmation_height_get (oslo::transaction const & transaction_a, oslo::account const & account_a, oslo::confirmation_height_info & confirmation_height_info_a) = 0;
	virtual bool confirmation_height_exists (oslo::transaction const & transaction_a, oslo::account const & account_a) const = 0;
	virtual void confirmation_height_del (oslo::write_transaction const & transaction_a, oslo::account const & account_a) = 0;
	virtual uint64_t confirmation_height_count (oslo::transaction const & transaction_a) = 0;
	virtual oslo::store_iterator<oslo::account, oslo::confirmation_height_info> confirmation_height_begin (oslo::transaction const & transaction_a, oslo::account const & account_a) = 0;
	virtual oslo::store_iterator<oslo::account, oslo::confirmation_height_info> confirmation_height_begin (oslo::transaction const & transaction_a) = 0;
	virtual oslo::store_iterator<oslo::account, oslo::confirmation_height_info> confirmation_height_end () = 0;

	virtual uint64_t block_account_height (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const = 0;
	virtual std::mutex & get_cache_mutex () = 0;

	virtual bool copy_db (boost::filesystem::path const & destination) = 0;
	virtual void rebuild_db (oslo::write_transaction const & transaction_a) = 0;

	/** Not applicable to all sub-classes */
	virtual void serialize_mdb_tracker (boost::property_tree::ptree &, std::chrono::milliseconds, std::chrono::milliseconds) = 0;

	virtual bool init_error () const = 0;

	/** Start read-write transaction */
	virtual oslo::write_transaction tx_begin_write (std::vector<oslo::tables> const & tables_to_lock = {}, std::vector<oslo::tables> const & tables_no_lock = {}) = 0;

	/** Start read-only transaction */
	virtual oslo::read_transaction tx_begin_read () = 0;

	virtual std::string vendor_get () const = 0;
};

std::unique_ptr<oslo::block_store> make_store (oslo::logger_mt & logger, boost::filesystem::path const & path, bool open_read_only = false, bool add_db_postfix = false, oslo::rocksdb_config const & rocksdb_config = oslo::rocksdb_config{}, oslo::txn_tracking_config const & txn_tracking_config_a = oslo::txn_tracking_config{}, std::chrono::milliseconds block_processor_batch_max_time_a = std::chrono::milliseconds (5000), oslo::lmdb_config const & lmdb_config_a = oslo::lmdb_config{}, size_t batch_size = 512, bool backup_before_upgrade = false, bool rocksdb_backend = false);
}

namespace std
{
template <>
struct hash<::oslo::tables>
{
	size_t operator() (::oslo::tables const & table_a) const
	{
		return static_cast<size_t> (table_a);
	}
};
}
