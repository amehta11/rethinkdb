#ifndef RETHINKDB_ROCKSTORE_STORE_HPP_
#define RETHINKDB_ROCKSTORE_STORE_HPP_

#include <string>

namespace rocksdb {
class DB;
}

namespace rockstore {

class store {
private:
    explicit store(rocksdb::DB *db) : db_(db) {}
    friend store create_rockstore(const std::string &base_dir);
    rocksdb::DB *db_ = nullptr;
};

// Creates the db's sole global rocksdb store.  Called once in the lifetime of the data
// directory.  Throws std::runtime_error.
store create_rockstore(const std::string &base_dir);


}  // namespace rockstore

#endif  // RETHINKDB_ROCKSTORE_STORE_HPP_
