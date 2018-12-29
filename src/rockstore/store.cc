#include "rockstore/store.hpp"

#include "rocksdb/db.h"

#include "arch/runtime/thread_pool.hpp"

namespace rockstore {

store create_rockstore(const std::string &base_dir) {
    std::string rocks_path = base_dir + "/rockstore";
    rocksdb::DB *db;
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        rocksdb::Options options;
        options.create_if_missing = true;
        status = rocksdb::DB::Open(options, rocks_path, &db);
    });
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("Could not create rockstore");
    }
    store ret(db);
    return ret;
}




}  // namespace rockstore
