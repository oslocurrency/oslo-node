#include <oslo/crypto_lib/random_pool.hpp>
#include <oslo/lib/threading.hpp>
#include <oslo/lib/utility.hpp>
#include <oslo/node/election.hpp>
#include <oslo/node/lmdb/lmdb_iterator.hpp>
#include <oslo/node/node.hpp>
#include <oslo/node/wallet.hpp>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/polymorphic_cast.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <future>

#include <argon2.h>

oslo::uint256_union oslo::wallet_store::check (oslo::transaction const & transaction_a)
{
	oslo::wallet_value value (entry_get_raw (transaction_a, oslo::wallet_store::check_special));
	return value.key;
}

oslo::uint256_union oslo::wallet_store::salt (oslo::transaction const & transaction_a)
{
	oslo::wallet_value value (entry_get_raw (transaction_a, oslo::wallet_store::salt_special));
	return value.key;
}

void oslo::wallet_store::wallet_key (oslo::raw_key & prv_a, oslo::transaction const & transaction_a)
{
	oslo::lock_guard<std::recursive_mutex> lock (mutex);
	oslo::raw_key wallet_l;
	wallet_key_mem.value (wallet_l);
	oslo::raw_key password_l;
	password.value (password_l);
	prv_a.decrypt (wallet_l.data, password_l, salt (transaction_a).owords[0]);
}

void oslo::wallet_store::seed (oslo::raw_key & prv_a, oslo::transaction const & transaction_a)
{
	oslo::wallet_value value (entry_get_raw (transaction_a, oslo::wallet_store::seed_special));
	oslo::raw_key password_l;
	wallet_key (password_l, transaction_a);
	prv_a.decrypt (value.key, password_l, salt (transaction_a).owords[seed_iv_index]);
}

void oslo::wallet_store::seed_set (oslo::transaction const & transaction_a, oslo::raw_key const & prv_a)
{
	oslo::raw_key password_l;
	wallet_key (password_l, transaction_a);
	oslo::uint256_union ciphertext;
	ciphertext.encrypt (prv_a, password_l, salt (transaction_a).owords[seed_iv_index]);
	entry_put_raw (transaction_a, oslo::wallet_store::seed_special, oslo::wallet_value (ciphertext, 0));
	deterministic_clear (transaction_a);
}

oslo::public_key oslo::wallet_store::deterministic_insert (oslo::transaction const & transaction_a)
{
	auto index (deterministic_index_get (transaction_a));
	auto prv = deterministic_key (transaction_a, index);
	oslo::public_key result (oslo::pub_key (prv));
	while (exists (transaction_a, result))
	{
		++index;
		prv = deterministic_key (transaction_a, index);
		result = oslo::pub_key (prv);
	}
	uint64_t marker (1);
	marker <<= 32;
	marker |= index;
	entry_put_raw (transaction_a, result, oslo::wallet_value (oslo::uint256_union (marker), 0));
	++index;
	deterministic_index_set (transaction_a, index);
	return result;
}

oslo::public_key oslo::wallet_store::deterministic_insert (oslo::transaction const & transaction_a, uint32_t const index)
{
	auto prv = deterministic_key (transaction_a, index);
	oslo::public_key result (oslo::pub_key (prv));
	uint64_t marker (1);
	marker <<= 32;
	marker |= index;
	entry_put_raw (transaction_a, result, oslo::wallet_value (oslo::uint256_union (marker), 0));
	return result;
}

oslo::private_key oslo::wallet_store::deterministic_key (oslo::transaction const & transaction_a, uint32_t index_a)
{
	debug_assert (valid_password (transaction_a));
	oslo::raw_key seed_l;
	seed (seed_l, transaction_a);
	return oslo::deterministic_key (seed_l, index_a);
}

uint32_t oslo::wallet_store::deterministic_index_get (oslo::transaction const & transaction_a)
{
	oslo::wallet_value value (entry_get_raw (transaction_a, oslo::wallet_store::deterministic_index_special));
	return static_cast<uint32_t> (value.key.number () & static_cast<uint32_t> (-1));
}

void oslo::wallet_store::deterministic_index_set (oslo::transaction const & transaction_a, uint32_t index_a)
{
	oslo::uint256_union index_l (index_a);
	oslo::wallet_value value (index_l, 0);
	entry_put_raw (transaction_a, oslo::wallet_store::deterministic_index_special, value);
}

void oslo::wallet_store::deterministic_clear (oslo::transaction const & transaction_a)
{
	oslo::uint256_union key (0);
	for (auto i (begin (transaction_a)), n (end ()); i != n;)
	{
		switch (key_type (oslo::wallet_value (i->second)))
		{
			case oslo::key_type::deterministic:
			{
				auto const & key (i->first);
				erase (transaction_a, key);
				i = begin (transaction_a, key);
				break;
			}
			default:
			{
				++i;
				break;
			}
		}
	}
	deterministic_index_set (transaction_a, 0);
}

bool oslo::wallet_store::valid_password (oslo::transaction const & transaction_a)
{
	oslo::raw_key zero;
	zero.data.clear ();
	oslo::raw_key wallet_key_l;
	wallet_key (wallet_key_l, transaction_a);
	oslo::uint256_union check_l;
	check_l.encrypt (zero, wallet_key_l, salt (transaction_a).owords[check_iv_index]);
	bool ok = check (transaction_a) == check_l;
	return ok;
}

bool oslo::wallet_store::attempt_password (oslo::transaction const & transaction_a, std::string const & password_a)
{
	bool result = false;
	{
		oslo::lock_guard<std::recursive_mutex> lock (mutex);
		oslo::raw_key password_l;
		derive_key (password_l, transaction_a, password_a);
		password.value_set (password_l);
		result = !valid_password (transaction_a);
	}
	if (!result)
	{
		switch (version (transaction_a))
		{
			case version_1:
				upgrade_v1_v2 (transaction_a);
			case version_2:
				upgrade_v2_v3 (transaction_a);
			case version_3:
				upgrade_v3_v4 (transaction_a);
			case version_4:
				break;
			default:
				debug_assert (false);
		}
	}
	return result;
}

bool oslo::wallet_store::rekey (oslo::transaction const & transaction_a, std::string const & password_a)
{
	oslo::lock_guard<std::recursive_mutex> lock (mutex);
	bool result (false);
	if (valid_password (transaction_a))
	{
		oslo::raw_key password_new;
		derive_key (password_new, transaction_a, password_a);
		oslo::raw_key wallet_key_l;
		wallet_key (wallet_key_l, transaction_a);
		oslo::raw_key password_l;
		password.value (password_l);
		password.value_set (password_new);
		oslo::uint256_union encrypted;
		encrypted.encrypt (wallet_key_l, password_new, salt (transaction_a).owords[0]);
		oslo::raw_key wallet_enc;
		wallet_enc.data = encrypted;
		wallet_key_mem.value_set (wallet_enc);
		entry_put_raw (transaction_a, oslo::wallet_store::wallet_key_special, oslo::wallet_value (encrypted, 0));
	}
	else
	{
		result = true;
	}
	return result;
}

void oslo::wallet_store::derive_key (oslo::raw_key & prv_a, oslo::transaction const & transaction_a, std::string const & password_a)
{
	auto salt_l (salt (transaction_a));
	kdf.phs (prv_a, password_a, salt_l);
}

oslo::fan::fan (oslo::uint256_union const & key, size_t count_a)
{
	auto first (std::make_unique<oslo::uint256_union> (key));
	for (auto i (1); i < count_a; ++i)
	{
		auto entry (std::make_unique<oslo::uint256_union> ());
		oslo::random_pool::generate_block (entry->bytes.data (), entry->bytes.size ());
		*first ^= *entry;
		values.push_back (std::move (entry));
	}
	values.push_back (std::move (first));
}

void oslo::fan::value (oslo::raw_key & prv_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	value_get (prv_a);
}

void oslo::fan::value_get (oslo::raw_key & prv_a)
{
	debug_assert (!mutex.try_lock ());
	prv_a.data.clear ();
	for (auto & i : values)
	{
		prv_a.data ^= *i;
	}
}

void oslo::fan::value_set (oslo::raw_key const & value_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	oslo::raw_key value_l;
	value_get (value_l);
	*(values[0]) ^= value_l.data;
	*(values[0]) ^= value_a.data;
}

// Wallet version number
oslo::account const oslo::wallet_store::version_special (0);
// Random number used to salt private key encryption
oslo::account const oslo::wallet_store::salt_special (1);
// Key used to encrypt wallet keys, encrypted itself by the user password
oslo::account const oslo::wallet_store::wallet_key_special (2);
// Check value used to see if password is valid
oslo::account const oslo::wallet_store::check_special (3);
// Representative account to be used if we open a new account
oslo::account const oslo::wallet_store::representative_special (4);
// Wallet seed for deterministic key generation
oslo::account const oslo::wallet_store::seed_special (5);
// Current key index for deterministic keys
oslo::account const oslo::wallet_store::deterministic_index_special (6);
int const oslo::wallet_store::special_count (7);
size_t const oslo::wallet_store::check_iv_index (0);
size_t const oslo::wallet_store::seed_iv_index (1);

oslo::wallet_store::wallet_store (bool & init_a, oslo::kdf & kdf_a, oslo::transaction & transaction_a, oslo::account representative_a, unsigned fanout_a, std::string const & wallet_a, std::string const & json_a) :
password (0, fanout_a),
wallet_key_mem (0, fanout_a),
kdf (kdf_a)
{
	init_a = false;
	initialize (transaction_a, init_a, wallet_a);
	if (!init_a)
	{
		MDB_val junk;
		debug_assert (mdb_get (tx (transaction_a), handle, oslo::mdb_val (version_special), &junk) == MDB_NOTFOUND);
		boost::property_tree::ptree wallet_l;
		std::stringstream istream (json_a);
		try
		{
			boost::property_tree::read_json (istream, wallet_l);
		}
		catch (...)
		{
			init_a = true;
		}
		for (auto i (wallet_l.begin ()), n (wallet_l.end ()); i != n; ++i)
		{
			oslo::account key;
			init_a = key.decode_hex (i->first);
			if (!init_a)
			{
				oslo::uint256_union value;
				init_a = value.decode_hex (wallet_l.get<std::string> (i->first));
				if (!init_a)
				{
					entry_put_raw (transaction_a, key, oslo::wallet_value (value, 0));
				}
				else
				{
					init_a = true;
				}
			}
			else
			{
				init_a = true;
			}
		}
		init_a |= mdb_get (tx (transaction_a), handle, oslo::mdb_val (version_special), &junk) != 0;
		init_a |= mdb_get (tx (transaction_a), handle, oslo::mdb_val (wallet_key_special), &junk) != 0;
		init_a |= mdb_get (tx (transaction_a), handle, oslo::mdb_val (salt_special), &junk) != 0;
		init_a |= mdb_get (tx (transaction_a), handle, oslo::mdb_val (check_special), &junk) != 0;
		init_a |= mdb_get (tx (transaction_a), handle, oslo::mdb_val (representative_special), &junk) != 0;
		oslo::raw_key key;
		key.data.clear ();
		password.value_set (key);
		key.data = entry_get_raw (transaction_a, oslo::wallet_store::wallet_key_special).key;
		wallet_key_mem.value_set (key);
	}
}

oslo::wallet_store::wallet_store (bool & init_a, oslo::kdf & kdf_a, oslo::transaction & transaction_a, oslo::account representative_a, unsigned fanout_a, std::string const & wallet_a) :
password (0, fanout_a),
wallet_key_mem (0, fanout_a),
kdf (kdf_a)
{
	init_a = false;
	initialize (transaction_a, init_a, wallet_a);
	if (!init_a)
	{
		int version_status;
		MDB_val version_value;
		version_status = mdb_get (tx (transaction_a), handle, oslo::mdb_val (version_special), &version_value);
		if (version_status == MDB_NOTFOUND)
		{
			version_put (transaction_a, version_current);
			oslo::uint256_union salt_l;
			random_pool::generate_block (salt_l.bytes.data (), salt_l.bytes.size ());
			entry_put_raw (transaction_a, oslo::wallet_store::salt_special, oslo::wallet_value (salt_l, 0));
			// Wallet key is a fixed random key that encrypts all entries
			oslo::raw_key wallet_key;
			random_pool::generate_block (wallet_key.data.bytes.data (), sizeof (wallet_key.data.bytes));
			oslo::raw_key password_l;
			password_l.data.clear ();
			password.value_set (password_l);
			oslo::raw_key zero;
			zero.data.clear ();
			// Wallet key is encrypted by the user's password
			oslo::uint256_union encrypted;
			encrypted.encrypt (wallet_key, zero, salt_l.owords[0]);
			entry_put_raw (transaction_a, oslo::wallet_store::wallet_key_special, oslo::wallet_value (encrypted, 0));
			oslo::raw_key wallet_key_enc;
			wallet_key_enc.data = encrypted;
			wallet_key_mem.value_set (wallet_key_enc);
			oslo::uint256_union check;
			check.encrypt (zero, wallet_key, salt_l.owords[check_iv_index]);
			entry_put_raw (transaction_a, oslo::wallet_store::check_special, oslo::wallet_value (check, 0));
			entry_put_raw (transaction_a, oslo::wallet_store::representative_special, oslo::wallet_value (representative_a, 0));
			oslo::raw_key seed;
			random_pool::generate_block (seed.data.bytes.data (), seed.data.bytes.size ());
			seed_set (transaction_a, seed);
			entry_put_raw (transaction_a, oslo::wallet_store::deterministic_index_special, oslo::wallet_value (oslo::uint256_union (0), 0));
		}
	}
	oslo::raw_key key;
	key.data = entry_get_raw (transaction_a, oslo::wallet_store::wallet_key_special).key;
	wallet_key_mem.value_set (key);
}

std::vector<oslo::account> oslo::wallet_store::accounts (oslo::transaction const & transaction_a)
{
	std::vector<oslo::account> result;
	for (auto i (begin (transaction_a)), n (end ()); i != n; ++i)
	{
		oslo::account const & account (i->first);
		result.push_back (account);
	}
	return result;
}

void oslo::wallet_store::initialize (oslo::transaction const & transaction_a, bool & init_a, std::string const & path_a)
{
	debug_assert (strlen (path_a.c_str ()) == path_a.size ());
	auto error (0);
	MDB_dbi handle_l;
	error |= mdb_dbi_open (tx (transaction_a), path_a.c_str (), MDB_CREATE, &handle_l);
	handle = handle_l;
	init_a = error != 0;
}

bool oslo::wallet_store::is_representative (oslo::transaction const & transaction_a)
{
	return exists (transaction_a, representative (transaction_a));
}

void oslo::wallet_store::representative_set (oslo::transaction const & transaction_a, oslo::account const & representative_a)
{
	entry_put_raw (transaction_a, oslo::wallet_store::representative_special, oslo::wallet_value (representative_a, 0));
}

oslo::account oslo::wallet_store::representative (oslo::transaction const & transaction_a)
{
	oslo::wallet_value value (entry_get_raw (transaction_a, oslo::wallet_store::representative_special));
	return reinterpret_cast<oslo::account const &> (value.key);
}

oslo::public_key oslo::wallet_store::insert_adhoc (oslo::transaction const & transaction_a, oslo::raw_key const & prv)
{
	debug_assert (valid_password (transaction_a));
	oslo::public_key pub (oslo::pub_key (prv.as_private_key ()));
	oslo::raw_key password_l;
	wallet_key (password_l, transaction_a);
	oslo::private_key ciphertext;
	ciphertext.encrypt (prv, password_l, pub.owords[0].number ());
	entry_put_raw (transaction_a, pub, oslo::wallet_value (ciphertext, 0));
	return pub;
}

bool oslo::wallet_store::insert_watch (oslo::transaction const & transaction_a, oslo::account const & pub_a)
{
	bool error (!valid_public_key (pub_a));
	if (!error)
	{
		entry_put_raw (transaction_a, pub_a, oslo::wallet_value (oslo::private_key (0), 0));
	}
	return error;
}

void oslo::wallet_store::erase (oslo::transaction const & transaction_a, oslo::account const & pub)
{
	auto status (mdb_del (tx (transaction_a), handle, oslo::mdb_val (pub), nullptr));
	(void)status;
	debug_assert (status == 0);
}

oslo::wallet_value oslo::wallet_store::entry_get_raw (oslo::transaction const & transaction_a, oslo::account const & pub_a)
{
	oslo::wallet_value result;
	oslo::mdb_val value;
	auto status (mdb_get (tx (transaction_a), handle, oslo::mdb_val (pub_a), value));
	if (status == 0)
	{
		result = oslo::wallet_value (value);
	}
	else
	{
		result.key.clear ();
		result.work = 0;
	}
	return result;
}

void oslo::wallet_store::entry_put_raw (oslo::transaction const & transaction_a, oslo::account const & pub_a, oslo::wallet_value const & entry_a)
{
	auto status (mdb_put (tx (transaction_a), handle, oslo::mdb_val (pub_a), oslo::mdb_val (sizeof (entry_a), const_cast<oslo::wallet_value *> (&entry_a)), 0));
	(void)status;
	debug_assert (status == 0);
}

oslo::key_type oslo::wallet_store::key_type (oslo::wallet_value const & value_a)
{
	auto number (value_a.key.number ());
	oslo::key_type result;
	auto text (number.convert_to<std::string> ());
	if (number > std::numeric_limits<uint64_t>::max ())
	{
		result = oslo::key_type::adhoc;
	}
	else
	{
		if ((number >> 32).convert_to<uint32_t> () == 1)
		{
			result = oslo::key_type::deterministic;
		}
		else
		{
			result = oslo::key_type::unknown;
		}
	}
	return result;
}

bool oslo::wallet_store::fetch (oslo::transaction const & transaction_a, oslo::account const & pub, oslo::raw_key & prv)
{
	auto result (false);
	if (valid_password (transaction_a))
	{
		oslo::wallet_value value (entry_get_raw (transaction_a, pub));
		if (!value.key.is_zero ())
		{
			switch (key_type (value))
			{
				case oslo::key_type::deterministic:
				{
					oslo::raw_key seed_l;
					seed (seed_l, transaction_a);
					uint32_t index (static_cast<uint32_t> (value.key.number () & static_cast<uint32_t> (-1)));
					prv.data = deterministic_key (transaction_a, index);
					break;
				}
				case oslo::key_type::adhoc:
				{
					// Ad-hoc keys
					oslo::raw_key password_l;
					wallet_key (password_l, transaction_a);
					prv.decrypt (value.key, password_l, pub.owords[0].number ());
					break;
				}
				default:
				{
					result = true;
					break;
				}
			}
		}
		else
		{
			result = true;
		}
	}
	else
	{
		result = true;
	}
	if (!result)
	{
		oslo::public_key compare (oslo::pub_key (prv.as_private_key ()));
		if (!(pub == compare))
		{
			result = true;
		}
	}
	return result;
}

bool oslo::wallet_store::valid_public_key (oslo::public_key const & pub)
{
	return pub.number () >= special_count;
}

bool oslo::wallet_store::exists (oslo::transaction const & transaction_a, oslo::public_key const & pub)
{
	return valid_public_key (pub) && find (transaction_a, pub) != end ();
}

void oslo::wallet_store::serialize_json (oslo::transaction const & transaction_a, std::string & string_a)
{
	boost::property_tree::ptree tree;
	for (oslo::store_iterator<oslo::uint256_union, oslo::wallet_value> i (std::make_unique<oslo::mdb_iterator<oslo::uint256_union, oslo::wallet_value>> (transaction_a, handle)), n (nullptr); i != n; ++i)
	{
		tree.put (i->first.to_string (), i->second.key.to_string ());
	}
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree);
	string_a = ostream.str ();
}

void oslo::wallet_store::write_backup (oslo::transaction const & transaction_a, boost::filesystem::path const & path_a)
{
	std::ofstream backup_file;
	backup_file.open (path_a.string ());
	if (!backup_file.fail ())
	{
		// Set permissions to 600
		boost::system::error_code ec;
		oslo::set_secure_perm_file (path_a, ec);

		std::string json;
		serialize_json (transaction_a, json);
		backup_file << json;
	}
}

bool oslo::wallet_store::move (oslo::transaction const & transaction_a, oslo::wallet_store & other_a, std::vector<oslo::public_key> const & keys)
{
	debug_assert (valid_password (transaction_a));
	debug_assert (other_a.valid_password (transaction_a));
	auto result (false);
	for (auto i (keys.begin ()), n (keys.end ()); i != n; ++i)
	{
		oslo::raw_key prv;
		auto error (other_a.fetch (transaction_a, *i, prv));
		result = result | error;
		if (!result)
		{
			insert_adhoc (transaction_a, prv);
			other_a.erase (transaction_a, *i);
		}
	}
	return result;
}

bool oslo::wallet_store::import (oslo::transaction const & transaction_a, oslo::wallet_store & other_a)
{
	debug_assert (valid_password (transaction_a));
	debug_assert (other_a.valid_password (transaction_a));
	auto result (false);
	for (auto i (other_a.begin (transaction_a)), n (end ()); i != n; ++i)
	{
		oslo::raw_key prv;
		auto error (other_a.fetch (transaction_a, i->first, prv));
		result = result | error;
		if (!result)
		{
			if (!prv.data.is_zero ())
			{
				insert_adhoc (transaction_a, prv);
			}
			else
			{
				insert_watch (transaction_a, i->first);
			}
			other_a.erase (transaction_a, i->first);
		}
	}
	return result;
}

bool oslo::wallet_store::work_get (oslo::transaction const & transaction_a, oslo::public_key const & pub_a, uint64_t & work_a)
{
	auto result (false);
	auto entry (entry_get_raw (transaction_a, pub_a));
	if (!entry.key.is_zero ())
	{
		work_a = entry.work;
	}
	else
	{
		result = true;
	}
	return result;
}

void oslo::wallet_store::work_put (oslo::transaction const & transaction_a, oslo::public_key const & pub_a, uint64_t work_a)
{
	auto entry (entry_get_raw (transaction_a, pub_a));
	debug_assert (!entry.key.is_zero ());
	entry.work = work_a;
	entry_put_raw (transaction_a, pub_a, entry);
}

unsigned oslo::wallet_store::version (oslo::transaction const & transaction_a)
{
	oslo::wallet_value value (entry_get_raw (transaction_a, oslo::wallet_store::version_special));
	auto entry (value.key);
	auto result (static_cast<unsigned> (entry.bytes[31]));
	return result;
}

void oslo::wallet_store::version_put (oslo::transaction const & transaction_a, unsigned version_a)
{
	oslo::uint256_union entry (version_a);
	entry_put_raw (transaction_a, oslo::wallet_store::version_special, oslo::wallet_value (entry, 0));
}

void oslo::wallet_store::upgrade_v1_v2 (oslo::transaction const & transaction_a)
{
	debug_assert (version (transaction_a) == 1);
	oslo::raw_key zero_password;
	oslo::wallet_value value (entry_get_raw (transaction_a, oslo::wallet_store::wallet_key_special));
	oslo::raw_key kdf;
	kdf.data.clear ();
	zero_password.decrypt (value.key, kdf, salt (transaction_a).owords[0]);
	derive_key (kdf, transaction_a, "");
	oslo::raw_key empty_password;
	empty_password.decrypt (value.key, kdf, salt (transaction_a).owords[0]);
	for (auto i (begin (transaction_a)), n (end ()); i != n; ++i)
	{
		oslo::public_key const & key (i->first);
		oslo::raw_key prv;
		if (fetch (transaction_a, key, prv))
		{
			// Key failed to decrypt despite valid password
			oslo::wallet_value data (entry_get_raw (transaction_a, key));
			prv.decrypt (data.key, zero_password, salt (transaction_a).owords[0]);
			oslo::public_key compare (oslo::pub_key (prv.as_private_key ()));
			if (compare == key)
			{
				// If we successfully decrypted it, rewrite the key back with the correct wallet key
				insert_adhoc (transaction_a, prv);
			}
			else
			{
				// Also try the empty password
				oslo::wallet_value data (entry_get_raw (transaction_a, key));
				prv.decrypt (data.key, empty_password, salt (transaction_a).owords[0]);
				oslo::public_key compare (oslo::pub_key (prv.as_private_key ()));
				if (compare == key)
				{
					// If we successfully decrypted it, rewrite the key back with the correct wallet key
					insert_adhoc (transaction_a, prv);
				}
			}
		}
	}
	version_put (transaction_a, 2);
}

void oslo::wallet_store::upgrade_v2_v3 (oslo::transaction const & transaction_a)
{
	debug_assert (version (transaction_a) == 2);
	oslo::raw_key seed;
	random_pool::generate_block (seed.data.bytes.data (), seed.data.bytes.size ());
	seed_set (transaction_a, seed);
	entry_put_raw (transaction_a, oslo::wallet_store::deterministic_index_special, oslo::wallet_value (oslo::uint256_union (0), 0));
	version_put (transaction_a, 3);
}

void oslo::wallet_store::upgrade_v3_v4 (oslo::transaction const & transaction_a)
{
	debug_assert (version (transaction_a) == 3);
	version_put (transaction_a, 4);
	debug_assert (valid_password (transaction_a));
	oslo::raw_key seed;
	oslo::wallet_value value (entry_get_raw (transaction_a, oslo::wallet_store::seed_special));
	oslo::raw_key password_l;
	wallet_key (password_l, transaction_a);
	seed.decrypt (value.key, password_l, salt (transaction_a).owords[0]);
	oslo::uint256_union ciphertext;
	ciphertext.encrypt (seed, password_l, salt (transaction_a).owords[seed_iv_index]);
	entry_put_raw (transaction_a, oslo::wallet_store::seed_special, oslo::wallet_value (ciphertext, 0));
	for (auto i (begin (transaction_a)), n (end ()); i != n; ++i)
	{
		oslo::wallet_value value (i->second);
		if (!value.key.is_zero ())
		{
			switch (key_type (i->second))
			{
				case oslo::key_type::adhoc:
				{
					oslo::raw_key key;
					if (fetch (transaction_a, oslo::public_key (i->first), key))
					{
						// Key failed to decrypt despite valid password
						key.decrypt (value.key, password_l, salt (transaction_a).owords[0]);
						oslo::uint256_union new_key_ciphertext;
						new_key_ciphertext.encrypt (key, password_l, (oslo::uint256_union (i->first)).owords[0].number ());
						oslo::wallet_value new_value (new_key_ciphertext, value.work);
						erase (transaction_a, oslo::public_key (i->first));
						entry_put_raw (transaction_a, oslo::public_key (i->first), new_value);
					}
				}
				case oslo::key_type::deterministic:
					break;
				default:
					debug_assert (false);
			}
		}
	}
}

void oslo::kdf::phs (oslo::raw_key & result_a, std::string const & password_a, oslo::uint256_union const & salt_a)
{
	static oslo::network_params network_params;
	oslo::lock_guard<std::mutex> lock (mutex);
	auto success (argon2_hash (1, network_params.kdf_work, 1, password_a.data (), password_a.size (), salt_a.bytes.data (), salt_a.bytes.size (), result_a.data.bytes.data (), result_a.data.bytes.size (), NULL, 0, Argon2_d, 0x10));
	debug_assert (success == 0);
	(void)success;
}

oslo::wallet::wallet (bool & init_a, oslo::transaction & transaction_a, oslo::wallets & wallets_a, std::string const & wallet_a) :
lock_observer ([](bool, bool) {}),
store (init_a, wallets_a.kdf, transaction_a, wallets_a.node.config.random_representative (), wallets_a.node.config.password_fanout, wallet_a),
wallets (wallets_a)
{
}

oslo::wallet::wallet (bool & init_a, oslo::transaction & transaction_a, oslo::wallets & wallets_a, std::string const & wallet_a, std::string const & json) :
lock_observer ([](bool, bool) {}),
store (init_a, wallets_a.kdf, transaction_a, wallets_a.node.config.random_representative (), wallets_a.node.config.password_fanout, wallet_a, json),
wallets (wallets_a)
{
}

void oslo::wallet::enter_initial_password ()
{
	oslo::raw_key password_l;
	{
		oslo::lock_guard<std::recursive_mutex> lock (store.mutex);
		store.password.value (password_l);
	}
	if (password_l.data.is_zero ())
	{
		auto transaction (wallets.tx_begin_write ());
		if (store.valid_password (transaction))
		{
			// Newly created wallets have a zero key
			store.rekey (transaction, "");
		}
		else
		{
			enter_password (transaction, "");
		}
	}
}

bool oslo::wallet::enter_password (oslo::transaction const & transaction_a, std::string const & password_a)
{
	auto result (store.attempt_password (transaction_a, password_a));
	if (!result)
	{
		auto this_l (shared_from_this ());
		wallets.node.background ([this_l]() {
			this_l->search_pending ();
		});
		wallets.node.logger.try_log ("Wallet unlocked");
	}
	else
	{
		wallets.node.logger.try_log ("Invalid password, wallet locked");
	}
	lock_observer (result, password_a.empty ());
	return result;
}

oslo::public_key oslo::wallet::deterministic_insert (oslo::transaction const & transaction_a, bool generate_work_a)
{
	oslo::public_key key (0);
	if (store.valid_password (transaction_a))
	{
		key = store.deterministic_insert (transaction_a);
		if (generate_work_a)
		{
			work_ensure (key, key);
		}
		auto half_principal_weight (wallets.node.minimum_principal_weight () / 2);
		if (wallets.check_rep (key, half_principal_weight))
		{
			oslo::lock_guard<std::mutex> lock (representatives_mutex);
			representatives.insert (key);
		}
	}
	return key;
}

oslo::public_key oslo::wallet::deterministic_insert (uint32_t const index, bool generate_work_a)
{
	auto transaction (wallets.tx_begin_write ());
	oslo::public_key key (0);
	if (store.valid_password (transaction))
	{
		key = store.deterministic_insert (transaction, index);
		if (generate_work_a)
		{
			work_ensure (key, key);
		}
	}
	return key;
}

oslo::public_key oslo::wallet::deterministic_insert (bool generate_work_a)
{
	auto transaction (wallets.tx_begin_write ());
	auto result (deterministic_insert (transaction, generate_work_a));
	return result;
}

oslo::public_key oslo::wallet::insert_adhoc (oslo::transaction const & transaction_a, oslo::raw_key const & key_a, bool generate_work_a)
{
	oslo::public_key key (0);
	if (store.valid_password (transaction_a))
	{
		key = store.insert_adhoc (transaction_a, key_a);
		auto block_transaction (wallets.node.store.tx_begin_read ());
		if (generate_work_a)
		{
			work_ensure (key, wallets.node.ledger.latest_root (block_transaction, key));
		}
		auto half_principal_weight (wallets.node.minimum_principal_weight () / 2);
		if (wallets.check_rep (key, half_principal_weight))
		{
			oslo::lock_guard<std::mutex> lock (representatives_mutex);
			representatives.insert (key);
		}
	}
	return key;
}

oslo::public_key oslo::wallet::insert_adhoc (oslo::raw_key const & account_a, bool generate_work_a)
{
	auto transaction (wallets.tx_begin_write ());
	auto result (insert_adhoc (transaction, account_a, generate_work_a));
	return result;
}

bool oslo::wallet::insert_watch (oslo::transaction const & transaction_a, oslo::public_key const & pub_a)
{
	return store.insert_watch (transaction_a, pub_a);
}

bool oslo::wallet::exists (oslo::public_key const & account_a)
{
	auto transaction (wallets.tx_begin_read ());
	return store.exists (transaction, account_a);
}

bool oslo::wallet::import (std::string const & json_a, std::string const & password_a)
{
	auto error (false);
	std::unique_ptr<oslo::wallet_store> temp;
	{
		auto transaction (wallets.tx_begin_write ());
		oslo::uint256_union id;
		random_pool::generate_block (id.bytes.data (), id.bytes.size ());
		temp = std::make_unique<oslo::wallet_store> (error, wallets.node.wallets.kdf, transaction, 0, 1, id.to_string (), json_a);
	}
	if (!error)
	{
		auto transaction (wallets.tx_begin_write ());
		error = temp->attempt_password (transaction, password_a);
	}
	auto transaction (wallets.tx_begin_write ());
	if (!error)
	{
		error = store.import (transaction, *temp);
	}
	temp->destroy (transaction);
	return error;
}

void oslo::wallet::serialize (std::string & json_a)
{
	auto transaction (wallets.tx_begin_read ());
	store.serialize_json (transaction, json_a);
}

void oslo::wallet_store::destroy (oslo::transaction const & transaction_a)
{
	auto status (mdb_drop (tx (transaction_a), handle, 1));
	(void)status;
	debug_assert (status == 0);
	handle = 0;
}

std::shared_ptr<oslo::block> oslo::wallet::receive_action (oslo::block const & send_a, oslo::account const & representative_a, oslo::uint128_union const & amount_a, uint64_t work_a, bool generate_work_a)
{
	oslo::account account;
	auto hash (send_a.hash ());
	std::shared_ptr<oslo::block> block;
	oslo::block_details details;
	details.is_receive = true;
	if (wallets.node.config.receive_minimum.number () <= amount_a.number ())
	{
		auto block_transaction (wallets.node.ledger.store.tx_begin_read ());
		auto transaction (wallets.tx_begin_read ());
		oslo::pending_info pending_info;
		if (wallets.node.store.block_exists (block_transaction, hash))
		{
			account = wallets.node.ledger.block_destination (block_transaction, send_a);
			if (!wallets.node.ledger.store.pending_get (block_transaction, oslo::pending_key (account, hash), pending_info))
			{
				oslo::raw_key prv;
				if (!store.fetch (transaction, account, prv))
				{
					if (work_a == 0)
					{
						store.work_get (transaction, account, work_a);
					}
					oslo::account_info info;
					auto new_account (wallets.node.ledger.store.account_get (block_transaction, account, info));
					if (!new_account)
					{
						block = std::make_shared<oslo::state_block> (account, info.head, info.representative, info.balance.number () + pending_info.amount.number (), hash, prv, account, work_a);
						details.epoch = std::max (info.epoch (), send_a.sideband ().details.epoch);
					}
					else
					{
						block = std::make_shared<oslo::state_block> (account, 0, representative_a, pending_info.amount, reinterpret_cast<oslo::link const &> (hash), prv, account, work_a);
						details.epoch = send_a.sideband ().details.epoch;
					}
				}
				else
				{
					wallets.node.logger.try_log ("Unable to receive, wallet locked");
				}
			}
			else
			{
				// Ledger doesn't have this marked as available to receive anymore
			}
		}
		else
		{
			// Ledger doesn't have this block anymore.
		}
	}
	else
	{
		wallets.node.logger.try_log (boost::str (boost::format ("Not receiving block %1% due to minimum receive threshold") % hash.to_string ()));
		// Someone sent us something below the threshold of receiving
	}
	if (block != nullptr)
	{
		if (action_complete (block, account, generate_work_a, details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

std::shared_ptr<oslo::block> oslo::wallet::change_action (oslo::account const & source_a, oslo::account const & representative_a, uint64_t work_a, bool generate_work_a)
{
	std::shared_ptr<oslo::block> block;
	oslo::block_details details;
	{
		auto transaction (wallets.tx_begin_read ());
		auto block_transaction (wallets.node.store.tx_begin_read ());
		if (store.valid_password (transaction))
		{
			auto existing (store.find (transaction, source_a));
			if (existing != store.end () && !wallets.node.ledger.latest (block_transaction, source_a).is_zero ())
			{
				oslo::account_info info;
				auto error1 (wallets.node.ledger.store.account_get (block_transaction, source_a, info));
				(void)error1;
				debug_assert (!error1);
				oslo::raw_key prv;
				auto error2 (store.fetch (transaction, source_a, prv));
				(void)error2;
				debug_assert (!error2);
				if (work_a == 0)
				{
					store.work_get (transaction, source_a, work_a);
				}
				block = std::make_shared<oslo::state_block> (source_a, info.head, representative_a, info.balance, 0, prv, source_a, work_a);
				details.epoch = info.epoch ();
			}
		}
	}
	if (block != nullptr)
	{
		if (action_complete (block, source_a, generate_work_a, details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

std::shared_ptr<oslo::block> oslo::wallet::send_action (oslo::account const & source_a, oslo::account const & account_a, oslo::uint128_t const & amount_a, uint64_t work_a, bool generate_work_a, boost::optional<std::string> id_a)
{
	boost::optional<oslo::mdb_val> id_mdb_val;
	if (id_a)
	{
		id_mdb_val = oslo::mdb_val (id_a->size (), const_cast<char *> (id_a->data ()));
	}

	auto prepare_send = [&id_mdb_val, &wallets = this->wallets, &store = this->store, &source_a, &amount_a, &work_a, &account_a](const auto & transaction) {
		auto block_transaction (wallets.node.store.tx_begin_read ());
		auto error (false);
		auto cached_block (false);
		std::shared_ptr<oslo::block> block;
		oslo::block_details details;
		details.is_send = true;
		if (id_mdb_val)
		{
			oslo::mdb_val result;
			auto status (mdb_get (wallets.env.tx (transaction), wallets.node.wallets.send_action_ids, *id_mdb_val, result));
			if (status == 0)
			{
				oslo::block_hash hash (result);
				block = wallets.node.store.block_get (block_transaction, hash);
				if (block != nullptr)
				{
					cached_block = true;
					wallets.node.network.flood_block (block, oslo::buffer_drop_policy::no_limiter_drop);
				}
			}
			else if (status != MDB_NOTFOUND)
			{
				error = true;
			}
		}
		if (!error && block == nullptr)
		{
			if (store.valid_password (transaction))
			{
				auto existing (store.find (transaction, source_a));
				if (existing != store.end ())
				{
					auto balance (wallets.node.ledger.account_balance (block_transaction, source_a));
					if (!balance.is_zero () && balance >= amount_a)
					{
						oslo::account_info info;
						auto error1 (wallets.node.ledger.store.account_get (block_transaction, source_a, info));
						(void)error1;
						debug_assert (!error1);
						oslo::raw_key prv;
						auto error2 (store.fetch (transaction, source_a, prv));
						(void)error2;
						debug_assert (!error2);
						if (work_a == 0)
						{
							store.work_get (transaction, source_a, work_a);
						}
						block = std::make_shared<oslo::state_block> (source_a, info.head, info.representative, balance - amount_a, account_a, prv, source_a, work_a);
						details.epoch = info.epoch ();
						if (id_mdb_val && block != nullptr)
						{
							auto status (mdb_put (wallets.env.tx (transaction), wallets.node.wallets.send_action_ids, *id_mdb_val, oslo::mdb_val (block->hash ()), 0));
							if (status != 0)
							{
								block = nullptr;
								error = true;
							}
						}
					}
				}
			}
		}
		return std::make_tuple (block, error, cached_block, details);
	};

	std::tuple<std::shared_ptr<oslo::block>, bool, bool, oslo::block_details> result;
	{
		if (id_mdb_val)
		{
			result = prepare_send (wallets.tx_begin_write ());
		}
		else
		{
			result = prepare_send (wallets.tx_begin_read ());
		}
	}

	std::shared_ptr<oslo::block> block;
	bool error;
	bool cached_block;
	oslo::block_details details;
	std::tie (block, error, cached_block, details) = result;

	if (!error && block != nullptr && !cached_block)
	{
		if (action_complete (block, source_a, generate_work_a, details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

bool oslo::wallet::action_complete (std::shared_ptr<oslo::block> const & block_a, oslo::account const & account_a, bool const generate_work_a, oslo::block_details const & details_a)
{
	bool error{ false };
	// Unschedule any work caching for this account
	wallets.delayed_work->erase (account_a);
	if (block_a != nullptr)
	{
		auto required_difficulty{ oslo::work_threshold (block_a->work_version (), details_a) };
		if (block_a->difficulty () < required_difficulty)
		{
			wallets.node.logger.try_log (boost::str (boost::format ("Cached or provided work for block %1% account %2% is invalid, regenerating") % block_a->hash ().to_string () % account_a.to_account ()));
			debug_assert (required_difficulty <= wallets.node.max_work_generate_difficulty (block_a->work_version ()));
			auto target_difficulty = std::max (required_difficulty, wallets.node.active.limited_active_difficulty (block_a->work_version (), required_difficulty));
			error = !wallets.node.work_generate_blocking (*block_a, target_difficulty).is_initialized ();
		}
		if (!error)
		{
			error = wallets.node.process_local (block_a, true).code != oslo::process_result::progress;
			debug_assert (error || block_a->sideband ().details == details_a);
		}
		if (!error && generate_work_a)
		{
			work_ensure (account_a, block_a->hash ());
		}
	}
	return error;
}

bool oslo::wallet::change_sync (oslo::account const & source_a, oslo::account const & representative_a)
{
	std::promise<bool> result;
	std::future<bool> future = result.get_future ();
	change_async (
	source_a, representative_a, [&result](std::shared_ptr<oslo::block> block_a) {
		result.set_value (block_a == nullptr);
	},
	true);
	return future.get ();
}

void oslo::wallet::change_async (oslo::account const & source_a, oslo::account const & representative_a, std::function<void(std::shared_ptr<oslo::block>)> const & action_a, uint64_t work_a, bool generate_work_a)
{
	auto this_l (shared_from_this ());
	wallets.node.wallets.queue_wallet_action (oslo::wallets::high_priority, this_l, [this_l, source_a, representative_a, action_a, work_a, generate_work_a](oslo::wallet & wallet_a) {
		auto block (wallet_a.change_action (source_a, representative_a, work_a, generate_work_a));
		action_a (block);
	});
}

bool oslo::wallet::receive_sync (std::shared_ptr<oslo::block> block_a, oslo::account const & representative_a, oslo::uint128_t const & amount_a)
{
	std::promise<bool> result;
	std::future<bool> future = result.get_future ();
	receive_async (
	block_a, representative_a, amount_a, [&result](std::shared_ptr<oslo::block> block_a) {
		result.set_value (block_a == nullptr);
	},
	true);
	return future.get ();
}

void oslo::wallet::receive_async (std::shared_ptr<oslo::block> block_a, oslo::account const & representative_a, oslo::uint128_t const & amount_a, std::function<void(std::shared_ptr<oslo::block>)> const & action_a, uint64_t work_a, bool generate_work_a)
{
	auto this_l (shared_from_this ());
	wallets.node.wallets.queue_wallet_action (amount_a, this_l, [this_l, block_a, representative_a, amount_a, action_a, work_a, generate_work_a](oslo::wallet & wallet_a) {
		auto block (wallet_a.receive_action (*block_a, representative_a, amount_a, work_a, generate_work_a));
		action_a (block);
	});
}

oslo::block_hash oslo::wallet::send_sync (oslo::account const & source_a, oslo::account const & account_a, oslo::uint128_t const & amount_a)
{
	std::promise<oslo::block_hash> result;
	std::future<oslo::block_hash> future = result.get_future ();
	send_async (
	source_a, account_a, amount_a, [&result](std::shared_ptr<oslo::block> block_a) {
		result.set_value (block_a->hash ());
	},
	true);
	return future.get ();
}

void oslo::wallet::send_async (oslo::account const & source_a, oslo::account const & account_a, oslo::uint128_t const & amount_a, std::function<void(std::shared_ptr<oslo::block>)> const & action_a, uint64_t work_a, bool generate_work_a, boost::optional<std::string> id_a)
{
	auto this_l (shared_from_this ());
	wallets.node.wallets.queue_wallet_action (oslo::wallets::high_priority, this_l, [this_l, source_a, account_a, amount_a, action_a, work_a, generate_work_a, id_a](oslo::wallet & wallet_a) {
		auto block (wallet_a.send_action (source_a, account_a, amount_a, work_a, generate_work_a, id_a));
		action_a (block);
	});
}

// Update work for account if latest root is root_a
void oslo::wallet::work_update (oslo::transaction const & transaction_a, oslo::account const & account_a, oslo::root const & root_a, uint64_t work_a)
{
	debug_assert (!oslo::work_validate_entry (oslo::work_version::work_1, root_a, work_a));
	debug_assert (store.exists (transaction_a, account_a));
	auto block_transaction (wallets.node.store.tx_begin_read ());
	auto latest (wallets.node.ledger.latest_root (block_transaction, account_a));
	if (latest == root_a)
	{
		store.work_put (transaction_a, account_a, work_a);
	}
	else
	{
		wallets.node.logger.try_log ("Cached work no longer valid, discarding");
	}
}

void oslo::wallet::work_ensure (oslo::account const & account_a, oslo::root const & root_a)
{
	using namespace std::chrono_literals;
	std::chrono::seconds const precache_delay = wallets.node.network_params.network.is_test_network () ? 1s : 10s;

	wallets.delayed_work->operator[] (account_a) = root_a;

	wallets.node.alarm.add (std::chrono::steady_clock::now () + precache_delay, [this_l = shared_from_this (), account_a, root_a] {
		auto delayed_work = this_l->wallets.delayed_work.lock ();
		auto existing (delayed_work->find (account_a));
		if (existing != delayed_work->end () && existing->second == root_a)
		{
			delayed_work->erase (existing);
			this_l->wallets.queue_wallet_action (oslo::wallets::generate_priority, this_l, [account_a, root_a](oslo::wallet & wallet_a) {
				wallet_a.work_cache_blocking (account_a, root_a);
			});
		}
	});
}

bool oslo::wallet::search_pending ()
{
	auto transaction (wallets.tx_begin_read ());
	auto result (!store.valid_password (transaction));
	if (!result)
	{
		wallets.node.logger.try_log ("Beginning pending block search");
		for (auto i (store.begin (transaction)), n (store.end ()); i != n; ++i)
		{
			auto block_transaction (wallets.node.store.tx_begin_read ());
			oslo::account const & account (i->first);
			// Don't search pending for watch-only accounts
			if (!oslo::wallet_value (i->second).key.is_zero ())
			{
				for (auto j (wallets.node.store.pending_begin (block_transaction, oslo::pending_key (account, 0))), k (wallets.node.store.pending_end ()); j != k && oslo::pending_key (j->first).account == account; ++j)
				{
					oslo::pending_key key (j->first);
					auto hash (key.hash);
					oslo::pending_info pending (j->second);
					auto amount (pending.amount.number ());
					if (wallets.node.config.receive_minimum.number () <= amount)
					{
						wallets.node.logger.try_log (boost::str (boost::format ("Found a pending block %1% for account %2%") % hash.to_string () % pending.source.to_account ()));
						auto block (wallets.node.store.block_get (block_transaction, hash));
						if (wallets.node.ledger.block_confirmed (block_transaction, hash))
						{
							// Receive confirmed block
							auto node_l (wallets.node.shared ());
							wallets.node.background ([node_l, block, hash]() {
								auto transaction (node_l->store.tx_begin_read ());
								node_l->receive_confirmed (transaction, block, hash);
							});
						}
						else
						{
							if (!wallets.node.confirmation_height_processor.is_processing_block (hash))
							{
								// Request confirmation for block which is not being processed yet
								wallets.node.block_confirm (block);
							}
						}
					}
				}
			}
		}
		wallets.node.logger.try_log ("Pending block search phase complete");
	}
	else
	{
		wallets.node.logger.try_log ("Stopping search, wallet is locked");
	}
	return result;
}

void oslo::wallet::init_free_accounts (oslo::transaction const & transaction_a)
{
	free_accounts.clear ();
	for (auto i (store.begin (transaction_a)), n (store.end ()); i != n; ++i)
	{
		free_accounts.insert (i->first);
	}
}

uint32_t oslo::wallet::deterministic_check (oslo::transaction const & transaction_a, uint32_t index)
{
	auto block_transaction (wallets.node.store.tx_begin_read ());
	for (uint32_t i (index + 1), n (index + 64); i < n; ++i)
	{
		auto prv = store.deterministic_key (transaction_a, i);
		oslo::keypair pair (prv.to_string ());
		// Check if account received at least 1 block
		auto latest (wallets.node.ledger.latest (block_transaction, pair.pub));
		if (!latest.is_zero ())
		{
			index = i;
			// i + 64 - Check additional 64 accounts
			// i/64 - Check additional accounts for large wallets. I.e. 64000/64 = 1000 accounts to check
			n = i + 64 + (i / 64);
		}
		else
		{
			// Check if there are pending blocks for account
			for (auto ii (wallets.node.store.pending_begin (block_transaction, oslo::pending_key (pair.pub, 0))), nn (wallets.node.store.pending_end ()); ii != nn && oslo::pending_key (ii->first).account == pair.pub; ++ii)
			{
				index = i;
				n = i + 64 + (i / 64);
				break;
			}
		}
	}
	return index;
}

oslo::public_key oslo::wallet::change_seed (oslo::transaction const & transaction_a, oslo::raw_key const & prv_a, uint32_t count)
{
	store.seed_set (transaction_a, prv_a);
	auto account = deterministic_insert (transaction_a);
	if (count == 0)
	{
		count = deterministic_check (transaction_a, 0);
	}
	for (uint32_t i (0); i < count; ++i)
	{
		// Disable work generation to prevent weak CPU nodes stuck
		account = deterministic_insert (transaction_a, false);
	}
	return account;
}

void oslo::wallet::deterministic_restore (oslo::transaction const & transaction_a)
{
	auto index (store.deterministic_index_get (transaction_a));
	auto new_index (deterministic_check (transaction_a, index));
	for (uint32_t i (index); i <= new_index && index != new_index; ++i)
	{
		// Disable work generation to prevent weak CPU nodes stuck
		deterministic_insert (transaction_a, false);
	}
}

bool oslo::wallet::live ()
{
	return store.handle != 0;
}

void oslo::wallet::work_cache_blocking (oslo::account const & account_a, oslo::root const & root_a)
{
	if (wallets.node.work_generation_enabled ())
	{
		auto difficulty (wallets.node.default_difficulty (oslo::work_version::work_1));
		auto opt_work_l (wallets.node.work_generate_blocking (oslo::work_version::work_1, root_a, difficulty, account_a));
		if (opt_work_l.is_initialized ())
		{
			auto transaction_l (wallets.tx_begin_write ());
			if (live () && store.exists (transaction_l, account_a))
			{
				work_update (transaction_l, account_a, root_a, *opt_work_l);
			}
		}
		else if (!wallets.node.stopped)
		{
			wallets.node.logger.try_log (boost::str (boost::format ("Could not precache work for root %1% due to work generation failure") % root_a.to_string ()));
		}
	}
}

oslo::work_watcher::work_watcher (oslo::node & node_a) :
node (node_a),
stopped (false)
{
	node.observers.blocks.add ([this](oslo::election_status const & status_a, oslo::account const & account_a, oslo::amount const & amount_a, bool is_state_send_a) {
		this->remove (*status_a.winner);
	});
}

oslo::work_watcher::~work_watcher ()
{
	stop ();
}

void oslo::work_watcher::stop ()
{
	oslo::unique_lock<std::mutex> lock (mutex);
	watched.clear ();
	stopped = true;
}

void oslo::work_watcher::add (std::shared_ptr<oslo::block> block_a)
{
	auto block_l (std::dynamic_pointer_cast<oslo::state_block> (block_a));
	if (!stopped && block_l != nullptr)
	{
		auto root_l (block_l->qualified_root ());
		oslo::unique_lock<std::mutex> lock (mutex);
		watched[root_l] = block_l;
		lock.unlock ();
		watching (root_l, block_l);
	}
}

void oslo::work_watcher::update (oslo::qualified_root const & root_a, std::shared_ptr<oslo::state_block> block_a)
{
	oslo::lock_guard<std::mutex> guard (mutex);
	watched[root_a] = block_a;
}

void oslo::work_watcher::watching (oslo::qualified_root const & root_a, std::shared_ptr<oslo::state_block> block_a)
{
	std::weak_ptr<oslo::work_watcher> watcher_w (shared_from_this ());
	node.alarm.add (std::chrono::steady_clock::now () + node.config.work_watcher_period, [block_a, root_a, watcher_w]() {
		auto watcher_l = watcher_w.lock ();
		if (watcher_l && !watcher_l->stopped && watcher_l->is_watched (root_a))
		{
			auto active_difficulty (watcher_l->node.active.limited_active_difficulty (*block_a));
			/*
			 * Work watcher should still watch blocks even without work generation, although no rework is done
			 * Functionality may be added in the future that does not require updating work
			 */
			if (active_difficulty > block_a->difficulty () && watcher_l->node.work_generation_enabled ())
			{
				watcher_l->node.work_generate (
				block_a->work_version (), block_a->root (), active_difficulty, [watcher_l, block_a, root_a](boost::optional<uint64_t> work_a) {
					if (watcher_l->is_watched (root_a))
					{
						if (work_a.is_initialized ())
						{
							debug_assert (oslo::work_difficulty (block_a->work_version (), block_a->root (), *work_a) > block_a->difficulty ());
							oslo::state_block_builder builder;
							std::error_code ec;
							std::shared_ptr<oslo::state_block> block (builder.from (*block_a).work (*work_a).build (ec));
							if (!ec)
							{
								watcher_l->node.network.flood_block_initial (block);
								watcher_l->node.active.update_difficulty (*block);
								watcher_l->update (root_a, block);
							}
						}
						watcher_l->watching (root_a, block_a);
					}
				},
				block_a->account ());
			}
			else
			{
				watcher_l->watching (root_a, block_a);
			}
		}
	});
}

void oslo::work_watcher::remove (oslo::block const & block_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	auto existing (watched.find (block_a.qualified_root ()));
	if (existing != watched.end ())
	{
		watched.erase (existing);
		node.observers.work_cancel.notify (block_a.root ());
	}
}

bool oslo::work_watcher::is_watched (oslo::qualified_root const & root_a)
{
	oslo::lock_guard<std::mutex> guard (mutex);
	auto exists (watched.find (root_a));
	return exists != watched.end ();
}

size_t oslo::work_watcher::size ()
{
	oslo::lock_guard<std::mutex> guard (mutex);
	return watched.size ();
}

void oslo::wallets::do_wallet_actions ()
{
	oslo::unique_lock<std::mutex> action_lock (action_mutex);
	while (!stopped)
	{
		if (!actions.empty ())
		{
			auto first (actions.begin ());
			auto wallet (first->second.first);
			auto current (std::move (first->second.second));
			actions.erase (first);
			if (wallet->live ())
			{
				action_lock.unlock ();
				observer (true);
				current (*wallet);
				observer (false);
				action_lock.lock ();
			}
		}
		else
		{
			condition.wait (action_lock);
		}
	}
}

oslo::wallets::wallets (bool error_a, oslo::node & node_a) :
observer ([](bool) {}),
node (node_a),
env (boost::polymorphic_downcast<oslo::mdb_wallets_store *> (node_a.wallets_store_impl.get ())->environment),
stopped (false),
watcher (std::make_shared<oslo::work_watcher> (node_a)),
thread ([this]() {
	oslo::thread_role::set (oslo::thread_role::name::wallet_actions);
	do_wallet_actions ();
})
{
	oslo::unique_lock<std::mutex> lock (mutex);
	if (!error_a)
	{
		auto transaction (tx_begin_write ());
		auto status (mdb_dbi_open (env.tx (transaction), nullptr, MDB_CREATE, &handle));
		split_if_needed (transaction, node.store);
		status |= mdb_dbi_open (env.tx (transaction), "send_action_ids", MDB_CREATE, &send_action_ids);
		debug_assert (status == 0);
		std::string beginning (oslo::uint256_union (0).to_string ());
		std::string end ((oslo::uint256_union (oslo::uint256_t (0) - oslo::uint256_t (1))).to_string ());
		oslo::store_iterator<std::array<char, 64>, oslo::no_value> i (std::make_unique<oslo::mdb_iterator<std::array<char, 64>, oslo::no_value>> (transaction, handle, oslo::mdb_val (beginning.size (), const_cast<char *> (beginning.c_str ()))));
		oslo::store_iterator<std::array<char, 64>, oslo::no_value> n (std::make_unique<oslo::mdb_iterator<std::array<char, 64>, oslo::no_value>> (transaction, handle, oslo::mdb_val (end.size (), const_cast<char *> (end.c_str ()))));
		for (; i != n; ++i)
		{
			oslo::wallet_id id;
			std::string text (i->first.data (), i->first.size ());
			auto error (id.decode_hex (text));
			debug_assert (!error);
			debug_assert (items.find (id) == items.end ());
			auto wallet (std::make_shared<oslo::wallet> (error, transaction, *this, text));
			if (!error)
			{
				items[id] = wallet;
			}
			else
			{
				// Couldn't open wallet
			}
		}
	}
	// Backup before upgrade wallets
	bool backup_required (false);
	if (node.config.backup_before_upgrade)
	{
		auto transaction (tx_begin_read ());
		for (auto & item : items)
		{
			if (item.second->store.version (transaction) != oslo::wallet_store::version_current)
			{
				backup_required = true;
				break;
			}
		}
	}
	if (backup_required)
	{
		const char * store_path;
		mdb_env_get_path (env, &store_path);
		const boost::filesystem::path path (store_path);
		oslo::mdb_store::create_backup_file (env, path, node_a.logger);
	}
	for (auto & item : items)
	{
		item.second->enter_initial_password ();
	}
	if (node_a.config.enable_voting)
	{
		lock.unlock ();
		ongoing_compute_reps ();
	}
}

oslo::wallets::~wallets ()
{
	stop ();
}

std::shared_ptr<oslo::wallet> oslo::wallets::open (oslo::wallet_id const & id_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	std::shared_ptr<oslo::wallet> result;
	auto existing (items.find (id_a));
	if (existing != items.end ())
	{
		result = existing->second;
	}
	return result;
}

std::shared_ptr<oslo::wallet> oslo::wallets::create (oslo::wallet_id const & id_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	debug_assert (items.find (id_a) == items.end ());
	std::shared_ptr<oslo::wallet> result;
	bool error;
	{
		auto transaction (tx_begin_write ());
		result = std::make_shared<oslo::wallet> (error, transaction, *this, id_a.to_string ());
	}
	if (!error)
	{
		items[id_a] = result;
		result->enter_initial_password ();
	}
	return result;
}

bool oslo::wallets::search_pending (oslo::wallet_id const & wallet_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	auto result (false);
	auto existing (items.find (wallet_a));
	result = existing == items.end ();
	if (!result)
	{
		auto wallet (existing->second);
		result = wallet->search_pending ();
	}
	return result;
}

void oslo::wallets::search_pending_all ()
{
	oslo::lock_guard<std::mutex> lock (mutex);
	for (auto i : items)
	{
		i.second->search_pending ();
	}
}

void oslo::wallets::destroy (oslo::wallet_id const & id_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	auto transaction (tx_begin_write ());
	// action_mutex should be after transactions to prevent deadlocks in deterministic_insert () & insert_adhoc ()
	oslo::lock_guard<std::mutex> action_lock (action_mutex);
	auto existing (items.find (id_a));
	debug_assert (existing != items.end ());
	auto wallet (existing->second);
	items.erase (existing);
	wallet->store.destroy (transaction);
}

void oslo::wallets::reload ()
{
	oslo::lock_guard<std::mutex> lock (mutex);
	auto transaction (tx_begin_write ());
	std::unordered_set<oslo::uint256_union> stored_items;
	std::string beginning (oslo::uint256_union (0).to_string ());
	std::string end ((oslo::uint256_union (oslo::uint256_t (0) - oslo::uint256_t (1))).to_string ());
	oslo::store_iterator<std::array<char, 64>, oslo::no_value> i (std::make_unique<oslo::mdb_iterator<std::array<char, 64>, oslo::no_value>> (transaction, handle, oslo::mdb_val (beginning.size (), const_cast<char *> (beginning.c_str ()))));
	oslo::store_iterator<std::array<char, 64>, oslo::no_value> n (std::make_unique<oslo::mdb_iterator<std::array<char, 64>, oslo::no_value>> (transaction, handle, oslo::mdb_val (end.size (), const_cast<char *> (end.c_str ()))));
	for (; i != n; ++i)
	{
		oslo::wallet_id id;
		std::string text (i->first.data (), i->first.size ());
		auto error (id.decode_hex (text));
		debug_assert (!error);
		// New wallet
		if (items.find (id) == items.end ())
		{
			auto wallet (std::make_shared<oslo::wallet> (error, transaction, *this, text));
			if (!error)
			{
				items[id] = wallet;
			}
		}
		// List of wallets on disk
		stored_items.insert (id);
	}
	// Delete non existing wallets from memory
	std::vector<oslo::wallet_id> deleted_items;
	for (auto i : items)
	{
		if (stored_items.find (i.first) == stored_items.end ())
		{
			deleted_items.push_back (i.first);
		}
	}
	for (auto & i : deleted_items)
	{
		debug_assert (items.find (i) == items.end ());
		items.erase (i);
	}
}

void oslo::wallets::queue_wallet_action (oslo::uint128_t const & amount_a, std::shared_ptr<oslo::wallet> wallet_a, std::function<void(oslo::wallet &)> const & action_a)
{
	{
		oslo::lock_guard<std::mutex> action_lock (action_mutex);
		actions.emplace (amount_a, std::make_pair (wallet_a, std::move (action_a)));
	}
	condition.notify_all ();
}

void oslo::wallets::foreach_representative (std::function<void(oslo::public_key const & pub_a, oslo::raw_key const & prv_a)> const & action_a)
{
	if (node.config.enable_voting)
	{
		std::vector<std::pair<oslo::public_key const, oslo::raw_key const>> action_accounts_l;
		{
			auto transaction_l (tx_begin_read ());
			oslo::lock_guard<std::mutex> lock (mutex);
			for (auto i (items.begin ()), n (items.end ()); i != n; ++i)
			{
				auto & wallet (*i->second);
				oslo::lock_guard<std::recursive_mutex> store_lock (wallet.store.mutex);
				decltype (wallet.representatives) representatives_l;
				{
					oslo::lock_guard<std::mutex> representatives_lock (wallet.representatives_mutex);
					representatives_l = wallet.representatives;
				}
				for (auto const & account : representatives_l)
				{
					if (wallet.store.exists (transaction_l, account))
					{
						if (!node.ledger.weight (account).is_zero ())
						{
							if (wallet.store.valid_password (transaction_l))
							{
								oslo::raw_key prv;
								auto error (wallet.store.fetch (transaction_l, account, prv));
								(void)error;
								debug_assert (!error);
								action_accounts_l.emplace_back (account, prv);
							}
							else
							{
								static auto last_log = std::chrono::steady_clock::time_point ();
								if (last_log < std::chrono::steady_clock::now () - std::chrono::seconds (60))
								{
									last_log = std::chrono::steady_clock::now ();
									node.logger.always_log (boost::str (boost::format ("Representative locked inside wallet %1%") % i->first.to_string ()));
								}
							}
						}
					}
				}
			}
		}
		for (auto const & representative : action_accounts_l)
		{
			action_a (representative.first, representative.second);
		}
	}
}

bool oslo::wallets::exists (oslo::transaction const & transaction_a, oslo::account const & account_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	auto result (false);
	for (auto i (items.begin ()), n (items.end ()); !result && i != n; ++i)
	{
		result = i->second->store.exists (transaction_a, account_a);
	}
	return result;
}

void oslo::wallets::stop ()
{
	{
		oslo::lock_guard<std::mutex> action_lock (action_mutex);
		stopped = true;
		actions.clear ();
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
	watcher->stop ();
}

oslo::write_transaction oslo::wallets::tx_begin_write ()
{
	return env.tx_begin_write ();
}

oslo::read_transaction oslo::wallets::tx_begin_read ()
{
	return env.tx_begin_read ();
}

void oslo::wallets::clear_send_ids (oslo::transaction const & transaction_a)
{
	auto status (mdb_drop (env.tx (transaction_a), send_action_ids, 0));
	(void)status;
	debug_assert (status == 0);
}

oslo::wallet_representatives oslo::wallets::reps () const
{
	oslo::lock_guard<std::mutex> counts_guard (reps_cache_mutex);
	return representatives;
}

bool oslo::wallets::check_rep (oslo::account const & account_a, oslo::uint128_t const & half_principal_weight_a, const bool acquire_lock_a)
{
	bool result (false);
	auto weight (node.ledger.weight (account_a));
	if (weight >= node.config.vote_minimum.number ())
	{
		oslo::unique_lock<std::mutex> lock;
		if (acquire_lock_a)
		{
			lock = oslo::unique_lock<std::mutex> (reps_cache_mutex);
		}
		result = true;
		representatives.accounts.insert (account_a);
		++representatives.voting;
		if (weight >= half_principal_weight_a)
		{
			++representatives.half_principal;
		}
	}
	return result;
}

void oslo::wallets::compute_reps ()
{
	oslo::lock_guard<std::mutex> guard (mutex);
	oslo::lock_guard<std::mutex> counts_guard (reps_cache_mutex);
	representatives.clear ();
	auto half_principal_weight (node.minimum_principal_weight () / 2);
	auto transaction (tx_begin_read ());
	for (auto i (items.begin ()), n (items.end ()); i != n; ++i)
	{
		auto & wallet (*i->second);
		decltype (wallet.representatives) representatives_l;
		for (auto ii (wallet.store.begin (transaction)), nn (wallet.store.end ()); ii != nn; ++ii)
		{
			auto account (ii->first);
			if (check_rep (account, half_principal_weight, false))
			{
				representatives_l.insert (account);
			}
		}
		oslo::lock_guard<std::mutex> representatives_guard (wallet.representatives_mutex);
		wallet.representatives.swap (representatives_l);
	}
}

void oslo::wallets::ongoing_compute_reps ()
{
	compute_reps ();
	auto & node_l (node);
	auto compute_delay (network_params.network.is_test_network () ? std::chrono::milliseconds (10) : std::chrono::milliseconds (15 * 60 * 1000)); // Representation drifts quickly on the test network but very slowly on the live network
	node.alarm.add (std::chrono::steady_clock::now () + compute_delay, [&node_l]() {
		node_l.wallets.ongoing_compute_reps ();
	});
}

void oslo::wallets::split_if_needed (oslo::transaction & transaction_destination, oslo::block_store & store_a)
{
	auto store_l (dynamic_cast<oslo::mdb_store *> (&store_a));
	if (store_l != nullptr)
	{
		if (items.empty ())
		{
			std::string beginning (oslo::uint256_union (0).to_string ());
			std::string end ((oslo::uint256_union (oslo::uint256_t (0) - oslo::uint256_t (1))).to_string ());

			auto get_store_it = [& handle = handle](oslo::transaction const & transaction_source, std::string const & hash) {
				return oslo::store_iterator<std::array<char, 64>, oslo::no_value> (std::make_unique<oslo::mdb_iterator<std::array<char, 64>, oslo::no_value>> (transaction_source, handle, oslo::mdb_val (hash.size (), const_cast<char *> (hash.c_str ()))));
			};

			// First do a read pass to check if there are any wallets that need extracting (to save holding a write lock and potentially being blocked)
			auto wallets_need_splitting (false);
			{
				auto transaction_source (store_l->tx_begin_read ());
				auto i = get_store_it (transaction_source, beginning);
				auto n = get_store_it (transaction_source, end);
				wallets_need_splitting = (i != n);
			}

			if (wallets_need_splitting)
			{
				auto transaction_source (store_l->tx_begin_write ());
				auto i = get_store_it (transaction_source, beginning);
				auto n = get_store_it (transaction_source, end);
				auto tx_source = static_cast<MDB_txn *> (transaction_source.get_handle ());
				auto tx_destination = static_cast<MDB_txn *> (transaction_destination.get_handle ());
				for (; i != n; ++i)
				{
					oslo::uint256_union id;
					std::string text (i->first.data (), i->first.size ());
					auto error1 (id.decode_hex (text));
					(void)error1;
					debug_assert (!error1);
					debug_assert (strlen (text.c_str ()) == text.size ());
					move_table (text, tx_source, tx_destination);
				}
			}
		}
	}
}

void oslo::wallets::move_table (std::string const & name_a, MDB_txn * tx_source, MDB_txn * tx_destination)
{
	MDB_dbi handle_source;
	auto error2 (mdb_dbi_open (tx_source, name_a.c_str (), MDB_CREATE, &handle_source));
	(void)error2;
	debug_assert (!error2);
	MDB_dbi handle_destination;
	auto error3 (mdb_dbi_open (tx_destination, name_a.c_str (), MDB_CREATE, &handle_destination));
	(void)error3;
	debug_assert (!error3);
	MDB_cursor * cursor;
	auto error4 (mdb_cursor_open (tx_source, handle_source, &cursor));
	(void)error4;
	debug_assert (!error4);
	MDB_val val_key;
	MDB_val val_value;
	auto cursor_status (mdb_cursor_get (cursor, &val_key, &val_value, MDB_FIRST));
	while (cursor_status == MDB_SUCCESS)
	{
		auto error5 (mdb_put (tx_destination, handle_destination, &val_key, &val_value, 0));
		(void)error5;
		debug_assert (!error5);
		cursor_status = mdb_cursor_get (cursor, &val_key, &val_value, MDB_NEXT);
	}
	auto error6 (mdb_drop (tx_source, handle_source, 1));
	(void)error6;
	debug_assert (!error6);
}

oslo::uint128_t const oslo::wallets::generate_priority = std::numeric_limits<oslo::uint128_t>::max ();
oslo::uint128_t const oslo::wallets::high_priority = std::numeric_limits<oslo::uint128_t>::max () - 1;

oslo::store_iterator<oslo::account, oslo::wallet_value> oslo::wallet_store::begin (oslo::transaction const & transaction_a)
{
	oslo::store_iterator<oslo::account, oslo::wallet_value> result (std::make_unique<oslo::mdb_iterator<oslo::account, oslo::wallet_value>> (transaction_a, handle, oslo::mdb_val (oslo::account (special_count))));
	return result;
}

oslo::store_iterator<oslo::account, oslo::wallet_value> oslo::wallet_store::begin (oslo::transaction const & transaction_a, oslo::account const & key)
{
	oslo::store_iterator<oslo::account, oslo::wallet_value> result (std::make_unique<oslo::mdb_iterator<oslo::account, oslo::wallet_value>> (transaction_a, handle, oslo::mdb_val (key)));
	return result;
}

oslo::store_iterator<oslo::account, oslo::wallet_value> oslo::wallet_store::find (oslo::transaction const & transaction_a, oslo::account const & key)
{
	auto result (begin (transaction_a, key));
	oslo::store_iterator<oslo::account, oslo::wallet_value> end (nullptr);
	if (result != end)
	{
		if (result->first == key)
		{
			return result;
		}
		else
		{
			return end;
		}
	}
	else
	{
		return end;
	}
	return result;
}

oslo::store_iterator<oslo::account, oslo::wallet_value> oslo::wallet_store::end ()
{
	return oslo::store_iterator<oslo::account, oslo::wallet_value> (nullptr);
}
oslo::mdb_wallets_store::mdb_wallets_store (boost::filesystem::path const & path_a, oslo::lmdb_config const & lmdb_config_a) :
environment (error, path_a, oslo::mdb_env::options::make ().set_config (lmdb_config_a).override_config_sync (oslo::lmdb_config::sync_strategy::always).override_config_map_size (1ULL * 1024 * 1024 * 1024))
{
}

bool oslo::mdb_wallets_store::init_error () const
{
	return error;
}

MDB_txn * oslo::wallet_store::tx (oslo::transaction const & transaction_a) const
{
	return static_cast<MDB_txn *> (transaction_a.get_handle ());
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (wallets & wallets, const std::string & name)
{
	size_t items_count;
	size_t actions_count;
	{
		oslo::lock_guard<std::mutex> guard (wallets.mutex);
		items_count = wallets.items.size ();
		actions_count = wallets.actions.size ();
	}

	auto sizeof_item_element = sizeof (decltype (wallets.items)::value_type);
	auto sizeof_actions_element = sizeof (decltype (wallets.actions)::value_type);
	auto sizeof_watcher_element = sizeof (decltype (wallets.watcher->watched)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "items", items_count, sizeof_item_element }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "actions", actions_count, sizeof_actions_element }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "work_watcher", wallets.watcher->size (), sizeof_watcher_element }));
	return composite;
}
