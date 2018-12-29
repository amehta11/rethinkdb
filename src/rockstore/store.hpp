#ifndef RETHINKDB_ROCKSTORE_STORE_HPP_
#define RETHINKDB_ROCKSTORE_STORE_HPP_

#include <string>
#include <vector>

namespace rocksdb {
class DB;
class WriteBatch;
}

class base_path_t;


namespace rockstore {

class store {
public:
    // Throws std::runtime_error.
    std::string read(const std::string &key);
    // Throws std::runtime_error.  False if value not found.
    std::pair<std::string, bool> try_read(const std::string &key);

    std::vector<std::pair<std::string, std::string>> read_all_prefixed(std::string prefix);

    // Throws std::runtime_error.
    void insert(const std::string &key, const std::string &value);
    // Throws std::runtime_error.
    void remove(const std::string &key);

    // Throws std::runtime_error.
    void write_batch(rocksdb::WriteBatch&& batch);

private:
    explicit store(rocksdb::DB *db) : db_(db) {}
    friend store create_rockstore(const base_path_t &base_path);
    rocksdb::DB *db_ = nullptr;
};

// Creates the db's sole global rocksdb store.  Called once in the lifetime of the data
// directory.  Throws std::runtime_error.
store create_rockstore(const base_path_t &base_path);


}  // namespace rockstore

#endif  // RETHINKDB_ROCKSTORE_STORE_HPP_
