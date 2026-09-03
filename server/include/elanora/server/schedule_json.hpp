#pragma once
//
// The round schedule, as JSON for the phone.
//
// The phone runs the protocol locally -- stimulus onset has to be tight, and a
// wifi round trip in that path would put jitter on the one timing that matters
// most. But it does not get to *decide* the protocol: the randomisation
// constraints (no control first or last, no two controls adjacent, jitter means
// drawn from the session's frequency set) are already implemented and tested in
// build_schedule(). Reimplementing them in JavaScript would create a second
// source of truth for the experimental design, and the two would drift.
//
// So the phone asks for a plan and executes it.

#include <string>
#include <vector>

#include "elanora/collector/session.hpp"

namespace elanora::server {

// Hand-rolled rather than pulling in a JSON library: this emits one shape, and
// the shape is four fields.
std::string schedule_json(const std::vector<collector::PlannedRound>& rounds);

}  // namespace elanora::server
