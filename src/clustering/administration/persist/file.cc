// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/administration/persist/file.hpp"

#include "rocksdb/options.h"
#include "rocksdb/write_batch.h"

#include "arch/io/disk.hpp"
#include "btree/depth_first_traversal.hpp"
#include "btree/types.hpp"
#include "buffer_cache/blob.hpp"
#include "buffer_cache/cache_balancer.hpp"
#include "buffer_cache/serialize_onto_blob.hpp"
#include "clustering/administration/persist/migrate/migrate_v1_16.hpp"
#include "clustering/administration/persist/migrate/migrate_v2_1.hpp"
#include "clustering/administration/persist/migrate/migrate_v2_3.hpp"
#include "clustering/administration/persist/migrate/rewrite.hpp"
#include "config/args.hpp"
#include "logger.hpp"
#include "paths.hpp"
#include "serializer/log/log_serializer.hpp"
#include "serializer/merger.hpp"

// TODO: Remove obsolete stuff like this.
const uint64_t METADATA_CACHE_SIZE = 32 * MEGABYTE;

const std::string METADATA_PREFIX = "rethinkdb/metadata/";
const std::string METADATA_VERSION_KEY = "rethinkdb/metadata/version";
const std::string METADATA_VERSION_VALUE = "v2_4";

ATTR_PACKED(struct metadata_disk_superblock_t {
    block_magic_t magic;

    block_id_t root_block;
    block_id_t stat_block;
});

// Etymology: In version 1.13, the magic was 'RDmd', for "(R)ethink(D)B (m)eta(d)ata".
// Every subsequent version, the last character has been incremented.
static const block_magic_t metadata_sb_magic = { { 'R', 'D', 'm', 'l' } };

void init_metadata_superblock(void *sb_void, size_t block_size) {
    memset(sb_void, 0, block_size);
    metadata_disk_superblock_t *sb = static_cast<metadata_disk_superblock_t *>(sb_void);
    sb->magic = metadata_sb_magic;
    sb->root_block = NULL_BLOCK_ID;
    sb->stat_block = NULL_BLOCK_ID;
}

void update_metadata_superblock_version(void *sb_void) {
    metadata_disk_superblock_t *sb = static_cast<metadata_disk_superblock_t *>(sb_void);
    sb->magic = metadata_sb_magic;
}

cluster_version_t magic_to_version(block_magic_t magic) {
    guarantee(magic.bytes[0] == metadata_sb_magic.bytes[0]);
    guarantee(magic.bytes[1] == metadata_sb_magic.bytes[1]);
    guarantee(magic.bytes[2] == metadata_sb_magic.bytes[2]);
    switch (magic.bytes[3]) {
    case 'd': // obsolete version - v1.13
        fail_due_to_user_error("This version of RethinkDB cannot migrate in-place "
            "from databases created by versions older than RethinkDB 1.14.");
    case 'e': return cluster_version_t::v1_14;
    case 'f': return cluster_version_t::v1_15;
    case 'g': return cluster_version_t::v1_16;
    case 'h': return cluster_version_t::v2_0;
    case 'i': return cluster_version_t::v2_1;
    case 'j': return cluster_version_t::v2_2;
    case 'k': return cluster_version_t::v2_3;
    case 'l': return cluster_version_t::v2_4_is_latest_disk;
    default:
        fail_due_to_user_error("You're trying to use an earlier version of RethinkDB "
            "to open a database created by a later version of RethinkDB.");
    }
    // This is here so you don't forget to add new versions above.
    // Please also update the value of metadata_sb_magic at the top of this file!
    static_assert(cluster_version_t::LATEST_DISK == cluster_version_t::v2_4,
        "Please add new version to magic_to_version.");
}

class metadata_superblock_t : public superblock_t {
public:
    explicit metadata_superblock_t(buf_lock_t &&sb_buf) : sb_buf_(std::move(sb_buf)) { }
    void release() {
        sb_buf_.reset_buf_lock();
    }
    block_id_t get_root_block_id() {
        buf_read_t read(&sb_buf_);
        auto ptr = static_cast<const metadata_disk_superblock_t *>(read.get_data_read());
        return ptr->root_block;
    }
    void set_root_block_id(const block_id_t new_root_block) {
        buf_write_t write(&sb_buf_);
        auto ptr = static_cast<metadata_disk_superblock_t *>(write.get_data_write());
        ptr->root_block = new_root_block;
    }
    block_id_t get_stat_block_id() {
        buf_read_t read(&sb_buf_);
        auto ptr = static_cast<const metadata_disk_superblock_t *>(read.get_data_read());
        return ptr->stat_block;
    }
    void set_stat_block_id(const block_id_t new_stat_block) {
        buf_write_t write(&sb_buf_);
        auto ptr = static_cast<metadata_disk_superblock_t *>(write.get_data_write());
        ptr->stat_block = new_stat_block;
    }
    buf_parent_t expose_buf() {
        return buf_parent_t(&sb_buf_);
    }
private:
    buf_lock_t sb_buf_;
};

class metadata_value_sizer_t : public value_sizer_t {
public:
    explicit metadata_value_sizer_t(max_block_size_t _bs) : bs(_bs) { }
    int size(const void *value) const {
        return blob::ref_size(
            bs,
            static_cast<const char *>(value),
            blob::btree_maxreflen);
    }
    bool fits(const void *value, int length_available) const {
        return blob::ref_fits(
            bs,
            length_available,
            static_cast<const char *>(value),
            blob::btree_maxreflen);
    }
    int max_possible_size() const {
        return blob::btree_maxreflen;
    }
    block_magic_t btree_leaf_magic() const {
        return block_magic_t { { 'R', 'D', 'l', 'n' } };
    }
    max_block_size_t block_size() const {
        return bs;
    }
private:
    max_block_size_t bs;
};

class metadata_value_deleter_t : public value_deleter_t {
public:
    void delete_value(buf_parent_t parent, const void *value) const {
        // To not destroy constness, we operate on a copy of the value
        metadata_value_sizer_t sizer(parent.cache()->max_block_size());
        scoped_malloc_t<void> value_copy(sizer.max_possible_size());
        memcpy(value_copy.get(), value, sizer.size(value));
        blob_t blob(
            parent.cache()->max_block_size(),
            static_cast<char *>(value_copy.get()),
            blob::btree_maxreflen);
        blob.clear(parent);
    }
};

class metadata_value_detacher_t : public value_deleter_t {
public:
    void delete_value(buf_parent_t parent, const void *value) const {
        /* This `const_cast` is needed because `blob_t` expects a non-const pointer. But
        it will not actually modify the contents if the only method we ever call is
        `detach_subtrees`. */
        blob_t blob(parent.cache()->max_block_size(),
                    static_cast<char *>(const_cast<void *>(value)),
                    blob::btree_maxreflen);
        blob.detach_subtrees(parent);
    }
};

metadata_file_t::read_txn_t::read_txn_t(
        metadata_file_t *f,
        signal_t *interruptor) :
    file(f),
    rwlock_acq(&file->rwlock, access_t::read, interruptor)
    { }

metadata_file_t::read_txn_t::read_txn_t(
        metadata_file_t *f,
        write_access_t,
        signal_t *interruptor) :
    file(f),
    rwlock_acq(&file->rwlock, access_t::write, interruptor)
    { }

void metadata_file_t::read_txn_t::blob_to_stream(
        buf_parent_t parent,
        const void *ref,
        const std::function<void(read_stream_t *)> &callback) {
    blob_t blob(
        file->cache->max_block_size(),
        /* `blob_t` requires a non-const pointer because it has functions that mutate the
        blob. But we're not using those functions. That's why there's a `const_cast`
        here. */
        static_cast<char *>(const_cast<void *>(ref)),
        blob::btree_maxreflen);
    blob_acq_t acq_group;
    buffer_group_t buf_group;
    blob.expose_all(parent, access_t::read, &buf_group, &acq_group);
    buffer_group_read_stream_t read_stream(const_view(&buf_group));
    callback(&read_stream);
}

std::pair<std::string, bool> metadata_file_t::read_txn_t::read_bin(
        const store_key_t &key) {
    return file->rocks->try_read(METADATA_PREFIX + key_to_unescaped_str(key));
}

void metadata_file_t::read_txn_t::read_many_bin(
        const store_key_t &key_prefix,
        const std::function<void(const std::string &key_suffix, read_stream_t *)> &cb,
        signal_t *interruptor) {
    // TODO: Use or remove interruptor.
    (void)interruptor;
    // TODO: Might there be any need to truly stream this?
    std::vector<std::pair<std::string, std::string>> all
        = file->rocks->read_all_prefixed(METADATA_PREFIX + key_to_unescaped_str(key_prefix));
    const size_t prefix_size = key_prefix.size();
    for (auto& p : all) {
        guarantee(p.first.size() >= prefix_size);
        guarantee(memcmp(p.first.data(), key_prefix.contents(), prefix_size) == 0);
        std::string suffix = p.first.substr(prefix_size);
        string_read_stream_t stream(std::move(p.second), 0);
        cb(suffix, &stream);
    }

    return;
}

metadata_file_t::write_txn_t::write_txn_t(
        metadata_file_t *_file,
        signal_t *interruptor) :
    read_txn_t(_file, write_access_t::write, interruptor)
    { }

void metadata_file_t::write_txn_t::write_bin(
        const store_key_t &key,
        const write_message_t *msg,
        signal_t *interruptor) {
    // TODO: Use or remove interruptor param.
    (void)interruptor;
    // TODO: Verify that we can stack writes and edletes on a rocksdb WriteBatch.
    std::string rockskey = METADATA_PREFIX + key_to_unescaped_str(key);
    if (msg == nullptr) {
        batch.Delete(rockskey);
    } else {
        string_stream_t stream;
        int res = send_write_message(&stream, msg);
        guarantee(res == 0);
        batch.Put(rockskey, stream.str());
    }
}

metadata_file_t::metadata_file_t(
        io_backender_t *io_backender,
        const base_path_t &base_path,
        perfmon_collection_t *perfmon_parent,
        signal_t *interruptor) :
    rocks(io_backender->rocks()),
    btree_stats(perfmon_parent, "metadata")
{
    filepath_file_opener_t file_opener(get_filename(base_path), io_backender);
    init_serializer(&file_opener, perfmon_parent);
    balancer.init(new dummy_cache_balancer_t(METADATA_CACHE_SIZE));
    cache.init(new cache_t(serializer.get(), balancer.get(), perfmon_parent));
    cache_conn.init(new cache_conn_t(cache.get()));

    /* Do not migrate data if necessary */
    if (interruptor->is_pulsed()) {
        throw interrupted_exc_t();
    }

    std::string metadata_version = rocks->read(METADATA_VERSION_KEY);
    if (metadata_version != METADATA_VERSION_VALUE) {
        // TODO
        throw std::runtime_error("Unsupported metadata version");
    }
}

metadata_file_t::metadata_file_t(
        io_backender_t *io_backender,
        const base_path_t &base_path,
        perfmon_collection_t *perfmon_parent,
        const std::function<void(write_txn_t *, signal_t *)> &initializer,
        signal_t *interruptor) :
    rocks(io_backender->rocks()),
    btree_stats(perfmon_parent, "metadata")
{
    filepath_file_opener_t file_opener(get_filename(base_path), io_backender);
    log_serializer_t::create(
        &file_opener,
        log_serializer_t::static_config_t());
    init_serializer(&file_opener, perfmon_parent);
    balancer.init(new dummy_cache_balancer_t(METADATA_CACHE_SIZE));
    cache.init(new cache_t(serializer.get(), balancer.get(), perfmon_parent));
    cache_conn.init(new cache_conn_t(cache.get()));

    if (interruptor->is_pulsed()) {
        throw interrupted_exc_t();
    }

    rocks->insert(METADATA_VERSION_KEY, METADATA_VERSION_VALUE);

    {
        cond_t non_interruptor;
        write_txn_t write_txn(this, &non_interruptor);
        initializer(&write_txn, &non_interruptor);
        write_txn.commit();
    }

    file_opener.move_serializer_file_to_permanent_location();
}

void metadata_file_t::init_serializer(
        filepath_file_opener_t *file_opener,
        perfmon_collection_t *perfmon_parent) {
    scoped_ptr_t<log_serializer_t> standard_ser(
        new log_serializer_t(
            log_serializer_t::dynamic_config_t(),
            file_opener,
            perfmon_parent));
    if (!standard_ser->coop_lock_and_check()) {
        throw file_in_use_exc_t();
    }
    serializer.init(new merger_serializer_t(
        std::move(standard_ser),
        MERGER_SERIALIZER_MAX_ACTIVE_WRITES));
}

serializer_filepath_t metadata_file_t::get_filename(const base_path_t &path) {
    return serializer_filepath_t(path, "metadata");
}

metadata_file_t::~metadata_file_t() {
    /* This is defined in the `.cc` file so the `.hpp` file doesn't need to see the
    definitions of `log_serializer_t` and `cache_balancer_t`. */
}

