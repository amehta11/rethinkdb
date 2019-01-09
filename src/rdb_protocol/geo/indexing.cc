// Copyright 2010-2016 RethinkDB, all rights reserved.
#include "rdb_protocol/geo/indexing.hpp"

#include <string>
#include <vector>

// TODO: Remove this include if we wrap the iterator stuff...
#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"

#include "arch/runtime/thread_pool.hpp"
#include "btree/keys.hpp"
#include "btree/leaf_node.hpp"
#include "btree/reql_specific.hpp"
#include "concurrency/interruptor.hpp"
#include "concurrency/signal.hpp"
#include "containers/scoped.hpp"
#include "rdb_protocol/geo/exceptions.hpp"
#include "rdb_protocol/geo/geojson.hpp"
#include "rdb_protocol/geo/geo_visitor.hpp"
#include "rdb_protocol/geo/s2/s2cell.h"
#include "rdb_protocol/geo/s2/s2cellid.h"
#include "rdb_protocol/geo/s2/s2latlngrect.h"
#include "rdb_protocol/geo/s2/s2polygon.h"
#include "rdb_protocol/geo/s2/s2polyline.h"
#include "rdb_protocol/geo/s2/s2regioncoverer.h"
#include "rdb_protocol/geo/s2/strings/strutil.h"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/pseudo_geometry.hpp"
#include "rockstore/rockshard.hpp"
#include "rockstore/store.hpp"

using geo::S2Cell;
using geo::S2CellId;
using geo::S2LatLngRect;
using geo::S2Point;
using geo::S2Polygon;
using geo::S2Polyline;
using geo::S2Region;
using geo::S2RegionCoverer;
using ql::datum_t;

// TODO (daniel): Consider making this configurable through an opt-arg
//   (...at index creation?)
extern const int GEO_INDEX_GOAL_GRID_CELLS = 8;

class compute_covering_t : public s2_geo_visitor_t<scoped_ptr_t<std::vector<S2CellId> > > {
public:
    explicit compute_covering_t(int goal_cells) {
        coverer_.set_max_cells(goal_cells);
    }

    scoped_ptr_t<std::vector<S2CellId> > on_point(const S2Point &point) {
        scoped_ptr_t<std::vector<S2CellId> > result(new std::vector<S2CellId>());
        result->push_back(S2CellId::FromPoint(point));
        return result;
    }
    scoped_ptr_t<std::vector<S2CellId> > on_line(const S2Polyline &line) {
        scoped_ptr_t<std::vector<S2CellId> > result(new std::vector<S2CellId>());
        coverer_.GetCovering(line, result.get());
        return result;
    }
    scoped_ptr_t<std::vector<S2CellId> > on_polygon(const S2Polygon &polygon) {
        scoped_ptr_t<std::vector<S2CellId> > result(new std::vector<S2CellId>());
        coverer_.GetCovering(polygon, result.get());
        return result;
    }
    scoped_ptr_t<std::vector<S2CellId> > on_latlngrect(const S2LatLngRect &rect) {
        scoped_ptr_t<std::vector<S2CellId> > result(new std::vector<S2CellId>());
        coverer_.GetCovering(rect, result.get());
        return result;
    }

private:
    S2RegionCoverer coverer_;
};

/* The interior covering is a set of grid cells that are guaranteed to be fully
contained in the geometry. This is useful for avoiding unnecessary intersection
tests during post-filtering. */
class compute_interior_covering_t :
    public s2_geo_visitor_t<scoped_ptr_t<std::vector<S2CellId> > > {
public:
    explicit compute_interior_covering_t(const std::vector<S2CellId> &exterior_covering)
        : exterior_covering_(exterior_covering) { }

    scoped_ptr_t<std::vector<S2CellId> > on_point(const S2Point &) {
        scoped_ptr_t<std::vector<S2CellId> > result(new std::vector<S2CellId>());
        // A point's interior is thin, so no cell is going to fit into it.
        return result;
    }
    scoped_ptr_t<std::vector<S2CellId> > on_line(const S2Polyline &) {
        scoped_ptr_t<std::vector<S2CellId> > result(new std::vector<S2CellId>());
        // A line's interior is thin, so no cell is going to fit into it.
        return result;
    }
    scoped_ptr_t<std::vector<S2CellId> > on_polygon(const S2Polygon &polygon) {
        return from_exterior(polygon);
    }
    scoped_ptr_t<std::vector<S2CellId> > on_latlngrect(const S2LatLngRect &rect) {
        return from_exterior(rect);
    }

private:
    scoped_ptr_t<std::vector<S2CellId> > from_exterior(const S2Region &region) {
        scoped_ptr_t<std::vector<S2CellId> > result(new std::vector<S2CellId>());
        // S2RegionCoverer has a `GetInteriorCovering` method.
        // However it's *extremely* slow (often in the order of a second or more).
        // We do something faster, at the risk of returning an empty or very sparse
        // covering more often: We simply take the regular covering of the polygon,
        // subdivide each cell at most once, and then prune out cells that are not
        // fully contained in the polygon.
        for (const auto &cell : exterior_covering_) {
            S2Cell parent(cell);
            S2Cell children[4];
            if (region.Contains(parent)) {
                result->push_back(parent.id());
            } else if (parent.Subdivide(children)) {
                for (size_t i = 0; i < 4; ++i) {
                    if (region.Contains(children[i])) {
                        result->push_back(children[i].id());
                    }
                }
            }
        }
        return result;
    }

    std::vector<S2CellId> exterior_covering_;
};


std::string s2cellid_to_key(S2CellId id) {
    // The important property of the result is that its lexicographic
    // ordering as a string must be equivalent to the integer ordering of id.
    // FastHex64ToBuffer() generates a hex representation of id that fulfills this
    // property (it comes padded with leading '0's).
    char buffer[geo::kFastToBufferSize];
    // "GC" = Geospatial Cell
    return std::string("GC") + geo::FastHex64ToBuffer(id.id(), buffer);
}

S2CellId key_to_s2cellid(const std::string &sid) {
    guarantee(sid.length() >= 2
              && sid[0] == 'G'
              && sid[1] == 'C');
    return S2CellId::FromToken(sid.substr(2));
}

/* Returns the S2CellId corresponding to the given key, which must be a correctly
formatted sindex key. */
S2CellId btree_key_to_s2cellid(const btree_key_t *key) {
    rassert(key != NULL);
    std::string tmp(reinterpret_cast<const char *>(key->contents), key->size);
    return key_to_s2cellid(
        datum_t::extract_secondary(tmp));
}

/* `key_or_null` represents a point to the left or right of a key in the B-tree
key-space. If `nullptr`, it means the point left of the leftmost key; otherwise, it means
the point right of `*key_or_null`. It need not be a valid sindex key.

`order_btree_key_relative_to_s2cellid_keys()` figures out where `key_or_null` lies
relative to geospatial sindex keys. There are four possible outcomes:
  - `key_or_null` lies within a range of sindex keys for a specific `S2CellId`. It will
    return `(cell ID, true)`.
  - `key_or_null` lies between two ranges of sindex keys for different `S2CellId`s. It
    will return `(cell ID to the right, false)`.
  - `key_or_null` lies after all possible sindex keys for `S2CellId`s. It will return
    `(S2CellId::Sentinel(), false)`.
  - `key_or_null` lies before all possible sindex keys for `S2CellId`s. It will return
    `(S2CellId::FromFacePosLevel(0, 0, geo::S2::kMaxCellLevel), false)`. */
std::pair<S2CellId, bool> order_btree_key_relative_to_s2cellid_keys(
        const btree_key_t *key_or_null) {
    static const std::pair<S2CellId, bool> before_all(
        S2CellId::FromFacePosLevel(0, 0, geo::S2::kMaxCellLevel), false);
    static const std::pair<S2CellId, bool> after_all(
        S2CellId::Sentinel(), false);

    /* A well-formed sindex key will start with the characters 'GC'. */
    uint8_t first_char = 'G';
    if (key_or_null == nullptr || key_or_null->size == 0) return before_all;
    if (key_or_null->contents[0] < first_char) return before_all;
    if (key_or_null->contents[0] > first_char) return after_all;
    if (key_or_null->size == 1) return before_all;
    if (key_or_null->contents[1] < 'C') return before_all;
    if (key_or_null->contents[1] > 'C') return after_all;

    /* A well-formed sindex key will next have 16 hexadecimal digits, using lowercase
    letters. If `key_or_null` starts with such a well-formed string, we'll set
    `cell_number` to the number represented by that string and `inside_cell` to `true`.
    Otherwise we'll set `cell_number` to the smallest number represented by a larger
    string and `inside_cell()` to `false`. */
    uint64_t cell_number = 0;
    bool inside_cell = true;
    for (int i = 0; i < 16; ++i) {
        if (i + 2 >= key_or_null->size) {
            /* The string is too short. For example, "123" -> (0x1230..., false). */
            inside_cell = false;
            break;
        }
        uint8_t hex_digit = key_or_null->contents[i + 2];
        if (hex_digit >= '0' && hex_digit <= '9') {
            /* The string is still valid, so keep going. */
            cell_number += static_cast<uint64_t>(hex_digit - '0') << (4 * (15 - i));
        } else if (hex_digit >= 'a' && hex_digit <= 'f') {
            /* The string is still valid, so keep going. */
            cell_number +=
                static_cast<uint64_t>(10 + (hex_digit - 'a')) << (4 * (15 - i));
        } else if (hex_digit < '0') {
            /* For example, "123/..." -> (0x1230..., false). ('/' comes before '0' in
            ASCII order.) */
            inside_cell = false;
            break;
        } else if (hex_digit > 'f') {
            /* For example, "123g..." -> (0x1240..., false). */
            if (i == 0) {
                /* This case corresponds to "g123..." -> (sentinel, false). We have to
                handle this separately from the other overflow case below because if
                `i` is zero then `change` won't fit into a 64-bit int. */
                return after_all;
            }
            uint64_t change = static_cast<uint64_t>(16) << (4 * (15 - i));
            if (change > 0xffffffffffffffffull - cell_number) {
                /* This case corresponds to "fffg..." -> (sentinel, false). */
                return after_all;
            }
            cell_number += change;
            inside_cell = false;
            break;
        } else if (hex_digit > '9' && hex_digit < 'a') {
            /* For example, "123:..." -> (0x123a..., false). (':' comes after '9' in
            ASCII order.) */
            cell_number += static_cast<uint64_t>(10) << (4 * (15 - i));
            inside_cell = false;
            break;
        } else {
            unreachable();
        }
    }

    /* Not all 64-bit integers are valid S2 cell IDs. There are two possible problems:
      - The face index can be 6 or 7. In this case, the key is larger than any valid ID,
        since the face index is the most significant three bits.
      - The last bit is not set properly. In this case, we set the first bit that we
        can set that will turn it into a valid cell ID. In this case we have to set
        `inside_cell` to `false`. */
    S2CellId cell_id(cell_number);
    if (cell_id.face() >= 6) return after_all;
    if (!cell_id.is_valid()) {
        inside_cell = false;
        cell_id = S2CellId(cell_number | 1);
        guarantee(cell_id.is_valid());
    }

    return std::make_pair(cell_id, inside_cell);
}

std::vector<std::string> compute_index_grid_keys(
        const ql::datum_t &key, int goal_cells) {
    // Compute a cover of grid cells
    std::vector<S2CellId> covering = compute_cell_covering(key, goal_cells);

    // Generate keys
    std::vector<std::string> result;
    result.reserve(covering.size());
    for (size_t i = 0; i < covering.size(); ++i) {
        result.push_back(s2cellid_to_key(covering[i]));
    }

    return result;
}

// Helper for `compute_cell_covering` and `compute_interior_cell_covering`
std::vector<S2CellId> compute_cell_covering(
        const ql::datum_t &key, int goal_cells) {
    rassert(key.has());
    if (!key.is_ptype(ql::pseudo::geometry_string)) {
        throw geo_exception_t(
            "Expected geometry but found " + key.get_type_name() + ".");
    }
    if (goal_cells <= 0) {
        throw geo_exception_t("goal_cells must be positive (and should be >= 4).");
    }

    // Compute a covering of grid cells
    compute_covering_t coverer(goal_cells);
    scoped_ptr_t<std::vector<S2CellId> > covering = visit_geojson(&coverer, key);
    return *covering;
}

std::vector<S2CellId> compute_interior_cell_covering(
        const ql::datum_t &key, const std::vector<S2CellId> &exterior_covering) {
    if (!key.is_ptype(ql::pseudo::geometry_string)) {
        throw geo_exception_t(
            "Expected geometry but found " + key.get_type_name() + ".");
    }

    // Compute an interior covering of grid cells
    compute_interior_covering_t coverer(exterior_covering);
    scoped_ptr_t<std::vector<S2CellId> > covering = visit_geojson(&coverer, key);
    return *covering;
}

geo_index_traversal_helper_t::geo_index_traversal_helper_t(
        const signal_t *interruptor)
    : is_initialized_(false), interruptor_(interruptor) { }

// Computes the query cells' ancestors, deduped and in sorted order.
std::vector<geo::S2CellId> compute_ancestors(const std::vector<geo::S2CellId> &query_cells) {
    std::vector<geo::S2CellId> build;
    for (geo::S2CellId cell : query_cells) {
        geo::S2CellId c = cell;
        while (c.level() != 0) {
            c = c.parent();
            build.push_back(c);
        }
    }
    std::sort(build.begin(), build.end());
    build.erase(std::unique(build.begin(), build.end()), build.end());
    return build;
}

void geo_index_traversal_helper_t::init_query(
        const std::vector<geo::S2CellId> &query_cell_covering,
        const std::vector<geo::S2CellId> &query_interior_cell_covering) {
    guarantee(!is_initialized_);
    rassert(query_cells_.empty());
    query_cells_ = query_cell_covering;
    std::sort(query_cells_.begin(), query_cells_.end());
    query_cell_ancestors_ = compute_ancestors(query_cells_);
    query_interior_cells_ = query_interior_cell_covering;
    is_initialized_ = true;
}

continue_bool_t
geo_index_traversal_helper_t::handle_pair(
    std::pair<const char *, size_t> key, std::pair<const char *, size_t> value)
        THROWS_ONLY(interrupted_exc_t) {
    guarantee(is_initialized_);

    if (interruptor_->is_pulsed()) {
        throw interrupted_exc_t();
    }

    store_key_t skey(key.second, reinterpret_cast<const uint8_t *>(key.first));
    const S2CellId key_cell = btree_key_to_s2cellid(skey.btree_key());
    if (any_cell_intersects(query_cells_, key_cell.range_min(), key_cell.range_max())) {
        bool definitely_intersects_if_point =
            any_cell_contains(query_interior_cells_, key_cell);
        return on_candidate(key, value, definitely_intersects_if_point);
    } else {
        return continue_bool_t::CONTINUE;
    }
}

// TODO: Remove if unused.
bool geo_index_traversal_helper_t::any_query_cell_intersects(
        const btree_key_t *left_excl_or_null, const btree_key_t *right_incl) const {
    std::pair<S2CellId, bool> left =
        order_btree_key_relative_to_s2cellid_keys(left_excl_or_null);
    std::pair<S2CellId, bool> right =
        order_btree_key_relative_to_s2cellid_keys(right_incl);

    /* This is more conservative than necessary. For example, if `left_excl_or_null` is
    after the largest possible cell or `right_incl` is before the smallest possible cell,
    we could shortcut and return `false`, but we don't. Also, if `right.second` were
    `false`, we could use the cell immediately before `right.first` instead of using
    `right.first`. But that would be more trouble than it's worth. */
    S2CellId left_cell = left.first == S2CellId::Sentinel()
        ? S2CellId::FromFacePosLevel(5, 0, 0) : left.first;
    S2CellId right_cell = right.first == S2CellId::Sentinel()
        ? S2CellId::FromFacePosLevel(5, 0, 0) : right.first;

    // Determine a S2CellId range that is a superset of what's intersecting
    // with anything stored in [left_cell, right_cell].
    int common_level;
    if (left_cell.face() != right_cell.face()) {
        // Case 1: left_cell and right_cell are on different faces of the cube.
        // In that case [left_cell, right_cell] intersects at most with the full
        // range of faces in the range [left_cell.face(), right_cell.range()].
        guarantee(left_cell.face() < right_cell.face());
        common_level = 0;
    } else {
        // Case 2: left_cell and right_cell are on the same face. We locate
        // their smallest common parent. [left_cell, right_cell] can at most
        // intersect with anything below their common parent.
        common_level = std::min(left_cell.level(), right_cell.level());
        while (left_cell.parent(common_level) != right_cell.parent(common_level)) {
            guarantee(common_level > 0);
            --common_level;
        }
    }
    S2CellId range_min = left_cell.parent(common_level).range_min();
    S2CellId range_max = right_cell.parent(common_level).range_max();

    return any_cell_intersects(query_cells_, range_min, range_max);
}

bool geo_index_traversal_helper_t::any_cell_intersects(
        const std::vector<S2CellId> &cells,
        const S2CellId left_min, const S2CellId right_max) {
    // Check if any of the cells intersects with the given range
    for (const auto &cell : cells) {
        if (cell_intersects_with_range(cell, left_min, right_max)) {
            return true;
        }
    }
    return false;
}

bool geo_index_traversal_helper_t::cell_intersects_with_range(
        const S2CellId c,
        const S2CellId left_min, const S2CellId right_max) {
    return left_min <= c.range_max() && right_max >= c.range_min();
}

bool geo_index_traversal_helper_t::any_cell_contains(
        const std::vector<S2CellId> &cells,
        const S2CellId key) {
    // Check if any of the cells contains `key`
    for (const auto &cell : cells) {
        if (cell.contains(key)) {
            return true;
        }
    }
    return false;
}

// The job of this function is to advance pos forward (or not at all) to the
// next key (or next key prefix) we're interested in.  This is either the
// beginning of a query cell, or an ancestor of a query cell, or pos itself, if
// pos lies within the range of a query cell range or ancestor cell value.
// Whatever is smallest and >=*pos, among all such values.
bool geo_index_traversal_helper_t::skip_forward_to_seek_key(std::string *pos) const {
    rassert(!query_cells_.empty());
    if (query_cells_.empty()) {  // TODO: Verify if this is impossible.
        return false;
    }

    // TODO: We parse this twice, I'm pretty sure.
    store_key_t skey(*pos);

    // TODO: Fragile code.
    geo::S2CellId pos_cell;
    if (*pos < "GC") {
        // The minimal cell id.
        pos_cell = geo::S2CellId(1);
    } else if (*pos < "GD") {
        pos_cell = btree_key_to_s2cellid(skey.btree_key());
    } else {
        return false;
    }

    bool has_candidate = false;
    geo::S2CellId candidate_pos;
    {
        auto it = std::lower_bound(query_cells_.begin(), query_cells_.end(), pos_cell);
        // The return pos might intersect *it or *(it-1).

        if (it != query_cells_.begin() && pos_cell.intersects(*(it - 1))) {
            // Don't advance pos, it's already in a range.
            return true;
        }
        if (it != query_cells_.end()) {
            if (pos_cell.intersects(*it)) {
                // Don't advance pos, it's already in a range.
                return true;
            }
            // First candidate is the beginning of a query cell (which we know
            // pos_cell is before, because it's before the midpoint and doesn't
            // intersect).
            candidate_pos = it->range_min();
            has_candidate = true;
        }
    }

    auto it = std::lower_bound(
        query_cell_ancestors_.begin(),
        query_cell_ancestors_.end(),
        pos_cell);
    if (it != query_cell_ancestors_.end()) {
        if (has_candidate) {
            candidate_pos = std::min<geo::S2CellId>(*it, candidate_pos);
        } else {
            has_candidate = true;
            candidate_pos = *it;
        }
    }
    if (has_candidate) {
        *pos = s2cellid_to_key(candidate_pos);
        return true;
    }
    return false;
}


continue_bool_t geo_traversal(
        rockshard rocksh,
        uuid_u sindex_uuid,
        sindex_superblock_t *superblock,
        release_superblock_t release_superblock,
        const key_range_t &sindex_range,
        geo_index_traversal_helper_t *helper) {
    std::string rocks_kv_prefix = rockstore::table_secondary_prefix(rocksh.table_id, rocksh.shard_no, sindex_uuid);

    // duh
    rocksdb::OptimisticTransactionDB *db = rocksh.rocks->db();

    // TODO: Use
    // linux_thread_pool_t::run_in_blocker_pool([&]() {

    // We'll overwrite prefixed_pos as we iterate.
    // TODO: Do we use prefixed_left_bound?
    std::string prefixed_left_bound = rocks_kv_prefix + key_to_unescaped_str(sindex_range.left);
    std::string prefixed_upper_bound;
    rocksdb::Slice prefixed_upper_bound_slice;
    // TODO: Proper rocksdb::ReadOptions()
    rocksdb::ReadOptions opts;
    if (!sindex_range.right.unbounded) {
        prefixed_upper_bound = rocks_kv_prefix + key_to_unescaped_str(sindex_range.right.key());
    } else {
        prefixed_upper_bound = rockstore::prefix_end(rocks_kv_prefix);
    }
    printf("prefixed_upper_bound = '%s'\n", prefixed_upper_bound.c_str());

    if (!prefixed_upper_bound.empty()) {
        // Note: prefixed_upper_bound_slice doesn't copy the string, it points into it.
        prefixed_upper_bound_slice = rocksdb::Slice(prefixed_upper_bound);
        opts.iterate_upper_bound = &prefixed_upper_bound_slice;
    }

    // TODO: Check if we must call NewIterator on the thread pool thread.
    // TODO: Switching threads for every key/value pair is kind of lame.
    scoped_ptr_t<rocksdb::Iterator> iter(db->NewIterator(opts));
    // Release superblock after snapshotted iterator created.
    if (release_superblock == release_superblock_t::RELEASE) {
        superblock->release();
    }

    // There are two modes of iteration:  Stepping forward to cells and cell
    // ancestors, and stepping through the contents of a cover cell or ancestor cell.

    std::string pos = key_to_unescaped_str(sindex_range.left);

    for (;;) {
        // At this point, we want to advance the iterator forward to the first
        // cell key intersecting the cover, greater than or equal to prefixed_left_bound.
        printf("Advancing forward to '%s'\n", pos.c_str());

        if (!helper->skip_forward_to_seek_key(&pos)) {
            printf("did not skip forward\n");
            return continue_bool_t::CONTINUE;
        }
        std::string prefixed_pos = rocks_kv_prefix + pos;
        printf("Hit seek key '%s'\n", prefixed_pos.c_str());
        iter->Seek(prefixed_pos);  // TODO: blocker pool
        if (!iter->Valid()) {
            printf("iter not valid\n");
            return continue_bool_t::CONTINUE;
        }

        rocksdb::Slice key = iter->key();
        printf("On key '%s'\n", key.ToString().c_str());
        key.remove_prefix(rocks_kv_prefix.size());
        store_key_t skey(key.size(), reinterpret_cast<const uint8_t *>(key.data()));
        S2CellId cellid = btree_key_to_s2cellid(skey.btree_key());

        bool found_cell = false;
        S2CellId max_cell;
        // And now we want to see: Are we intersecting?  Or do we need to seek further?
        for (S2CellId cell : helper->query_cells()) {
            if (cell.contains(cellid)) {
                // We're inside the cell.  Iterate through it entirely.
                max_cell = cell.range_max();
                found_cell = true;
                break;
            } else if (cellid.contains(cell)) {
                // Iterate through all keys with the entire ancestor's _value_.
                max_cell = cellid;
                found_cell = true;
                break;
            } else {
                // We're outside the cell.  Go to the next one.
                continue;
            }
        }

        if (!found_cell) {
            printf("no cell found for key '%s'\n", key.ToString().c_str());
            pos = key.ToString();
            continue;
        }

        std::string stop_line
            = rocks_kv_prefix + rockstore::prefix_end(s2cellid_to_key(max_cell));
        printf("Iterating to stop line '%s'\n", stop_line.c_str());

        rocksdb::Slice key_slice = iter->key();
        rocksdb::Slice value_slice = iter->value();
        bool was_valid = true;
        for (;;) {
            rocksdb::Slice prefixless_key = key_slice;
            prefixless_key.remove_prefix(rocks_kv_prefix.size());
            printf("handling pair '%s'\n", prefixless_key.ToString().c_str());
            continue_bool_t contbool = helper->handle_pair(
                std::make_pair(prefixless_key.data(), prefixless_key.size()),
                std::make_pair(value_slice.data(), value_slice.size()));
            if (contbool == continue_bool_t::ABORT) {
                printf("told to abort\n");
                return continue_bool_t::ABORT;
            }

            iter->Next();
            was_valid = iter->Valid();
            if (!was_valid) {
                break;
            }
            key_slice = iter->key();
            if (key_slice.ToString() >= stop_line) {  // TODO: Perf.
                break;
            }

            value_slice = iter->value();
        }

        // At this point, maybe we've iterated through an entire cell's range or
        // value, maybe not.  The iterator is now pointing at the key _past_
        // that cell (or is not valid).  We continue through the loop if it's
        // valid.
        if (!iter->Valid()) {
            printf("iter not valid after cell, exiting\n");
            return continue_bool_t::CONTINUE;
        }
        key = iter->key();
        key.remove_prefix(rocks_kv_prefix.size());
        pos = key.ToString();
    }

}
