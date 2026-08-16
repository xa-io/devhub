#include "devhub/SuggestionDetector.h"
#include "devhub/Util.h"

#include <algorithm>
#include <cmath>

namespace devhub {

SuggestionDetector::SuggestionDetector()
    : patterns_(defaultPatterns()) {}

SuggestionDetector::SuggestionDetector(std::vector<DetectionPattern> patterns,
                                       double threshold)
    : patterns_(std::move(patterns)), threshold_(threshold) {}

std::vector<DetectionPattern> SuggestionDetector::defaultPatterns() {
    // Weights: a single strong phrase (0.5+) is enough on its own once the
    // question/imperative bonuses land; weak signals need reinforcement.
    return {
        // --- feature suggestions ---
        {"can you add", "suggestion", 0.65}, {"could you add", "suggestion", 0.65},
        {"can we add", "suggestion", 0.65},  {"could we add", "suggestion", 0.65},
        {"can you make", "suggestion", 0.55},{"could you make", "suggestion", 0.55},
        {"can we get", "suggestion", 0.55},  {"could we get", "suggestion", 0.55},
        {"please add", "suggestion", 0.65},  {"pls add", "suggestion", 0.65},
        {"would be nice", "suggestion", 0.6},{"would be cool", "suggestion", 0.6},
        {"would be great", "suggestion", 0.6},{"would be awesome", "suggestion", 0.6},
        {"would love", "suggestion", 0.5},   {"i wish", "suggestion", 0.5},
        {"wish there was", "suggestion", 0.6},{"any chance", "suggestion", 0.5},
        {"is it possible to", "suggestion", 0.5},
        {"feature request", "suggestion", 0.8},
        {"suggestion:", "suggestion", 0.8},  {"idea:", "suggestion", 0.7},
        {"i suggest", "suggestion", 0.7},    {"my suggestion", "suggestion", 0.7},
        {"you should add", "suggestion", 0.65},{"we should add", "suggestion", 0.6},
        {"should support", "suggestion", 0.5},
        {"how about adding", "suggestion", 0.65},
        {"what if it", "suggestion", 0.4},   {"what about adding", "suggestion", 0.65},
        {"petition to", "suggestion", 0.5},  {"qol", "suggestion", 0.35},
        {"quality of life", "suggestion", 0.35},
        {"it should", "suggestion", 0.3},    {"option to", "suggestion", 0.35},
        {"toggle for", "suggestion", 0.35},  {"support for", "suggestion", 0.35},
        {"^add ", "suggestion", 0.35},       {"^implement ", "suggestion", 0.4},
        {"^allow ", "suggestion", 0.35},     {"^support ", "suggestion", 0.35},
        // --- bug reports ---
        {"bug:", "bug", 0.8},                {"bug report", "bug", 0.8},
        {"found a bug", "bug", 0.8},         {"there's a bug", "bug", 0.8},
        {"theres a bug", "bug", 0.8},        {"crash", "bug", 0.5},
        {"crashes", "bug", 0.55},            {"crashed", "bug", 0.5},
        {"broken", "bug", 0.5},              {"doesn't work", "bug", 0.55},
        {"does not work", "bug", 0.55},      {"doesnt work", "bug", 0.55},
        {"not working", "bug", 0.55},        {"stopped working", "bug", 0.6},
        {"won't work", "bug", 0.5},          {"wont work", "bug", 0.5},
        {"error when", "bug", 0.55},         {"error message", "bug", 0.5},
        {"exception", "bug", 0.45},          {"freezes", "bug", 0.55},
        {"freeze", "bug", 0.4},              {"hangs", "bug", 0.45},
        {"fails to", "bug", 0.5},            {"failing", "bug", 0.4},
        {"issue with", "bug", 0.45},         {"issue when", "bug", 0.5},
        {"problem with", "bug", 0.45},       {"problem when", "bug", 0.5},
        {"^fix ", "bug", 0.4},               {"stack trace", "bug", 0.6},
        {"repro", "bug", 0.4},               {"stuck", "bug", 0.3},
    };
}

DetectionResult SuggestionDetector::analyze(const std::string& message) const {
    DetectionResult r;
    std::string text = toLower(trim(message));
    if (text.size() < 4) return r;

    double sugg = 0.0, bug = 0.0;
    for (const auto& p : patterns_) {
        bool hit = false;
        if (!p.phrase.empty() && p.phrase[0] == '^')
            hit = startsWith(text, p.phrase.substr(1));
        else
            hit = text.find(toLower(p.phrase)) != std::string::npos;
        if (!hit) continue;
        if (p.kind == "bug") bug += p.weight; else sugg += p.weight;
        r.matched.push_back(p.phrase);
    }

    // Contextual bonuses: questions with modal verbs lean suggestion; past
    // tense "was/when i" narratives lean bug.
    bool question = text.find('?') != std::string::npos;
    bool modal = text.find("can ") != std::string::npos ||
                 text.find("could ") != std::string::npos ||
                 text.find("would ") != std::string::npos;
    if (question && modal && sugg > 0.0) sugg += 0.15;
    if ((text.find("when i ") != std::string::npos ||
         text.find("after i ") != std::string::npos) && bug > 0.0)
        bug += 0.15;

    double best = (std::max)(sugg, bug);
    if (best <= 0.0) return r;

    r.score = (std::min)(1.0, best);
    r.kind = (bug > sugg) ? "bug" : "suggestion";
    r.isCandidate = r.score >= threshold_;
    if (!r.isCandidate) r.kind = "none";
    return r;
}

nlohmann::json SuggestionDetector::patternsToJson(const std::vector<DetectionPattern>& pats) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : pats)
        arr.push_back({{"phrase", p.phrase}, {"kind", p.kind}, {"weight", p.weight}});
    return arr;
}

std::vector<DetectionPattern> SuggestionDetector::patternsFromJson(const nlohmann::json& j) {
    std::vector<DetectionPattern> pats;
    if (!j.is_array()) return pats;
    for (const auto& e : j) {
        // Settings and restored databases are trust boundaries. value() throws
        // when called on a non-object, so reject malformed entries before
        // touching any field and keep detector construction non-throwing.
        if (!e.is_object()) continue;
        const auto phrase = e.find("phrase");
        if (phrase == e.end() || !phrase->is_string()) continue;

        DetectionPattern p;
        p.phrase = trim(phrase->get<std::string>());
        if (p.phrase.empty() || p.phrase == "^" || p.phrase.size() > 200)
            continue;

        const auto kind = e.find("kind");
        if (kind != e.end() && kind->is_string()) {
            const std::string requestedKind = kind->get<std::string>();
            p.kind = requestedKind == "bug" || requestedKind == "suggestion"
                         ? requestedKind
                         : "suggestion";
        } else {
            p.kind = "suggestion";
        }

        const auto weight = e.find("weight");
        if (weight != e.end() && weight->is_number()) {
            const double requestedWeight = weight->get<double>();
            if (std::isfinite(requestedWeight)) p.weight = requestedWeight;
        }
        p.weight = (std::max)(0.0, (std::min)(p.weight, 1.0));

        pats.push_back(std::move(p));
        if (pats.size() >= 500) break;
    }
    return pats;
}

} // namespace devhub
