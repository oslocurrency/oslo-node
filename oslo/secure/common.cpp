#define IGNORE_GTEST_INCL
#include <oslo/core_test/testutil.hpp>
#include <oslo/crypto_lib/random_pool.hpp>
#include <oslo/lib/config.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/secure/blockstore.hpp>
#include <oslo/secure/common.hpp>

#include <crypto/cryptopp/words.h>

#include <boost/endian/conversion.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/variant/get.hpp>

#include <limits>
#include <queue>

#include <crypto/ed25519-donna/ed25519.h>

size_t constexpr oslo::send_block::size;
size_t constexpr oslo::receive_block::size;
size_t constexpr oslo::open_block::size;
size_t constexpr oslo::change_block::size;
size_t constexpr oslo::state_block::size;

oslo::oslo_networks oslo::network_constants::active_network = oslo::oslo_networks::ACTIVE_NETWORK;

namespace
{
char const * test_private_key_data = "34F0A37AAD20F4A260F0A5B3CB3D7FB50673212263E58A380BC10474BB039CE4";
char const * test_public_key_data = "B0311EA55708D6A53C75CDBF88300259C6D018522FE3D4D0A242E431F9E8B6D0"; // oslo_3e3j5tkog48pnny9dmfzj1r16pg8t1e76dz5tmac6iq689wyjfpiij4txtdo
char const * beta_public_key_data = "259A44787AE60E2E241C01BE4CB72F6BBB5B4552F8D0AC23477AB2560D3FA138"; // oslo_1betajw9osig7rk3r1fybkukytxudf4o7y8ioijngyokcr8mzabrup3jcktw
char const * live_public_key_data = "658C588F0EF8884530A3973E70AED17C9C6C76DEEC9C4D18571A875715D36460"; // oslo_1seed49ixy6aanrc97syg4qf4z6wfjufxu6wbne7g8n9cwcx8s51eiz1jgaa
char const * test_genesis_data = R"%%%({
	"type": "open",
	"source": "B0311EA55708D6A53C75CDBF88300259C6D018522FE3D4D0A242E431F9E8B6D0",
	"representative": "oslo_3e3j5tkog48pnny9dmfzj1r16pg8t1e76dz5tmac6iq689wyjfpiij4txtdo",
	"account": "oslo_3e3j5tkog48pnny9dmfzj1r16pg8t1e76dz5tmac6iq689wyjfpiij4txtdo",
	"work": "7b42a00ee91d5810",
	"signature": "ECDA914373A2F0CA1296475BAEE40500A7F0A7AD72A5A80C81D7FAB7F6C802B2CC7DB50F5DD0FB25B2EF11761FA7344A158DD5A700B21BD47DE5BD0F63153A02"
	})%%%";

char const * beta_genesis_data = R"%%%({
	"type": "open",
	"source": "259A4394DB16B1FFE5568476655E844BA59E8EB5D222F20D42E50684D0C16B54",
	"representative": "oslo_1betajw9osig7rk3r1fybkukytxudf4o7y8ioijngyokcr8mzabrup3jcktw",
	"account": "oslo_1betajw9osig7rk3r1fybkukytxudf4o7y8ioijngyokcr8mzabrup3jcktw",
	"work": "7f5c2eb5e2658e81",
	"signature": "DB9EFAC98A28EEA048E722F91C2A2720E1D8EF2A81453C80FC53B453180C0A264CE021D38D5B4540B1BBB0C378B80F2DF7389027593C08DDEF9F47934B9CF805"
	})%%%";

char const * live_genesis_data = R"%%%({
    "type": "open",
    "source": "658C588F0EF8884530A3973E70AED17C9C6C76DEEC9C4D18571A875715D36460",
    "representative": "nano_1seed49ixy6aanrc97syg4qf4z6wfjufxu6wbne7g8n9cwcx8s51eiz1jgaa",
    "account": "nano_1seed49ixy6aanrc97syg4qf4z6wfjufxu6wbne7g8n9cwcx8s51eiz1jgaa",
    "work": "b3b778577e923c35",
    "signature": "867662FE7CF58F29E75E8DFA9EF15FDD0D47481F694AA080F5B36E0DCCF80EAA6DF656F00054212F4E8107F30FB112428B4CB81516B0EDEC14193D21191A6209"
	})%%%";

std::shared_ptr<oslo::block> parse_block_from_genesis_data (std::string const & genesis_data_a)
{
	boost::property_tree::ptree tree;
	std::stringstream istream (genesis_data_a);
	boost::property_tree::read_json (istream, tree);
	return oslo::deserialize_block_json (tree);
}
}

oslo::network_params::network_params () :
network_params (network_constants::active_network)
{
}

oslo::network_params::network_params (oslo::oslo_networks network_a) :
network (network_a), ledger (network), voting (network), node (network), portmapping (network), bootstrap (network)
{
	unsigned constexpr kdf_full_work = 64 * 1024;
	unsigned constexpr kdf_test_work = 8;
	kdf_work = network.is_test_network () ? kdf_test_work : kdf_full_work;
	header_magic_number = network.is_test_network () ? std::array<uint8_t, 2>{ { 'R', 'A' } } : network.is_beta_network () ? std::array<uint8_t, 2>{ { 'N', 'D' } } : std::array<uint8_t, 2>{ { 'R', 'C' } };
}

uint8_t oslo::protocol_constants::protocol_version_min (bool use_epoch_2_min_version_a) const
{
	return use_epoch_2_min_version_a ? protocol_version_min_epoch_2 : protocol_version_min_pre_epoch_2;
}

oslo::ledger_constants::ledger_constants (oslo::network_constants & network_constants) :
ledger_constants (network_constants.network ())
{
}

oslo::ledger_constants::ledger_constants (oslo::oslo_networks network_a) :
zero_key ("0"),
test_genesis_key (test_private_key_data),
oslo_test_account (test_public_key_data),
oslo_beta_account (beta_public_key_data),
oslo_live_account (live_public_key_data),
oslo_test_genesis (test_genesis_data),
oslo_beta_genesis (beta_genesis_data),
oslo_live_genesis (live_genesis_data),
genesis_account (network_a == oslo::oslo_networks::oslo_test_network ? oslo_test_account : network_a == oslo::oslo_networks::oslo_beta_network ? oslo_beta_account : oslo_live_account),
genesis_block (network_a == oslo::oslo_networks::oslo_test_network ? oslo_test_genesis : network_a == oslo::oslo_networks::oslo_beta_network ? oslo_beta_genesis : oslo_live_genesis),
genesis_hash (parse_block_from_genesis_data (genesis_block)->hash ()),
genesis_amount (std::numeric_limits<oslo::uint128_t>::max ()),
burn_account (0)
{
	oslo::link epoch_link_v1;
	const char * epoch_message_v1 ("epoch v1 block");
	strncpy ((char *)epoch_link_v1.bytes.data (), epoch_message_v1, epoch_link_v1.bytes.size ());
	epochs.add (oslo::epoch::epoch_1, genesis_account, epoch_link_v1);

	oslo::link epoch_link_v2;
	oslo::account oslo_live_epoch_v2_signer;
	auto error (oslo_live_epoch_v2_signer.decode_account ("oslo_3qb6o6i1tkzr6jwr5s7eehfxwg9x6eemitdinbpi7u8bjjwsgqfj4wzser3x"));
	debug_assert (!error);
	auto epoch_v2_signer (network_a == oslo::oslo_networks::oslo_test_network ? oslo_test_account : network_a == oslo::oslo_networks::oslo_beta_network ? oslo_beta_account : oslo_live_epoch_v2_signer);
	const char * epoch_message_v2 ("epoch v2 block");
	strncpy ((char *)epoch_link_v2.bytes.data (), epoch_message_v2, epoch_link_v2.bytes.size ());
	epochs.add (oslo::epoch::epoch_2, epoch_v2_signer, epoch_link_v2);
}

oslo::random_constants::random_constants ()
{
	oslo::random_pool::generate_block (not_an_account.bytes.data (), not_an_account.bytes.size ());
	oslo::random_pool::generate_block (random_128.bytes.data (), random_128.bytes.size ());
}

oslo::node_constants::node_constants (oslo::network_constants & network_constants)
{
	period = network_constants.is_test_network () ? std::chrono::seconds (1) : std::chrono::seconds (60);
	half_period = network_constants.is_test_network () ? std::chrono::milliseconds (500) : std::chrono::milliseconds (30 * 1000);
	idle_timeout = network_constants.is_test_network () ? period * 15 : period * 2;
	cutoff = period * 5;
	syn_cookie_cutoff = std::chrono::seconds (5);
	backup_interval = std::chrono::minutes (5);
	search_pending_interval = network_constants.is_test_network () ? std::chrono::seconds (1) : std::chrono::seconds (5 * 60);
	peer_interval = search_pending_interval;
	unchecked_cleaning_interval = std::chrono::minutes (30);
	process_confirmed_interval = network_constants.is_test_network () ? std::chrono::milliseconds (50) : std::chrono::milliseconds (500);
	max_peers_per_ip = network_constants.is_test_network () ? 10 : 5;
	max_weight_samples = network_constants.is_live_network () ? 4032 : 864;
	weight_period = 5 * 60; // 5 minutes
}

oslo::voting_constants::voting_constants (oslo::network_constants & network_constants)
{
	max_cache = network_constants.is_test_network () ? 2 : 64 * 1024;
}

oslo::portmapping_constants::portmapping_constants (oslo::network_constants & network_constants)
{
	mapping_timeout = network_constants.is_test_network () ? 53 : 3593;
	check_timeout = network_constants.is_test_network () ? 17 : 53;
}

oslo::bootstrap_constants::bootstrap_constants (oslo::network_constants & network_constants)
{
	lazy_max_pull_blocks = network_constants.is_test_network () ? 2 : 512;
	lazy_min_pull_blocks = network_constants.is_test_network () ? 1 : 32;
	frontier_retry_limit = network_constants.is_test_network () ? 2 : 16;
	lazy_retry_limit = network_constants.is_test_network () ? 2 : frontier_retry_limit * 10;
	lazy_destinations_retry_limit = network_constants.is_test_network () ? 1 : frontier_retry_limit / 4;
	gap_cache_bootstrap_start_interval = network_constants.is_test_network () ? std::chrono::milliseconds (5) : std::chrono::milliseconds (30 * 1000);
}

/* Convenience constants for core_test which is always on the test network */
namespace
{
oslo::ledger_constants test_constants (oslo::oslo_networks::oslo_test_network);
}

oslo::keypair const & oslo::zero_key (test_constants.zero_key);
oslo::keypair const & oslo::test_genesis_key (test_constants.test_genesis_key);
oslo::account const & oslo::oslo_test_account (test_constants.oslo_test_account);
std::string const & oslo::oslo_test_genesis (test_constants.oslo_test_genesis);
oslo::account const & oslo::genesis_account (test_constants.genesis_account);
oslo::block_hash const & oslo::genesis_hash (test_constants.genesis_hash);
oslo::uint128_t const & oslo::genesis_amount (test_constants.genesis_amount);
oslo::account const & oslo::burn_account (test_constants.burn_account);

// Create a new random keypair
oslo::keypair::keypair ()
{
	random_pool::generate_block (prv.data.bytes.data (), prv.data.bytes.size ());
	ed25519_publickey (prv.data.bytes.data (), pub.bytes.data ());
}

// Create a keypair given a private key
oslo::keypair::keypair (oslo::raw_key && prv_a) :
prv (std::move (prv_a))
{
	ed25519_publickey (prv.data.bytes.data (), pub.bytes.data ());
}

// Create a keypair given a hex string of the private key
oslo::keypair::keypair (std::string const & prv_a)
{
	auto error (prv.data.decode_hex (prv_a));
	(void)error;
	debug_assert (!error);
	ed25519_publickey (prv.data.bytes.data (), pub.bytes.data ());
}

// Serialize a block prefixed with an 8-bit typecode
void oslo::serialize_block (oslo::stream & stream_a, oslo::block const & block_a)
{
	write (stream_a, block_a.type ());
	block_a.serialize (stream_a);
}

oslo::account_info::account_info (oslo::block_hash const & head_a, oslo::account const & representative_a, oslo::block_hash const & open_block_a, oslo::amount const & balance_a, uint64_t modified_a, uint64_t block_count_a, oslo::epoch epoch_a) :
head (head_a),
representative (representative_a),
open_block (open_block_a),
balance (balance_a),
modified (modified_a),
block_count (block_count_a),
epoch_m (epoch_a)
{
}

bool oslo::account_info::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		oslo::read (stream_a, head.bytes);
		oslo::read (stream_a, representative.bytes);
		oslo::read (stream_a, open_block.bytes);
		oslo::read (stream_a, balance.bytes);
		oslo::read (stream_a, modified);
		oslo::read (stream_a, block_count);
		oslo::read (stream_a, epoch_m);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

bool oslo::account_info::operator== (oslo::account_info const & other_a) const
{
	return head == other_a.head && representative == other_a.representative && open_block == other_a.open_block && balance == other_a.balance && modified == other_a.modified && block_count == other_a.block_count && epoch () == other_a.epoch ();
}

bool oslo::account_info::operator!= (oslo::account_info const & other_a) const
{
	return !(*this == other_a);
}

size_t oslo::account_info::db_size () const
{
	debug_assert (reinterpret_cast<const uint8_t *> (this) == reinterpret_cast<const uint8_t *> (&head));
	debug_assert (reinterpret_cast<const uint8_t *> (&head) + sizeof (head) == reinterpret_cast<const uint8_t *> (&representative));
	debug_assert (reinterpret_cast<const uint8_t *> (&representative) + sizeof (representative) == reinterpret_cast<const uint8_t *> (&open_block));
	debug_assert (reinterpret_cast<const uint8_t *> (&open_block) + sizeof (open_block) == reinterpret_cast<const uint8_t *> (&balance));
	debug_assert (reinterpret_cast<const uint8_t *> (&balance) + sizeof (balance) == reinterpret_cast<const uint8_t *> (&modified));
	debug_assert (reinterpret_cast<const uint8_t *> (&modified) + sizeof (modified) == reinterpret_cast<const uint8_t *> (&block_count));
	debug_assert (reinterpret_cast<const uint8_t *> (&block_count) + sizeof (block_count) == reinterpret_cast<const uint8_t *> (&epoch_m));
	return sizeof (head) + sizeof (representative) + sizeof (open_block) + sizeof (balance) + sizeof (modified) + sizeof (block_count) + sizeof (epoch_m);
}

oslo::epoch oslo::account_info::epoch () const
{
	return epoch_m;
}

size_t oslo::block_counts::sum () const
{
	return send + receive + open + change + state;
}

oslo::pending_info::pending_info (oslo::account const & source_a, oslo::amount const & amount_a, oslo::epoch epoch_a) :
source (source_a),
amount (amount_a),
epoch (epoch_a)
{
}

bool oslo::pending_info::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		oslo::read (stream_a, source.bytes);
		oslo::read (stream_a, amount.bytes);
		oslo::read (stream_a, epoch);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

size_t oslo::pending_info::db_size () const
{
	return sizeof (source) + sizeof (amount) + sizeof (epoch);
}

bool oslo::pending_info::operator== (oslo::pending_info const & other_a) const
{
	return source == other_a.source && amount == other_a.amount && epoch == other_a.epoch;
}

oslo::pending_key::pending_key (oslo::account const & account_a, oslo::block_hash const & hash_a) :
account (account_a),
hash (hash_a)
{
}

bool oslo::pending_key::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		oslo::read (stream_a, account.bytes);
		oslo::read (stream_a, hash.bytes);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

bool oslo::pending_key::operator== (oslo::pending_key const & other_a) const
{
	return account == other_a.account && hash == other_a.hash;
}

oslo::account const & oslo::pending_key::key () const
{
	return account;
}

oslo::unchecked_info::unchecked_info (std::shared_ptr<oslo::block> block_a, oslo::account const & account_a, uint64_t modified_a, oslo::signature_verification verified_a, bool confirmed_a) :
block (block_a),
account (account_a),
modified (modified_a),
verified (verified_a),
confirmed (confirmed_a)
{
}

void oslo::unchecked_info::serialize (oslo::stream & stream_a) const
{
	debug_assert (block != nullptr);
	oslo::serialize_block (stream_a, *block);
	oslo::write (stream_a, account.bytes);
	oslo::write (stream_a, modified);
	oslo::write (stream_a, verified);
}

bool oslo::unchecked_info::deserialize (oslo::stream & stream_a)
{
	block = oslo::deserialize_block (stream_a);
	bool error (block == nullptr);
	if (!error)
	{
		try
		{
			oslo::read (stream_a, account.bytes);
			oslo::read (stream_a, modified);
			oslo::read (stream_a, verified);
		}
		catch (std::runtime_error const &)
		{
			error = true;
		}
	}
	return error;
}

oslo::endpoint_key::endpoint_key (const std::array<uint8_t, 16> & address_a, uint16_t port_a) :
address (address_a), network_port (boost::endian::native_to_big (port_a))
{
}

const std::array<uint8_t, 16> & oslo::endpoint_key::address_bytes () const
{
	return address;
}

uint16_t oslo::endpoint_key::port () const
{
	return boost::endian::big_to_native (network_port);
}

oslo::confirmation_height_info::confirmation_height_info (uint64_t confirmation_height_a, oslo::block_hash const & confirmed_frontier_a) :
height (confirmation_height_a),
frontier (confirmed_frontier_a)
{
}

void oslo::confirmation_height_info::serialize (oslo::stream & stream_a) const
{
	oslo::write (stream_a, height);
	oslo::write (stream_a, frontier);
}

bool oslo::confirmation_height_info::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		oslo::read (stream_a, height);
		oslo::read (stream_a, frontier);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

oslo::block_info::block_info (oslo::account const & account_a, oslo::amount const & balance_a) :
account (account_a),
balance (balance_a)
{
}

bool oslo::vote::operator== (oslo::vote const & other_a) const
{
	auto blocks_equal (true);
	if (blocks.size () != other_a.blocks.size ())
	{
		blocks_equal = false;
	}
	else
	{
		for (auto i (0); blocks_equal && i < blocks.size (); ++i)
		{
			auto block (blocks[i]);
			auto other_block (other_a.blocks[i]);
			if (block.which () != other_block.which ())
			{
				blocks_equal = false;
			}
			else if (block.which ())
			{
				if (boost::get<oslo::block_hash> (block) != boost::get<oslo::block_hash> (other_block))
				{
					blocks_equal = false;
				}
			}
			else
			{
				if (!(*boost::get<std::shared_ptr<oslo::block>> (block) == *boost::get<std::shared_ptr<oslo::block>> (other_block)))
				{
					blocks_equal = false;
				}
			}
		}
	}
	return sequence == other_a.sequence && blocks_equal && account == other_a.account && signature == other_a.signature;
}

bool oslo::vote::operator!= (oslo::vote const & other_a) const
{
	return !(*this == other_a);
}

void oslo::vote::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("account", account.to_account ());
	tree.put ("signature", signature.number ());
	tree.put ("sequence", std::to_string (sequence));
	boost::property_tree::ptree blocks_tree;
	for (auto block : blocks)
	{
		boost::property_tree::ptree entry;
		if (block.which ())
		{
			entry.put ("", boost::get<oslo::block_hash> (block).to_string ());
		}
		else
		{
			entry.put ("", boost::get<std::shared_ptr<oslo::block>> (block)->hash ().to_string ());
		}
		blocks_tree.push_back (std::make_pair ("", entry));
	}
	tree.add_child ("blocks", blocks_tree);
}

std::string oslo::vote::to_json () const
{
	std::stringstream stream;
	boost::property_tree::ptree tree;
	serialize_json (tree);
	boost::property_tree::write_json (stream, tree);
	return stream.str ();
}

oslo::vote::vote (oslo::vote const & other_a) :
sequence (other_a.sequence),
blocks (other_a.blocks),
account (other_a.account),
signature (other_a.signature)
{
}

oslo::vote::vote (bool & error_a, oslo::stream & stream_a, oslo::block_uniquer * uniquer_a)
{
	error_a = deserialize (stream_a, uniquer_a);
}

oslo::vote::vote (bool & error_a, oslo::stream & stream_a, oslo::block_type type_a, oslo::block_uniquer * uniquer_a)
{
	try
	{
		oslo::read (stream_a, account.bytes);
		oslo::read (stream_a, signature.bytes);
		oslo::read (stream_a, sequence);

		while (stream_a.in_avail () > 0)
		{
			if (type_a == oslo::block_type::not_a_block)
			{
				oslo::block_hash block_hash;
				oslo::read (stream_a, block_hash);
				blocks.push_back (block_hash);
			}
			else
			{
				std::shared_ptr<oslo::block> block (oslo::deserialize_block (stream_a, type_a, uniquer_a));
				if (block == nullptr)
				{
					throw std::runtime_error ("Block is null");
				}
				blocks.push_back (block);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}

	if (blocks.empty ())
	{
		error_a = true;
	}
}

oslo::vote::vote (oslo::account const & account_a, oslo::raw_key const & prv_a, uint64_t sequence_a, std::shared_ptr<oslo::block> block_a) :
sequence (sequence_a),
blocks (1, block_a),
account (account_a),
signature (oslo::sign_message (prv_a, account_a, hash ()))
{
}

oslo::vote::vote (oslo::account const & account_a, oslo::raw_key const & prv_a, uint64_t sequence_a, std::vector<oslo::block_hash> const & blocks_a) :
sequence (sequence_a),
account (account_a)
{
	debug_assert (!blocks_a.empty ());
	debug_assert (blocks_a.size () <= 12);
	blocks.reserve (blocks_a.size ());
	std::copy (blocks_a.cbegin (), blocks_a.cend (), std::back_inserter (blocks));
	signature = oslo::sign_message (prv_a, account_a, hash ());
}

std::string oslo::vote::hashes_string () const
{
	std::string result;
	for (auto hash : *this)
	{
		result += hash.to_string ();
		result += ", ";
	}
	return result;
}

const std::string oslo::vote::hash_prefix = "vote ";

oslo::block_hash oslo::vote::hash () const
{
	oslo::block_hash result;
	blake2b_state hash;
	blake2b_init (&hash, sizeof (result.bytes));
	if (blocks.size () > 1 || (!blocks.empty () && blocks.front ().which ()))
	{
		blake2b_update (&hash, hash_prefix.data (), hash_prefix.size ());
	}
	for (auto block_hash : *this)
	{
		blake2b_update (&hash, block_hash.bytes.data (), sizeof (block_hash.bytes));
	}
	union
	{
		uint64_t qword;
		std::array<uint8_t, 8> bytes;
	};
	qword = sequence;
	blake2b_update (&hash, bytes.data (), sizeof (bytes));
	blake2b_final (&hash, result.bytes.data (), sizeof (result.bytes));
	return result;
}

oslo::block_hash oslo::vote::full_hash () const
{
	oslo::block_hash result;
	blake2b_state state;
	blake2b_init (&state, sizeof (result.bytes));
	blake2b_update (&state, hash ().bytes.data (), sizeof (hash ().bytes));
	blake2b_update (&state, account.bytes.data (), sizeof (account.bytes.data ()));
	blake2b_update (&state, signature.bytes.data (), sizeof (signature.bytes.data ()));
	blake2b_final (&state, result.bytes.data (), sizeof (result.bytes));
	return result;
}

void oslo::vote::serialize (oslo::stream & stream_a, oslo::block_type type) const
{
	write (stream_a, account);
	write (stream_a, signature);
	write (stream_a, sequence);
	for (auto const & block : blocks)
	{
		if (block.which ())
		{
			debug_assert (type == oslo::block_type::not_a_block);
			write (stream_a, boost::get<oslo::block_hash> (block));
		}
		else
		{
			if (type == oslo::block_type::not_a_block)
			{
				write (stream_a, boost::get<std::shared_ptr<oslo::block>> (block)->hash ());
			}
			else
			{
				boost::get<std::shared_ptr<oslo::block>> (block)->serialize (stream_a);
			}
		}
	}
}

void oslo::vote::serialize (oslo::stream & stream_a) const
{
	write (stream_a, account);
	write (stream_a, signature);
	write (stream_a, sequence);
	for (auto const & block : blocks)
	{
		if (block.which ())
		{
			write (stream_a, oslo::block_type::not_a_block);
			write (stream_a, boost::get<oslo::block_hash> (block));
		}
		else
		{
			oslo::serialize_block (stream_a, *boost::get<std::shared_ptr<oslo::block>> (block));
		}
	}
}

bool oslo::vote::deserialize (oslo::stream & stream_a, oslo::block_uniquer * uniquer_a)
{
	auto error (false);
	try
	{
		oslo::read (stream_a, account);
		oslo::read (stream_a, signature);
		oslo::read (stream_a, sequence);

		oslo::block_type type;

		while (true)
		{
			if (oslo::try_read (stream_a, type))
			{
				// Reached the end of the stream
				break;
			}

			if (type == oslo::block_type::not_a_block)
			{
				oslo::block_hash block_hash;
				oslo::read (stream_a, block_hash);
				blocks.push_back (block_hash);
			}
			else
			{
				std::shared_ptr<oslo::block> block (oslo::deserialize_block (stream_a, type, uniquer_a));
				if (block == nullptr)
				{
					throw std::runtime_error ("Block is empty");
				}

				blocks.push_back (block);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	if (blocks.empty ())
	{
		error = true;
	}

	return error;
}

bool oslo::vote::validate () const
{
	return oslo::validate_message (account, hash (), signature);
}

oslo::block_hash oslo::iterate_vote_blocks_as_hash::operator() (boost::variant<std::shared_ptr<oslo::block>, oslo::block_hash> const & item) const
{
	oslo::block_hash result;
	if (item.which ())
	{
		result = boost::get<oslo::block_hash> (item);
	}
	else
	{
		result = boost::get<std::shared_ptr<oslo::block>> (item)->hash ();
	}
	return result;
}

boost::transform_iterator<oslo::iterate_vote_blocks_as_hash, oslo::vote_blocks_vec_iter> oslo::vote::begin () const
{
	return boost::transform_iterator<oslo::iterate_vote_blocks_as_hash, oslo::vote_blocks_vec_iter> (blocks.begin (), oslo::iterate_vote_blocks_as_hash ());
}

boost::transform_iterator<oslo::iterate_vote_blocks_as_hash, oslo::vote_blocks_vec_iter> oslo::vote::end () const
{
	return boost::transform_iterator<oslo::iterate_vote_blocks_as_hash, oslo::vote_blocks_vec_iter> (blocks.end (), oslo::iterate_vote_blocks_as_hash ());
}

oslo::vote_uniquer::vote_uniquer (oslo::block_uniquer & uniquer_a) :
uniquer (uniquer_a)
{
}

std::shared_ptr<oslo::vote> oslo::vote_uniquer::unique (std::shared_ptr<oslo::vote> vote_a)
{
	auto result (vote_a);
	if (result != nullptr && !result->blocks.empty ())
	{
		if (!result->blocks.front ().which ())
		{
			result->blocks.front () = uniquer.unique (boost::get<std::shared_ptr<oslo::block>> (result->blocks.front ()));
		}
		oslo::block_hash key (vote_a->full_hash ());
		oslo::lock_guard<std::mutex> lock (mutex);
		auto & existing (votes[key]);
		if (auto block_l = existing.lock ())
		{
			result = block_l;
		}
		else
		{
			existing = vote_a;
		}

		release_assert (std::numeric_limits<CryptoPP::word32>::max () > votes.size ());
		for (auto i (0); i < cleanup_count && !votes.empty (); ++i)
		{
			auto random_offset = oslo::random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (votes.size () - 1));

			auto existing (std::next (votes.begin (), random_offset));
			if (existing == votes.end ())
			{
				existing = votes.begin ();
			}
			if (existing != votes.end ())
			{
				if (auto block_l = existing->second.lock ())
				{
					// Still live
				}
				else
				{
					votes.erase (existing);
				}
			}
		}
	}
	return result;
}

size_t oslo::vote_uniquer::size ()
{
	oslo::lock_guard<std::mutex> lock (mutex);
	return votes.size ();
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (vote_uniquer & vote_uniquer, const std::string & name)
{
	auto count = vote_uniquer.size ();
	auto sizeof_element = sizeof (vote_uniquer::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "votes", count, sizeof_element }));
	return composite;
}

oslo::genesis::genesis ()
{
	static oslo::network_params network_params;
	open = parse_block_from_genesis_data (network_params.ledger.genesis_block);
	debug_assert (open != nullptr);
}

oslo::block_hash oslo::genesis::hash () const
{
	return open->hash ();
}

oslo::wallet_id oslo::random_wallet_id ()
{
	oslo::wallet_id wallet_id;
	oslo::uint256_union dummy_secret;
	random_pool::generate_block (dummy_secret.bytes.data (), dummy_secret.bytes.size ());
	ed25519_publickey (dummy_secret.bytes.data (), wallet_id.bytes.data ());
	return wallet_id;
}

oslo::unchecked_key::unchecked_key (oslo::block_hash const & previous_a, oslo::block_hash const & hash_a) :
previous (previous_a),
hash (hash_a)
{
}

bool oslo::unchecked_key::deserialize (oslo::stream & stream_a)
{
	auto error (false);
	try
	{
		oslo::read (stream_a, previous.bytes);
		oslo::read (stream_a, hash.bytes);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

bool oslo::unchecked_key::operator== (oslo::unchecked_key const & other_a) const
{
	return previous == other_a.previous && hash == other_a.hash;
}

oslo::block_hash const & oslo::unchecked_key::key () const
{
	return previous;
}

void oslo::generate_cache::enable_all ()
{
	reps = true;
	cemented_count = true;
	unchecked_count = true;
	account_count = true;
	epoch_2 = true;
}
