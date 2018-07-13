#ifndef DMULTIMAP_HPP_INCLUDED
#define DMULTIMAP_HPP_INCLUDED

#include "bsort.hpp"
#include <iostream>
#include <string>
#include "sdsl/bit_vectors.hpp"

namespace seqwish {

/*
'dmultimap' is a disk-backed multimap where keys and values are stored
in a binary file. The key space is assumed to be numeric, but values
may be of arbitrary size.  To build the multimap we first append
key/value pairs.  To query the multimap we must first index it.  We
first sort by key using bsort.  Then we pad the key space so that we
have one entry per integer in the range [0, max(keys)], sorting again
to put the padding pairs in the right positions.  We record the key
space by marking a bitvector of length equal to max(keys) with 1 at
those positions corresponding to the first record of each key in the
sorted array.  We compress this bitvector and build select supports on
it We are now able to memory map the sorted array and seach into it
using select queries on this bitvector.
*/

template <typename Key, typename Value> class dmultimap {

private:

    std::ofstream writer;
    std::ifstream reader;
    std::string filename;
    std::string index_filename;
    bool sorted = false;
    // bsort parameters
    int char_start = 0;
    int char_stop = 255;
    int stack_size = 12;
    int cut_off = 4;
    size_t record_size = 0;
    // key information
    Key max_key = 0;
    // null key and value
    Key nullkey;
    Value nullvalue;
    // compressed bitvector marked at key starts
    sdsl::sd_vector<> key_cbv;
    //sdsl::sd_vector<>::rank_1_type key_cbv_rank;
    // select support for the key cbv
    sdsl::sd_vector<>::select_1_type key_cbv_select;
    bool indexed = false;
    uint32_t OUTPUT_VERSION = 1; // update as we change our format

    void init(void) {
        record_size = sizeof(Key) + sizeof(Value);
        nullkey = 0;
        for (size_t i; i < sizeof(Value); ++i) {
            ((uint8_t*)&nullvalue)[i] = 0;
        }
    }

public:

    // constructor
    dmultimap(void) { init(); }

    dmultimap(const std::string& f) : filename(f) { init(); }

    ~dmultimap(void) { }

    void set_base_filename(const std::string& f) {
        filename = f;
        index_filename = filename+".idx";
    }

    // load from base file name
    void load(const std::string& f) {
        open_reader();
        set_base_filename(f);
        std::ifstream in(index_filename.c_str());
        std::string magic;
        in.read((char*)magic.c_str(), 9);
        uint32_t version;
        in.read((char*) &version, sizeof(version));
        assert(version == OUTPUT_VERSION);
        size_t n_records, record_size_in_bytes;
        sdsl::read_member(record_size_in_bytes, in);
        assert(record_size_in_bytes == record_size);
        sdsl::read_member(n_records, in);
        assert(n_records == record_count());
        sdsl::read_member(max_key, in);
        assert(max_key == nth_key(n_records));
        key_cbv.load(in);
        key_cbv_select.load(in);
    }

    // save indexes
    size_t save(sdsl::structure_tree_node* s = NULL, std::string name = "") {
        assert(max_key && indexed);
        sdsl::structure_tree_node* child = sdsl::structure_tree::add_child(s, name, sdsl::util::class_name(*this));
        // open the sdsl index
        std::ofstream out(index_filename.c_str());
        size_t written = 0;
        out << "dmultimap"; written += 9;
        uint32_t version_buffer = OUTPUT_VERSION;
        out.write((char*) &version_buffer, sizeof(version_buffer));
        written += sdsl::write_member(record_size, out, child, "record_size");
        written += sdsl::write_member(record_count(), out, child, "record_count");
        written += sdsl::write_member(max_key, out, child, "max_key");
        written += key_cbv.serialize(out, child, "key_cbv");
        written += key_cbv_select.serialize(out, child, "key_cbv_select");
        out.close();
        return written;
    }

    // close/open backing file
    void open_writer(const std::string& f) {
        set_base_filename(f);
        open_writer();
    }

    void open_writer(void) {
        if (writer.is_open()) {
            writer.seekp(0, std::ios_base::end); // seek to the end for appending
            return;
        }
        assert(!filename.empty());
        // open in binary append mode as that's how we write into the file
        writer.open(filename.c_str(), std::ios::binary | std::ios::app);
        if (writer.fail()) {
            throw std::ios_base::failure(std::strerror(errno));
        }
    }

    void open_reader(void) {
        if (reader.is_open()) {
            reader.seekg(0); // reset to start
            return;
        }
        assert(!filename.empty());
        // open in binary mode as we are reading from this interface
        reader.open(filename, std::ios::binary);
        if (reader.fail()) {
            throw std::ios_base::failure(std::strerror(errno));
        }
    }

    void close_writer(void) {
        if (writer.is_open()) {
            writer.close();
        }
    }

    void close_reader(void) {
        if (reader.is_open()) {
            reader.close();
        }
    }

    /// write the pair to end of backing file
    void append(const Key& k, const Value& v) {
        sorted = false; // assume we break the sort
        // write to the end of the file
        writer.write((char*)&k, sizeof(Key));
        writer.write((char*)&v, sizeof(Value));
    }

    /// get the record count
    size_t record_count(void) {
        open_reader();
        auto pos = reader.tellg();
        reader.seekg(0, std::ios_base::end); // seek to the end
        assert(reader.tellg() % record_size == 0); // must be even records
        size_t count = reader.tellg() / record_size;
        reader.seekg(pos);
        return count;
    }

    /// sort the record in the backing file by key
    void sort(void) {
        if (sorted) return;
        close_reader();
        close_writer();
        struct bsort::sort sort;
        if (-1==bsort::open_sort(filename.c_str(), &sort)) {
            assert(false);
        }
        size_t key_size = sizeof(Key);
        bsort::radixify((unsigned char*)sort.buffer,
                        sort.size / record_size,
                        0,
                        char_start,
                        char_stop,
                        record_size,
                        key_size,
                        stack_size,
                        cut_off);
        bsort::close_sort(&sort);
        sorted = true;
    }

    // pad our key space so that we can query it directly with select operations
    void pad(void) {
        assert(sorted);
        open_reader();
        // open the same file for append output
        open_writer();
        // get the number of records
        size_t n_records = record_count();
        // we need to record the max value and record state during the iteration
        Key curr, prev=0;
        Value value;
        bool missing_records = false;
        // go through the records of the file and write records [k_i, 0x0] for each k_i that we don't see up to the max record we've seen
        for (size_t i = 0; i < n_records; i+=record_size) {
            reader.read((char*)&curr, sizeof(Key));
            reader.read((char*)&value, sizeof(Value));
            while (prev+1 < curr) {
                missing_records = true;
                append(prev, nullvalue);
                ++prev;
            }
            append(curr, value);
            prev = curr;
        }
        // we have to sort again if we found any empty records
        if (missing_records) {
            sort();
        }
    }

    // index
    void index(void) {
        sort();
        pad();
        open_reader();
        size_t n_records = record_count();
        sdsl::bit_vector key_bv(n_records);
        // record the key starts
        Key last, curr;
        reader.read((char*)&last, sizeof(Key));
        key_bv[0] = 1;
        for (size_t i = 1; i < n_records; i+=record_size) {
            reader.read((char*)&curr, sizeof(Key));
            if (curr != last) {
                key_bv[i] = 1;
                last = curr;
            }
        }
        // the last key in the sort is our max key
        max_key = curr;
        // build the compressed bitvector
        sdsl::util::assign(key_cbv, sdsl::sd_vector<>(key_bv));
        key_bv.resize(0); // memory could be tight
        //sdsl::util::assign(key_cbv_rank, sdsl::sd_vector<>::rank_1_type(&key_cbv));
        // build the select supports on the key bitvector
        sdsl::util::assign(key_cbv_select, sdsl::sd_vector<>::select_1_type(&key_cbv));
        indexed = true;
    }

    Key nth_key(size_t n) {
        Key key;
        reader.seekg(n*record_size);
        reader.read((char*)&key, sizeof(Key));
        return key;
    }

    Value nth_value(size_t n) {
        Value value;
        reader.seekg(n*record_size+sizeof(Key));
        reader.read((char*)&value, sizeof(Value));
        return value;
    }

    std::vector<Value> values(const Key& key) {
        if (!reader.is_open()) open_reader();
        std::vector<Value> values;
        size_t i = key_cbv_select(key);
        size_t j = (key < max_key ? key_cbv_select(key+1) : record_count());
        for ( ; i < j; ++i) {
            values.push_back(nth_value(i));
        }
        return values;
    }
};

}

#endif