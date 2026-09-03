#include "elanora/server/schedule_json.hpp"

#include "elanora/csv.hpp"

namespace elanora::server {

std::string schedule_json(const std::vector<collector::PlannedRound>& rounds) {
    std::string out = "{\"rounds\":[";
    for (std::size_t i = 0; i < rounds.size(); ++i) {
        if (i) out += ',';
        out += "{\"index\":" + std::to_string(i + 1);
        out += ",\"condition\":\"";
        out += condition_name(rounds[i].cond);
        // fmt6 rather than std::to_string: the same six-decimal formatting the
        // CSVs use, so a frequency reads identically in the schedule the phone
        // received and in the trial row it later writes.
        out += "\",\"frequency_hz\":" + fmt6(rounds[i].hz);
        out += ",\"jitter_mean_hz\":" + fmt6(rounds[i].jitter_mean_hz) + "}";
    }
    return out + "]}";
}

}  // namespace elanora::server
