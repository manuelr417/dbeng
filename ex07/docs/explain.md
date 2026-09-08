# Binary Person Block Storage Explanation

This project extends the memory-buffer examples from `ex05` and `ex06` into a persistent binary file format.

The program stores data in fixed-size blocks of **4096 bytes**. A `Person` is serialized field by field instead of writing the C++ structure directly. With `city[3]`—two city characters plus a null terminator—the serialized record size is:

```text
pid       4 bytes
name     10 bytes
age       4 bytes
city      3 bytes
----------------
total    21 bytes
```

Therefore, each 4096-byte block holds `4096 / 21 = 195` complete records, with 1 remaining padding byte.

## a) `BlockFile.h` and `BlockFile.cpp`

`BlockFile` is responsible for managing the binary file. It isolates all low-level UNIX-style file operations from the rest of the program.

### File descriptors

The `block_file::File` structure stores the file descriptor returned by `open`:

```cpp
struct File {
    int fd = -1;
};
```

The value `-1` means that no file is currently open. The module provides separate functions for opening a file for appending or reading:

```cpp
open_for_append(path, file, error);
open_for_read(path, file, error);
```

Appending uses:

```cpp
open(path.c_str(), O_RDWR | O_CREAT, 0644);
```

Reading uses `O_RDONLY`. The `close` function closes the descriptor and resets it to `-1`.

### Block addressing with `lseek`

A block number is converted into a byte offset using:

```text
byte offset = block number * 4096
```

For example, block 0 starts at byte 0, block 1 starts at byte 4096, and block 2 starts at byte 8192. The module calls:

```cpp
lseek(fd, offset, SEEK_SET);
```

Before seeking, it checks that the multiplication fits in the platform's `off_t` type.

### Complete reads and writes

The operating system is allowed to read or write fewer bytes than requested. The private `read_all` and `write_all` helpers therefore loop until all 4096 bytes have been processed.

They also retry when the operation is interrupted by `EINTR`. A zero-byte operation before completion is treated as an error so the loop cannot continue forever.

### Appending records

`append_records` performs these steps:

1. Use `lseek(fd, 0, SEEK_END)` to find the file size.
2. Confirm that the size is a multiple of 4096.
3. Read the last block if one already exists, and find its first empty slot (identified by `pid == 0`) — this resume-position logic lives in the private `locate_append_position` helper.
4. Serialize new records into available slots.
5. Write the complete 4096-byte block back to the file.
6. Create and write another block when the current block becomes full.

The result is an appendable file made only of complete blocks.

### Counting records

`record_count` reports how many populated records the file holds in total. Since every block except possibly the last is guaranteed full, it only needs to read the last block and add its occupancy to `(block_count - 1) * kRecordsPerBlock`. `main.cpp`'s `bulk` command uses this to pick the next pid to assign, so repeated bulk inserts do not reuse pids already in the file.

## b) `PersonSerializer.h` and `PersonSerializer.cpp`

`PersonSerializer` owns the binary representation of one `Person` and the layout of a block. This is the persistent equivalent of the memory-buffer work demonstrated in `ex05` and `ex06`.

### Why `sizeof(Person)` is not used

The C++ compiler may insert padding between fields in a structure. The in-memory size of `Person` is therefore not guaranteed to equal the sum of its logical field sizes.

This project defines a stable educational record layout explicitly:

```text
pid, name[10], age, city[3]
```

The code copies each field separately with `memcpy`, using an offset that advances after every field:

```cpp
std::size_t offset = 0;
std::memcpy(buffer + offset, &person.pid, sizeof(person.pid));
offset += sizeof(person.pid);
std::memcpy(buffer + offset, person.name, sizeof(person.name));
offset += sizeof(person.name);
std::memcpy(buffer + offset, &person.age, sizeof(person.age));
offset += sizeof(person.age);
std::memcpy(buffer + offset, person.city, sizeof(person.city));
```

`deserialize` performs the same operations in reverse, copying bytes from the buffer back into a zero-initialized `Person`.

### Block layout

The serializer defines these constants:

```cpp
constexpr std::size_t kRecordSize = 21;
constexpr std::size_t kBlockSize = 4096;
constexpr std::size_t kRecordsPerBlock = kBlockSize / kRecordSize;
```

`initialize_block` sets all 4096 bytes to zero. `serialize_block` writes records consecutively at:

```text
block + record index * kRecordSize
```

Unused space remains zero-filled. When a block is read, `deserialize_block` stops at the first record whose `pid` is zero. This provides a simple empty-slot marker for this exercise.

## c) `PersonCsv.h` and `PersonCsv.cpp`

`PersonCsv` converts human-readable CSV input into validated `Person` values. Keeping it separate means neither `main.cpp` nor `BlockFile.cpp` needs to understand CSV syntax.

The expected row format is:

```text
pid,name,age,city
```

For example:

```text
1,Ana,20,PR
2,Bob,31,NY
```

### Validation

The loader rejects rows with:

- Fewer or more than four fields.
- A nonnumeric or nonpositive `pid`.
- A nonnumeric or negative `age`.
- A name longer than 9 characters, leaving room for `\0` in `name[10]`.
- A city longer than 2 characters, leaving room for `\0` in `city[3]`.

Each `Person` is zero-initialized before the CSV values are copied. This ensures unused bytes in the fixed-size character arrays are null-filled.

### Error handling

The loader returns both valid people and a count of skipped rows:

```cpp
struct LoadResult {
    std::vector<Person> people;
    std::size_t skipped = 0;
};
```

Invalid rows are reported with their line number, but later rows are still processed. This allows one malformed row to be skipped without losing the rest of the CSV file.

## d) `PersonGenerator.h` and `PersonGenerator.cpp`

`PersonGenerator` produces synthetic `Person` records for bulk-insert testing, without needing a hand-written CSV file. It exposes a single function:

```cpp
std::vector<Person> generate(std::size_t count, int first_pid);
```

Pids are assigned sequentially starting at `first_pid`. Names are derived from the pid (`"P" + pid`) so they always fit within `name[10]` regardless of how large `count` is. Ages cycle through a fixed range, and city codes cycle through a small fixed list of two-letter codes, so every record still satisfies the same field-length rules as CSV-loaded records.

Callers are responsible for choosing `first_pid`; the module itself does not know about existing file contents. `main.cpp`'s `bulk` command (see below) picks `first_pid` using `BlockFile::record_count`, so repeated bulk inserts into the same file continue the pid sequence instead of restarting at 1.

## e) `tests.cpp`

`tests.cpp` is a small executable-style test program using C++ `assert` statements. It tests the modules directly without going through the command-line interface.

### Serialization test

The test creates a `Person`, serializes it into a fixed-size buffer, deserializes it, and compares the restored fields. This verifies that the field order and byte offsets match in both directions.

### CSV test

The test creates a temporary CSV file containing two valid rows and one invalid row. It verifies that:

```text
2 rows are loaded
1 row is skipped
```

It also verifies that two-character city codes such as `PR` and `NY` survive parsing.

### File and block test

The test appends the valid records to a temporary binary file, reads block 0, deserializes it, and verifies both record IDs.

It then appends 196 records to another file. Since one block holds 195 records, this verifies that:

- Two blocks are created.
- Block 0 is full.
- Block 1 contains one record.

The temporary files are removed after the assertions complete.

### Generator and bulk-insert test

The test calls `person_generator::generate` directly and checks the pid sequence and derived name. It then bulk-inserts generated records into a temporary binary file in two calls, using `BlockFile::record_count` between them to confirm the second call's pids continue from the first call's count rather than restarting at 1.

## f) `main.cpp`

`main.cpp` is intentionally a thin command-line driver. It does not contain serialization logic, CSV parsing logic, or direct calls to `open`, `lseek`, `read`, or `write`.

### Commands

Append CSV records:

```bash
./build/ex07 append sample.csv people.bin
```

Read one block:

```bash
./build/ex07 read people.bin 0
```

Scan the whole file:

```bash
./build/ex07 scan people.bin
```

Bulk-insert generated records:

```bash
./build/ex07 bulk 4000 people.bin
```

The append command loads the CSV through `person_csv::load`, opens the binary file through `block_file::open_for_append`, and passes the resulting people to `block_file::append_records`.

The read command opens the file through `block_file::open_for_read`, requests one block through `block_file::read_block`, and passes that memory block to `person_serializer::deserialize_block`.

The scan command opens the file the same way, obtains the total number of blocks through `block_file::block_count`, and then repeats the read command's steps for every block in order, printing each block's records in the same format as `read` followed by a final summary line (`scanned N block(s), M record(s)`).

The bulk command opens the file through `block_file::open_for_append`, reads the file's current record count through `block_file::record_count`, generates `count` records starting at `record_count + 1` through `person_generator::generate`, and passes the resulting people to `block_file::append_records` — the same appending path used by the CSV command.

### Displaying records

After deserialization, `main.cpp` prints the block number and each populated record. Presentation belongs here because it is part of the user interface rather than file storage or data encoding.

### Error flow

The module functions return `false` and place a description in an error string when an operation fails. `main.cpp` prints that error and returns a nonzero exit status. This keeps system-level details inside the modules while still giving the user useful feedback.

## Complete data flow

```text
CSV file                     (count, first_pid)
   |                                |
   v                                v
PersonCsv::load          PersonGenerator::generate
   |                                |
   +----------------+---------------+
                     |
                     v
               vector<Person>
                     |
                     v
          BlockFile::append_records
                     |
                     +--> PersonSerializer::serialize
                     |
                     v
            4096-byte binary block
                     |
                     v
             BlockFile::read_block
                     |
                     v
       PersonSerializer::deserialize_block
                     |
                     v
        main.cpp prints Person records
```
