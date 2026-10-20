#include "bufpool/BufferManager.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "schema/TableCsv.h"
#include "schema/TableGenerator.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "schema/TupleSerializer.h"
#include "tuple/Column.h"
#include "tuple/Tuple.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string temp_path() {
    return std::string("/tmp/ex14-tuple-") + std::to_string(getpid()) + ".bin";
}

}

int run_storage_checks() {
    std::string error;

    // --- 1. Column construction: type and length mirror the value ---
    {
        const tuple::Column ci(42);
        check(ci.column_type() == tuple::ColumnType::Integer, "int column type");
        check(ci.column_length() == 4, "int column length is 4");
        check(std::get<int>(ci.column_value()) == 42, "int column value");

        const tuple::Column cd(3.14);
        check(cd.column_type() == tuple::ColumnType::Double, "double column type");
        check(cd.column_length() == 8, "double column length is 8");
        check(std::get<double>(cd.column_value()) == 3.14, "double column value");

        const tuple::Column cs(std::string("Ruth"));
        check(cs.column_type() == tuple::ColumnType::String, "string column type");
        check(cs.column_length() == 4, "string column length is its byte size");
        check(std::get<std::string>(cs.column_value()) == "Ruth", "string column value");

        const tuple::Column ce(std::string(""));
        check(ce.column_type() == tuple::ColumnType::String && ce.column_length() == 0,
              "empty string column has length 0");
    }

    // --- 2. Default column ---
    {
        const tuple::Column c;
        check(c.column_type() == tuple::ColumnType::Integer, "default column type");
        check(c.column_length() == 4, "default column length is 4");
        check(std::get<int>(c.column_value()) == 0, "default column value is 0");
    }

    // --- 3. Tuple API ---
    {
        tuple::Tuple t;
        check(t.column_count() == 0, "fresh tuple is empty");
        t.add(1);
        t.add(2.5);
        t.add(std::string("ab"));
        check(t.column_count() == 3, "add overloads build columns");
        check(t.column(0).column_type() == tuple::ColumnType::Integer &&
                  t.column(1).column_type() == tuple::ColumnType::Double &&
                  t.column(2).column_type() == tuple::ColumnType::String,
              "add overloads pick the right types");
        t.add_column(tuple::Column(9.5));
        check(t.column_count() == 4 &&
                  t.column(3).column_type() == tuple::ColumnType::Double,
              "add_column accepts a prebuilt Column");
        t.clear();
        check(t.column_count() == 0, "clear empties the tuple");
        bool threw = false;
        try {
            (void)t.column(99);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        check(threw, "column(99) throws std::out_of_range");
    }

    // --- 4. Record serialization: values only, layout from the schema ---
    {
        schema::TableSchema person("");
        check(schema::load_table_schema("tablecatalog/person.json", person, error),
              "load person.json for the record tests");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple t;
        t.add(42);
        t.add(std::string("Ruth"));
        t.add(7);
        t.add(std::string("HU"));
        std::array<char, 64> buf{};
        std::memset(buf.data(), 0x5A, buf.size());
        check(serializer.serialize(t, buf.data(), buf.size(), error),
              "serialize succeeds");
        check(error.empty(), "no error left behind on success");
        check(serializer.record_size() == 21,
              "record_size is 21 (4 + 10 + 4 + 3)");

        // Bytes past the record are untouched: max_len is a bound.
        bool marker_survives = true;
        for (std::size_t i = 21; i < buf.size(); ++i) {
            if (buf[i] != 0x5A) {
                marker_survives = false;
            }
        }
        check(marker_survives, "bytes past the record are untouched");

        // Round-trip over a non-empty target: old columns are gone.
        tuple::Tuple target = person.make_tuple();
        target.clear();
        target.add(999);
        target.add(std::string("stale"));
        target.add(999);
        target.add(std::string("stale"));
        check(serializer.deserialize(buf.data(), buf.size(), target, error),
              "deserialize over a non-empty tuple");
        check(target == t, "old columns are gone after deserialize");

        // The read needs the whole record; anything less refuses with the
        // out tuple empty.
        tuple::Tuple out;
        check(!serializer.deserialize(buf.data(), 20, out, error) &&
                  !error.empty() && out.column_count() == 0,
              "a truncated record fails");

        // Same for the write: the bound is checked before any byte moves.
        std::array<char, 20> small{};
        check(!serializer.serialize(t, small.data(), small.size(), error) &&
                  !error.empty(),
              "serialize refuses a too-small buffer");
    }

    // --- 5. Round-trips through the one serializer ---
    {
        schema::TableSchema person(""), car("");
        check(schema::load_table_schema("tablecatalog/person.json", person, error),
              "load person.json for the round-trips");
        check(schema::load_table_schema("tablecatalog/car.json", car, error),
              "load car.json for the round-trips");
        const auto round_trip = [&](const schema::TableSchema& schema,
                                    const tuple::Tuple& in) {
            const schema::TupleSerializer serializer(schema);
            std::array<char, 128> buf{};
            if (!serializer.serialize(in, buf.data(), buf.size(), error)) {
                return false;
            }
            tuple::Tuple out;
            if (!serializer.deserialize(buf.data(), buf.size(), out, error)) {
                return false;
            }
            return out == in;
        };
        tuple::Tuple short_str;
        short_str.add(1);
        short_str.add(std::string("Ruth"));
        short_str.add(42);
        short_str.add(std::string("HU"));
        check(round_trip(person, short_str), "round-trip: short strings");
        tuple::Tuple empty_str;
        empty_str.add(2);
        empty_str.add(std::string(""));
        empty_str.add(3);
        empty_str.add(std::string(""));
        check(round_trip(person, empty_str), "round-trip: empty strings");
        tuple::Tuple full_str;
        full_str.add(3);
        full_str.add(std::string("1234567890"));
        full_str.add(4);
        full_str.add(std::string("ABC"));
        check(round_trip(person, full_str), "round-trip: full-width strings");
        tuple::Tuple neg;
        neg.add(-5);
        neg.add(std::string("x"));
        neg.add(-2);
        neg.add(std::string("y"));
        check(round_trip(person, neg), "round-trip: negative ints");
        tuple::Tuple car_tuple;
        car_tuple.add(9);
        car_tuple.add(std::string("Volvo"));
        car_tuple.add(std::string("240"));
        car_tuple.add(1976);
        car_tuple.add(3);
        check(round_trip(car, car_tuple), "round-trip: a car record");
    }

    // --- 5b. The schema-bound TupleSerializer class ---
    {
        schema::TableSchema person(""), car(""), code_built("people");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the serializer class tests");
        check(schema::load_table_schema("tablecatalog/car.json", car, err),
              "load car.json for the serializer class tests");
        check(code_built.add_column("a", tuple::ColumnType::Integer, err) &&
                  code_built.add_column("b", tuple::ColumnType::String, err),
              "build a width-less schema for the serializer class tests");

        // The binding happens once at construction; every operation works
        // against the bound schema, with no per-call schema parameter.
        const schema::TupleSerializer serializer(person);
        check(serializer.record_size() == 21,
              "the bound serializer knows the record size (4 + 10 + 4 + 3)");

        tuple::Tuple t;
        t.add(7);
        t.add(std::string("Ruth"));
        t.add(42);
        t.add(std::string("HU"));

        // Byte-for-byte against the hand-built values-only layout: the
        // on-disk compatibility guard — these bytes are the format.
        std::array<char, 64> buf{};
        std::memset(buf.data(), 0x5A, buf.size());
        check(serializer.serialize(t, buf.data(), buf.size(), err),
              "the bound serializer packs the tuple");
        std::array<char, 21> expected{};
        const int pid = 7, age = 42;
        std::memcpy(expected.data() + 0, &pid, 4);
        std::memcpy(expected.data() + 4, "Ruth", 4);
        std::memcpy(expected.data() + 14, &age, 4);
        std::memcpy(expected.data() + 18, "HU", 2);
        check(std::memcmp(buf.data(), expected.data(), 21) == 0,
              "record bytes match the hand-built layout exactly");
        bool marker = true;
        for (std::size_t i = 21; i < buf.size(); ++i) {
            marker = marker && buf[i] == 0x5A;
        }
        check(marker, "bytes past the record are untouched");

        // Short strings are NUL-padded to their declared width.
        tuple::Tuple short_t;
        short_t.add(1);
        short_t.add(std::string("Ru"));
        short_t.add(2);
        short_t.add(std::string("H"));
        check(serializer.serialize(short_t, buf.data(), buf.size(), err),
              "pack the short-string tuple");
        bool padded = buf[4] == 'R' && buf[5] == 'u';
        for (std::size_t i = 6; i < 14; ++i) {
            padded = padded && buf[i] == '\0';
        }
        padded = padded && buf[18] == 'H' && buf[19] == '\0' && buf[20] == '\0';
        check(padded, "short strings are NUL-padded to their declared width");

        // Round trip through the bound serializer: old columns are gone.
        tuple::Tuple target = person.make_tuple();
        target.clear();
        target.add(999);
        target.add(std::string("stale"));
        target.add(999);
        target.add(std::string("stale"));
        check(serializer.deserialize(buf.data(), buf.size(), target, err),
              "unpack over a non-empty tuple");
        check(target == short_t, "the unpacked tuple replaces the old columns");

        // All-or-nothing pack: a tuple the schema refuses never touches the
        // buffer, and the bytes behind a refusal stay exactly as they were.
        std::memset(buf.data(), 0x5A, buf.size());
        tuple::Tuple arity;
        arity.add(1);
        arity.add(std::string("x"));
        check(!serializer.serialize(arity, buf.data(), buf.size(), err) &&
                  !err.empty(),
              "wrong arity is refused by the bound serializer");
        tuple::Tuple car_tuple;
        car_tuple.add(9);
        car_tuple.add(std::string("Volvo"));
        car_tuple.add(std::string("240"));
        car_tuple.add(1976);
        car_tuple.add(3);
        check(!serializer.serialize(car_tuple, buf.data(), buf.size(), err) &&
                  !err.empty(),
              "a tuple for another schema is refused by the binding");
        bool untouched = true;
        for (const char c : buf) {
            untouched = untouched && c == 0x5A;
        }
        check(untouched, "refused packs leave the buffer untouched");
        std::array<char, 20> small{};
        check(!serializer.serialize(t, small.data(), small.size(), err) &&
                  !err.empty(),
              "an undersized buffer is refused before any write");

        // All-or-nothing unpack: damage is a named error, the out tuple empty.
        check(serializer.serialize(t, buf.data(), buf.size(), err),
              "pack for the unpack refusal tests");
        buf[9] = 'X'; // name-field padding (value ends at 7, NUL at 8)
        tuple::Tuple out;
        check(!serializer.deserialize(buf.data(), buf.size(), out, err) &&
                  !err.empty() && err.find("padding") != std::string::npos &&
                  out.column_count() == 0,
              "garbage in the string padding is refused");
        check(!serializer.deserialize(buf.data(), 20, out, err) && !err.empty() &&
                  out.column_count() == 0,
              "a truncated record is refused");

        // A schema without declared widths has no record layout.
        const schema::TupleSerializer loose(code_built);
        check(loose.record_size() == 0, "undeclared widths: record_size is 0");
        tuple::Tuple cb;
        cb.add(1);
        cb.add(std::string("x"));
        check(!loose.serialize(cb, buf.data(), buf.size(), err) && !err.empty() &&
                  err.find("no declared width") != std::string::npos,
              "the undeclared-width schema is refused, naming the column");
    }

    // --- 6. Through the buffer pool ---
    {
        const std::string path = temp_path();
        bufman::BufferManager mgr;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        check(mgr.open_file(path, file, error), "open scratch file through the manager");

        schema::TableSchema person("");
        check(schema::load_table_schema("tablecatalog/person.json", person, error),
              "load person.json for the pool test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple out;
        out.add(7);
        out.add(std::string("persist"));
        out.add(2);
        out.add(std::string("XY"));

        std::size_t f;
        check(mgr.alloc_page(file, 0, f, error), "alloc_page block 0");
        check(serializer.serialize(out, mgr.frame(f).buffer,
                                   bufman::kPageSize, error),
              "serialize into the frame buffer");
        mgr.unpin(f, true, error);
        check(error.empty(), "unpin dirty");

        std::size_t g;
        check(mgr.alloc_page(file, 1, g, error),
              "alloc_page block 1 evicts the dirty frame");
        check(mgr.frame(g).buffer[0] == 0 && mgr.frame(g).dirty,
              "new page is zeroed and dirty");
        check(mgr.frame(g).pin_count == 1, "new page comes back pinned");
        mgr.unpin(g, false, error);
        check(error.empty(), "unpin clean");

        std::size_t h;
        check(mgr.pin(file, 0, h, error), "re-pin block 0 (miss: back from disk)");
        tuple::Tuple in;
        check(serializer.deserialize(mgr.frame(h).buffer,
                                     bufman::kPageSize, in, error),
              "deserialize from the frame buffer");
        check(in == out, "tuple survives the pool/disk path unchanged");

        mgr.close_all(error);
        std::remove(path.c_str());
    }

    // --- Schema test 1: ColumnSchema ---
    {
        const schema::ColumnSchema c("age", tuple::ColumnType::Integer);
        check(c.column_name() == "age" &&
                  c.column_type() == tuple::ColumnType::Integer &&
                  c.column_size() == 0,
              "ColumnSchema holds name, type, and declared size");
        const schema::ColumnSchema same("age", tuple::ColumnType::Integer);
        const schema::ColumnSchema other("age", tuple::ColumnType::String, 4);
        check(c == same && !(c == other), "ColumnSchema equality");
    }

    // --- Schema test 2: building a schema ---
    {
        schema::TableSchema people("people");
        check(people.table_name() == "people", "table name stored");
        std::string err;
        check(people.add_column("name", tuple::ColumnType::String, err), "add name");
        check(people.add_column("age", tuple::ColumnType::Integer, err), "add age");
        check(people.add_column("height", tuple::ColumnType::Double, err), "add height");
        check(people.column_count() == 3, "column_count grows");
        check(people.column_info(0).column_name() == "name" &&
                  people.column_info(0).column_type() == tuple::ColumnType::String,
              "column_info(0) is name");
        check(people.column_info(1).column_name() == "age" &&
                  people.column_info(1).column_type() == tuple::ColumnType::Integer,
              "column_info(1) is age");
        check(people.column_info(2).column_name() == "height" &&
                  people.column_info(2).column_type() == tuple::ColumnType::Double,
              "column_info(2) is height");
        check(!people.add_column("age", tuple::ColumnType::Integer, err) && !err.empty(),
              "duplicate column name fails");
        check(!people.add_column("", tuple::ColumnType::Integer, err) && !err.empty(),
              "empty column name fails");
        bool threw = false;
        try {
            (void)people.column_info(99);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        check(threw, "column_info(99) throws std::out_of_range");
    }

    // --- Schema test 3: column_name_map ---
    {
        schema::TableSchema people("people");
        std::string err;
        people.add_column("name", tuple::ColumnType::String, err);
        people.add_column("age", tuple::ColumnType::Integer, err);
        const auto idx = people.column_index("age");
        check(idx.has_value() && *idx == 1, "column_index finds age at 1");
        check(!people.column_index("nope").has_value(),
              "unknown name returns nullopt");
    }

    // --- Schema test 4: validate ---
    {
        schema::TableSchema people("people");
        std::string err;
        people.add_column("name", tuple::ColumnType::String, err);
        people.add_column("age", tuple::ColumnType::Integer, err);
        people.add_column("height", tuple::ColumnType::Double, err);
        check(people.validate(people.make_tuple(), err),
              "a make_tuple product validates");
        tuple::Tuple one;
        one.add(1);
        check(!people.validate(one, err) && !err.empty(),
              "wrong column count fails");
        tuple::Tuple wrong_type;
        wrong_type.add(std::string("Ruth"));
        wrong_type.add(2.5); // slot 1 must be Integer
        wrong_type.add(0.0);
        check(!people.validate(wrong_type, err) && !err.empty(),
              "wrong type at a position fails");
        check(err.find("1") != std::string::npos, "error names the position");
    }

    // --- Schema test 5: make_tuple ---
    {
        schema::TableSchema people("people");
        std::string err;
        people.add_column("name", tuple::ColumnType::String, err);
        people.add_column("age", tuple::ColumnType::Integer, err);
        people.add_column("height", tuple::ColumnType::Double, err);
        const tuple::Tuple t = people.make_tuple();
        check(t.column_count() == 3, "make_tuple column count");
        check(t.column(0).column_type() == tuple::ColumnType::String &&
                  std::get<std::string>(t.column(0).column_value()).empty(),
              "slot 0 defaults to an empty string");
        check(t.column(1).column_type() == tuple::ColumnType::Integer &&
                  std::get<int>(t.column(1).column_value()) == 0,
              "slot 1 defaults to 0");
        check(t.column(2).column_type() == tuple::ColumnType::Double &&
                  std::get<double>(t.column(2).column_value()) == 0.0,
              "slot 2 defaults to 0.0");
    }

    // --- Schema test 6: creation order, byte-for-byte ---
    {
        schema::TableSchema people("people");
        std::string err;
        people.add_column("name", tuple::ColumnType::String, err);
        people.add_column("age", tuple::ColumnType::Integer, err);
        people.add_column("height", tuple::ColumnType::Double, err);
        // Code-built schemas have no declared widths — load one that does.
        schema::TableSchema person("");
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the byte test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple logical;
        logical.add(7);
        logical.add(std::string("Ruth"));
        logical.add(42);
        logical.add(std::string("HU"));
        check(person.validate(logical, err), "hand-built logical tuple validates");
        std::array<char, 64> got{};
        check(serializer.serialize(logical, got.data(), got.size(), err),
              "serialize in creation order");
        // Hand-built values-only layout: pid@0, name@4 padded to 10, age@14,
        // city@18 padded to 3.
        std::array<char, 21> expected{};
        const int pid = 7, age = 42;
        std::memcpy(expected.data() + 0, &pid, 4);
        std::memcpy(expected.data() + 4, "Ruth", 4);
        std::memcpy(expected.data() + 14, &age, 4);
        std::memcpy(expected.data() + 18, "HU", 2);
        check(std::memcmp(got.data(), expected.data(), 21) == 0,
              "creation order: bytes match the value layout exactly");
    }

    // --- Schema test 7: value-layout corruption is caught on read ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the corruption test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple logical;
        logical.add(7);
        logical.add(std::string("Ruth"));
        logical.add(42);
        logical.add(std::string("HU"));
        std::array<char, 64> buf{};
        check(serializer.serialize(logical, buf.data(), buf.size(), err),
              "serialize for the padding corruption");

        // Byte 9 is name-field padding (value ends at 7, NUL at 8). With no
        // tags to validate, the padding check is what catches damage.
        buf[9] = 'X';
        tuple::Tuple out;
        check(!serializer.deserialize(buf.data(), buf.size(), out, err) &&
                  !err.empty() && out.column_count() == 0,
              "garbage in the string padding is refused");
    }

    // --- Schema test 8: failure discipline on serialize ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the refusal test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple wrong;
        wrong.add(1);
        std::array<char, 64> buf{};
        check(!serializer.serialize(wrong, buf.data(), buf.size(), err) &&
                  !err.empty(),
              "the serializer refuses a mismatched tuple");
    }

    // --- Schema test 9: by-name workflow ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the by-name workflow");
        const schema::TupleSerializer serializer(person);

        const std::size_t age_slot = *person.column_index("age");
        check(age_slot == 2, "by-name mapping lands on tuple slot 2");

        // Tuple columns are immutable values: filling by name means
        // rebuilding with the named slot replaced.
        tuple::Tuple filled;
        const tuple::Tuple fresh = person.make_tuple();
        for (std::size_t p = 0; p < fresh.column_count(); ++p) {
            filled.add_column(p == age_slot ? tuple::Column(42) : fresh.column(p));
        }
        check(person.validate(filled, err), "filled tuple validates");

        std::array<char, 64> buf{};
        check(serializer.serialize(filled, buf.data(), buf.size(), err),
              "serialize the by-name-filled tuple");
        tuple::Tuple back;
        check(serializer.deserialize(buf.data(), buf.size(), back, err),
              "deserialize it back");
        check(std::get<int>(back.column(age_slot).column_value()) == 42,
              "read 'age' back by name");
    }

    // --- Catalog test 1: loading person.json ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load tablecatalog/person.json");
        check(err.empty(), "no error left behind on a good catalog");
        check(person.table_name() == "person", "table name is 'person'");
        check(person.column_count() == 4, "four columns");
        const std::vector<std::string> names = {"pid", "name", "age", "city"};
        const std::vector<tuple::ColumnType> types = {
            tuple::ColumnType::Integer, tuple::ColumnType::String,
            tuple::ColumnType::Integer, tuple::ColumnType::String};
        const std::vector<std::size_t> sizes = {4, 10, 4, 3};
        bool names_ok = true, types_ok = true, sizes_ok = true;
        for (std::size_t i = 0; i < person.column_count(); ++i) {
            const schema::ColumnSchema& c = person.column_info(i);
            names_ok = names_ok && c.column_name() == names[i];
            types_ok = types_ok && c.column_type() == types[i];
            sizes_ok = sizes_ok && c.column_size() == sizes[i];
        }
        check(names_ok, "column names in file order");
        check(types_ok, "column types match the catalog");
        check(sizes_ok, "declared sizes match the catalog");
        check(person.column_index("age").has_value() &&
                  !person.column_index("nope").has_value(),
              "column_name_map works on a loaded schema");
        check(person.validate(person.make_tuple(), err),
              "a make_tuple product of the loaded schema validates");
    }

    // --- Catalog test 2: the catalog matches the hardwired Person layout ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the layout check");
        std::size_t total = 0;
        for (std::size_t i = 0; i < person.column_count(); ++i) {
            total += person.column_info(i).column_size();
        }
        check(total == 21,  // the legacy record width: 4 + 10 + 4 + 3
              "declared sizes sum to the legacy record width");
    }

    // --- Catalog test 3: loading car.json ---
    {
        schema::TableSchema car("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/car.json", car, err),
              "load tablecatalog/car.json");
        check(car.table_name() == "car" && car.column_count() == 5,
              "five columns");
        const std::vector<std::size_t> sizes = {4, 15, 10, 4, 4};
        bool sizes_ok = true;
        for (std::size_t i = 0; i < car.column_count(); ++i) {
            sizes_ok = sizes_ok && car.column_info(i).column_size() == sizes[i];
        }
        check(sizes_ok, "declared sizes 4/15/10/4/4");
        check(car.validate(car.make_tuple(), err), "a car tuple validates");
    }

    // --- Catalog test 4: the loaded schema drives the wire ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the wire test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple t;
        t.add(7);
        t.add(std::string("Ruth"));
        t.add(42);
        t.add(std::string("HU"));
        check(person.validate(t, err), "a hand-built person tuple validates");
        std::array<char, 64> buf{};
        check(serializer.serialize(t, buf.data(), buf.size(), err),
              "serialize a person tuple over the loaded schema");
        tuple::Tuple back;
        check(serializer.deserialize(buf.data(), buf.size(), back, err),
              "deserialize it back");
        check(back == t, "round-trip over the loaded schema");
        const std::size_t age_slot = *person.column_index("age");
        check(std::get<int>(back.column(age_slot).column_value()) == 42,
              "read 'age' back by name");
    }

    // --- Catalog test 5: overwrite semantics ---
    {
        const std::string person_text =
            "{\n"
            "  \"table_name\": \"person\",\n"
            "  \"columns\": [\n"
            "    { \"column_name\": \"pid\",  \"column_type\": \"Integer\", \"size\": 4 },\n"
            "    { \"column_name\": \"name\", \"column_type\": \"String\",  \"size\": 10 },\n"
            "    { \"column_name\": \"age\",  \"column_type\": \"Integer\", \"size\": 4 },\n"
            "    { \"column_name\": \"city\", \"column_type\": \"String\",  \"size\": 3 }\n"
            "  ]\n"
            "}\n";
        schema::TableSchema target("stale");
        std::string err;
        check(target.add_column("junk", tuple::ColumnType::Double, err),
              "stale column in place");
        check(schema::parse_table_schema(person_text, target, err),
              "parse over a non-empty schema");
        check(target.table_name() == "person" && target.column_count() == 4 &&
                  target.column_info(0).column_name() == "pid",
              "old columns are gone after a successful parse");
        check(!schema::parse_table_schema("{", target, err) && !err.empty() &&
                  target.column_count() == 0 && target.table_name().empty(),
              "a failed parse leaves the out-schema empty");
    }

    // --- Catalog test 6: parse-level errors name the line ---
    {
        schema::TableSchema out("");
        std::string err;
        check(!schema::parse_table_schema("", out, err) && !err.empty() &&
                  err.find("line 1") != std::string::npos,
              "empty text fails, naming line 1");
        check(!schema::parse_table_schema("{\n  \"table_name\": true\n}", out, err) &&
                  !err.empty() && err.find("line 2") != std::string::npos,
              "true as a value fails, naming line 2");
        check(!schema::parse_table_schema("{\"table_name\": \"per", out, err) &&
                  !err.empty(),
              "unterminated string fails");
        check(!schema::parse_table_schema("{\"table_name\": \"a\\qb\"}", out, err) &&
                  !err.empty(),
              "unknown escape fails");
        check(!schema::parse_table_schema(
                  "{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                  "\"column_type\": \"Integer\", \"size\": 4.5}]}",
                  out, err) &&
                  !err.empty(),
              "floating-point size fails");
        check(!schema::parse_table_schema("{} ", out, err) && !err.empty(),
              "missing keys fail");
        check(!schema::parse_table_schema(
                  "{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                  "\"column_type\": \"Integer\", \"size\": 4}]} x",
                  out, err) &&
                  !err.empty() && err.find("after") != std::string::npos,
              "trailing garbage fails");
    }

    // --- Catalog test 7: content errors name the key or column ---
    {
        schema::TableSchema out("");
        std::string err;
        const auto fails = [&](const std::string& text) {
            err.clear();
            return !schema::parse_table_schema(text, out, err) && !err.empty() &&
                   out.column_count() == 0;
        };
        check(fails("{\"columns\": []}"), "missing 'table_name' fails");
        check(fails("{\"table_name\": \"\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\", \"size\": 4}]}"),
              "empty 'table_name' fails");
        check(fails("{\"table_name\": \"t\"}"), "missing 'columns' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": 3}"),
              "'columns' not an array fails");
        check(fails("{\"table_name\": \"t\", \"columns\": []}"),
              "empty columns array fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [3]}"),
              "column entry not an object fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_type\": "
                    "\"Integer\", \"size\": 4}]}"),
              "missing 'column_name' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"size\": 4}]}"),
              "missing 'column_type' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\"}]}"),
              "missing 'size' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Float\", \"size\": 4}]}") &&
                  err.find("Float") != std::string::npos,
              "unknown type spelling fails, naming it");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\", \"size\": 0}]}"),
              "size 0 fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\", \"size\": 4}, "
                    "{\"column_name\": \"a\", \"column_type\": \"String\", "
                    "\"size\": 2}]}") &&
                  err.find("already exists") != std::string::npos,
              "duplicate column name fails with add_column's message");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"\", "
                    "\"column_type\": \"Integer\", \"size\": 4}]}"),
              "empty column name fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"nope\": 1}]}"),
              "unexpected key fails");
    }

    // --- Catalog test 8: failure discipline across bad catalogs ---
    {
        const std::vector<std::pair<std::string, std::string>> bad = {
            {"", "empty text"},
            {"not json", "not json"},
            {"[1, 2]", "array instead of an object"},
            {"{", "truncated"},
            {"{\"table_name\": \"t\"}", "missing columns"},
            {"{\"table_name\": \"t\", \"columns\": [{}]}", "empty column entry"},
        };
        bool all_refused = true;
        schema::TableSchema out("");
        std::string err;
        for (const auto& entry : bad) {
            if (schema::parse_table_schema(entry.first, out, err) || err.empty() ||
                out.column_count() != 0) {
                all_refused = false;
            }
        }
        check(all_refused, "every bad catalog: refused, error set, out empty");
    }

    // --- Catalog test 9: missing file ---
    {
        schema::TableSchema out("");
        std::string err;
        check(!schema::load_table_schema("tablecatalog/no_such_table.json", out,
                                         err) &&
                  !err.empty() &&
                  err.find("no_such_table.json") != std::string::npos,
              "a missing file fails, naming the path");
        check(out.column_count() == 0, "the out-schema stays empty");
    }

    // --- Heap test 1: the fixed-width record layout ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the layout test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple t;
        t.add(7);
        t.add(std::string("Ruth"));
        t.add(42);
        t.add(std::string("HU"));
        std::array<char, 64> buf{};
        check(serializer.serialize(t, buf.data(), buf.size(), err),
              "serialize a person tuple");
        // Hand-built from the layout table: pid@0, name@4 with NUL padding
        // to 10, age@14, city@18 with NUL padding to 3.
        std::array<char, 21> expected{};
        const int pid = 7;
        const int age = 42;
        std::memcpy(expected.data() + 0, &pid, 4);
        std::memcpy(expected.data() + 4, "Ruth", 4);
        std::memcpy(expected.data() + 14, &age, 4);
        std::memcpy(expected.data() + 18, "HU", 2);
        check(std::memcmp(buf.data(), expected.data(), 21) == 0,
              "record bytes match the layout table");
    }

    // --- Heap test 2: codec round-trips ---
    {
        schema::TableSchema person(""), car("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the round-trips");
        check(schema::load_table_schema("tablecatalog/car.json", car, err),
              "load car.json for the round-trips");
        const auto round_trip = [&](const schema::TableSchema& schema,
                                    const tuple::Tuple& in) {
            const schema::TupleSerializer serializer(schema);
            std::array<char, 128> buf{};
            if (!serializer.serialize(in, buf.data(), buf.size(), err)) {
                return false;
            }
            tuple::Tuple out;
            if (!serializer.deserialize(buf.data(), buf.size(), out, err)) {
                return false;
            }
            return out == in;
        };
        tuple::Tuple short_str;
        short_str.add(1);
        short_str.add(std::string("Ruth"));
        short_str.add(42);
        short_str.add(std::string("HU"));
        check(round_trip(person, short_str), "round-trip: short strings");
        tuple::Tuple empty_str;
        empty_str.add(2);
        empty_str.add(std::string(""));
        empty_str.add(3);
        empty_str.add(std::string(""));
        check(round_trip(person, empty_str), "round-trip: empty strings");
        tuple::Tuple full_str;
        full_str.add(3);
        full_str.add(std::string("1234567890"));
        full_str.add(4);
        full_str.add(std::string("ABC"));
        check(round_trip(person, full_str), "round-trip: full-width strings");
        tuple::Tuple car_tuple;
        car_tuple.add(9);
        car_tuple.add(std::string("Volvo"));
        car_tuple.add(std::string("240"));
        car_tuple.add(1976);
        car_tuple.add(3);
        check(round_trip(car, car_tuple), "round-trip: a car record");
    }

    // --- Heap test 3: codec errors ---
    {
        schema::TableSchema person(""), code_built("people");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the codec errors");
        check(code_built.add_column("a", tuple::ColumnType::Integer, err) &&
                  code_built.add_column("b", tuple::ColumnType::String, err),
              "build a code-only schema");
        const schema::TupleSerializer serializer(person);
        const schema::TupleSerializer loose(code_built);
        std::array<char, 64> buf{};
        std::memset(buf.data(), 0x5A, buf.size());

        tuple::Tuple wide;
        wide.add(7);
        wide.add(std::string("12345678901"));
        wide.add(42);
        wide.add(std::string("HU"));
        check(!serializer.serialize(wide, buf.data(), buf.size(), err) &&
                  !err.empty() && err.find("width") != std::string::npos,
              "a string wider than its column is refused, naming the width");

        tuple::Tuple with_nul;
        with_nul.add(7);
        with_nul.add(std::string("Ru\0th", 5));
        with_nul.add(42);
        with_nul.add(std::string("HU"));
        check(!serializer.serialize(with_nul, buf.data(), buf.size(), err) &&
                  !err.empty() && err.find("NUL") != std::string::npos,
              "a string containing NUL is refused");

        tuple::Tuple arity;
        arity.add(1);
        arity.add(std::string("x"));
        check(!serializer.serialize(arity, buf.data(), buf.size(), err) &&
                  !err.empty(),
              "wrong arity is refused");

        tuple::Tuple wrong_type;
        wrong_type.add(7);
        wrong_type.add(std::string("Ruth"));
        wrong_type.add(2.5);
        wrong_type.add(std::string("HU"));
        check(!serializer.serialize(wrong_type, buf.data(), buf.size(), err) &&
                  !err.empty(),
              "wrong type is refused");

        check(loose.record_size() == 0,
              "undeclared widths: record_size is 0");
        tuple::Tuple cb;
        cb.add(1);
        cb.add(std::string("x"));
        check(!loose.serialize(cb, buf.data(), buf.size(), err) &&
                  !err.empty() &&
                  err.find("no declared width") != std::string::npos,
              "a code-built schema is refused for record packing");

        bool marker = true;
        for (const char c : buf) {
            if (c != 0x5A) {
                marker = false;
            }
        }
        check(marker, "failed serializations leave the buffer untouched");
    }

    // --- Heap test 4: padding integrity ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the padding test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple t;
        t.add(1);
        t.add(std::string("Ruth"));
        t.add(2);
        t.add(std::string("HU"));
        std::array<char, 64> buf{};
        check(serializer.serialize(t, buf.data(), buf.size(), err),
              "serialize for the padding corruption");
        buf[9] = 'X'; // name field: value ends at 7, NUL at 8, byte 9 is padding
        tuple::Tuple out;
        check(!serializer.deserialize(buf.data(), buf.size(), out, err) &&
                  !err.empty() &&
                  err.find("padding") != std::string::npos &&
                  out.column_count() == 0,
              "garbage in the string padding is refused");
    }

    // --- Heap test 5: page math ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the page math");
        const schema::TupleSerializer serializer(person);
        check(serializer.record_size() == 21,
              "record_size is 21 (the legacy record width)");
        check(heapfile::slot_capacity(21) == 185, "slot_capacity(21) is 185");
    }

    // --- Heap test 6: the header page ---
    {
        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person(""), car("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the header-page tests");
        check(schema::load_table_schema("tablecatalog/car.json", car, err),
              "load car.json for the header-page tests");

        const std::string path = temp_path() + "-h6";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err),
              "create writes the identification record");
        check(file.open(path, person, err),
              "open with the matching catalog");
        check(!file.open(path, person, err) && !err.empty(),
              "a second open is refused");
        check(file.close(err), "close");
        check(!file.open(path, car, err) && !err.empty() &&
                  err.find("'person'") != std::string::npos,
              "a wrong catalog is refused by name");

        const std::string text_path = temp_path() + "-h6-text";
        {
            // One full block of non-heap bytes: it passes the block-size
            // and slot-count arithmetic, so only the magic check can
            // refuse it.
            std::string junk = "not a database";
            junk.resize(4096, 'x');
            std::ofstream out(text_path, std::ios::binary);
            out << junk;
        }
        heapfile::HeapFile text_file(mgr);
        check(!text_file.open(text_path, person, err) && !err.empty() &&
                  err.find("not a tuple heap file") != std::string::npos,
              "a text file is not a tuple heap file");

        const std::string empty_path = temp_path() + "-h6-empty";
        { std::ofstream out(empty_path, std::ios::binary); }
        heapfile::HeapFile empty_file(mgr);
        check(!empty_file.open(empty_path, person, err) && !err.empty(),
              "an empty file is refused");

        std::remove(path.c_str());
        std::remove(text_path.c_str());
        std::remove(empty_path.c_str());
    }

    // --- Heap test 7: insert/find/scan round-trip ---
    {
        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the round-trip test");
        const std::size_t pid_slot = *person.column_index("pid");
        const std::size_t name_slot = *person.column_index("name");

        const std::string path = temp_path() + "-h7";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err), "create for the round-trip test");
        check(file.open(path, person, err), "open for the round-trip test");
        const auto person_tuple = [](int pid, const char* name, int age,
                                     const char* city) {
            tuple::Tuple t;
            t.add(pid);
            t.add(std::string(name));
            t.add(age);
            t.add(std::string(city));
            return t;
        };

        heapfile::RowId r1, r2, r3;
        check(file.insert(person_tuple(1, "Ruth", 42, "HU"), r1, err),
              "insert Ruth");
        check(r1.page_id == 1 && r1.slot == 0,
              "the first record lands at page 1, slot 0");
        check(file.insert(person_tuple(2, "Dieter", 51, "KO"), r2, err),
              "insert Dieter");
        check(file.insert(person_tuple(3, "Gina", 7, "BA"), r3, err),
              "insert Gina");

        tuple::Tuple back;
        check(file.find(r2, back, err), "find by row id");
        check(std::get<std::string>(back.column(name_slot).column_value()) ==
                  "Dieter",
              "read 'name' back by name");

        std::size_t count = 0;
        int last_pid = 0;
        bool in_order = true;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple t;
        while (it.next(t, err)) {
            ++count;
            const int pid = std::get<int>(t.column(pid_slot).column_value());
            if (pid < last_pid) {
                in_order = false;
            }
            last_pid = pid;
        }
        check(err.empty(), "scan exhausted cleanly");
        check(count == 3 && in_order, "scan yields 3 records in chain order");

        file.close(err);
        std::remove(path.c_str());
    }

    // --- Heap test 8: page split ---
    {
        bufman::BufferManager mgr;
        mgr.init(4, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the page-split test");

        const std::string path = temp_path() + "-h8";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err) && file.open(path, person, err),
              "create and open for the page-split test");
        const auto person_tuple = [](int pid) {
            tuple::Tuple t;
            t.add(pid);
            t.add(std::string("name"));
            t.add(pid);
            t.add(std::string("AA"));
            return t;
        };

        heapfile::RowId first, last;
        for (int i = 1; i <= 200; ++i) {
            heapfile::RowId rid;
            if (!file.insert(person_tuple(i), rid, err)) {
                break;
            }
            if (i == 1) {
                first = rid;
            }
            last = rid;
        }
        check(err.empty(), "200 inserts succeed");
        check(first.page_id == 1 && last.page_id == 2,
              "the chain split onto a second page (185 per page)");

        std::size_t count = 0;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple t;
        while (it.next(t, err)) {
            ++count;
        }
        check(err.empty() && count == 200, "scan sees all 200 records");

        tuple::Tuple back;
        check(file.find(first, back, err), "early row ids survive the split");

        file.close(err);
        std::remove(path.c_str());
    }

    // --- Heap test 9: erase and reuse ---
    {
        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the erase test");

        const std::string path = temp_path() + "-h9";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err) && file.open(path, person, err),
              "create and open for the erase test");
        const auto person_tuple = [](int pid) {
            tuple::Tuple t;
            t.add(pid);
            t.add(std::string("name"));
            t.add(pid);
            t.add(std::string("AA"));
            return t;
        };
        for (int i = 1; i <= 10; ++i) {
            heapfile::RowId rid;
            check(file.insert(person_tuple(i), rid, err), "insert into page 1");
        }
        const heapfile::RowId erased_a{1, 3};
        const heapfile::RowId erased_b{1, 7};
        check(file.erase(erased_a, err) && file.erase(erased_b, err),
              "erase two mid-page records");
        tuple::Tuple back;
        check(!file.find(erased_a, back, err) && !err.empty() &&
                  err.find("free") != std::string::npos,
              "an erased row id reads as free");

        heapfile::RowId reuse_a, reuse_b;
        check(file.insert(person_tuple(11), reuse_a, err) &&
                  reuse_a == erased_a,
              "the first re-insert takes the lowest free slot");
        check(file.insert(person_tuple(12), reuse_b, err) &&
                  reuse_b == erased_b,
              "the second re-insert takes the next free slot");

        std::size_t count = 0;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple t;
        while (it.next(t, err)) {
            ++count;
        }
        check(err.empty() && count == 10, "scan skips erased and counts 10");

        file.close(err);
        std::remove(path.c_str());
    }

    // --- Heap test 10: update ---
    {
        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the update test");
        const std::size_t name_slot = *person.column_index("name");
        const std::size_t city_slot = *person.column_index("city");

        const std::string path = temp_path() + "-h10";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err) && file.open(path, person, err),
              "create and open for the update test");
        const auto person_tuple = [](int pid) {
            tuple::Tuple t;
            t.add(pid);
            t.add(std::string("name"));
            t.add(pid);
            t.add(std::string("AA"));
            return t;
        };
        for (int i = 1; i <= 3; ++i) {
            heapfile::RowId rid;
            check(file.insert(person_tuple(i), rid, err), "insert");
        }
        tuple::Tuple nova;
        nova.add(2);
        nova.add(std::string("Nova"));
        nova.add(30);
        nova.add(std::string("ZZ"));
        check(file.update(heapfile::RowId{1, 1}, nova, err),
              "update in place (same size, always)");
        tuple::Tuple back;
        check(file.find(heapfile::RowId{1, 1}, back, err), "find the update");
        check(std::get<std::string>(back.column(name_slot).column_value()) ==
                      "Nova" &&
                  std::get<std::string>(back.column(city_slot).column_value()) ==
                      "ZZ",
              "the updated fields read back");
        std::size_t count = 0;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple t;
        while (it.next(t, err)) {
            ++count;
        }
        check(err.empty() && count == 3, "occupancy unchanged after update");

        file.close(err);
        std::remove(path.c_str());
    }

    // --- Heap test 11: schema guard on write ---
    {
        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the guard test");

        const std::string path = temp_path() + "-h11";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err) && file.open(path, person, err),
              "create and open for the guard test");
        const auto person_tuple = [](int pid) {
            tuple::Tuple t;
            t.add(pid);
            t.add(std::string("name"));
            t.add(pid);
            t.add(std::string("AA"));
            return t;
        };
        heapfile::RowId rid;
        check(file.insert(person_tuple(1), rid, err) &&
                  file.insert(person_tuple(2), rid, err),
              "two valid inserts");

        tuple::Tuple arity;
        arity.add(1);
        arity.add(std::string("x"));
        check(!file.insert(arity, rid, err) && !err.empty(),
              "wrong arity is refused");

        tuple::Tuple wrong_type;
        wrong_type.add(3);
        wrong_type.add(std::string("name"));
        wrong_type.add(2.5);
        wrong_type.add(std::string("AA"));
        check(!file.insert(wrong_type, rid, err) && !err.empty(),
              "wrong type is refused");

        tuple::Tuple wide;
        wide.add(4);
        wide.add(std::string("12345678901"));
        wide.add(4);
        wide.add(std::string("AA"));
        check(!file.insert(wide, rid, err) && !err.empty(),
              "a too-wide string is refused");

        std::size_t count = 0;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple t;
        while (it.next(t, err)) {
            ++count;
        }
        check(err.empty() && count == 2,
              "the refusals left the file unchanged");

        file.close(err);
        std::remove(path.c_str());
    }

    // --- Heap test 12: persistence ---
    {
        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the persistence test");

        const std::string path = temp_path() + "-h12";
        heapfile::RowId saved;
        {
            heapfile::HeapFile file(mgr);
            check(file.create(path, person, err) &&
                      file.open(path, person, err),
                  "create and open for the persistence test");
            for (int i = 1; i <= 5; ++i) {
                tuple::Tuple t;
                t.add(i);
                t.add(std::string("name"));
                t.add(i);
                t.add(std::string("AA"));
                heapfile::RowId rid;
                check(file.insert(t, rid, err), "insert");
            }
            saved = heapfile::RowId{1, 4};
            check(file.close(err), "close flushes");
        }
        {
            heapfile::HeapFile file(mgr);
            check(file.open(path, person, err), "reopen with the same schema");
            tuple::Tuple back;
            check(file.find(saved, back, err), "the record survives the close");
            std::size_t count = 0;
            heapfile::HeapFileIterator it = file.scan();
            tuple::Tuple t;
            while (it.next(t, err)) {
                ++count;
            }
            check(err.empty() && count == 5, "all 5 records persist");
            check(file.close(err), "close again");
        }
        std::remove(path.c_str());
    }

    // --- Heap test 13: discipline on bad row ids and closed files ---
    {
        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the discipline test");

        const std::string path = temp_path() + "-h13";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err) && file.open(path, person, err),
              "create and open for the discipline test");
        for (int i = 1; i <= 3; ++i) {
            tuple::Tuple t;
            t.add(i);
            t.add(std::string("name"));
            t.add(i);
            t.add(std::string("AA"));
            heapfile::RowId rid;
            check(file.insert(t, rid, err), "insert");
        }
        check(file.erase(heapfile::RowId{1, 2}, err), "erase slot 2");

        tuple::Tuple back;
        check(!file.find(heapfile::RowId{0, 0}, back, err) && !err.empty() &&
                  err.find("header") != std::string::npos,
              "page 0 is the header page");
        check(!file.find(heapfile::RowId{500, 0}, back, err) && !err.empty() &&
                  err.find("beyond") != std::string::npos,
              "a page beyond the file is refused");
        check(!file.find(heapfile::RowId{1, 999}, back, err) && !err.empty(),
              "an out-of-range slot is refused");
        check(!file.find(heapfile::RowId{1, 2}, back, err) && !err.empty() &&
                  err.find("free") != std::string::npos,
              "a free slot is refused");

        check(file.close(err), "close");
        {
            heapfile::RowId rid;
            check(!file.insert(back, rid, err) && !err.empty() &&
                      err.find("not open") != std::string::npos,
                  "operations on a closed file are refused");
        }
        std::remove(path.c_str());
    }

    // --- Heap test 14: two files share one manager; close is per file ---
    {
        bufman::BufferManager mgr;
        mgr.init(4, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person(""), car("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the shared-manager test");
        check(schema::load_table_schema("tablecatalog/car.json", car, err),
              "load car.json for the shared-manager test");
        const std::string people_path = temp_path() + "-h14-people";
        const std::string cars_path = temp_path() + "-h14-cars";

        heapfile::HeapFile people(mgr), cars(mgr);
        check(people.create(people_path, person, err) &&
                  people.open(people_path, person, err),
              "the first file opens through the shared manager");
        check(cars.create(cars_path, car, err) && cars.open(cars_path, car, err),
              "the second file opens through the same manager");
        check(mgr.is_open(people_path) && mgr.is_open(cars_path),
              "the manager holds both files");

        tuple::Tuple p;
        p.add(1);
        p.add(std::string("Ruth"));
        p.add(42);
        p.add(std::string("HU"));
        heapfile::RowId rid;
        check(people.insert(p, rid, err), "insert into the first file");
        tuple::Tuple c;
        c.add(9);
        c.add(std::string("Bolt"));
        c.add(std::string("M8"));
        c.add(2020);
        c.add(7);
        check(cars.insert(c, rid, err), "insert into the second file");

        check(people.close(err), "close the first file");
        check(!mgr.is_open(people_path) && mgr.is_open(cars_path),
              "the manager still holds the second file");
        tuple::Tuple back;
        check(cars.find(heapfile::RowId{1, 0}, back, err),
              "the second file is untouched by the first file's close");
        check(people.open(people_path, person, err),
              "the first file reopens in the same session");
        check(people.find(heapfile::RowId{1, 0}, back, err),
              "its records are intact after the flush-on-close");
        check(people.close(err) && cars.close(err), "both files close");

        std::remove(people_path.c_str());
        std::remove(cars_path.c_str());
    }

    // --- Heap test 15: iterator row ids, residency, and close_file ---
    {
        bufman::BufferManager mgr;
        mgr.init(4, std::make_unique<bufman::LRUPolicy>());
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the accessor tests");
        const std::string path = temp_path() + "-h15";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err) && file.open(path, person, err),
              "create and open for the accessor tests");
        for (int i = 1; i <= 3; ++i) {
            tuple::Tuple t;
            t.add(i);
            t.add(std::string("name"));
            t.add(i);
            t.add(std::string("AA"));
            heapfile::RowId rid;
            check(file.insert(t, rid, err), "insert");
        }
        check(file.resident(1), "page 1 is resident after the inserts");
        check(!file.resident(9), "page 9 was never brought in");

        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple t;
        bool ids_ok = true;
        std::uint32_t slot = 0;
        while (it.next(t, err)) {
            const heapfile::RowId& rid = it.row_id();
            ids_ok = ids_ok && rid.page_id == 1 && rid.slot == slot;
            ++slot;
        }
        check(err.empty() && ids_ok && slot == 3,
              "row_id() tracks the records the scan yields");
        check(file.resident(1), "the scan left its pages in the pool");

        file.close(err);
        std::remove(path.c_str());
    }

    // --- Heap test 16: close_file refuses pinned frames, spares others ---
    {
        bufman::BufferManager mgr;
        mgr.init(4, std::make_unique<bufman::LRUPolicy>());
        std::string err;
        const std::string pa = temp_path() + "-h16a";
        const std::string pb = temp_path() + "-h16b";
        bufman::File a, b;
        check(mgr.open_file(pa, a, err) && mgr.open_file(pb, b, err),
              "two raw files through one manager");
        std::size_t frame = 0;
        check(mgr.alloc_page(a, 0, frame, err), "page for file a");
        mgr.unpin(frame, true, err);
        check(mgr.alloc_page(b, 0, frame, err), "page for file b");
        mgr.unpin(frame, true, err);

        check(mgr.pin(a, 0, frame, err), "pin file a's page");
        check(!mgr.close_file(a, err) && !err.empty() &&
                  err.find("pinned") != std::string::npos,
              "close_file refuses while a frame is pinned");
        mgr.unpin(frame, false, err);
        check(mgr.close_file(a, err), "close_file after unpin");
        check(a.fd == -1, "the caller's handle is invalidated");

        check(!mgr.is_open(pa) && mgr.is_open(pb),
              "the other file survives the close");
        check(mgr.pin(b, 0, frame, err), "file b's page still resolves");
        mgr.unpin(frame, false, err);
        mgr.close_all(err);
        std::remove(pa.c_str());
        std::remove(pb.c_str());
    }

    // --- CLI module test 1: CSV row parsing ---
    {
        schema::TableSchema person(""), code_built("mixed");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the CSV row tests");
        check(code_built.add_column("a", tuple::ColumnType::Integer, err) &&
                  code_built.add_column("b", tuple::ColumnType::Double, err) &&
                  code_built.add_column("c", tuple::ColumnType::String, err),
              "build a schema with a Double column");

        tuple::Tuple t;
        check(schema::parse_csv_row(person, "7,Ruth,42,HU", t, err),
              "a valid row parses");
        tuple::Tuple expected;
        expected.add(7);
        expected.add(std::string("Ruth"));
        expected.add(42);
        expected.add(std::string("HU"));
        check(t == expected, "the parsed tuple matches the row");

        check(schema::parse_csv_row(person, "1,,2,", t, err) &&
                  std::get<std::string>(t.column(1).column_value()) == "" &&
                  std::get<std::string>(t.column(3).column_value()) == "",
              "empty tokens are empty strings");
        check(schema::parse_csv_row(person, "3,1234567890,4,ABC", t, err),
              "a full-width string parses");

        check(!schema::parse_csv_row(person, "7,Ruth", t, err) &&
                  !err.empty() && err.find("2 column(s)") != std::string::npos,
              "too few tokens are refused, naming the arity");
        check(!schema::parse_csv_row(person, "7,Ruth,42,HU,x", t, err) &&
                  !err.empty(),
              "too many tokens are refused");
        check(!schema::parse_csv_row(person, "abc,Ruth,42,HU", t, err) &&
                  !err.empty() &&
                  err.find("column 'pid': expected an integer") !=
                      std::string::npos,
              "a bad integer names its column");
        check(!schema::parse_csv_row(person, "7,Ruth,4.5,HU", t, err) &&
                  !err.empty(),
              "a fractional age is refused");

        check(schema::parse_csv_row(code_built, "1,2.5,x", t, err),
              "a Double column parses");
        check(!schema::parse_csv_row(code_built, "1,2.5.3,x", t, err) &&
                  !err.empty() &&
                  err.find("column 'b': expected a number") !=
                      std::string::npos,
              "a bad double names its column");
        check(!schema::parse_csv_row(code_built, "1,,x", t, err) &&
                  !err.empty(),
              "an empty numeric token is refused");
    }

    // --- CLI module test 2: width rules surface at insert, not parse ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the width test");
        tuple::Tuple t;
        check(schema::parse_csv_row(person, "8,12345678901,9,AA", t, err),
              "a too-wide string parses fine (syntax is legal)");

        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        const std::string path = temp_path() + "-csv-width";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err) && file.open(path, person, err),
              "create and open for the width test");
        heapfile::RowId rid;
        check(!file.insert(t, rid, err) && !err.empty() &&
                  err.find("exceeds the declared width") != std::string::npos,
              "insert refuses it with the schema's named error");
        file.close(err);
        std::remove(path.c_str());
    }

    // --- CLI module test 3: CSV file loading ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the CSV file test");
        const std::string csv_path = temp_path() + "-load.csv";
        {
            std::ofstream out(csv_path, std::ios::binary);
            out << "pid,name,age,city\r\n";      // header, CRLF ending
            out << "1,Alice,30,PR\r\n";          // valid
            out << "\r\n";                       // blank line: silent skip
            out << "2,Bob,old,NY\n";             // bad age
            out << "3,Cara,21,PARIS\n";          // too-wide city
            out << "4,Dan,19,HU\n";              // valid
        }
        std::ostringstream diagnostics;
        schema::CsvLoadResult result;
        check(schema::load_csv(person, csv_path, result, diagnostics, err),
              "load_csv reads the file");
        // The too-wide city row parses fine (width is insert-time), so the
        // load yields 3 valid rows and skips the header and the bad age.
        check(result.tuples.size() == 3 && result.skipped == 2,
              "3 valid rows, 2 skipped");
        check(std::get<std::string>(result.tuples[0].column(1).column_value()) ==
                      "Alice" &&
                  std::get<std::string>(result.tuples[1].column(1).column_value()) ==
                      "Cara" &&
                  std::get<std::string>(result.tuples[2].column(1).column_value()) ==
                      "Dan",
              "the valid rows come back in file order");
        const std::string text = diagnostics.str();
        check(text.find("line 1: column 'pid': expected an integer") !=
                      std::string::npos &&
                  text.find("line 4: column 'age': expected an integer") !=
                      std::string::npos,
              "skipped rows are reported as line N: reason");

        check(!schema::load_csv(person, temp_path() + "-no-such.csv", result,
                                diagnostics, err) &&
                  !err.empty() && result.tuples.empty(),
              "a missing file is a named error with an empty result");
        std::remove(csv_path.c_str());
    }

    // --- CLI module test 4: generation ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the generator tests");
        const std::vector<tuple::Tuple> a = schema::generate_tuples(person, 10, 42);
        const std::vector<tuple::Tuple> b = schema::generate_tuples(person, 10, 42);
        const std::vector<tuple::Tuple> c = schema::generate_tuples(person, 10, 43);
        check(a == b, "same seed produces identical tuples");
        check(!(a == c), "a different seed produces different values");
        check(a.size() == 10, "the count is exact");
        bool all_valid = true;
        for (const tuple::Tuple& t : a) {
            all_valid = all_valid && person.validate(t, err);
        }
        check(all_valid, "every generated tuple passes validate");
    }

    // --- CLI module test 5: generator conventions ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the convention tests");
        const schema::TupleSerializer serializer(person);
        const std::vector<tuple::Tuple> tuples =
            schema::generate_tuples(person, 5, 7, 100);
        bool sequence_ok = true;
        bool strings_ok = true;
        for (std::size_t i = 0; i < tuples.size(); ++i) {
            sequence_ok = sequence_ok &&
                          std::get<int>(tuples[i].column(0).column_value()) ==
                              100 + static_cast<int>(i);
            const std::string& name =
                std::get<std::string>(tuples[i].column(1).column_value());
            strings_ok = strings_ok && !name.empty() && name.size() <= 10 &&
                         name.find('\0') == std::string::npos;
        }
        check(sequence_ok, "the first Integer column counts first_id upward");
        check(strings_ok, "strings stay within their declared width, no NUL");
        std::array<char, 64> buf{};
        check(serializer.serialize(tuples[0], buf.data(), buf.size(), err),
              "serialize a generated tuple");
        tuple::Tuple back;
        check(serializer.deserialize(buf.data(), buf.size(), back, err) &&
                  back == tuples[0],
              "generated rows round-trip through the serializer");
    }

    // --- ex14: catalog indexes — loader extension and the writer ---
    {
        schema::TableSchema person("");
        std::vector<schema::TableIndex> indexes;
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person,
                                        indexes, err),
              "load person.json through the indexes overload");
        check(indexes.empty(),
              "a catalog without 'indexes' loads with an empty index list");
        check(person.table_name() == "person" && person.column_count() == 4,
              "back-compat: person.json loads unchanged");

        schema::TableSchema parsed("");
        std::vector<schema::TableIndex> parsed_indexes;
        check(schema::parse_table_schema(
                  "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                  "\"column_type\":\"Integer\",\"size\":4}],"
                  "\"indexes\":[{\"name\":\"t_a\",\"column\":\"a\"}]}",
                  parsed, parsed_indexes, err) &&
                  parsed_indexes.size() == 1 &&
                  parsed_indexes[0].name == "t_a" &&
                  parsed_indexes[0].column == "a",
              "an indexes array parses into name/column pairs");
        check(schema::parse_table_schema(
                  "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                  "\"column_type\":\"Integer\",\"size\":4}],\"indexes\":[]}",
                  parsed, parsed_indexes, err) &&
                  parsed_indexes.empty(),
              "an empty indexes array parses to no indexes");
        check(schema::parse_table_schema(
                  "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                  "\"column_type\":\"Integer\",\"size\":4}],"
                  "\"indexes\":[{\"name\":\"t_a\",\"column\":\"a\","
                  "\"kind\":\"hash\"}]}",
                  parsed, parsed_indexes, err) &&
                  parsed_indexes.size() == 1 &&
                  parsed_indexes[0].kind == "hash",
              "an index entry's kind parses");
        check(schema::parse_table_schema(
                  "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                  "\"column_type\":\"Integer\",\"size\":4}],"
                  "\"indexes\":[{\"name\":\"t_a\",\"column\":\"a\"}]}",
                  parsed, parsed_indexes, err) &&
                  parsed_indexes.size() == 1 &&
                  parsed_indexes[0].kind == "isam",
              "a kind-less index entry loads as isam");
        {
            schema::TableSchema bad_kind_schema("");
            std::vector<schema::TableIndex> bad_kind_indexes;
            std::string bad_kind_error;
            check(!schema::parse_table_schema(
                      "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                      "\"column_type\":\"Integer\",\"size\":4}],"
                      "\"indexes\":[{\"name\":\"t_a\",\"column\":\"a\","
                      "\"kind\":\"btree\"}]}",
                      bad_kind_schema, bad_kind_indexes, bad_kind_error) &&
                      bad_kind_error.find("btree") != std::string::npos &&
                      bad_kind_indexes.empty(),
                  "an unknown index kind is refused, naming it");
        }

        const char* bad_catalogs[] = {
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"i\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"column\":\"a\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"\",\"column\":\"a\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"i\",\"column\":\"a\"},"
            "{\"name\":\"i\",\"column\":\"a\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"i\",\"column\":\"a\",\"extra\":1}]}",
        };
        for (const char* bad : bad_catalogs) {
            schema::TableSchema bad_schema("");
            std::vector<schema::TableIndex> bad_indexes;
            std::string bad_error;
            check(!schema::parse_table_schema(bad, bad_schema, bad_indexes,
                                              bad_error) &&
                      bad_error.rfind("line ", 0) == 0 &&
                      bad_indexes.empty(),
                  "malformed index entry refused with a named error");
        }

        // The writer: round-trip through a temp catalog file.
        const std::string catalog_path = temp_path() + "-catalog.json";
        std::remove(catalog_path.c_str());
        schema::TableSchema written("");
        std::vector<schema::TableIndex> written_indexes;
        check(schema::save_table_schema(catalog_path, person,
                                        {{"person_pid", "pid"}}, err),
              "save_table_schema writes a catalog with an index");
        check(schema::load_table_schema(catalog_path, written,
                                        written_indexes, err) &&
                  written.table_name() == "person" &&
                  written.column_count() == person.column_count(),
              "a saved catalog loads again");
        bool columns_match = written.column_count() == person.column_count();
        for (std::size_t i = 0; columns_match && i < written.column_count(); ++i) {
            columns_match = written.column_info(i).column_name() ==
                                person.column_info(i).column_name() &&
                            written.column_info(i).column_type() ==
                                person.column_info(i).column_type() &&
                            written.column_info(i).column_size() ==
                                person.column_info(i).column_size();
        }
        check(columns_match, "saved columns round-trip exactly");
        check(written_indexes.size() == 1 &&
                  written_indexes[0].name == "person_pid" &&
                  written_indexes[0].column == "pid",
              "saved indexes round-trip exactly");
        check(schema::save_table_schema(catalog_path, person,
                                        {{"person_pid", "pid", "hash"}}, err),
              "save_table_schema writes a catalog with a hash kind");
        check(schema::load_table_schema(catalog_path, written,
                                        written_indexes, err) &&
                  written_indexes.size() == 1 &&
                  written_indexes[0].kind == "hash",
              "a saved kind round-trips");
        {
            schema::TableSchema bad_kind_out("");
            std::vector<schema::TableIndex> bad_kind_indexes = {
                {"person_pid", "pid", "btree"}};
            std::string bad_kind_error;
            auto read_file = [](const std::string& path) {
                std::ifstream in(path);
                return std::string(std::istreambuf_iterator<char>{in},
                                   std::istreambuf_iterator<char>{});
            };
            const std::string before = read_file(catalog_path);
            check(!schema::save_table_schema(catalog_path, person,
                                             bad_kind_indexes, bad_kind_error) &&
                      bad_kind_error.find("btree") != std::string::npos,
                  "saving an unknown kind is refused, naming it");
            check(read_file(catalog_path) == before,
                  "the refused kind save left the catalog unchanged");
        }
        check(schema::save_table_schema(catalog_path, person, {}, err),
              "saving with no indexes overwrites cleanly");
        check(schema::load_table_schema(catalog_path, written,
                                        written_indexes, err) &&
                  written_indexes.empty(),
              "the overwritten catalog has no indexes");

        // A refused save leaves the installed catalog byte-for-byte.
        std::ifstream before(catalog_path, std::ios::binary);
        std::ostringstream before_text;
        before_text << before.rdbuf();
        before.close();
        check(!schema::save_table_schema(catalog_path, person,
                                         {{"bad", "nosuch"}}, err) &&
                  !err.empty(),
              "an index naming an unknown column refuses to save");
        std::ifstream after(catalog_path, std::ios::binary);
        std::ostringstream after_text;
        after_text << after.rdbuf();
        after.close();
        check(before_text.str() == after_text.str(),
              "a failed save leaves the catalog unchanged");
        check(std::remove(catalog_path.c_str()) == 0,
              "catalog temp file cleaned up");
    }

    std::cout << (failures == 0
                      ? "storage suite: all checks passed."
                      : "storage suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}
