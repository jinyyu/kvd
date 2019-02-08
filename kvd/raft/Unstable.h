#pragma once
#include <kvd/raft/proto.h>

namespace kvd
{


// Unstable.entries[i] has raft log position i+unstable.offset.
// Note that unstable.offset may be less than the highest log
// position in storage; this means that the next write to storage
// might need to truncate the log before persisting unstable.entries.
class Unstable
{
public:
    explicit Unstable(uint64_t offset)
        : offset_(offset)
    {

    }

    // maybeFirstIndex returns the index of the first possible entry in entries
    // if it has a snapshot.
    void maybe_first_index(uint64_t& index, bool& maybe);

    // maybeLastIndex returns the last index if it has at least one
    // unstable entry or snapshot.
    void maybe_last_index(uint64_t& index, bool& maybe);

    // maybeTerm returns the term of the entry at index i, if there
    // is any.
    void maybe_term(uint64_t index, uint64_t& term, bool& maybe);

    void stable_to(uint64_t index, uint64_t term);

    void stable_snap_to(uint64_t index);

    void restore(proto::SnapshotPtr snapshot);

    void truncate_and_append(std::vector<proto::EntryPtr> entries);


private:
    // the incoming unstable snapshot, if any.
    proto::SnapshotPtr snapshot_;
    // all entries that have not yet been written to storage.

    std::vector<proto::EntryPtr> entries_;
    uint64_t offset_;
};

}
