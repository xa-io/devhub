#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace devhub {

// A phrase (or "^word" start-of-message anchor) that votes for a kind.
struct DetectionPattern {
    std::string phrase; // matched case-insensitively; "^add" anchors to start
    std::string kind;   // "suggestion" or "bug"
    double weight = 0.4;
};

struct DetectionResult {
    bool isCandidate = false;      // true when score >= threshold
    std::string kind = "none";     // suggestion | bug | none
    double score = 0.0;            // 0..1
    std::vector<std::string> matched;
};

// Classifies free-form chat messages as feature suggestions or bug reports.
// Pure logic, fully unit-testable; patterns are serializable so the app can
// store user-tuned templates in the settings table.
class SuggestionDetector {
public:
    SuggestionDetector();
    explicit SuggestionDetector(std::vector<DetectionPattern> patterns,
                                double threshold = 0.5);

    DetectionResult analyze(const std::string& message) const;

    static std::vector<DetectionPattern> defaultPatterns();
    static nlohmann::json patternsToJson(const std::vector<DetectionPattern>& pats);
    static std::vector<DetectionPattern> patternsFromJson(const nlohmann::json& j);

    double threshold() const { return threshold_; }
    const std::vector<DetectionPattern>& patterns() const { return patterns_; }

private:
    std::vector<DetectionPattern> patterns_;
    double threshold_ = 0.5;
};

} // namespace devhub
