#pragma once

#include <oslo/crypto/blake2/blake2.h>
#include <oslo/lib/epoch.hpp>
#include <oslo/lib/errors.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/lib/optional_ptr.hpp>
#include <oslo/lib/stream.hpp>
#include <oslo/lib/utility.hpp>
#include <oslo/lib/work.hpp>

#include <boost/property_tree/ptree_fwd.hpp>

#include <unordered_map>

namespace oslo
{
class block_visitor;
class mutable_block_visitor;
enum class block_type : uint8_t
{
	invalid = 0,
	not_a_block = 1,
	send = 2,
	receive = 3,
	open = 4,
	change = 5,
	state = 6
};
class block_details
{
	static_assert (std::is_same<std::underlying_type<oslo::epoch>::type, uint8_t> (), "Epoch enum is not the proper type");
	static_assert (static_cast<uint8_t> (oslo::epoch::max) < (1 << 5), "Epoch max is too large for the sideband");

public:
	block_details () = default;
	block_details (oslo::epoch const epoch_a, bool const is_send_a, bool const is_receive_a, bool const is_epoch_a);
	static constexpr size_t size ();
	bool operator== (block_details const & other_a) const;
	void serialize (oslo::stream &) const;
	bool deserialize (oslo::stream &);
	oslo::epoch epoch{ oslo::epoch::epoch_0 };
	bool is_send{ false };
	bool is_receive{ false };
	bool is_epoch{ false };

private:
	uint8_t packed () const;
	void unpack (uint8_t);
};

std::string state_subtype (oslo::block_details const);

class block_sideband final
{
public:
	block_sideband () = default;
	block_sideband (oslo::account const &, oslo::block_hash const &, oslo::amount const &, uint64_t, uint64_t, oslo::block_details const &);
	block_sideband (oslo::account const &, oslo::block_hash const &, oslo::amount const &, uint64_t, uint64_t, oslo::epoch, bool is_send, bool is_receive, bool is_epoch);
	void serialize (oslo::stream &, oslo::block_type) const;
	bool deserialize (oslo::stream &, oslo::block_type);
	static size_t size (oslo::block_type);
	oslo::block_hash successor{ 0 };
	oslo::account account{ 0 };
	oslo::amount balance{ 0 };
	uint64_t height{ 0 };
	uint64_t timestamp{ 0 };
	oslo::block_details details;
};
class block
{
public:
	// Return a digest of the hashables in this block.
	oslo::block_hash const & hash () const;
	// Return a digest of hashables and non-hashables in this block.
	oslo::block_hash full_hash () const;
	oslo::block_sideband const & sideband () const;
	void sideband_set (oslo::block_sideband const &);
	bool has_sideband () const;
	std::string to_json () const;
	virtual void hash (blake2b_state &) const = 0;
	virtual uint64_t block_work () const = 0;
	virtual void block_work_set (uint64_t) = 0;
	virtual oslo::account const & account () const;
	// Previous block in account's chain, zero for open block
	virtual oslo::block_hash const & previous () const = 0;
	// Source block for open/receive blocks, zero otherwise.
	virtual oslo::block_hash const & source () const;
	// Previous block or account number for open blocks
	virtual oslo::root const & root () const = 0;
	// Qualified root value based on previous() and root()
	virtual oslo::qualified_root qualified_root () const;
	// Link field for state blocks, zero otherwise.
	virtual oslo::link const & link () const;
	virtual oslo::account const & representative () const;
	virtual oslo::amount const & balance () const;
	virtual void serialize (oslo::stream &) const = 0;
	virtual void serialize_json (std::string &, bool = false) const = 0;
	virtual void serialize_json (boost::property_tree::ptree &) const = 0;
	virtual void visit (oslo::block_visitor &) const = 0;
	virtual void visit (oslo::mutable_block_visitor &) = 0;
	virtual bool operator== (oslo::block const &) const = 0;
	virtual oslo::block_type type () const = 0;
	virtual oslo::signature const & block_signature () const = 0;
	virtual void signature_set (oslo::signature const &) = 0;
	virtual ~block () = default;
	virtual bool valid_predecessor (oslo::block const &) const = 0;
	static size_t size (oslo::block_type);
	virtual oslo::work_version work_version () const;
	uint64_t difficulty () const;
	// If there are any changes to the hashables, call this to update the cached hash
	void refresh ();

protected:
	mutable oslo::block_hash cached_hash{ 0 };
	/**
	 * Contextual details about a block, some fields may or may not be set depending on block type.
	 * This field is set via sideband_set in ledger processing or deserializing blocks from the database.
	 * Otherwise it may be null (for example, an old block or fork).
	 */
	oslo::optional_ptr<oslo::block_sideband> sideband_m;

private:
	oslo::block_hash generate_hash () const;
};
class send_hashables
{
public:
	send_hashables () = default;
	send_hashables (oslo::block_hash const &, oslo::account const &, oslo::amount const &);
	send_hashables (bool &, oslo::stream &);
	send_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	oslo::block_hash previous;
	oslo::account destination;
	oslo::amount balance;
	static size_t constexpr size = sizeof (previous) + sizeof (destination) + sizeof (balance);
};
class send_block : public oslo::block
{
public:
	send_block () = default;
	send_block (oslo::block_hash const &, oslo::account const &, oslo::amount const &, oslo::raw_key const &, oslo::public_key const &, uint64_t);
	send_block (bool &, oslo::stream &);
	send_block (bool &, boost::property_tree::ptree const &);
	virtual ~send_block () = default;
	using oslo::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	oslo::block_hash const & previous () const override;
	oslo::root const & root () const override;
	oslo::amount const & balance () const override;
	void serialize (oslo::stream &) const override;
	bool deserialize (oslo::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (oslo::block_visitor &) const override;
	void visit (oslo::mutable_block_visitor &) override;
	oslo::block_type type () const override;
	oslo::signature const & block_signature () const override;
	void signature_set (oslo::signature const &) override;
	bool operator== (oslo::block const &) const override;
	bool operator== (oslo::send_block const &) const;
	bool valid_predecessor (oslo::block const &) const override;
	send_hashables hashables;
	oslo::signature signature;
	uint64_t work;
	static size_t constexpr size = oslo::send_hashables::size + sizeof (signature) + sizeof (work);
};
class receive_hashables
{
public:
	receive_hashables () = default;
	receive_hashables (oslo::block_hash const &, oslo::block_hash const &);
	receive_hashables (bool &, oslo::stream &);
	receive_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	oslo::block_hash previous;
	oslo::block_hash source;
	static size_t constexpr size = sizeof (previous) + sizeof (source);
};
class receive_block : public oslo::block
{
public:
	receive_block () = default;
	receive_block (oslo::block_hash const &, oslo::block_hash const &, oslo::raw_key const &, oslo::public_key const &, uint64_t);
	receive_block (bool &, oslo::stream &);
	receive_block (bool &, boost::property_tree::ptree const &);
	virtual ~receive_block () = default;
	using oslo::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	oslo::block_hash const & previous () const override;
	oslo::block_hash const & source () const override;
	oslo::root const & root () const override;
	void serialize (oslo::stream &) const override;
	bool deserialize (oslo::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (oslo::block_visitor &) const override;
	void visit (oslo::mutable_block_visitor &) override;
	oslo::block_type type () const override;
	oslo::signature const & block_signature () const override;
	void signature_set (oslo::signature const &) override;
	bool operator== (oslo::block const &) const override;
	bool operator== (oslo::receive_block const &) const;
	bool valid_predecessor (oslo::block const &) const override;
	receive_hashables hashables;
	oslo::signature signature;
	uint64_t work;
	static size_t constexpr size = oslo::receive_hashables::size + sizeof (signature) + sizeof (work);
};
class open_hashables
{
public:
	open_hashables () = default;
	open_hashables (oslo::block_hash const &, oslo::account const &, oslo::account const &);
	open_hashables (bool &, oslo::stream &);
	open_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	oslo::block_hash source;
	oslo::account representative;
	oslo::account account;
	static size_t constexpr size = sizeof (source) + sizeof (representative) + sizeof (account);
};
class open_block : public oslo::block
{
public:
	open_block () = default;
	open_block (oslo::block_hash const &, oslo::account const &, oslo::account const &, oslo::raw_key const &, oslo::public_key const &, uint64_t);
	open_block (oslo::block_hash const &, oslo::account const &, oslo::account const &, std::nullptr_t);
	open_block (bool &, oslo::stream &);
	open_block (bool &, boost::property_tree::ptree const &);
	virtual ~open_block () = default;
	using oslo::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	oslo::block_hash const & previous () const override;
	oslo::account const & account () const override;
	oslo::block_hash const & source () const override;
	oslo::root const & root () const override;
	oslo::account const & representative () const override;
	void serialize (oslo::stream &) const override;
	bool deserialize (oslo::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (oslo::block_visitor &) const override;
	void visit (oslo::mutable_block_visitor &) override;
	oslo::block_type type () const override;
	oslo::signature const & block_signature () const override;
	void signature_set (oslo::signature const &) override;
	bool operator== (oslo::block const &) const override;
	bool operator== (oslo::open_block const &) const;
	bool valid_predecessor (oslo::block const &) const override;
	oslo::open_hashables hashables;
	oslo::signature signature;
	uint64_t work;
	static size_t constexpr size = oslo::open_hashables::size + sizeof (signature) + sizeof (work);
};
class change_hashables
{
public:
	change_hashables () = default;
	change_hashables (oslo::block_hash const &, oslo::account const &);
	change_hashables (bool &, oslo::stream &);
	change_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	oslo::block_hash previous;
	oslo::account representative;
	static size_t constexpr size = sizeof (previous) + sizeof (representative);
};
class change_block : public oslo::block
{
public:
	change_block () = default;
	change_block (oslo::block_hash const &, oslo::account const &, oslo::raw_key const &, oslo::public_key const &, uint64_t);
	change_block (bool &, oslo::stream &);
	change_block (bool &, boost::property_tree::ptree const &);
	virtual ~change_block () = default;
	using oslo::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	oslo::block_hash const & previous () const override;
	oslo::root const & root () const override;
	oslo::account const & representative () const override;
	void serialize (oslo::stream &) const override;
	bool deserialize (oslo::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (oslo::block_visitor &) const override;
	void visit (oslo::mutable_block_visitor &) override;
	oslo::block_type type () const override;
	oslo::signature const & block_signature () const override;
	void signature_set (oslo::signature const &) override;
	bool operator== (oslo::block const &) const override;
	bool operator== (oslo::change_block const &) const;
	bool valid_predecessor (oslo::block const &) const override;
	oslo::change_hashables hashables;
	oslo::signature signature;
	uint64_t work;
	static size_t constexpr size = oslo::change_hashables::size + sizeof (signature) + sizeof (work);
};
class state_hashables
{
public:
	state_hashables () = default;
	state_hashables (oslo::account const &, oslo::block_hash const &, oslo::account const &, oslo::amount const &, oslo::link const &);
	state_hashables (bool &, oslo::stream &);
	state_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	// Account# / public key that operates this account
	// Uses:
	// Bulk signature validation in advance of further ledger processing
	// Arranging uncomitted transactions by account
	oslo::account account;
	// Previous transaction in this chain
	oslo::block_hash previous;
	// Representative of this account
	oslo::account representative;
	// Current balance of this account
	// Allows lookup of account balance simply by looking at the head block
	oslo::amount balance;
	// Link field contains source block_hash if receiving, destination account if sending
	oslo::link link;
	// Serialized size
	static size_t constexpr size = sizeof (account) + sizeof (previous) + sizeof (representative) + sizeof (balance) + sizeof (link);
};
class state_block : public oslo::block
{
public:
	state_block () = default;
	state_block (oslo::account const &, oslo::block_hash const &, oslo::account const &, oslo::amount const &, oslo::link const &, oslo::raw_key const &, oslo::public_key const &, uint64_t);
	state_block (bool &, oslo::stream &);
	state_block (bool &, boost::property_tree::ptree const &);
	virtual ~state_block () = default;
	using oslo::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	oslo::block_hash const & previous () const override;
	oslo::account const & account () const override;
	oslo::root const & root () const override;
	oslo::link const & link () const override;
	oslo::account const & representative () const override;
	oslo::amount const & balance () const override;
	void serialize (oslo::stream &) const override;
	bool deserialize (oslo::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (oslo::block_visitor &) const override;
	void visit (oslo::mutable_block_visitor &) override;
	oslo::block_type type () const override;
	oslo::signature const & block_signature () const override;
	void signature_set (oslo::signature const &) override;
	bool operator== (oslo::block const &) const override;
	bool operator== (oslo::state_block const &) const;
	bool valid_predecessor (oslo::block const &) const override;
	oslo::state_hashables hashables;
	oslo::signature signature;
	uint64_t work;
	static size_t constexpr size = oslo::state_hashables::size + sizeof (signature) + sizeof (work);
};
class block_visitor
{
public:
	virtual void send_block (oslo::send_block const &) = 0;
	virtual void receive_block (oslo::receive_block const &) = 0;
	virtual void open_block (oslo::open_block const &) = 0;
	virtual void change_block (oslo::change_block const &) = 0;
	virtual void state_block (oslo::state_block const &) = 0;
	virtual ~block_visitor () = default;
};
class mutable_block_visitor
{
public:
	virtual void send_block (oslo::send_block &) = 0;
	virtual void receive_block (oslo::receive_block &) = 0;
	virtual void open_block (oslo::open_block &) = 0;
	virtual void change_block (oslo::change_block &) = 0;
	virtual void state_block (oslo::state_block &) = 0;
	virtual ~mutable_block_visitor () = default;
};
/**
 * This class serves to find and return unique variants of a block in order to minimize memory usage
 */
class block_uniquer
{
public:
	using value_type = std::pair<const oslo::uint256_union, std::weak_ptr<oslo::block>>;

	std::shared_ptr<oslo::block> unique (std::shared_ptr<oslo::block>);
	size_t size ();

private:
	std::mutex mutex;
	std::unordered_map<std::remove_const_t<value_type::first_type>, value_type::second_type> blocks;
	static unsigned constexpr cleanup_count = 2;
};

std::unique_ptr<container_info_component> collect_container_info (block_uniquer & block_uniquer, const std::string & name);

std::shared_ptr<oslo::block> deserialize_block (oslo::stream &);
std::shared_ptr<oslo::block> deserialize_block (oslo::stream &, oslo::block_type, oslo::block_uniquer * = nullptr);
std::shared_ptr<oslo::block> deserialize_block_json (boost::property_tree::ptree const &, oslo::block_uniquer * = nullptr);
void serialize_block (oslo::stream &, oslo::block const &);
void block_memory_pool_purge ();
}
