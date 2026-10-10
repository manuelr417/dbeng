# SlottedPage layout notes

How `SlottedPage` lays out a 4096-byte block, and in particular where the
method below comes from:

```cpp
// heapfile/SlottedPage.cpp
std::size_t SlottedPage::occupancy_offset(std::size_t slot) const {
    return kSlotCountOffset - capacity_ + slot;
}
```

## Constants

- `kPageSize = 4096` (`bufpool/DataFrame.h`)
- `kPageHeaderSize = 12` — three native-endian u32s: slot count, prev id, next id
- `kSlotCountOffset = kPageSize - kPageHeaderSize = 4084`
- `kPrevPageIdOffset = 4088`, `kNextPageIdOffset = 4092`
- `kRecordSize = sizeof(int) + 10 + sizeof(int) + 3 = 21` (`part/PersonSerializer.h`)
- capacity `C = slot_capacity(R) = (kPageSize - kPageHeaderSize) / (R + 1)`
  = `(4096 - 12) / 22 = 185` slots for `R = 21`

## Page layout

The header is pinned to the **top** (end) of the page; the occupancy array
hangs directly **below** it; records grow **up** from offset 0.

```
offset 0     slot 0 record (21 B)
offset 21    slot 1 record
...
offset 3864  slot 184 record            records end at 3885
offset 3885  ... 14 bytes dead space ...
offset 3899  occ[0]                     occupancy array, 185 bytes
offset 4083  occ[184]                   (4084 - 185 + 184)
offset 4084  slot count (u32)           <- kSlotCountOffset = 4096 - 12
offset 4088  prev page id (u32)
offset 4092  next page id (u32)
```

## The formula

```cpp
return kSlotCountOffset - capacity_ + slot;
//     └──── occ[0]'s offset ────┘   └ walk within array ┘
```

- The array's **end** is pinned at `kSlotCountOffset`.
- Its **length** is `capacity_` (one byte per slot), so its **start** is
  `kSlotCountOffset - capacity_` — that is `occ[0]` (3899 above).
- `+ slot` then indexes within it: `occ[i] = start + i`.

## Why it is built this way

- The header fields sit at fixed offsets *from the top* (`kPageSize - 12/8/4`)
  so they are always findable. The occupancy array's **size varies with
  record size**, so it cannot have a fixed offset from the bottom — but
  anchoring its *end* to the header keeps it adjacent to the header no matter
  how big it is.
- `slot_capacity` charges each slot `record_size + 1` bytes — that `+1` *is*
  the occupancy byte. That accounting is exactly what guarantees
  `C*(R+1) <= 4084`, i.e. the C records at the bottom plus the C occupancy
  bytes at the top always fit, with `dead_space()` bytes of gap in between
  (`dead_space(21) = 4084 - 185*22 = 14`).
- `capacity_` (not a global constant) is used because it is per-page state,
  bound from `format()`/`attach()` — the page is self-describing via its
  stored slot count.

Every occupancy read/write funnels through `occupancy_offset`
(`is_occupied`, `first_free_slot`, `put`, `erase`), so the layout decision
lives in exactly one place.
