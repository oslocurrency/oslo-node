#include <oslo/crypto_lib/random_pool.hpp>
#include <oslo/lib/blocks.hpp>
#include <oslo/lib/memory.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/lib/threading.hpp>

#include <crypto/cryptopp/words.h>

#include <boost/endian/conversion.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <bitset>

/** Compare blocks, first by type, then content. This is an optimization over dynamic_cast, which is very slow on some platforms. */
namespace
{
template <typename T>
bool blocks_equal (T const & first, oslo::block const & second)
{
	static_assert (std::is_base_of<oslo::block, T>::value, "Input parameter is not a block type");
	return (first.type () == second.type ()) && (static_cast<T const &> (second)) == first;
}

template <typename block>
std::shared_ptr<block> deserialize_block (oslo::stream & stream_a)
{
	auto error (false);
	auto result = oslo::make_shared<block> (error, stream_a);
	if (error)
	{
		result = nullptr;
	}

	return result;
}
}

void oslo::block_memory_pool_purge ()
{
	oslo::purge_singleton_pool_memory<oslo::open_block> ();
	oslo::purge_singleton_pool_memory<oslo::state_block> ();
	oslo::purge_singleton_pool_memory<oslo::send_block> ();
	oslo::purge_singleton_pool_memory<oslo::change_block> ();
}

std::string oslo::block::to_json () const
{
	std::string result;
	serialize_json (result);
	return result;
}

size_t oslo::block::size (oslo::block_type type_a)
{
	size_t result (0);
	switch (type_a)
	{
		case oslo::block_type::invalid:
		case oslo::block_type::not_a_block:
			debug_assert (false);
			break;
		case oslo::block_type::send:
			result = oslo::send_block::size;
			break;
		case oslo::block_type::receive:
			result = oslo::receive_block::size;
			break;
		case oslo::block_type::change:
			result = oslo::change_block::size;
			break;
		case oslo::block_type::open:
			result = oslo::open_block::size;
			break;
		case oslo::block_type::state:
			result = oslo::state_block::size;
			break;
	}
	return result;
}

oslo::work_version oslo::block::work_version () const
{
	return oslo::work_version::work_1;
}

uint64_t oslo::block::difficulty () const
{
	return oslo::work_difficulty (this->work_version (), this->root (), this->block_work ());
}

oslo::block_hash oslo::block::generate_hash () const
{
	oslo::block_hash result;
	blake2b_state hash_l;
	auto status (blake2b_init (&hash_l, sizeof (result.bytes)));
	debug_assert (status == 0);
	hash (hash_l);
	status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

void oslo::block::refresh ()
{
	if (!cached_hash.is_zero ())
	{
		cached_hash = generate_hash ();
	}
}

oslo::block_hash const & oslo::block::hash () const
{
	if (!cached_hash.is_zero ())
	{
		// Once a block is created, it should not be modified (unless using refresh ())
		// This would invalidate the cache; check it hasn't changed.
		debug_assert (cached_hash == generate_hash ());
	}
	else
	{
		cached_hash = generate_hash ();
	}

	return cached_hash;
}

oslo::block_hash oslo::block::full_hash () const
{
	oslo::block_hash result;
	blake2b_state state;
	blake2b_init (&state, sizeof (result.bytes));
	blake2b_update (&state, hash ().bytes.data (), sizeof (hash ()));
	auto signature (block_signature ());
	blake2b_update (&state, signature.bytes.data (), sizeof (signature));
	auto work (block_work ());
	blake2b_update (&state, &work, sizeof (work));
	blake2b_final (&state, result.bytes.data (), sizeof (result.bytes));
	return result;
}

oslo::block_sideband const & oslo::block::sideband () const
{
	debug_assert (sideband_m.is_initialized ());
	return *sideband_m;
}

void oslo::block::sideband_set (oslo::block_sideband const & sideband_a)
{
	sideband_m = sideband_a;
}

bool oslo::block::has_sideband () const
{
	return sideband_m.is_initialized ();
}

oslo::account const & oslo::block::representative () const
{
	static oslo::account rep{ 0 };
	return rep;
}

oslo::block_hash const & oslo::block::source () const
{
	static oslo::block_hash source{ 0 };
	return source;
}

oslo::link const & oslo::block::link () const
{
	static oslo::link link{ 0 };
	return link;
}

oslo::account const & oslo::block::account () const
{
	static oslo::account account{ 0 };
	return account;
}

oslo::qualified_root oslo::block::qualified_root () const
{
	return oslo::qualified_root (previous (), root ());
}

oslo::amount const & oslo::block::balance () const
{
	static oslo::amount amount{ 0 };
	return amount;
}

void oslo::send_block::visit (oslo::block_visitor & visitor_a) const
{
	visitor_a.send_block (*this);
}

void oslo::send_block::visit (oslo::mutable_block_visitor & visitor_a)
{
	visitor_a.send_block (*this);
}

void oslo::send_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t oslo::send_block::block_work () const
{
	return work;
}

void oslo::send_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

oslo::send_hashables::send_hashables (oslo::block_hash const & previous_a, oslo::account const & destination_a, oslo::amount const & balance_a) :
previous (previous_a),
destination (destination_a),
balance (balance_a)
{
}

oslo::send_hashables::send_hashables (bool & error_a, oslo::stream & stream_a)
{
	try
	{
		oslo::read (stream_a, previous.bytes);
		oslo::read (stream_a, destination.bytes);
		oslo::read (stream_a, balance.bytes);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

oslo::send_hashables::send_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto destination_l (tree_a.get<std::string> ("destination"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = destination.decode_account (destination_l);
			if (!error_a)
			{
				error_a = balance.decode_hex (balance_l);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void oslo::send_hashables::hash (blake2b_state & hash_a) const
{
	auto status (blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes)));
	debug_assert (status == 0);
	status = blake2b_update (&hash_a, destination.bytes.data (), sizeof (destination.bytes));
	debug_assert (status == 0);
	status = blake2b_update (&hash_a, balance.bytes.data (), sizeof (balance.bytes));
	debug_assert (status == 0);
}

void oslo::send_block::serialize (oslo::stream & stream_a) const
{
	write (stream_a, hashables.previous.bytes);
	write (stream_a, hashables.destination.bytes);
	write (stream_a, hashables.balance.bytes);
	write (stream_a, signature.bytes);
	write (stream_a, work);
}

bool oslo::send_block::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.previous.bytes);
		read (stream_a, hashables.destination.bytes);
		read (stream_a, hashables.balance.bytes);
		read (stream_a, signature.bytes);
		read (stream_a, work);
	}
	catch (std::exception const &)
	{
		error = true;
	}

	return error;
}

void oslo::send_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void oslo::send_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "send");
	std::string previous;
	hashables.previous.encode_hex (previous);
	tree.put ("previous", previous);
	tree.put ("destination", hashables.destination.to_account ());
	std::string balance;
	hashables.balance.encode_hex (balance);
	tree.put ("balance", balance);
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("work", oslo::to_string_hex (work));
	tree.put ("signature", signature_l);
}

bool oslo::send_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "send");
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto destination_l (tree_a.get<std::string> ("destination"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.previous.decode_hex (previous_l);
		if (!error)
		{
			error = hashables.destination.decode_account (destination_l);
			if (!error)
			{
				error = hashables.balance.decode_hex (balance_l);
				if (!error)
				{
					error = oslo::from_string_hex (work_l, work);
					if (!error)
					{
						error = signature.decode_hex (signature_l);
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

oslo::send_block::send_block (oslo::block_hash const & previous_a, oslo::account const & destination_a, oslo::amount const & balance_a, oslo::raw_key const & prv_a, oslo::public_key const & pub_a, uint64_t work_a) :
hashables (previous_a, destination_a, balance_a),
signature (oslo::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

oslo::send_block::send_block (bool & error_a, oslo::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			oslo::read (stream_a, signature.bytes);
			oslo::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

oslo::send_block::send_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = signature.decode_hex (signature_l);
			if (!error_a)
			{
				error_a = oslo::from_string_hex (work_l, work);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

bool oslo::send_block::operator== (oslo::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool oslo::send_block::valid_predecessor (oslo::block const & block_a) const
{
	bool result;
	switch (block_a.type ())
	{
		case oslo::block_type::send:
		case oslo::block_type::receive:
		case oslo::block_type::open:
		case oslo::block_type::change:
			result = true;
			break;
		default:
			result = false;
			break;
	}
	return result;
}

oslo::block_type oslo::send_block::type () const
{
	return oslo::block_type::send;
}

bool oslo::send_block::operator== (oslo::send_block const & other_a) const
{
	auto result (hashables.destination == other_a.hashables.destination && hashables.previous == other_a.hashables.previous && hashables.balance == other_a.hashables.balance && work == other_a.work && signature == other_a.signature);
	return result;
}

oslo::block_hash const & oslo::send_block::previous () const
{
	return hashables.previous;
}

oslo::root const & oslo::send_block::root () const
{
	return hashables.previous;
}

oslo::amount const & oslo::send_block::balance () const
{
	return hashables.balance;
}

oslo::signature const & oslo::send_block::block_signature () const
{
	return signature;
}

void oslo::send_block::signature_set (oslo::signature const & signature_a)
{
	signature = signature_a;
}

oslo::open_hashables::open_hashables (oslo::block_hash const & source_a, oslo::account const & representative_a, oslo::account const & account_a) :
source (source_a),
representative (representative_a),
account (account_a)
{
}

oslo::open_hashables::open_hashables (bool & error_a, oslo::stream & stream_a)
{
	try
	{
		oslo::read (stream_a, source.bytes);
		oslo::read (stream_a, representative.bytes);
		oslo::read (stream_a, account.bytes);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

oslo::open_hashables::open_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto source_l (tree_a.get<std::string> ("source"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto account_l (tree_a.get<std::string> ("account"));
		error_a = source.decode_hex (source_l);
		if (!error_a)
		{
			error_a = representative.decode_account (representative_l);
			if (!error_a)
			{
				error_a = account.decode_account (account_l);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void oslo::open_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, source.bytes.data (), sizeof (source.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
	blake2b_update (&hash_a, account.bytes.data (), sizeof (account.bytes));
}

oslo::open_block::open_block (oslo::block_hash const & source_a, oslo::account const & representative_a, oslo::account const & account_a, oslo::raw_key const & prv_a, oslo::public_key const & pub_a, uint64_t work_a) :
hashables (source_a, representative_a, account_a),
signature (oslo::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
	debug_assert (!representative_a.is_zero ());
}

oslo::open_block::open_block (oslo::block_hash const & source_a, oslo::account const & representative_a, oslo::account const & account_a, std::nullptr_t) :
hashables (source_a, representative_a, account_a),
work (0)
{
	signature.clear ();
}

oslo::open_block::open_block (bool & error_a, oslo::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			oslo::read (stream_a, signature);
			oslo::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

oslo::open_block::open_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto work_l (tree_a.get<std::string> ("work"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			error_a = oslo::from_string_hex (work_l, work);
			if (!error_a)
			{
				error_a = signature.decode_hex (signature_l);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void oslo::open_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t oslo::open_block::block_work () const
{
	return work;
}

void oslo::open_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

oslo::block_hash const & oslo::open_block::previous () const
{
	static oslo::block_hash result{ 0 };
	return result;
}

oslo::account const & oslo::open_block::account () const
{
	return hashables.account;
}

void oslo::open_block::serialize (oslo::stream & stream_a) const
{
	write (stream_a, hashables.source);
	write (stream_a, hashables.representative);
	write (stream_a, hashables.account);
	write (stream_a, signature);
	write (stream_a, work);
}

bool oslo::open_block::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.source);
		read (stream_a, hashables.representative);
		read (stream_a, hashables.account);
		read (stream_a, signature);
		read (stream_a, work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void oslo::open_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void oslo::open_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "open");
	tree.put ("source", hashables.source.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("account", hashables.account.to_account ());
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("work", oslo::to_string_hex (work));
	tree.put ("signature", signature_l);
}

bool oslo::open_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "open");
		auto source_l (tree_a.get<std::string> ("source"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto account_l (tree_a.get<std::string> ("account"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.source.decode_hex (source_l);
		if (!error)
		{
			error = hashables.representative.decode_hex (representative_l);
			if (!error)
			{
				error = hashables.account.decode_hex (account_l);
				if (!error)
				{
					error = oslo::from_string_hex (work_l, work);
					if (!error)
					{
						error = signature.decode_hex (signature_l);
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

void oslo::open_block::visit (oslo::block_visitor & visitor_a) const
{
	visitor_a.open_block (*this);
}

void oslo::open_block::visit (oslo::mutable_block_visitor & visitor_a)
{
	visitor_a.open_block (*this);
}

oslo::block_type oslo::open_block::type () const
{
	return oslo::block_type::open;
}

bool oslo::open_block::operator== (oslo::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool oslo::open_block::operator== (oslo::open_block const & other_a) const
{
	return hashables.source == other_a.hashables.source && hashables.representative == other_a.hashables.representative && hashables.account == other_a.hashables.account && work == other_a.work && signature == other_a.signature;
}

bool oslo::open_block::valid_predecessor (oslo::block const & block_a) const
{
	return false;
}

oslo::block_hash const & oslo::open_block::source () const
{
	return hashables.source;
}

oslo::root const & oslo::open_block::root () const
{
	return hashables.account;
}

oslo::account const & oslo::open_block::representative () const
{
	return hashables.representative;
}

oslo::signature const & oslo::open_block::block_signature () const
{
	return signature;
}

void oslo::open_block::signature_set (oslo::signature const & signature_a)
{
	signature = signature_a;
}

oslo::change_hashables::change_hashables (oslo::block_hash const & previous_a, oslo::account const & representative_a) :
previous (previous_a),
representative (representative_a)
{
}

oslo::change_hashables::change_hashables (bool & error_a, oslo::stream & stream_a)
{
	try
	{
		oslo::read (stream_a, previous);
		oslo::read (stream_a, representative);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

oslo::change_hashables::change_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = representative.decode_account (representative_l);
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void oslo::change_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
}

oslo::change_block::change_block (oslo::block_hash const & previous_a, oslo::account const & representative_a, oslo::raw_key const & prv_a, oslo::public_key const & pub_a, uint64_t work_a) :
hashables (previous_a, representative_a),
signature (oslo::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

oslo::change_block::change_block (bool & error_a, oslo::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			oslo::read (stream_a, signature);
			oslo::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

oslo::change_block::change_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto work_l (tree_a.get<std::string> ("work"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			error_a = oslo::from_string_hex (work_l, work);
			if (!error_a)
			{
				error_a = signature.decode_hex (signature_l);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void oslo::change_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t oslo::change_block::block_work () const
{
	return work;
}

void oslo::change_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

oslo::block_hash const & oslo::change_block::previous () const
{
	return hashables.previous;
}

void oslo::change_block::serialize (oslo::stream & stream_a) const
{
	write (stream_a, hashables.previous);
	write (stream_a, hashables.representative);
	write (stream_a, signature);
	write (stream_a, work);
}

bool oslo::change_block::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.previous);
		read (stream_a, hashables.representative);
		read (stream_a, signature);
		read (stream_a, work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void oslo::change_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void oslo::change_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "change");
	tree.put ("previous", hashables.previous.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("work", oslo::to_string_hex (work));
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("signature", signature_l);
}

bool oslo::change_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "change");
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.previous.decode_hex (previous_l);
		if (!error)
		{
			error = hashables.representative.decode_hex (representative_l);
			if (!error)
			{
				error = oslo::from_string_hex (work_l, work);
				if (!error)
				{
					error = signature.decode_hex (signature_l);
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

void oslo::change_block::visit (oslo::block_visitor & visitor_a) const
{
	visitor_a.change_block (*this);
}

void oslo::change_block::visit (oslo::mutable_block_visitor & visitor_a)
{
	visitor_a.change_block (*this);
}

oslo::block_type oslo::change_block::type () const
{
	return oslo::block_type::change;
}

bool oslo::change_block::operator== (oslo::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool oslo::change_block::operator== (oslo::change_block const & other_a) const
{
	return hashables.previous == other_a.hashables.previous && hashables.representative == other_a.hashables.representative && work == other_a.work && signature == other_a.signature;
}

bool oslo::change_block::valid_predecessor (oslo::block const & block_a) const
{
	bool result;
	switch (block_a.type ())
	{
		case oslo::block_type::send:
		case oslo::block_type::receive:
		case oslo::block_type::open:
		case oslo::block_type::change:
			result = true;
			break;
		default:
			result = false;
			break;
	}
	return result;
}

oslo::root const & oslo::change_block::root () const
{
	return hashables.previous;
}

oslo::account const & oslo::change_block::representative () const
{
	return hashables.representative;
}

oslo::signature const & oslo::change_block::block_signature () const
{
	return signature;
}

void oslo::change_block::signature_set (oslo::signature const & signature_a)
{
	signature = signature_a;
}

oslo::state_hashables::state_hashables (oslo::account const & account_a, oslo::block_hash const & previous_a, oslo::account const & representative_a, oslo::amount const & balance_a, oslo::link const & link_a) :
account (account_a),
previous (previous_a),
representative (representative_a),
balance (balance_a),
link (link_a)
{
}

oslo::state_hashables::state_hashables (bool & error_a, oslo::stream & stream_a)
{
	try
	{
		oslo::read (stream_a, account);
		oslo::read (stream_a, previous);
		oslo::read (stream_a, representative);
		oslo::read (stream_a, balance);
		oslo::read (stream_a, link);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

oslo::state_hashables::state_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto account_l (tree_a.get<std::string> ("account"));
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto link_l (tree_a.get<std::string> ("link"));
		error_a = account.decode_account (account_l);
		if (!error_a)
		{
			error_a = previous.decode_hex (previous_l);
			if (!error_a)
			{
				error_a = representative.decode_account (representative_l);
				if (!error_a)
				{
					error_a = balance.decode_dec (balance_l);
					if (!error_a)
					{
						error_a = link.decode_account (link_l) && link.decode_hex (link_l);
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void oslo::state_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, account.bytes.data (), sizeof (account.bytes));
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
	blake2b_update (&hash_a, balance.bytes.data (), sizeof (balance.bytes));
	blake2b_update (&hash_a, link.bytes.data (), sizeof (link.bytes));
}

oslo::state_block::state_block (oslo::account const & account_a, oslo::block_hash const & previous_a, oslo::account const & representative_a, oslo::amount const & balance_a, oslo::link const & link_a, oslo::raw_key const & prv_a, oslo::public_key const & pub_a, uint64_t work_a) :
hashables (account_a, previous_a, representative_a, balance_a, link_a),
signature (oslo::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

oslo::state_block::state_block (bool & error_a, oslo::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			oslo::read (stream_a, signature);
			oslo::read (stream_a, work);
			boost::endian::big_to_native_inplace (work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

oslo::state_block::state_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto type_l (tree_a.get<std::string> ("type"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = type_l != "state";
			if (!error_a)
			{
				error_a = oslo::from_string_hex (work_l, work);
				if (!error_a)
				{
					error_a = signature.decode_hex (signature_l);
				}
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void oslo::state_block::hash (blake2b_state & hash_a) const
{
	oslo::uint256_union preamble (static_cast<uint64_t> (oslo::block_type::state));
	blake2b_update (&hash_a, preamble.bytes.data (), preamble.bytes.size ());
	hashables.hash (hash_a);
}

uint64_t oslo::state_block::block_work () const
{
	return work;
}

void oslo::state_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

oslo::block_hash const & oslo::state_block::previous () const
{
	return hashables.previous;
}

oslo::account const & oslo::state_block::account () const
{
	return hashables.account;
}

void oslo::state_block::serialize (oslo::stream & stream_a) const
{
	write (stream_a, hashables.account);
	write (stream_a, hashables.previous);
	write (stream_a, hashables.representative);
	write (stream_a, hashables.balance);
	write (stream_a, hashables.link);
	write (stream_a, signature);
	write (stream_a, boost::endian::native_to_big (work));
}

bool oslo::state_block::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.account);
		read (stream_a, hashables.previous);
		read (stream_a, hashables.representative);
		read (stream_a, hashables.balance);
		read (stream_a, hashables.link);
		read (stream_a, signature);
		read (stream_a, work);
		boost::endian::big_to_native_inplace (work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void oslo::state_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void oslo::state_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "state");
	tree.put ("account", hashables.account.to_account ());
	tree.put ("previous", hashables.previous.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("balance", hashables.balance.to_string_dec ());
	tree.put ("link", hashables.link.to_string ());
	tree.put ("link_as_account", hashables.link.to_account ());
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("signature", signature_l);
	tree.put ("work", oslo::to_string_hex (work));
}

bool oslo::state_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "state");
		auto account_l (tree_a.get<std::string> ("account"));
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto link_l (tree_a.get<std::string> ("link"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.account.decode_account (account_l);
		if (!error)
		{
			error = hashables.previous.decode_hex (previous_l);
			if (!error)
			{
				error = hashables.representative.decode_account (representative_l);
				if (!error)
				{
					error = hashables.balance.decode_dec (balance_l);
					if (!error)
					{
						error = hashables.link.decode_account (link_l) && hashables.link.decode_hex (link_l);
						if (!error)
						{
							error = oslo::from_string_hex (work_l, work);
							if (!error)
							{
								error = signature.decode_hex (signature_l);
							}
						}
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

void oslo::state_block::visit (oslo::block_visitor & visitor_a) const
{
	visitor_a.state_block (*this);
}

void oslo::state_block::visit (oslo::mutable_block_visitor & visitor_a)
{
	visitor_a.state_block (*this);
}

oslo::block_type oslo::state_block::type () const
{
	return oslo::block_type::state;
}

bool oslo::state_block::operator== (oslo::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool oslo::state_block::operator== (oslo::state_block const & other_a) const
{
	return hashables.account == other_a.hashables.account && hashables.previous == other_a.hashables.previous && hashables.representative == other_a.hashables.representative && hashables.balance == other_a.hashables.balance && hashables.link == other_a.hashables.link && signature == other_a.signature && work == other_a.work;
}

bool oslo::state_block::valid_predecessor (oslo::block const & block_a) const
{
	return true;
}

oslo::root const & oslo::state_block::root () const
{
	if (!hashables.previous.is_zero ())
	{
		return hashables.previous;
	}
	else
	{
		return hashables.account;
	}
}

oslo::link const & oslo::state_block::link () const
{
	return hashables.link;
}

oslo::account const & oslo::state_block::representative () const
{
	return hashables.representative;
}

oslo::amount const & oslo::state_block::balance () const
{
	return hashables.balance;
}

oslo::signature const & oslo::state_block::block_signature () const
{
	return signature;
}

void oslo::state_block::signature_set (oslo::signature const & signature_a)
{
	signature = signature_a;
}

std::shared_ptr<oslo::block> oslo::deserialize_block_json (boost::property_tree::ptree const & tree_a, oslo::block_uniquer * uniquer_a)
{
	std::shared_ptr<oslo::block> result;
	try
	{
		auto type (tree_a.get<std::string> ("type"));
		bool error (false);
		std::unique_ptr<oslo::block> obj;
		if (type == "receive")
		{
			obj = std::make_unique<oslo::receive_block> (error, tree_a);
		}
		else if (type == "send")
		{
			obj = std::make_unique<oslo::send_block> (error, tree_a);
		}
		else if (type == "open")
		{
			obj = std::make_unique<oslo::open_block> (error, tree_a);
		}
		else if (type == "change")
		{
			obj = std::make_unique<oslo::change_block> (error, tree_a);
		}
		else if (type == "state")
		{
			obj = std::make_unique<oslo::state_block> (error, tree_a);
		}

		if (!error)
		{
			result = std::move (obj);
		}
	}
	catch (std::runtime_error const &)
	{
	}
	if (uniquer_a != nullptr)
	{
		result = uniquer_a->unique (result);
	}
	return result;
}

std::shared_ptr<oslo::block> oslo::deserialize_block (oslo::stream & stream_a)
{
	oslo::block_type type;
	auto error (try_read (stream_a, type));
	std::shared_ptr<oslo::block> result;
	if (!error)
	{
		result = oslo::deserialize_block (stream_a, type);
	}
	return result;
}

std::shared_ptr<oslo::block> oslo::deserialize_block (oslo::stream & stream_a, oslo::block_type type_a, oslo::block_uniquer * uniquer_a)
{
	std::shared_ptr<oslo::block> result;
	switch (type_a)
	{
		case oslo::block_type::receive:
		{
			result = ::deserialize_block<oslo::receive_block> (stream_a);
			break;
		}
		case oslo::block_type::send:
		{
			result = ::deserialize_block<oslo::send_block> (stream_a);
			break;
		}
		case oslo::block_type::open:
		{
			result = ::deserialize_block<oslo::open_block> (stream_a);
			break;
		}
		case oslo::block_type::change:
		{
			result = ::deserialize_block<oslo::change_block> (stream_a);
			break;
		}
		case oslo::block_type::state:
		{
			result = ::deserialize_block<oslo::state_block> (stream_a);
			break;
		}
		default:
#ifndef OSLO_FUZZER_TEST
			debug_assert (false);
#endif
			break;
	}
	if (uniquer_a != nullptr)
	{
		result = uniquer_a->unique (result);
	}
	return result;
}

void oslo::receive_block::visit (oslo::block_visitor & visitor_a) const
{
	visitor_a.receive_block (*this);
}

void oslo::receive_block::visit (oslo::mutable_block_visitor & visitor_a)
{
	visitor_a.receive_block (*this);
}

bool oslo::receive_block::operator== (oslo::receive_block const & other_a) const
{
	auto result (hashables.previous == other_a.hashables.previous && hashables.source == other_a.hashables.source && work == other_a.work && signature == other_a.signature);
	return result;
}

void oslo::receive_block::serialize (oslo::stream & stream_a) const
{
	write (stream_a, hashables.previous.bytes);
	write (stream_a, hashables.source.bytes);
	write (stream_a, signature.bytes);
	write (stream_a, work);
}

bool oslo::receive_block::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.previous.bytes);
		read (stream_a, hashables.source.bytes);
		read (stream_a, signature.bytes);
		read (stream_a, work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void oslo::receive_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void oslo::receive_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "receive");
	std::string previous;
	hashables.previous.encode_hex (previous);
	tree.put ("previous", previous);
	std::string source;
	hashables.source.encode_hex (source);
	tree.put ("source", source);
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("work", oslo::to_string_hex (work));
	tree.put ("signature", signature_l);
}

bool oslo::receive_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "receive");
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto source_l (tree_a.get<std::string> ("source"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.previous.decode_hex (previous_l);
		if (!error)
		{
			error = hashables.source.decode_hex (source_l);
			if (!error)
			{
				error = oslo::from_string_hex (work_l, work);
				if (!error)
				{
					error = signature.decode_hex (signature_l);
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

oslo::receive_block::receive_block (oslo::block_hash const & previous_a, oslo::block_hash const & source_a, oslo::raw_key const & prv_a, oslo::public_key const & pub_a, uint64_t work_a) :
hashables (previous_a, source_a),
signature (oslo::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

oslo::receive_block::receive_block (bool & error_a, oslo::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			oslo::read (stream_a, signature);
			oslo::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

oslo::receive_block::receive_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = signature.decode_hex (signature_l);
			if (!error_a)
			{
				error_a = oslo::from_string_hex (work_l, work);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void oslo::receive_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t oslo::receive_block::block_work () const
{
	return work;
}

void oslo::receive_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

bool oslo::receive_block::operator== (oslo::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool oslo::receive_block::valid_predecessor (oslo::block const & block_a) const
{
	bool result;
	switch (block_a.type ())
	{
		case oslo::block_type::send:
		case oslo::block_type::receive:
		case oslo::block_type::open:
		case oslo::block_type::change:
			result = true;
			break;
		default:
			result = false;
			break;
	}
	return result;
}

oslo::block_hash const & oslo::receive_block::previous () const
{
	return hashables.previous;
}

oslo::block_hash const & oslo::receive_block::source () const
{
	return hashables.source;
}

oslo::root const & oslo::receive_block::root () const
{
	return hashables.previous;
}

oslo::signature const & oslo::receive_block::block_signature () const
{
	return signature;
}

void oslo::receive_block::signature_set (oslo::signature const & signature_a)
{
	signature = signature_a;
}

oslo::block_type oslo::receive_block::type () const
{
	return oslo::block_type::receive;
}

oslo::receive_hashables::receive_hashables (oslo::block_hash const & previous_a, oslo::block_hash const & source_a) :
previous (previous_a),
source (source_a)
{
}

oslo::receive_hashables::receive_hashables (bool & error_a, oslo::stream & stream_a)
{
	try
	{
		oslo::read (stream_a, previous.bytes);
		oslo::read (stream_a, source.bytes);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

oslo::receive_hashables::receive_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto source_l (tree_a.get<std::string> ("source"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = source.decode_hex (source_l);
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void oslo::receive_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, source.bytes.data (), sizeof (source.bytes));
}

oslo::block_details::block_details (oslo::epoch const epoch_a, bool const is_send_a, bool const is_receive_a, bool const is_epoch_a) :
epoch (epoch_a), is_send (is_send_a), is_receive (is_receive_a), is_epoch (is_epoch_a)
{
}

constexpr size_t oslo::block_details::size ()
{
	return 1;
}

bool oslo::block_details::operator== (oslo::block_details const & other_a) const
{
	return epoch == other_a.epoch && is_send == other_a.is_send && is_receive == other_a.is_receive && is_epoch == other_a.is_epoch;
}

uint8_t oslo::block_details::packed () const
{
	std::bitset<8> result (static_cast<uint8_t> (epoch));
	result.set (7, is_send);
	result.set (6, is_receive);
	result.set (5, is_epoch);
	return static_cast<uint8_t> (result.to_ulong ());
}

void oslo::block_details::unpack (uint8_t details_a)
{
	constexpr std::bitset<8> epoch_mask{ 0b00011111 };
	auto as_bitset = static_cast<std::bitset<8>> (details_a);
	is_send = as_bitset.test (7);
	is_receive = as_bitset.test (6);
	is_epoch = as_bitset.test (5);
	epoch = static_cast<oslo::epoch> ((as_bitset & epoch_mask).to_ulong ());
}

void oslo::block_details::serialize (oslo::stream & stream_a) const
{
	oslo::write (stream_a, packed ());
}

bool oslo::block_details::deserialize (oslo::stream & stream_a)
{
	bool result (false);
	try
	{
		uint8_t packed{ 0 };
		oslo::read (stream_a, packed);
		unpack (packed);
	}
	catch (std::runtime_error &)
	{
		result = true;
	}

	return result;
}

std::string oslo::state_subtype (oslo::block_details const details_a)
{
	debug_assert (details_a.is_epoch + details_a.is_receive + details_a.is_send <= 1);
	if (details_a.is_send)
	{
		return "send";
	}
	else if (details_a.is_receive)
	{
		return "receive";
	}
	else if (details_a.is_epoch)
	{
		return "epoch";
	}
	else
	{
		return "change";
	}
}

oslo::block_sideband::block_sideband (oslo::account const & account_a, oslo::block_hash const & successor_a, oslo::amount const & balance_a, uint64_t height_a, uint64_t timestamp_a, oslo::block_details const & details_a) :
successor (successor_a),
account (account_a),
balance (balance_a),
height (height_a),
timestamp (timestamp_a),
details (details_a)
{
}

oslo::block_sideband::block_sideband (oslo::account const & account_a, oslo::block_hash const & successor_a, oslo::amount const & balance_a, uint64_t height_a, uint64_t timestamp_a, oslo::epoch epoch_a, bool is_send, bool is_receive, bool is_epoch) :
successor (successor_a),
account (account_a),
balance (balance_a),
height (height_a),
timestamp (timestamp_a),
details (epoch_a, is_send, is_receive, is_epoch)
{
}

size_t oslo::block_sideband::size (oslo::block_type type_a)
{
	size_t result (0);
	result += sizeof (successor);
	if (type_a != oslo::block_type::state && type_a != oslo::block_type::open)
	{
		result += sizeof (account);
	}
	if (type_a != oslo::block_type::open)
	{
		result += sizeof (height);
	}
	if (type_a == oslo::block_type::receive || type_a == oslo::block_type::change || type_a == oslo::block_type::open)
	{
		result += sizeof (balance);
	}
	result += sizeof (timestamp);
	if (type_a == oslo::block_type::state)
	{
		static_assert (sizeof (oslo::epoch) == oslo::block_details::size (), "block_details is larger than the epoch enum");
		result += oslo::block_details::size ();
	}
	return result;
}

void oslo::block_sideband::serialize (oslo::stream & stream_a, oslo::block_type type_a) const
{
	oslo::write (stream_a, successor.bytes);
	if (type_a != oslo::block_type::state && type_a != oslo::block_type::open)
	{
		oslo::write (stream_a, account.bytes);
	}
	if (type_a != oslo::block_type::open)
	{
		oslo::write (stream_a, boost::endian::native_to_big (height));
	}
	if (type_a == oslo::block_type::receive || type_a == oslo::block_type::change || type_a == oslo::block_type::open)
	{
		oslo::write (stream_a, balance.bytes);
	}
	oslo::write (stream_a, boost::endian::native_to_big (timestamp));
	if (type_a == oslo::block_type::state)
	{
		details.serialize (stream_a);
	}
}

bool oslo::block_sideband::deserialize (oslo::stream & stream_a, oslo::block_type type_a)
{
	bool result (false);
	try
	{
		oslo::read (stream_a, successor.bytes);
		if (type_a != oslo::block_type::state && type_a != oslo::block_type::open)
		{
			oslo::read (stream_a, account.bytes);
		}
		if (type_a != oslo::block_type::open)
		{
			oslo::read (stream_a, height);
			boost::endian::big_to_native_inplace (height);
		}
		else
		{
			height = 1;
		}
		if (type_a == oslo::block_type::receive || type_a == oslo::block_type::change || type_a == oslo::block_type::open)
		{
			oslo::read (stream_a, balance.bytes);
		}
		oslo::read (stream_a, timestamp);
		boost::endian::big_to_native_inplace (timestamp);
		if (type_a == oslo::block_type::state)
		{
			result = details.deserialize (stream_a);
		}
	}
	catch (std::runtime_error &)
	{
		result = true;
	}

	return result;
}

std::shared_ptr<oslo::block> oslo::block_uniquer::unique (std::shared_ptr<oslo::block> block_a)
{
	auto result (block_a);
	if (result != nullptr)
	{
		oslo::uint256_union key (block_a->full_hash ());
		oslo::lock_guard<std::mutex> lock (mutex);
		auto & existing (blocks[key]);
		if (auto block_l = existing.lock ())
		{
			result = block_l;
		}
		else
		{
			existing = block_a;
		}
		release_assert (std::numeric_limits<CryptoPP::word32>::max () > blocks.size ());
		for (auto i (0); i < cleanup_count && !blocks.empty (); ++i)
		{
			auto random_offset (oslo::random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (blocks.size () - 1)));
			auto existing (std::next (blocks.begin (), random_offset));
			if (existing == blocks.end ())
			{
				existing = blocks.begin ();
			}
			if (existing != blocks.end ())
			{
				if (auto block_l = existing->second.lock ())
				{
					// Still live
				}
				else
				{
					blocks.erase (existing);
				}
			}
		}
	}
	return result;
}

size_t oslo::block_uniquer::size ()
{
	oslo::lock_guard<std::mutex> lock (mutex);
	return blocks.size ();
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (block_uniquer & block_uniquer, const std::string & name)
{
	auto count = block_uniquer.size ();
	auto sizeof_element = sizeof (block_uniquer::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocks", count, sizeof_element }));
	return composite;
}
