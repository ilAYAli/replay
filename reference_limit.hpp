#pragma once

#include "lichess_analysis.hpp"

enum class ReferenceLimitKind {
    Nodes,
    Depth,
    LoggedDepth
};

struct ReferenceLimit {
    ReferenceLimitKind kind = ReferenceLimitKind::Nodes;
    int value = kDefaultReferenceNodes;
};

inline ReferenceLimit resolveReferenceLimit(const ReferenceLimit& limit, int logged_depth) {
    if (limit.kind != ReferenceLimitKind::LoggedDepth)
        return limit;

    return {ReferenceLimitKind::Depth, logged_depth > 1 ? logged_depth : 1};
}
