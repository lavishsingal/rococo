#pragma once

#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <ctime>

#include "utils.h"
#include "schema.h"
#include "locking.h"

#include "rrr.hpp"

namespace mdb {

// forward declartion
class Schema;
class Table;

// RO-6: do GC for every GC_THRESHOLD old values
const int GC_THRESHOLD = 100;
// RO-6: GC time for striping out old versions = 5000 ms
const int VERSION_SAFE_TIME = 5000;

class Row: public RefCounted {
    // fixed size part
    char* fixed_part_;

    enum {
        DENSE,
        SPARSE,
    };

    int kind_;

    union {
        // for DENSE rows
        struct {
            // var size part
            char* dense_var_part_;

            // index table for var size part (marks the stop of a var segment)
            int* dense_var_idx_;
        };

        // for SPARSE rows
        std::string* sparse_var_;
    };

    Table* tbl_;

protected:

    void update_fixed(const Schema::column_info* col, void* ptr, int len);

    bool rdonly_;
    const Schema* schema_;

    // hidden ctor, factory model
    Row(): fixed_part_(nullptr), kind_(DENSE),
           dense_var_part_(nullptr), dense_var_idx_(nullptr),
           tbl_(nullptr), rdonly_(false), schema_(nullptr) {}

    // RefCounted should have protected dtor
    virtual ~Row();

    void copy_into(Row* row) const;

    // generic row creation
    static Row* create(Row* raw_row, const Schema* schema, const std::vector<const Value*>& values);

    // helper function for row creation
    static void fill_values_ptr(const Schema* schema, std::vector<const Value*>& values_ptr,
                                const Value& value, size_t fill_counter) {
        values_ptr[fill_counter] = &value;
    }

    // helper function for row creation
    static void fill_values_ptr(const Schema* schema, std::vector<const Value*>& values_ptr,
                                const std::pair<const std::string, Value>& pair, size_t fill_counter) {
        int col_id = schema->get_column_id(pair.first);
        verify(col_id >= 0);
        values_ptr[col_id] = &pair.second;
    }

public:

    virtual symbol_t rtti() const {
        return symbol_t::ROW_BASIC;
    }

    const Schema* schema() const {
        return schema_;
    }
    bool readonly() const {
        return rdonly_;
    }
    void make_readonly() {
        rdonly_ = true;
    }
    void make_sparse();
    void set_table(Table* tbl) {
        if (tbl != nullptr) {
            verify(tbl_ == nullptr);
        }
        tbl_ = tbl;
    }
    const Table* get_table() const {
        return tbl_;
    }

    Value get_column(int column_id) const;
    Value get_column(const std::string& col_name) const {
        return get_column(schema_->get_column_id(col_name));
    }
    virtual MultiBlob get_key() const;

    blob get_blob(int column_id) const;
    blob get_blob(const std::string& col_name) const {
        return get_blob(schema_->get_column_id(col_name));
    }

    virtual void update(int column_id, i32 v) {
        const Schema::column_info* info = schema_->get_column_info(column_id);
        verify(info->type == Value::I32);
        update_fixed(info, &v, sizeof(v));
    }

    virtual void update(int column_id, i64 v) {
        const Schema::column_info* info = schema_->get_column_info(column_id);
        verify(info->type == Value::I64);
        update_fixed(info, &v, sizeof(v));
    }
    virtual void update(int column_id, double v) {
        const Schema::column_info* info = schema_->get_column_info(column_id);
        verify(info->type == Value::DOUBLE);
        update_fixed(info, &v, sizeof(v));
    }
    virtual void update(int column_id, const std::string& str);
    virtual void update(int column_id, const Value& v);

    virtual void update(const std::string& col_name, i32 v) {
        this->update(schema_->get_column_id(col_name), v);
    }
    virtual void update(const std::string& col_name, i64 v) {
        this->update(schema_->get_column_id(col_name), v);
    }
    virtual void update(const std::string& col_name, double v) {
        this->update(schema_->get_column_id(col_name), v);
    }
    virtual void update(const std::string& col_name, const std::string& v) {
        this->update(schema_->get_column_id(col_name), v);
    }
    virtual void update(const std::string& col_name, const Value& v) {
        this->update(schema_->get_column_id(col_name), v);
    }

    // compare based on keys
    // must have same schema!
    int compare(const Row& another) const;

    bool operator ==(const Row& o) const {
        return compare(o) == 0;
    }
    bool operator !=(const Row& o) const {
        return compare(o) != 0;
    }
    bool operator <(const Row& o) const {
        return compare(o) == -1;
    }
    bool operator >(const Row& o) const {
        return compare(o) == 1;
    }
    bool operator <=(const Row& o) const {
        return compare(o) != 1;
    }
    bool operator >=(const Row& o) const {
        return compare(o) != -1;
    }

    virtual Row* copy() const {
        Row* row = new Row();
        copy_into(row);
        return row;
    }

    template <class Container>
    static Row* create(const Schema* schema, const Container& values) {
        verify(values.size() == schema->columns_count());
        std::vector<const Value*> values_ptr(values.size(), nullptr);
        size_t fill_counter = 0;
        for (auto it = values.begin(); it != values.end(); ++it) {
            fill_values_ptr(schema, values_ptr, *it, fill_counter);
            fill_counter++;
        }
        return Row::create(new Row(), schema, values_ptr);
    }

    void to_string(std::string &str) {
        size_t s = str.size();
        int len = s;
        len += (sizeof(schema_->fixed_part_size_)
            + schema_->fixed_part_size_
            + sizeof(kind_));
        if (kind_ == DENSE && schema_->var_size_cols_ > 0) {
            len += schema_->var_size_cols_;
            len += dense_var_idx_[schema_->var_size_cols_ - 1];
            str.resize(len);
            int i = s;
            memcpy((void *)(str.data() + i), (void *)(&schema_->fixed_part_size_), sizeof(schema_->fixed_part_size_));
            i += sizeof(schema_->fixed_part_size_);
            memcpy((void *)(str.data() + i), (void *)(fixed_part_), schema_->fixed_part_size_);
            i += schema_->fixed_part_size_;
            memcpy((void *)(str.data() + i), (void *)(&kind_), sizeof(kind_));
            i += sizeof(kind_);
            memcpy((void *)(str.data() + i), (void *)dense_var_idx_, schema_->var_size_cols_);
            i += schema_->var_size_cols_;
            memcpy((void *)(str.data() + i), (void *)dense_var_part_, dense_var_idx_[schema_->var_size_cols_ - 1]);
            i += dense_var_idx_[schema_->var_size_cols_ - 1];
            verify(i == len);
        } else {
            str.resize(len);
            int i = s;
            memcpy((void *)(str.data() + i), (void *)(&schema_->fixed_part_size_), sizeof(schema_->fixed_part_size_));
            i += sizeof(schema_->fixed_part_size_);
            memcpy((void *)(str.data() + i), (void *)(fixed_part_), schema_->fixed_part_size_);
            i += schema_->fixed_part_size_;
            memcpy((void *)(str.data() + i), (void *)(&kind_), sizeof(kind_));
            i += sizeof(kind_);
            verify(i == len);
        }
    }
};



class CoarseLockedRow: public Row {
    RWLock lock_;

protected:

    // protected dtor as required by RefCounted
    ~CoarseLockedRow() {}

    void copy_into(CoarseLockedRow* row) const {
        this->Row::copy_into((Row *) row);
        row->lock_ = lock_;
    }

public:

    virtual symbol_t rtti() const {
        return symbol_t::ROW_COARSE;
    }

    bool rlock_row_by(lock_owner_t o) {
        return lock_.rlock_by(o);
    }
    bool wlock_row_by(lock_owner_t o) {
        return lock_.wlock_by(o);
    }
    bool unlock_row_by(lock_owner_t o) {
        return lock_.unlock_by(o);
    }

    virtual Row* copy() const {
        CoarseLockedRow* row = new CoarseLockedRow();
        copy_into(row);
        return row;
    }

    template <class Container>
    static CoarseLockedRow* create(const Schema* schema, const Container& values) {
        verify(values.size() == schema->columns_count());
        std::vector<const Value*> values_ptr(values.size(), nullptr);
        size_t fill_counter = 0;
        for (auto it = values.begin(); it != values.end(); ++it) {
            fill_values_ptr(schema, values_ptr, *it, fill_counter);
            fill_counter++;
        }
        return (CoarseLockedRow * ) Row::create(new CoarseLockedRow(), schema, values_ptr);
    }
};

/*
class FineLockedRow: public Row {
    RWLock* lock_;
    void init_lock(int n_columns) {
        lock_ = new RWLock[n_columns];
    }

protected:

    // protected dtor as required by RefCounted
    ~FineLockedRow() {
        delete[] lock_;
    }

    void copy_into(FineLockedRow* row) const {
        this->Row::copy_into((Row *) row);
        int n_columns = schema_->columns_count();
        row->init_lock(n_columns);
        for (int i = 0; i < n_columns; i++) {
            row->lock_[i] = lock_[i];
        }
    }

public:

    virtual symbol_t rtti() const {
        return symbol_t::ROW_FINE;
    }

    bool rlock_column_by(column_id_t column_id, lock_owner_t o) {
        return lock_[column_id].rlock_by(o);
    }
    bool rlock_column_by(const std::string& col_name, lock_owner_t o) {
        column_id_t column_id = schema_->get_column_id(col_name);
        return lock_[column_id].rlock_by(o);
    }
    bool wlock_column_by(column_id_t column_id, lock_owner_t o) {
        return lock_[column_id].wlock_by(o);
    }
    bool wlock_column_by(const std::string& col_name, lock_owner_t o) {
        int column_id = schema_->get_column_id(col_name);
        return lock_[column_id].wlock_by(o);
    }
    bool unlock_column_by(column_id_t column_id, lock_owner_t o) {
        return lock_[column_id].unlock_by(o);
    }
    bool unlock_column_by(const std::string& col_name, lock_owner_t o) {
        column_id_t column_id = schema_->get_column_id(col_name);
        return lock_[column_id].unlock_by(o);
    }

    virtual Row* copy() const {
        FineLockedRow* row = new FineLockedRow();
        copy_into(row);
        return row;
    }

    template <class Container>
    static FineLockedRow* create(const Schema* schema, const Container& values) {
        verify(values.size() == schema->columns_count());
        std::vector<const Value*> values_ptr(values.size(), nullptr);
        size_t fill_counter = 0;
        for (auto it = values.begin(); it != values.end(); ++it) {
            fill_values_ptr(schema, values_ptr, *it, fill_counter);
            fill_counter++;
        }
        FineLockedRow* raw_row = new FineLockedRow();
        raw_row->init_lock(schema->columns_count());
        return (FineLockedRow * ) Row::create(raw_row, schema, values_ptr);
    }
};
*/
class FineLockedRow: public Row {
    typedef enum {
        WAIT_DIE,
        WOUND_DIE,
        TIMEOUT
    } type_2pl_t;
    static type_2pl_t type_2pl_;
    rrr::ALock *lock_;
    //rrr::ALock *lock_;
    void init_lock(int n_columns) {
        //lock_ = new rrr::ALock *[n_columns];
        switch (type_2pl_) {
            case WAIT_DIE:
            {
                lock_ = new rrr::WaitDieALock[n_columns];
                //rrr::WaitDieALock *locks = new rrr::WaitDieALock[n_columns];
                //for (int i = 0; i < n_columns; i++)
                //    lock_[i] = (locks + i);
                break;
            }
            case WOUND_DIE:
            {
                lock_ = new rrr::WoundDieALock[n_columns];
                //rrr::WoundDieALock *locks = new rrr::WoundDieALock[n_columns];
                //for (int i = 0; i < n_columns; i++)
                //    lock_[i] = (locks + i);
                break;
            }
            case TIMEOUT:
            {
                lock_ = new rrr::TimeoutALock[n_columns];
                //rrr::TimeoutALock *locks = new rrr::TimeoutALock[n_columns];
                //for (int i = 0; i < n_columns; i++)
                //    lock_[i] = (locks + i);
                break;
            }
            default:
                verify(0);
        }
    }

protected:

    // protected dtor as required by RefCounted
    ~FineLockedRow() {
        switch (type_2pl_) {
            case WAIT_DIE:
                delete[] ((rrr::WaitDieALock *)lock_);
                //delete[] ((rrr::WaitDieALock *)lock_[0]);
                break;
            case WOUND_DIE:
                delete[] ((rrr::WoundDieALock *)lock_);
                //delete[] ((rrr::WoundDieALock *)lock_[0]);
                break;
            case TIMEOUT:
                delete[] ((rrr::TimeoutALock *)lock_);
                //delete[] ((rrr::TimeoutALock *)lock_[0]);
                break;
            default:
                verify(0);
        }
        //delete [] lock_;
    }

    //FIXME
    void copy_into(FineLockedRow* row) const {
        verify(0);
        this->Row::copy_into((Row *) row);
        int n_columns = schema_->columns_count();
        row->init_lock(n_columns);
        for (int i = 0; i < n_columns; i++) {
            //row->lock_[i] = lock_[i];
        }
    }

public:
    static void set_wait_die() {
        type_2pl_ = WAIT_DIE;
    }

    static void set_wound_die() {
        type_2pl_ = WOUND_DIE;
    }

    virtual symbol_t rtti() const {
        return symbol_t::ROW_FINE;
    }

    rrr::ALock *get_alock(column_id_t column_id) {
        //return lock_[column_id];
        switch (type_2pl_) {
            case WAIT_DIE:
                return ((rrr::WaitDieALock *)lock_) + column_id;
            case WOUND_DIE:
                return ((rrr::WoundDieALock *)lock_) + column_id;
            case TIMEOUT:
                return ((rrr::TimeoutALock *)lock_) + column_id;
            default:
                verify(0);
        }
    }

    uint64_t reg_wlock(column_id_t column_id,
            std::function<void(uint64_t)> succ_callback,
            std::function<void(void)> fail_callback);

    uint64_t reg_rlock(column_id_t column_id,
            std::function<void(uint64_t)> succ_callback,
            std::function<void(void)> fail_callback);

    void abort_lock_req(column_id_t column_id, uint64_t lock_req_id);

    void unlock_column_by(column_id_t column_id, uint64_t lock_req_id);

    virtual Row* copy() const {
        FineLockedRow* row = new FineLockedRow();
        copy_into(row);
        return row;
    }

    template <class Container>
    static FineLockedRow* create(const Schema* schema, const Container& values) {
        verify(values.size() == schema->columns_count());
        std::vector<const Value*> values_ptr(values.size(), nullptr);
        size_t fill_counter = 0;
        for (auto it = values.begin(); it != values.end(); ++it) {
            fill_values_ptr(schema, values_ptr, *it, fill_counter);
            fill_counter++;
        }
        FineLockedRow* raw_row = new FineLockedRow();
        raw_row->init_lock(schema->columns_count());
        return (FineLockedRow * ) Row::create(raw_row, schema, values_ptr);
    }
};


// inherit from CoarseLockedRow since we need locking on commit phase, when doing 2 phase commit
class VersionedRow: public CoarseLockedRow {
    version_t* ver_;
    void init_ver(int n_columns) {
        ver_ = new version_t[n_columns];
        memset(ver_, 0, sizeof(version_t) * n_columns);
    }

protected:

    // protected dtor as required by RefCounted
    ~VersionedRow() {
        delete[] ver_;
    }

    void copy_into(VersionedRow* row) const {
        this->CoarseLockedRow::copy_into((CoarseLockedRow *)row);
        int n_columns = schema_->columns_count();
        row->init_ver(n_columns);
        memcpy(row->ver_, this->ver_, n_columns * sizeof(version_t));
    }

public:

    virtual symbol_t rtti() const {
        return symbol_t::ROW_VERSIONED;
    }

    version_t get_column_ver(column_id_t column_id) const {
        return ver_[column_id];
    }

    void incr_column_ver(column_id_t column_id) const {
        ver_[column_id]++;
    }

    virtual Row* copy() const {
        VersionedRow* row = new VersionedRow();
        copy_into(row);
        return row;
    }

    template <class Container>
    static VersionedRow* create(const Schema* schema, const Container& values) {
        verify(values.size() == schema->columns_count());
        std::vector<const Value*> values_ptr(values.size(), nullptr);
        size_t fill_counter = 0;
        for (auto it = values.begin(); it != values.end(); ++it) {
            fill_values_ptr(schema, values_ptr, *it, fill_counter);
            fill_counter++;
        }
        VersionedRow* raw_row = new VersionedRow();
        raw_row->init_ver(schema->columns_count());
        return (VersionedRow * ) Row::create(raw_row, schema, values_ptr);
    }
};

/*
 * RO-6: This class defines a row which keeps old versions of each column.
 * Old versions are stored in a map of maps <column_id, map<timestamp, value> >.
 * column_id is used to point to specific columns; then for each column, it stores
 * old values and each old value is associated a timestamp to uniquely identify each version.
 *
 * We use a global counter ver_s as timestamp.
 *
 * Then, O(1) for querying a specific old version; O(k/GC_THRESHOLD) for garbage collection - k is constant as number
 * of versions on average are kept within 5 secs.
 */
class MultiVersionedRow: public Row {
public:

    virtual symbol_t rtti() const {
        return symbol_t::ROW_MULTIVER;
    }

    virtual Row* copy() const {
        MultiVersionedRow* row = new MultiVersionedRow();
        copy_into(row);
        return row;
    }

    /*
     * Update the list of old versions for each row update
     * Overrides Row::update(~)
     * For update, simply push to the back of the map;
     *
     * We create similar functions for different augment types
     *
     * Note: We need to pass set of read_txn_ids taken on by each write txn all the way down to here
     */
    void update(int column_id, i64 v) {
        update_internal(column_id, v);
    }

    void update(int column_id, i32 v) {
        update_internal(column_id, v);
    }

    void update(int column_id, double v) {
        update_internal(column_id, v);
    }

    void update(int column_id, const std::string& str) {
        update_internal(column_id, str);
    }

    void update(int column_id, const Value& v) {
        update_internal(column_id, v);
    }

    /*
     * For update by column name
     */
    void update(const std::string& col_name, i64 v) {
        this->update(schema_->get_column_id(col_name), v);
    }

    void update(const std::string& col_name, i32 v) {
        this->update(schema_->get_column_id(col_name), v);
    }

    void update(const std::string& col_name, double v) {
        this->update(schema_->get_column_id(col_name), v);
    }

    void update(const std::string& col_name, const std::string& str) {
        this->update(schema_->get_column_id(col_name), str);
    }

    void update(const std::string& col_name, const Value& v) {
        this->update(schema_->get_column_id(col_name), v);
    }



    /*
     * For reads, we need the id for this read txn
    Value get_column(int column_id, i64 txnId) const;

    Value get_column(const std::string& col_name, i64 txnId) const {
        return get_column(schema_->get_column_id(col_name), txnId);
    }
     */

    // retrieve current version number
    version_t getCurrentVersion(int column_id);

    // get a value specified by a version number
    Value get_column_by_version(int column_id, i64 version_num) const;

    // TODO: do some tests to see how slow it is

private:
    static version_t ver_s;

    // garbage collection
    void garbageCollection(int column_id, std::map<i64, Value>::iterator itr);

    // Internal update logic, a template function to accomodate all types
    template<typename Type>
    void update_internal(int column_id, Type v) {
        // first get current value before update, and put current value in old_values_
        Value currentValue = Row::get_column(column_id);
        // get current version
        version_t currentVersionNumber = getCurrentVersion(column_id);
        // push this new value to the old versions map
        std::pair <i64, Value> valueEntry = std::make_pair(next_version(), currentValue);
        // insert this old version to old_values_
        std::map<i64, Value>::iterator newElementItr = (old_values_[column_id].insert(valueEntry)).first;
        if (old_values_[column_id].size() % GC_THRESHOLD == 0) {
            // do Garbage Collection
            garbageCollection(column_id, newElementItr);
        }

        /*
         * Should move this part to upper level -> RO6DTxn
        // Now we need to update rtxn_tracker first
        for (i64 txnId : txnIds) {
            rtxn_tracker.checkIfTxnIdBeenRecorded(column_id, txnId, true, currentVersionNumber);
        }
         */
        // then update column as normal
        Row::update(column_id, v);
    }

public:
    static version_t next_version() {
        return ++MultiVersionedRow::ver_s;
    }

    template <class Container>
    static MultiVersionedRow* create(const Schema* schema, const Container& values) {
        verify(values.size() == schema->columns_count());
        std::vector<const Value*> values_ptr(values.size(), nullptr);
        size_t fill_counter = 0;
        for (auto it = values.begin(); it != values.end(); ++it) {
            fill_values_ptr(schema, values_ptr, *it, fill_counter);
            fill_counter++;
        }
        MultiVersionedRow* raw_row = new MultiVersionedRow();
        return (MultiVersionedRow * ) Row::create(raw_row, schema, values_ptr);
    }
    /*
     * These functions are for testing purpoes. Uncomment them to use
     *
    int getTotalVerionNums(int column_id) {
        return old_values_[column_id].size();
    }

    i64 getVersionHead(int column_id) {
        return old_values_[column_id].begin()->second.get_i64();
    }

    i64 getVersionTail(int column_id) {
        return (--old_values_[column_id].end())->second.get_i64();
    }
     *
     */
private:
    // data structure to keep all old versions for a row
    std::map<column_id_t, std::map<i64, Value> > old_values_;
    // data structure to keep real time for each 100 versions. used for GC
    std::map<column_id_t, std::map<i64, std::map<i64, Value>::iterator> >time_segment;
    // one ReadTxnIdTracker object for each row, for tracking seen read trasactions
    // should be tracked and updated by *this* row's update and read.
    //ReadTxnIdTracker rtxn_tracker;
};

/*
 * RO-6: A class that keeps tracking of observed read txns.
 * A write (row_update/insert/delete) should update this class of the corresponding row first by
 * doing dep_check on a remote row, get rtxn_ids from there, and update its own row's keyToReadTxnIds map
 * A read will also check this map first to see if it needs to query an old version. The targetted version
 * number will be also included in this map if so.
 *
 * Should be instantiated only by MultiVersionedRow class privately.
 */
/*
class ReadTxnIdTracker {
public:
    std::map<int, i64> keyToLastAccessedTime;

    version_t checkIfTxnIdBeenRecorded(int column_id, i64 txnId, bool forWrites, version_t chosenVersion);

    std::vector<i64> getReadTxnIds(int column_id);

    void clearContext() {
        keyToReadTxnIds.clear();
        keyToLastAccessedTime.clear();
    }

private:
    // a pair of information for each read txn (txn_id) we recorded.
    // first i64 is version number that will be used for later read. Second i64 is the real sys time when this txn_id
    // is recorded (for GC use)
    typedef std::pair<i64, i64> TxnTimes;
    // One entry is for one read txn. i64 is txn_id.
    typedef std::map<i64, TxnTimes> ReadTxnEntry;
    // The main data structure used to keep a list of recorded read txns.
    // "int" is column_id. We
    std::map<int, ReadTxnEntry> keyToReadTxnIds;
};
*/
} // namespace mdb
