#include <DB/Dictionaries/MongoDBBlockInputStream.h>
#include <DB/Dictionaries/MongoDBDictionarySource.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <mongo/client/dbclient.h>
#include <ext/collection_cast.hpp>
#include <ext/enumerate.hpp>
#include <ext/size.hpp>


namespace DB
{

namespace ErrorCodes
{
	extern const int UNSUPPORTED_METHOD;
	extern const int WRONG_PASSWORD;
	extern const int MONGODB_INIT_FAILED;
}


static const size_t max_block_size = 8192;

/// mongo-cxx driver requires global initialization before using any functionality
static void init()
{
	static const auto mongo_init_status = mongo::client::initialize();

	if (!mongo_init_status.isOK())
		throw DB::Exception{
			"mongo::client::initialize() failed: " + mongo_init_status.toString(),
			ErrorCodes::MONGODB_INIT_FAILED
		};

	LOG_TRACE(&Logger::get("MongoDBDictionarySource"), "mongo::client::initialize() ok");
}



MongoDBDictionarySource::MongoDBDictionarySource(
	const DictionaryStructure & dict_struct, const std::string & host, const std::string & port,
	const std::string & user, const std::string & password,
	const std::string & db, const std::string & collection,
	const Block & sample_block)
	: dict_struct{dict_struct}, host{host}, port{port}, user{user}, password{password},
		db{db}, collection{collection}, sample_block{sample_block},
		connection{std::make_unique<mongo::DBClientConnection>(true)}
{
	init();

	connection->connect(host + ':' + port);

	/// @todo: should connection.auth be called after or before .connect ?
	if (!user.empty())
	{
		std::string error;
		if (!connection->auth(db, user, password, error))
			throw DB::Exception{
				"Could not authenticate to a MongoDB database " + db + " with provided credentials: " + error,
				ErrorCodes::WRONG_PASSWORD
			};
	}

	/// compose BSONObj containing all requested fields
	mongo::BSONObjBuilder builder;
	builder << "_id" << 0;

	for (const auto & column : sample_block.getColumns())
		builder << column.name << 1;

	fields_to_query = std::make_unique<mongo::BSONObj>(builder.obj());
}


MongoDBDictionarySource::MongoDBDictionarySource(
	const DictionaryStructure & dict_struct, const Poco::Util::AbstractConfiguration & config,
	const std::string & config_prefix, Block & sample_block)
	: MongoDBDictionarySource{
		dict_struct,
		config.getString(config_prefix + ".host"),
		config.getString(config_prefix + ".port"),
		config.getString(config_prefix + ".user", ""),
		config.getString(config_prefix + ".password", ""),
		config.getString(config_prefix + ".db", ""),
		config.getString(config_prefix + ".collection"),
		sample_block
	}
{
}


MongoDBDictionarySource::MongoDBDictionarySource(const MongoDBDictionarySource & other)
	: MongoDBDictionarySource{
		other.dict_struct, other.host, other.port, other.user, other.password,
		other.db, other.collection, other.sample_block}
{
}


MongoDBDictionarySource::~MongoDBDictionarySource() = default;


BlockInputStreamPtr MongoDBDictionarySource::loadAll()
{
	return std::make_shared<MongoDBBlockInputStream>(
		connection->query(db + '.' + collection, {}, 0, 0, fields_to_query.get()),
		sample_block, max_block_size);
}


BlockInputStreamPtr MongoDBDictionarySource::loadIds(const std::vector<UInt64> & ids)
{
	if (!dict_struct.id)
		throw Exception{"'id' is required for selective loading", ErrorCodes::UNSUPPORTED_METHOD};

	/// mongo::BSONObj has shitty design and does not use fixed width integral types
	const auto query = BSON(
		dict_struct.id.value().name << BSON("$in" << ext::collection_cast<std::vector<long long int>>(ids)));

	return std::make_shared<MongoDBBlockInputStream>(
		connection->query(db + '.' + collection, query, 0, 0, fields_to_query.get()),
		sample_block, max_block_size);
}


BlockInputStreamPtr MongoDBDictionarySource::loadKeys(
	const ConstColumnPlainPtrs & key_columns, const std::vector<std::size_t> & requested_rows)
{
	if (!dict_struct.key)
		throw Exception{"'key' is required for selective loading", ErrorCodes::UNSUPPORTED_METHOD};

	std::string query_string;

	{
		WriteBufferFromString out{query_string};

		writeString("{$or:[", out);

		auto first = true;

		for (const auto row : requested_rows)
		{
			if (!first)
				writeChar(',', out);

			first = false;

			writeChar('{', out);

			for (const auto idx_key : ext::enumerate(*dict_struct.key))
			{
				if (idx_key.first != 0)
					writeChar(',', out);

				writeString(idx_key.second.name, out);
				writeChar(':', out);
				idx_key.second.type->serializeTextQuoted(*key_columns[idx_key.first], row, out);
			}

			writeChar('}', out);
		}

		writeString("]}", out);
	}

	return std::make_shared<MongoDBBlockInputStream>(
		connection->query(db + '.' + collection, query_string, 0, 0, fields_to_query.get()),
		sample_block, max_block_size);
}


std::string MongoDBDictionarySource::toString() const
{
	return "MongoDB: " + db + '.' + collection + ',' + (user.empty() ? " " : " " + user + '@') + host + ':' + port;
}

}
